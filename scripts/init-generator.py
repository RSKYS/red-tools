#!/usr/bin/env python3

# init-generator.py (Systemd/OpenRC)

# Copyright 2023-2026 Pouria Rezaei <Pouria.rz@outlook.com>
# All rights reserved.
#
# Redistribution and use of this script, with or without modification, is
# permitted provided that the following conditions are met:
#
# 1. Redistributions of this script must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
#
#  THIS SOFTWARE IS PROVIDED BY THE AUTHOR "AS IS" AND ANY EXPRESS OR IMPLIED
#  WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
#  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO
#  EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
#  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
#  PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
#  OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
#  WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
#  OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
#  ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

from __future__ import annotations
from pathlib import Path
import atexit
import os
import re
import shutil
import signal
import subprocess
import sys
import shlex

PROGRAM = Path(sys.argv[0]).name
SYSTEMD_DIR = Path("/etc/systemd/system")
OPENRC_DIR = Path("/etc/init.d")
HELPER_DIR = Path("/usr/local/libexec")

os.umask(0o022)

tmp: Path | None = None
backup: Path | None = None
helper_tmp: Path | None = None
helper_backup: Path | None = None


class ScriptError(Exception):
	pass


def die(message: str) -> None:
	raise ScriptError(message)


def require_root() -> None:
	if os.geteuid() != 0:
		die("Administrator privileges are required; rerun this script with sudo")

	sudo_user = os.environ.get("SUDO_USER")
	if sudo_user and sudo_user != "root":
		print(f"Privilege check: running as root through sudo (user: {sudo_user}).", file=sys.stderr)
	else:
		print("Privilege check: running as root.", file=sys.stderr)


def ask(prompt: str) -> str:
	sys.stderr.write(prompt)
	sys.stderr.flush()

	line = sys.stdin.readline()
	if line == "":
		sys.stderr.write("\n")
		die("input ended unexpectedly")

	return line[:-1] if line.endswith("\n") else line


def remove_if_present(path: Path | None) -> None:
	if path is None:
		return

	try:
		path.unlink(missing_ok=True)
	except OSError:
		pass


def cleanup() -> None:
	for path in (tmp, backup, helper_tmp, helper_backup):
		remove_if_present(path)


def signal_exit(_signum: int, _frame: object) -> None:
	raise SystemExit(1)


def some_quote(value: str) -> str:
	return value.replace("\\", "\\\\").replace('"', '\\"').replace("%", "%%")


def shell_quote(value: str) -> str:
	return "'" + value.replace("'", "'\\''") + "'"


def exclusive_file(path: Path):
	try:
		descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
	except OSError:
		die(f"could not create temporary file: {path}")

	return os.fdopen(descriptor, "w", encoding="utf-8", newline="\n")


def restore(target: Path, saved: Path | None) -> None:
	remove_if_present(target)
	if saved is not None and saved.exists():
		os.replace(saved, target)


def confirm_overwrite(path: Path) -> None:
	if not path.exists():
		return

	answer = ask(f"{path} already exists. Overwrite it? [y/N]: ")
	if answer not in {"y", "Y", "yes", "Yes", "YES"}:
		die("operation cancelled")


def detect_init() -> str:
	if Path("/run/systemd/system").is_dir() and shutil.which("systemctl"):
		return "systemd"

	if (
		Path("/sbin/openrc-run").is_file()
		and os.access("/sbin/openrc-run", os.X_OK)
		and shutil.which("rc-service")
		and shutil.which("rc-update")
	):
		return "openrc"

	die("neither an active systemd installation nor OpenRC was found")


def resolve_executable(requested: str) -> str | None:
	if requested.startswith("/"):
		path = Path(requested)
		if path.is_file() and os.access(path, os.X_OK):
			return requested
		return None

	if "/" in requested:
		return None

	return shutil.which(requested)


def verify_shell_script(path: Path, description: str) -> None:
	result = subprocess.run(["/bin/sh", "-n", str(path)], check=False)
	if result.returncode != 0:
		die(f"the generated {description} has invalid syntax")


def write_systemd(
	service: str,
	description: str,
	directory: str,
	executable: str,
	systemd_args: str,
	restart: str,
	delay: str,
) -> None:
	global tmp, backup

	unit = f"{service}.service"
	target = SYSTEMD_DIR / unit
	confirm_overwrite(target)

	description_q = some_quote(description)
	directory_q = some_quote(directory)
	executable_q = executable
	tmp = Path(f"{target}.tmp.{os.getpid()}")
	backup = None

	with exclusive_file(tmp) as stream:
		stream.write(
			f'''[Unit]\n'''
			f'''Description="{description_q}"\n'''
			f'''After=network.target\n'''
			f'''\n'''
			f'''[Service]\n'''
			f'''Type=simple\n'''
			f'''User=root\n'''
			f'''WorkingDirectory={directory_q}\n'''
			f'''ExecStart="{executable_q}"{systemd_args}\n'''
			f'''Restart={restart}\n'''
			f'''RestartSec={delay}\n'''
			f'''\n'''
			f'''StandardOutput=journal\n'''
			f'''StandardError=journal\n'''
			f'''\n'''
			f'''[Install]\n'''
			f'''WantedBy=multi-user.target\n'''
		)

	os.chmod(tmp, 0o644)

	if target.exists():
		backup = Path(f"{target}.backup.{os.getpid()}")
		shutil.copy2(target, backup)

	os.replace(tmp, target)
	tmp = None

	if shutil.which("systemd-analyze"):
		result = subprocess.run(
			["systemd-analyze", "verify", str(target)],
			check=False,
		)
		if result.returncode != 0:
			restore(target, backup)
			backup = None
			die("systemd rejected the generated unit file")

	remove_if_present(backup)
	backup = None

	result = subprocess.run(["systemctl", "daemon-reload"], check=False)
	if result.returncode != 0:
		die("systemctl daemon-reload failed")

	print(f"\nCreated systemd unit: {target}")
	print(f"Executable: {executable}")
	print(f"\nEnable and start it with:\n  systemctl enable --now {unit}")
	print("\nView its status and logs with:")
	print(f"  systemctl status {unit}")
	print(f"  journalctl -u {unit} -f")


def write_helper(helper: Path, helper_command: str, delay: str) -> None:
	global helper_tmp

	HELPER_DIR.mkdir(parents=True, exist_ok=True)
	helper_tmp = Path(f"{helper}.tmp.{os.getpid()}")

	with exclusive_file(helper_tmp) as stream:
		stream.write(
			f'''#!/bin/sh\n'''
			f'''set -u\n'''
			f'''\n'''
			f'''child=\n'''
			f'''stopping=0\n'''
			f'''\n'''
			f'''stop_child()\n'''
			f'''{{\n'''
			f'''    stopping=1\n'''
			f'''    [ -z "${{child:-}}" ] || kill -TERM "$child" 2>/dev/null || :\n'''
			f'''}}\n'''
			f'''\n'''
			f'''trap stop_child HUP INT TERM\n'''
			f'''\n'''
			f'''while :; do\n'''
			f'''    {helper_command} &\n'''
			f'''    child=$!\n'''
			f'''    status=0\n'''
			f'''    wait "$child" || status=$?\n'''
			f'''    child=\n'''
			f'''\n'''
			f'''    [ "$stopping" -eq 0 ] || exit 0\n'''
			f'''    [ "$status" -ne 0 ] || exit 0\n'''
			f'''\n'''
			f'''    sleep {delay} &\n'''
			f'''    child=$!\n'''
			f'''    wait "$child" || :\n'''
			f'''    child=\n'''
			f'''    [ "$stopping" -eq 0 ] || exit 0\n'''
			f'''done\n'''
		)

	os.chmod(helper_tmp, 0o755)
	verify_shell_script(helper_tmp, "OpenRC on-failure helper")


def write_openrc(
	service: str,
	description: str,
	directory: str,
	executable: str,
	openrc_args: str,
	helper_command: str,
	restart: str,
	delay: str,
) -> None:
	global tmp, backup, helper_tmp, helper_backup

	target = OPENRC_DIR / service
	helper = HELPER_DIR / f"{service}-on-failure"
	confirm_overwrite(target)

	description_q = shell_quote(description)
	directory_q = shell_quote(directory)
	executable_q = shell_quote(executable)
	openrc_args_q = shell_quote(openrc_args)
	tmp = Path(f"{target}.tmp.{os.getpid()}")
	backup = None
	helper_backup = None

	if restart == "on-failure":
		write_helper(helper, helper_command, delay)
		command_q = shell_quote(str(helper))
	else:
		command_q = executable_q

	with exclusive_file(tmp) as stream:
		if restart == "always":
			stream.write(
				f'''#!/sbin/openrc-run\n'''
				f'''\n'''
				f'''description={description_q}\n'''
				f'''supervisor=supervise-daemon\n'''
				f'''command={command_q}\n'''
				f'''command_args={openrc_args_q}\n'''
				f'''command_user=root\n'''
				f'''directory={directory_q}\n'''
				f'''pidfile="/run/${{RC_SVCNAME}}.pid"\n'''
				f'''respawn_delay={delay}\n'''
				f'''respawn_max=0\n'''
				f'''retry="TERM/10/KILL/5"\n'''
				f'''umask=022\n'''
				f'''\n'''
				f'''depend()\n'''
				f'''{{\n'''
				f'''    after net\n'''
				f'''}}\n'''
			)
		else:
			stream.write(
				f'''#!/sbin/openrc-run\n'''
				f'''\n'''
				f'''description={description_q}\n'''
				f'''command={command_q}\n'''
				f'''command_user=root\n'''
				f'''directory={directory_q}\n'''
				f'''command_background=true\n'''
				f'''pidfile="/run/${{RC_SVCNAME}}.pid"\n'''
				f'''retry="TERM/10/KILL/5"\n'''
				f'''stopgroup=true\n'''
				f'''umask=022\n'''
				f'''\n'''
				f'''depend()\n'''
				f'''{{\n'''
				f'''    after net\n'''
				f'''}}\n'''
			)

	os.chmod(tmp, 0o755)
	verify_shell_script(tmp, "OpenRC service script")

	if target.exists():
		backup = Path(f"{target}.backup.{os.getpid()}")
		shutil.copy2(target, backup)

	if restart == "on-failure":
		if helper.exists():
			helper_backup = Path(f"{helper}.backup.{os.getpid()}")
			shutil.copy2(helper, helper_backup)

		try:
			os.replace(helper_tmp, helper)
		except OSError:
			die("could not install the OpenRC restart helper")
		helper_tmp = None

	try:
		os.replace(tmp, target)
	except OSError:
		if restart == "on-failure":
			restore(helper, helper_backup)
			helper_backup = None
		die("could not install the OpenRC service script")
	tmp = None

	remove_if_present(backup)
	backup = None

	if restart == "on-failure":
		remove_if_present(helper_backup)
		helper_backup = None
	else:
		remove_if_present(helper)

	print(f"\nCreated OpenRC service: {target}")
	if restart == "on-failure":
		print(f"Created restart helper: {helper}")
	print(f"Executable: {executable}")
	print("\nEnable and start it with:")
	print(f"  rc-update add {service} default")
	print(f"  rc-service {service} start")
	print(f"\nView its status with:\n  rc-service {service} status")
	if shutil.which("logread"):
		print("\nView system logs with:\n  logread -f")


def read_service_name() -> str:
	while True:
		answer = ask("Service name (without .service): ")
		service = answer.removesuffix(".service")

		if not service:
			sys.stderr.write("Service name cannot be empty.\n")
		elif not re.fullmatch(r"[A-Za-z0-9_.@-]+", service):
			sys.stderr.write(
				"Use only letters, digits, underscore, dot, @, or hyphen.\n"
			)
		elif service in {".", ".."}:
			sys.stderr.write("Service name cannot be '.' or '..'.\n")
		else:
			return service


def read_directory() -> str:
	while True:
		answer = ask("Absolute application directory: ")
		if not answer.startswith("/"):
			sys.stderr.write("Enter an absolute directory path.\n")
			continue

		try:
			if Path(answer).is_dir() and os.access(answer, os.X_OK):
				return os.path.realpath(answer)
		except OSError:
			pass

		sys.stderr.write(
			"The application directory does not exist or is inaccessible.\n"
		)


def read_executable() -> tuple[str, str]:
	while True:
		answer = ask("Executable path or command name (for example: exec): ")
		if not answer:
			sys.stderr.write("Executable cannot be empty.\n")
			continue

		parts = answer.split(maxsplit=1)
		cmd = parts[0]
		args = parts[1] if len(parts) > 1 else ""

		executable = resolve_executable(cmd)
		if executable is not None:
			return executable, args

		sys.stderr.write(
			"Executable was not found or is not executable.\n"
		)


def read_arguments(executable: str, initial_args: str = "") -> tuple[str, str, str]:
	systemd_args = ""
	openrc_arguments: list[str] = []
	helper_parts = [shell_quote(executable)]
	number = 1

	if initial_args:
		for arg in shlex.split(initial_args):
			systemd_args += f' "{some_quote(arg)}"'
			quoted = shell_quote(arg)
			openrc_arguments.append(quoted)
			helper_parts.append(quoted)
			number += 1

	if number > 1:
		sys.stderr.write(f"Detected {number - 1} initial argument(s).\n")

	sys.stderr.write(
		"Enter additional executable arguments one at a time.\n"
		"Leave an argument empty to finish.\n"
	)

	while True:
		answer = ask(f"Argument {number}: ")
		if not answer:
			break

		systemd_args += f' "{some_quote(answer)}"'
		quoted = shell_quote(answer)
		openrc_arguments.append(quoted)
		helper_parts.append(quoted)
		number += 1

	return systemd_args, " ".join(openrc_arguments), " ".join(helper_parts)


def read_restart_policy() -> str:
	while True:
		answer = ask("Restart policy: always or failure [failure]: ")
		if answer == "":
			return "on-failure"
		if answer == "always":
			return "always"
		if answer in {"failure", "on-failure"}:
			return "on-failure"
		sys.stderr.write("Enter 'always' or 'failure'.\n")


def read_restart_delay() -> str:
	while True:
		answer = ask("Restart delay in seconds [3]: ")
		delay = answer or "3"
		if re.fullmatch(r"[0-9]+", delay):
			return delay
		sys.stderr.write("Restart delay must be a non-negative whole number.\n")


def main() -> None:
	if sys.version_info < (3, 11):
		die("Python 3.11 or newer is required")
	require_root()
	if len(sys.argv) != 1:
		die("this script accepts no arguments")

	init = detect_init()
	print(f"Detected init system: {init}", file=sys.stderr)
	service = read_service_name()

	answer = ask("Description [My Application]: ")
	description = answer or "My Application"

	directory = read_directory()
	executable, initial_args = read_executable()
	systemd_args, openrc_args, helper_command = read_arguments(executable, initial_args)
	restart = read_restart_policy()
	delay = read_restart_delay()

	if init == "systemd":
		write_systemd(
			service,
			description,
			directory,
			executable,
			systemd_args,
			restart,
			delay,
		)
	else:
		write_openrc(
			service,
			description,
			directory,
			executable,
			openrc_args,
			helper_command,
			restart,
			delay,
		)


atexit.register(cleanup)
for handled_signal in (signal.SIGHUP, signal.SIGINT, signal.SIGTERM):
	signal.signal(handled_signal, signal_exit)

if __name__ == "__main__":
	try:
		main()
	except ScriptError as error:
		print(f"{PROGRAM}: {error}", file=sys.stderr)
		raise SystemExit(1) from None
	except OSError as error:
		print(f"{PROGRAM}: {error}", file=sys.stderr)
		raise SystemExit(1) from None
	except KeyboardInterrupt:
		raise SystemExit(1) from None
