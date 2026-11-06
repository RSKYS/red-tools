#!/bin/sh
#
# build-debian-iso.sh
#
# Copyright 2026 Pouria Rezaei <Pouria.rz@outlook.com>
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

# Environment:
#   OUTPUT_ISO                  Output ISO path (default: ./debian-live-server.iso)
#   LOG_FILE                    Build log path (default: ./debian-live-build.log)
#   WORKSPACE_DIR               Build workspace; must not already exist
#   DEBIAN_SUITE                Suite to build (default: stable)
#   LOCALE, KEYMAP, TIMEZONE    Installer locale, keyboard and time zone
#   TARGET_HOSTNAME             Preseeded hostname
#   TARGET_DOMAIN               Preseeded domain
#   TARGET_DISK                 Whole disk to erase, e.g. /dev/sda (requires
#                               AUTHORIZE_DESTRUCTIVE_INSTALL=YES)
#   AUTHORIZE_DESTRUCTIVE_INSTALL=YES   Explicit consent to erase TARGET_DISK
#   CHECK_ONLY=1                Run preflight checks and exit without building
#   BOOT_TIMEOUT_SECONDS=5      Boot menu timeout before the installer starts (1-600)
#

set -eu
umask 077

CWD=$(pwd -P)
OUTPUT_ISO=${OUTPUT_ISO:-$CWD/debian-live-server_red.iso}
LOG_FILE=${LOG_FILE:-$CWD/debian-live-build.log}
WORKSPACE_DIR=${WORKSPACE_DIR:-}
DEBIAN_SUITE=${DEBIAN_SUITE:-stable}
LOCALE=${LOCALE:-en_US.UTF-8}
KEYMAP=${KEYMAP:-us}
TIMEZONE=${TIMEZONE:-UTC}
TARGET_HOSTNAME=${TARGET_HOSTNAME:-}
TARGET_DOMAIN=${TARGET_DOMAIN:-}
TARGET_DISK=${TARGET_DISK:-}
AUTHORIZE_DESTRUCTIVE_INSTALL=${AUTHORIZE_DESTRUCTIVE_INSTALL:-NO}
CHECK_ONLY=${CHECK_ONLY:-0}
BOOT_TIMEOUT_SECONDS=${BOOT_TIMEOUT_SECONDS:-5}

DEBIAN_MIRROR=https://deb.debian.org/debian
SECURITY_MIRROR=https://security.debian.org/debian-security
KEYRING=/usr/share/keyrings/debian-archive-keyring.gpg
MIN_FREE_KB=20000000
MIN_ISO_BYTES=104857600
LOGGING=0

# ---- Helpers ----------------------------------------------------------------

fail()
{
	printf 'ERROR: %s\n' "$*" >&2
	if [ "$LOGGING" = 1 ]; then
		printf 'ERROR: %s\n' "$*" >&3
	fi
	exit 1
}

say()
{
	printf '%s\n' "$*" >&3
	printf '%s\n' "$*"
}

run()
{
	say "+ $*"
	"$@" || {
		rc=$?
		say "ERROR: command failed (exit $rc): $*"
		tail -n 80 "$LOG_FILE" >&3 2>/dev/null || :
		exit "$rc"
	}
}

cleanup()
{
	rc=$?
	trap - 0 HUP INT TERM
	if [ "$rc" -ne 0 ]; then
		printf 'Build failed (exit %s).\nWorkspace: %s\nLog: %s\n' \
			"$rc" "${WORKSPACE_DIR:-not-created}" "$LOG_FILE" >&3
	fi
	exit "$rc"
}

require_cmds()
{
	for _cmd; do
		command -v "$_cmd" >/dev/null 2>&1 || fail "Required command is missing: $_cmd"
	done
}

has_dotdot()
{
	case "/$1/" in
		*/../*) return 0 ;;
		*) return 1 ;;
	esac
}

path_exists()
{
	[ -e "$1" ] || [ -L "$1" ]
}

timestamp()
{
	date -u '+%Y-%m-%dT%H:%M:%SZ'
}

sha256_of()
{
	sha256sum "$1" | awk '{ print $1 }'
}

put()
{
	cat > "$2"
	chmod "$1" "$2"
}

verify_inrelease()
{
	curl -fsSL --retry 3 --connect-timeout 20 "$1" -o "$WORKSPACE_DIR/$2.InRelease"
	gpgv --keyring "$KEYRING" "$WORKSPACE_DIR/$2.InRelease" \
		> "$WORKSPACE_DIR/$2-signature.txt" 2>&1 ||
		fail "The $3 InRelease signature could not be verified."
}

iso_report()
{
	_name=$1
	shift
	xorriso -indev "$OUTPUT_ISO" "$@" > "$WORKSPACE_DIR/$_name" 2>&1 || :
}

iso_extract()
{
	xorriso -osirrox on -indev "$OUTPUT_ISO" -extract "$1" "$2" \
		>> "$WORKSPACE_DIR/extract.log" 2>&1 ||
		fail "Cannot extract $1 from the generated ISO; see $WORKSPACE_DIR/extract.log."
	[ -s "$2" ] || fail "$1 is missing or empty on the generated ISO."
}

canon_path()
{
	if [ -L "$1" ]; then
		readlink -f -- "$1" || printf '%s\n' "$1"
		return 0
	fi
	_cp_base=$(basename "$1")
	_cp_dir=$(dirname "$1")
	_cp_rest=
	while [ ! -d "$_cp_dir" ] && [ "$_cp_dir" != / ]; do
		_cp_rest=$(basename "$_cp_dir")${_cp_rest:+/}$_cp_rest
		_cp_dir=$(dirname "$_cp_dir")
	done
	_cp_real=$(cd -P "$_cp_dir" && pwd -P) || _cp_real=$_cp_dir
	if [ -n "$_cp_rest" ]; then
		_cp_real=${_cp_real%/}/$_cp_rest
	fi
	case $_cp_real in
		/) printf '/%s\n' "$_cp_base" ;;
		*) printf '%s/%s\n' "$_cp_real" "$_cp_base" ;;
	esac
}

require_distinct()
{
	_rd_a=$(canon_path "$2")
	_rd_b=$(canon_path "$4")
	if [ "$_rd_a" = "$_rd_b" ]; then
		fail "Conflicting output paths: $1 ($2) and $3 ($4) both refer to $_rd_a."
	fi
}

# ---- Host and parameter validation ------------------------------------------

require_cmds id dpkg grep awk sed df dirname mkdir wc tr tail date uname cat \
	cp mv rm chmod mktemp sha256sum readlink

[ "$(id -u)" -eq 0 ] || fail "Run this script as root."
[ -r /etc/os-release ] || fail "Cannot identify the build host OS."
grep -q '^ID=debian$' /etc/os-release || fail "The build host must be Debian."
[ "$(dpkg --print-architecture)" = amd64 ] || fail "The build host must be AMD64."

case $OUTPUT_ISO in /*) ;; *) OUTPUT_ISO=$CWD/$OUTPUT_ISO ;; esac
case $LOG_FILE in /*) ;; *) LOG_FILE=$CWD/$LOG_FILE ;; esac
ISO_SHA_FILE=$OUTPUT_ISO.sha256
ISO_PROVENANCE=$OUTPUT_ISO.provenance.txt

for _p in "$OUTPUT_ISO" "$LOG_FILE"; do
	if has_dotdot "$_p"; then
		fail "Output and log paths must not contain '..' components."
	fi
done
case $OUTPUT_ISO in */) fail "OUTPUT_ISO must name a file, not a directory." ;; esac
case $LOG_FILE in */) fail "LOG_FILE must name a file, not a directory." ;; esac

# Collision checks run before the log is opened, so LOG_FILE can never alias an output.
require_distinct OUTPUT_ISO "$OUTPUT_ISO" LOG_FILE "$LOG_FILE"
require_distinct OUTPUT_ISO "$OUTPUT_ISO" OUTPUT_ISO.sha256 "$ISO_SHA_FILE"
require_distinct OUTPUT_ISO "$OUTPUT_ISO" OUTPUT_ISO.provenance.txt "$ISO_PROVENANCE"
require_distinct LOG_FILE "$LOG_FILE" OUTPUT_ISO.sha256 "$ISO_SHA_FILE"
require_distinct LOG_FILE "$LOG_FILE" OUTPUT_ISO.provenance.txt "$ISO_PROVENANCE"
require_distinct OUTPUT_ISO.sha256 "$ISO_SHA_FILE" OUTPUT_ISO.provenance.txt "$ISO_PROVENANCE"

case $DEBIAN_SUITE in ''|*[!a-zA-Z0-9.-]*) fail "Invalid DEBIAN_SUITE." ;; esac
case $LOCALE in ''|*[!A-Za-z0-9_.@-]*) fail "Invalid LOCALE." ;; esac
case $KEYMAP in ''|*[!A-Za-z0-9_-]*) fail "Invalid KEYMAP." ;; esac
case $TIMEZONE in ''|/*|*..*|*[!A-Za-z0-9_+./-]*) fail "Invalid TIMEZONE." ;; esac

case $BOOT_TIMEOUT_SECONDS in ''|*[!0-9]*) fail "BOOT_TIMEOUT_SECONDS must be a whole number of seconds." ;; esac
if [ "$BOOT_TIMEOUT_SECONDS" -lt 1 ] || [ "$BOOT_TIMEOUT_SECONDS" -gt 600 ]; then
	fail "BOOT_TIMEOUT_SECONDS must be between 1 and 600."
fi

if [ -n "$TARGET_HOSTNAME" ]; then
	case $TARGET_HOSTNAME in -*|*-|*[!a-zA-Z0-9-]*) fail "Invalid TARGET_HOSTNAME." ;; esac
	[ "${#TARGET_HOSTNAME}" -le 63 ] || fail "TARGET_HOSTNAME is too long."
fi

# Domain labels: letters, digits and hyphens; no empty labels; no leading or trailing hyphen or dot.
if [ -n "$TARGET_DOMAIN" ]; then
	case $TARGET_DOMAIN in
		-*|*-|.*|*.|*..*|*.-*|*-.*|*[!a-zA-Z0-9.-]*) fail "Invalid TARGET_DOMAIN." ;;
	esac
fi

if [ -n "$TARGET_DISK" ] || [ "$AUTHORIZE_DESTRUCTIVE_INSTALL" = YES ]; then
	[ -n "$TARGET_DISK" ] ||
		fail "TARGET_DISK is required when destructive-install authorization is enabled."
	[ "$AUTHORIZE_DESTRUCTIVE_INSTALL" = YES ] ||
		fail "Set AUTHORIZE_DESTRUCTIVE_INSTALL=YES to authorize erasure of TARGET_DISK."
	case $TARGET_DISK in /dev/*) ;; *) fail "TARGET_DISK must be an explicit /dev device path." ;; esac
	DISK_NAME=${TARGET_DISK#/dev/}
	case $DISK_NAME in
		''|.|..|*/*|*[!A-Za-z0-9._-]*) fail "TARGET_DISK must name a whole disk directly under /dev." ;;
	esac
fi

for _f in "$OUTPUT_ISO" "$ISO_SHA_FILE" "$ISO_PROVENANCE"; do
	if path_exists "$_f"; then
		fail "Refusing to overwrite $_f."
	fi
done

free_kb=$(df -Pk "$CWD" | awk 'NR == 2 { print $4 }')
case $free_kb in ''|*[!0-9]*) fail "Cannot determine free space on $CWD." ;; esac
[ "$free_kb" -ge "$MIN_FREE_KB" ] ||
	fail "At least 20 GB free on the current filesystem is required; available: $((free_kb / 1024)) MB."

if [ "$CHECK_ONLY" = 1 ]; then
	printf '%s\n' "Preflight passed." \
		"Build directory: $CWD" \
		"Output ISO: $OUTPUT_ISO" \
		"No target interface, IP, prefix, gateway, DNS, disk, or credentials are required to build."
	exit 0
fi

# ---- Logging and workspace --------------------------------------------------

[ ! -L "$LOG_FILE" ] || fail "Refusing to write through a symbolic-link log path: $LOG_FILE."
if [ -e "$LOG_FILE" ] && [ ! -f "$LOG_FILE" ]; then
	fail "Log path is not a regular file: $LOG_FILE."
fi
mkdir -p "$(dirname "$OUTPUT_ISO")" "$(dirname "$LOG_FILE")"
printf '\n===== Build started: %s =====\n' "$(timestamp)" >> "$LOG_FILE"
chmod 600 "$LOG_FILE"
exec 3>&1
exec >> "$LOG_FILE" 2>&1
LOGGING=1

if [ -n "$WORKSPACE_DIR" ]; then
	case $WORKSPACE_DIR in /*) ;; *) WORKSPACE_DIR=$CWD/$WORKSPACE_DIR ;; esac
	if has_dotdot "$WORKSPACE_DIR"; then
		fail "WORKSPACE_DIR must not contain '..' components."
	fi
	if path_exists "$WORKSPACE_DIR"; then
		fail "Refusing to reuse existing workspace $WORKSPACE_DIR."
	fi
	mkdir -m 700 "$WORKSPACE_DIR"
else
	WORKSPACE_DIR=$(mktemp -d "$CWD/.debian-live-build.XXXXXX")
fi

trap cleanup 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

say "Preparing a Debian Live Standard AMD64 build."
say "Build directory: $CWD"
say "Workspace: $WORKSPACE_DIR"
say "Output ISO: $OUTPUT_ISO"
say "Target network configuration will be discovered only by the installer."

# ---- Build dependencies and repository verification -------------------------

export DEBIAN_FRONTEND=noninteractive
run apt-get update
run apt-get install -y --no-install-recommends \
	live-build debootstrap xorriso isolinux syslinux-common \
	grub-pc-bin grub-efi-amd64-bin mtools dosfstools squashfs-tools \
	curl ca-certificates debian-archive-keyring gnupg gpgv rsync xz-utils

require_cmds lb debootstrap xorriso curl gpgv

LB_VERSION=$(dpkg-query -W -f='${Version}' live-build)
LB_HELP=$(lb config --help 2>&1) || fail "Cannot query live-build options."
case $LB_HELP in
	*--debian-installer-gui*) ;;
	*) fail "Installed live-build lacks --debian-installer-gui support." ;;
esac

verify_inrelease "$DEBIAN_MIRROR/dists/$DEBIAN_SUITE/InRelease" source "Debian suite"

CODENAME=$(awk -F ': ' '$1 == "Codename" { print $2; exit }' \
	"$WORKSPACE_DIR/source.InRelease" | tr -d '\r')
[ -n "$CODENAME" ] || fail "Could not resolve Debian codename from signed InRelease metadata."
case $CODENAME in *[!a-z0-9]*) fail "Unexpected Debian codename: $CODENAME" ;; esac

verify_inrelease "$SECURITY_MIRROR/dists/${CODENAME}-security/InRelease" security "Debian security"

say "Resolved Debian suite: $DEBIAN_SUITE ($CODENAME)"
say "live-build version: $LB_VERSION"
cd "$WORKSPACE_DIR"

# ---- live-build configuration -----------------------------------------------

# Use root for the live console autologin. Disable cloud-init only for this live boot:
# the live image does not need cloud metadata discovery, which can delay startup.
BOOTAPPEND_LIVE="boot=live components username=root cloud-init=disabled systemd.mask=systemd-networkd-wait-online.service quiet"
BOOTAPPEND_INSTALL="auto=true priority=critical preseed/file=/cdrom/preseed.cfg"
ISO_VOLUME="CUSTOM-DEBIAN-${CODENAME}-AMD64"

run lb config \
	--mode debian \
	--system live \
	--distribution "$CODENAME" \
	--architectures amd64 \
	--archive-areas "main contrib non-free non-free-firmware" \
	--mirror-bootstrap "$DEBIAN_MIRROR" \
	--mirror-chroot "$DEBIAN_MIRROR" \
	--mirror-chroot-security "$SECURITY_MIRROR" \
	--mirror-binary "$DEBIAN_MIRROR" \
	--mirror-binary-security "$SECURITY_MIRROR" \
	--linux-flavours amd64 \
	--binary-images iso-hybrid \
	--bootloader "syslinux,grub-efi" \
	--debian-installer live \
	--debian-installer-gui true \
	--debian-installer-distribution "$CODENAME" \
	--bootappend-install "$BOOTAPPEND_INSTALL" \
	--bootappend-live "$BOOTAPPEND_LIVE" \
	--firmware-chroot true \
	--firmware-binary true \
	--apt-secure true \
	--apt-recommends true \
	--apt-indices true \
	--security true \
	--updates true \
	--backports false \
	--memtest none \
	--checksums sha256 \
	--image-name "debian-live-server-live-standard-${CODENAME}-amd64" \
	--iso-application "Custom Debian Live Standard" \
	--iso-preparer "Local live-build derivative" \
	--iso-publisher "Custom Debian Live build" \
	--iso-volume "$ISO_VOLUME"

mkdir -p config/package-lists config/includes.chroot/etc/netplan \
	config/hooks/normal config/includes.binary config/bootloaders/isolinux config/bootloaders/grub-pc

cat > config/package-lists/custom-standard.list.chroot <<'PACKAGES'
bcache-tools
byobu
ca-certificates
cryptsetup
curl
dosfstools
fastfetch
firmware-linux
git
htop
iproute2
linux-headers-amd64
live-boot
live-config
live-config-systemd
live-task-standard
live-tools
multipath-tools
netplan.io
open-iscsi
openssh-server
overlayroot
parted
pciutils
python-is-python3
python3-pip
python3-venv
screen
sos
sudo
systemd-resolved
systemd-timesyncd
tmux
usbutils
vim
wget
zstd
PACKAGES

# cloud-init is installed on the target by pkgsel, not in the live root, to avoid its boot-time probes.
# Static Netplan for the live system. The installed system gets its own from the installer hook.
put 600 config/includes.chroot/etc/netplan/50-cloud-init.yaml <<'NETPLAN'
network:
  version: 2
  renderer: networkd
  ethernets:
    all-ethernet:
      match:
        name: "e*"
      dhcp4: true
      dhcp6: false
NETPLAN

put 644 config/includes.chroot/etc/sysctl.conf <<'SYSCTL'
net.ipv4.conf.all.accept_redirects = 0
net.ipv4.conf.all.rp_filter = 1
net.ipv4.conf.default.accept_redirects = 0
net.ipv4.conf.default.rp_filter = 1
net.ipv4.icmp_echo_ignore_broadcasts = 1
net.ipv4.tcp_sack = 1
net.ipv4.tcp_slow_start_after_idle = 0
net.ipv4.tcp_syncookies = 1
net.ipv4.tcp_timestamps = 1
net.ipv4.tcp_window_scaling = 1
net.ipv6.conf.all.disable_ipv6 = 1
net.ipv6.conf.default.disable_ipv6 = 1
net.ipv6.conf.lo.disable_ipv6 = 1
SYSCTL
cp config/includes.chroot/etc/sysctl.conf \
	config/includes.binary/sysctl.conf

put 755 config/hooks/normal/0100-network-stack.hook.chroot <<'HOOK'
#!/bin/sh
set -eu
rm -f /etc/resolv.conf
ln -s /run/systemd/resolve/stub-resolv.conf /etc/resolv.conf
for unit in systemd-networkd systemd-resolved systemd-timesyncd; do
	systemctl --root=/ enable "$unit.service"
done
HOOK

# Live-root hook: adds Fastfetch to /etc/profile after package installation
put 755 config/hooks/normal/0200-fastfetch-profile.hook.chroot <<'HOOK'
#!/bin/sh
set -eu

PROFILE=/etc/profile
[ -f "$PROFILE" ] || {
	printf '%s\n' "Cannot enable Fastfetch: $PROFILE does not exist." >&2
	exit 1
}
command -v fastfetch >/dev/null 2>&1 || {
	printf '%s\n' "Cannot enable Fastfetch: fastfetch is not installed in the live environment." >&2
	exit 1
}

TMP_PROFILE=$(mktemp "${PROFILE}.XXXXXX") || {
	printf '%s\n' "Cannot create a temporary file for $PROFILE." >&2
	exit 1
}
if ! awk '
	$0 != "fastfetch" { lines[++count] = $0 }
	END {
		while (count > 0 && lines[count] == "") count--
		for (i = 1; i <= count; i++) print lines[i]
		print ""
		print "fastfetch"
	}
' "$PROFILE" > "$TMP_PROFILE"; then
	rm -f "$TMP_PROFILE"
	printf '%s\n' "Cannot prepare the updated $PROFILE." >&2
	exit 1
fi

# Write through the existing file to preserve its ownership and permissions.
if ! cat "$TMP_PROFILE" > "$PROFILE"; then
	rm -f "$TMP_PROFILE"
	printf '%s\n' "Cannot update $PROFILE." >&2
	exit 1
fi
rm -f "$TMP_PROFILE"
HOOK

# Target network hook: convert discovered installer network state into static Netplan.
# If discovery is incomplete, preserve the existing DHCP configuration instead.
put 755 config/includes.binary/debian-live-server-target-system.sh <<'SYSTEM_HOOK'
#!/bin/sh
#
# debian-live-server-target-system.sh
# Apply target-wide settings as independent steps; one optional setting must not
# prevent the remaining post-install configuration from being attempted.
#
set -u
umask 077

TARGET_ROOT=${TARGET_ROOT:-/target}
SYSCTL_SOURCE=${SYSCTL_SOURCE:-/cdrom/sysctl.conf}

[ -d "$TARGET_ROOT/etc" ] || {
	printf 'SYSTEM CONFIGURATION ERROR: target is not mounted at %s.\n' "$TARGET_ROOT" >&2
	exit 1
}
mkdir -p "$TARGET_ROOT/var/log" || exit 1
exec >> "$TARGET_ROOT/var/log/debian-live-server-system.log" 2>&1
printf 'Starting target system configuration at %s.\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')"

failed=0

run_step()
{
	_step_name=$1
	shift
	printf '\n--- %s ---\n' "$_step_name"
	if "$@"; then
		printf 'RESULT: %s completed.\n' "$_step_name"
	else
		_step_rc=$?
		printf 'ERROR: %s failed with exit code %s.\n' "$_step_name" "$_step_rc"
		failed=1
	fi
}

configure_cloud_init()
{
	# cloud-init is optional at runtime; if installed, stop it from replacing Netplan.
	if [ ! -d "$TARGET_ROOT/etc/cloud" ]; then
		printf 'cloud-init configuration directory is absent; no cloud-init network override is needed.\n'
		return 0
	fi
	mkdir -p "$TARGET_ROOT/etc/cloud/cloud.cfg.d" || return 1
	_cloud_tmp=$(mktemp "$TARGET_ROOT/etc/cloud/cloud.cfg.d/.99-disable-network-config.XXXXXX") || return 1
	printf '%s\n' 'network: {config: disabled}' > "$_cloud_tmp" || {
		rm -f "$_cloud_tmp"
		return 1
	}
	chmod 644 "$_cloud_tmp" || {
		rm -f "$_cloud_tmp"
		return 1
	}
	mv -f "$_cloud_tmp" "$TARGET_ROOT/etc/cloud/cloud.cfg.d/99-disable-network-config.cfg"
}

ensure_netplan()
{
	mkdir -p "$TARGET_ROOT/etc/netplan" || return 1
	_have_netplan=0
	for _yaml in "$TARGET_ROOT"/etc/netplan/*.yaml; do
		[ -f "$_yaml" ] || continue
		_have_netplan=1
		break
	done
	if [ "$_have_netplan" -eq 0 ]; then
		_fallback_tmp=$(mktemp "$TARGET_ROOT/etc/netplan/.10-installer-fallback.XXXXXX") || return 1
		cat > "$_fallback_tmp" <<'NETPLAN'
network:
  version: 2
  renderer: networkd
  ethernets:
    all-ethernet:
      match:
        name: "e*"
      dhcp4: true
      dhcp6: false
NETPLAN
		chmod 600 "$_fallback_tmp" || {
			rm -f "$_fallback_tmp"
			return 1
		}
		mv -f "$_fallback_tmp" "$TARGET_ROOT/etc/netplan/10-installer-fallback.yaml" || return 1
		printf 'Created DHCP fallback Netplan configuration.\n'
	fi
}

configure_fastfetch()
{
	[ -x "$TARGET_ROOT/usr/bin/fastfetch" ] || {
		printf 'Fastfetch executable is missing from the installed target.\n'
		return 1
	}
	_profile=$TARGET_ROOT/etc/profile
	[ -f "$_profile" ] || {
		printf 'Installed target profile is missing: %s\n' "$_profile"
		return 1
	}
	_profile_tmp=$(mktemp "${_profile}.XXXXXX") || return 1
	if ! awk '
		$0 != "fastfetch" { lines[++count] = $0 }
		END {
			while (count > 0 && lines[count] == "") count--
			for (i = 1; i <= count; i++) print lines[i]
			print ""
			print "fastfetch"
		}
	' "$_profile" > "$_profile_tmp"; then
		rm -f "$_profile_tmp"
		return 1
	fi
	if ! cat "$_profile_tmp" > "$_profile"; then
		rm -f "$_profile_tmp"
		return 1
	fi
	rm -f "$_profile_tmp"
	printf 'Enabled Fastfetch in %s.\n' "$_profile"
}

install_sysctl()
{
	[ -r "$SYSCTL_SOURCE" ] || {
		printf 'Missing sysctl source: %s\n' "$SYSCTL_SOURCE"
		return 1
	}
	_sysctl_target=$TARGET_ROOT/etc/sysctl.conf
	[ ! -L "$_sysctl_target" ] || {
		printf 'Refusing to write through a symbolic-link sysctl configuration.\n'
		return 1
	}
	_sysctl_tmp=$(mktemp "$TARGET_ROOT/etc/.sysctl.conf.XXXXXX") || return 1
	if ! cp "$SYSCTL_SOURCE" "$_sysctl_tmp" || ! chmod 644 "$_sysctl_tmp"; then
		rm -f "$_sysctl_tmp"
		return 1
	fi
	mv -f "$_sysctl_tmp" "$_sysctl_target" || {
		rm -f "$_sysctl_tmp"
		return 1
	}
	printf 'Installed sysctl configuration at %s.\n' "$_sysctl_target"
}

configure_resolver()
{
	_resolv_target=$TARGET_ROOT/etc/resolv.conf
	[ ! -d "$_resolv_target" ] || {
		printf 'Target /etc/resolv.conf is unexpectedly a directory.\n'
		return 1
	}
	rm -f "$_resolv_target" || return 1
	ln -s /run/systemd/resolve/stub-resolv.conf "$_resolv_target" || return 1
	printf 'Configured /etc/resolv.conf for systemd-resolved.\n'
}

enable_network_services()
{
	command -v in-target >/dev/null 2>&1 || {
		printf 'Debian Installer in-target command is unavailable.\n'
		return 1
	}
	_service_failed=0
	for _unit in systemd-networkd systemd-resolved systemd-timesyncd; do
		if ! in-target systemctl enable "$_unit.service"; then
			printf 'Could not enable %s.service.\n' "$_unit"
			_service_failed=1
		fi
	done
	for _unit in NetworkManager networking; do
		if [ -e "$TARGET_ROOT/usr/lib/systemd/system/$_unit.service" ] ||
		   [ -e "$TARGET_ROOT/lib/systemd/system/$_unit.service" ]; then
			if ! in-target systemctl mask "$_unit.service"; then
				printf 'Could not mask conflicting %s.service.\n' "$_unit"
				_service_failed=1
			fi
		fi
	done
	return "$_service_failed"
}

run_step "Cloud-init network ownership" configure_cloud_init
run_step "Netplan fallback" ensure_netplan
run_step "Fastfetch login profile" configure_fastfetch
run_step "Sysctl configuration" install_sysctl
run_step "Resolver configuration" configure_resolver
run_step "Network service configuration" enable_network_services

if [ "$failed" -ne 0 ]; then
	printf '\nTarget system configuration completed with errors; see this log and the combined post-install log.\n'
	exit 1
fi

printf '\nTarget system configuration completed successfully.\n'
SYSTEM_HOOK

put 755 config/includes.binary/debian-live-server-target-network.sh <<'NETWORK_HOOK'
#!/bin/sh
#
# debian-live-server-target-network.sh
#
# Preseed late_command, run by the Debian Installer as /cdrom/debian-live-server-target-network.sh.
# Discovers every link that holds a global IPv4 address, classifies each as Ethernet,
# Wi-Fi, VLAN, or unsupported, and writes one authoritative Netplan file for the
# installed system: $TARGET_ROOT/etc/netplan/50-cloud-init.yaml (mode 600).
#
# Limits:
#   - The installer configures only the interface it chose. Only links that already
#     hold IPv4 addresses in the installer environment can be carried over.
#   - Wi-Fi credentials come from the installer's wpa_supplicant.conf. The installer
#     cannot read the live session's stored secrets.
#   - Every DHCP-derived address must be reserved by an administrator in DHCP/IPAM.
#     This script cannot create reservations.
#
set -eu
umask 077

# SYSROOT is empty on a real installer. It exists so that tests can point discovery
# at a fixture tree.
SYSROOT=${SYSROOT:-}
TARGET_ROOT=${TARGET_ROOT:-/target}
RESOLV_CONF=$SYSROOT/etc/resolv.conf
RESOLVED_RESOLV=$SYSROOT/run/systemd/resolve/resolv.conf
SYS_NET=$SYSROOT/sys/class/net
SYS_VIRT=$SYSROOT/sys/devices/virtual/net
NETPLAN_FILE=$TARGET_ROOT/etc/netplan/10-installer.yaml
NETWORK_LOG=$TARGET_ROOT/var/log/debian-live-server-network-discovery.log
WORK=
NETPLAN_BACKUP=
NETPLAN_BACKUP_READY=0
TMP_NETPLAN=

WPA_CONF=
for _wpa in "$SYSROOT/etc/wpa_supplicant/wpa_supplicant.conf" "$SYSROOT/etc/wpa_supplicant.conf"; do
	if [ -r "$_wpa" ]; then
		WPA_CONF=$_wpa
		break
	fi
done

restore_netplan()
{
	[ "$NETPLAN_BACKUP_READY" = 1 ] || return 0
	for _old in "$TARGET_ROOT"/etc/netplan/*.yaml; do
		[ -e "$_old" ] || continue
		rm -f "$_old" || return 1
	done
	if [ -d "$NETPLAN_BACKUP" ]; then
		cp -a "$NETPLAN_BACKUP/." "$TARGET_ROOT/etc/netplan/" || return 1
	fi
	NETPLAN_BACKUP_READY=0
}

fail()
{
	_message=$*
	if [ "$NETPLAN_BACKUP_READY" = 1 ] && ! restore_netplan; then
		printf 'NETWORK CONFIGURATION ERROR: %s; failed to restore the previous Netplan files.\n' "$_message" >&2
		exit 1
	fi
	printf 'WARNING: Static network migration was skipped: %s. Existing Netplan configuration was preserved.\n' "$_message" >&2
	exit 0
}

cleanup()
{
	_rc=$?
	trap - 0
	if [ "$NETPLAN_BACKUP_READY" = 1 ] && ! restore_netplan; then
		printf 'NETWORK CONFIGURATION ERROR: failed to restore the previous Netplan configuration.\n' >&2
		_rc=1
	fi
	[ -z "$TMP_NETPLAN" ] || rm -f "$TMP_NETPLAN"
	[ -z "$WORK" ] || rm -rf "$WORK"
	exit "$_rc"
}

# field KEY LINE: print the word that follows KEY in LINE.
field()
{
	printf '%s\n' "$2" | awk -v key="$1" '
		{ for (i = 1; i < NF; i++) if ($i == key) { print $(i + 1); exit } }'
}

# is_ipv4 ADDRESS: succeed for a dotted-quad IPv4 address without leading zeros.
is_ipv4()
{
	printf '%s\n' "$1" | awk -F. '
		NF != 4 { exit 1 }
		{
			for (i = 1; i <= 4; i++)
				if ($i !~ /^[0-9]+$/ || length($i) > 3 || $i + 0 > 255 ||
					(length($i) > 1 && substr($i, 1, 1) == "0")) exit 1
		}'
}

# set_kv FILE KEY VALUE: replace an existing KEY= line or append one.
# Writes through the existing file so its mode is preserved.
set_kv()
{
	if grep -q "^$2=" "$1"; then
		sed "s|^$2=.*|$2=$3|" "$1" > "$1.tmp"
		cat "$1.tmp" > "$1"
		rm -f "$1.tmp"
	else
		printf '%s=%s\n' "$2" "$3" >> "$1"
	fi
}

[ -d "$TARGET_ROOT/etc" ] || fail "The installed target is not mounted at $TARGET_ROOT."
mkdir -p "$TARGET_ROOT/var/log"
exec > "$NETWORK_LOG" 2>&1
printf 'Starting target network discovery at %s.\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')"

command -v ip >/dev/null 2>&1 || fail "The installer environment has no ip command."
command -v awk >/dev/null 2>&1 || fail "The installer environment has no awk command."
[ -r "$RESOLV_CONF" ] || fail "The installer resolver configuration is unavailable."
[ -d "$SYS_NET" ] || fail "The kernel network interface directory $SYS_NET is unavailable."

# Directory modes are set explicitly: the umask above applies to files only.
mkdir -p -m 755 "$TARGET_ROOT/etc/netplan"
WORK=/tmp/debian-live-server-net.$$
mkdir -m 700 "$WORK"
trap cleanup 0

ip -br link || :
ip -4 route show || :

# The preferred route is the one the kernel uses for Internet-bound traffic. It
# identifies the primary interface and the gateway the installed system must keep.
route=$(ip -4 route get 1.1.1.1 2>/dev/null | sed -n '1p') || route=
[ -n "$route" ] || fail "No usable IPv4 route exists after installer DHCP configuration."

p_iface=$(field dev "$route")
p_src=$(field src "$route")
p_gw=$(field via "$route")
[ -n "$p_iface" ] || fail "The selected route has no outgoing interface."
[ -n "$p_src" ] || fail "The selected route has no source IPv4 address."
[ -n "$p_gw" ] || fail "No IPv4 default gateway was discovered."
case "$p_iface" in
	''|lo|*[!A-Za-z0-9_.-]*) fail "Invalid or unsupported interface name: $p_iface" ;;
esac
[ "${#p_iface}" -le 15 ] || fail "Interface name exceeds Linux's 15-character limit."
is_ipv4 "$p_src" || fail "Invalid discovered IPv4 source address: $p_src"
is_ipv4 "$p_gw" || fail "Invalid discovered IPv4 gateway: $p_gw"
printf 'Preferred route: %s\n' "$route"

# Discovery writes tab-separated facts; the renderer decides what to configure.
FACTS=$WORK/facts
: > "$FACTS"

for _link in "$SYS_NET"/*; do
	[ -e "$_link" ] || continue
	name=${_link##*/}
	[ "$name" = lo ] && continue
	case "$name" in
		*[!A-Za-z0-9_.-]*)
			printf 'Skipping link with an unsupported name: %s\n' "$name"
			continue ;;
	esac

	kind=other
	parent=
	vid=
	dinfo=$(ip -d -o link show dev "$name" 2>/dev/null) || dinfo=
	lnk=$(ip -o link show dev "$name" 2>/dev/null) || lnk=
	if [ -d "$SYS_NET/$name/bridge" ]; then
		kind=bridge
	elif [ -d "$SYS_NET/$name/bonding" ]; then
		kind=bond
	elif [ -d "$SYS_NET/$name/wireless" ] || [ -e "$SYS_NET/$name/phy80211" ]; then
		kind=wifi
	else
		case "$dinfo" in
			*"vlan protocol"*)
				kind=vlan
				vid=$(printf '%s\n' "$dinfo" |
					sed -n 's/.*vlan protocol [^ ]* id \([0-9][0-9]*\).*/\1/p' | sed -n '1p')
				parent=$(printf '%s\n' "$lnk" | awk '
					NR == 1 { n = $2; sub(/:$/, "", n); if (index(n, "@")) { sub(/^[^@]*@/, "", n); print n } }')
				;;
			*)
				if [ -e "$SYS_VIRT/$name" ]; then
					kind=virtual
				elif [ "$(cat "$SYS_NET/$name/type" 2>/dev/null || printf 0)" = 1 ]; then
					kind=eth
				fi
				;;
		esac
	fi

	state=$(cat "$SYS_NET/$name/operstate" 2>/dev/null || printf unknown)
	if [ "$state" = up ] || [ "$state" = unknown ]; then up=up; else up=down; fi
	mac=$(cat "$SYS_NET/$name/address" 2>/dev/null || printf '')
	printf 'L\t%s\t%s\t%s\t%s\t%s\t%s\n' "$name" "$kind" "$up" "$mac" "$parent" "$vid" >> "$FACTS"

	ip -4 -o address show dev "$name" scope global 2>/dev/null |
		awk -v n="$name" '$3 == "inet" { printf "A\t%s\t%s\n", n, $4 }' >> "$FACTS" || :
	ip -4 route show dev "$name" 2>/dev/null |
		awk -v n="$name" '
			{
				to = $1; via = ""; met = ""
				for (i = 2; i < NF; i++) {
					if ($i == "via") via = $(i + 1)
					if ($i == "metric") met = $(i + 1)
				}
				if (via != "") printf "R\t%s\t%s\t%s\t%s\n", n, to, via, met
			}' >> "$FACTS" || :
done

printf 'P\t%s\t%s\t%s\n' "$p_iface" "$p_gw" "$p_src" >> "$FACTS"

DNS_SOURCE=$RESOLV_CONF
if [ -r "$RESOLVED_RESOLV" ] &&
   grep -Eq '^[[:space:]]*nameserver[[:space:]]+[0-9]+\.' "$RESOLVED_RESOLV"; then
	DNS_SOURCE=$RESOLVED_RESOLV
fi

dns_list=$(awk '$1 == "nameserver" && NF >= 2 { print $2 }' "$DNS_SOURCE" |
	while IFS= read -r ns; do
		is_ipv4 "$ns" || continue
		case "$ns" in
			127.*|169.254.*|0.0.0.0|255.255.255.255) continue ;;
		esac
		printf '%s\n' "$ns"
	done | awk '!seen[$0]++')
[ -n "$dns_list" ] ||
	fail "No usable IPv4 DNS server was discovered in installer resolver configuration."
printf '%s\n' "$dns_list" | awk '{ printf "D\t%s\n", $0 }' >> "$FACTS"
printf 'DNS source: %s\nDNS servers:\n%s\n' "$DNS_SOURCE" "$dns_list"

# The renderer applies the decisions and writes the YAML to stdout. It also writes
# warnings and the list of configured links to stderr, which is this log.
cat > "$WORK/render.awk" <<'AWK'
# render.awk - derive the static Netplan configuration from discovered link facts.
#
# Inputs:  -v wpa=PATH   installer wpa_supplicant.conf, or /dev/null
#          facts file    tab-separated records:
#                          L name kind up mac parent vlanid
#                          A name cidr
#                          R name to via metric     (to = "default" or a prefix)
#                          P name gateway source    (the preferred route)
#                          D dns-server
# Output:  Netplan YAML on stdout. Diagnostics go to stderr, which the installer
#          logs. Wi-Fi passphrases are written only to the YAML output.

BEGIN {
	FS = "\t"
	failed = 0
	nif = 0; nd = 0; nn = 0; ndef = 0; nx = 0
	pname = ""
	parse_wpa(wpa)
}

function warn(msg)  { print "WARNING: " msg > "/dev/stderr" }
function info(msg)  { print msg > "/dev/stderr" }
function fatal(msg) { print "NETWORK CONFIGURATION ERROR: " msg > "/dev/stderr"; failed = 1; exit 1 }

function is_digits(s) { return s ~ /^[0-9]+$/ }

function ipnum(s,   o, i, v) {
	split(s, o, ".")
	v = 0
	for (i = 1; i <= 4; i++) v = v * 256 + o[i]
	return v
}

function is_ipv4(s,   o, n, i) {
	n = split(s, o, ".")
	if (n != 4) return 0
	for (i = 1; i <= 4; i++) {
		if (o[i] !~ /^[0-9]+$/ || length(o[i]) > 3 || o[i] + 0 > 255 ||
			(length(o[i]) > 1 && substr(o[i], 1, 1) == "0")) return 0
	}
	return 1
}

function cidr_addr(c,   t) { t = c; sub(/\/.*$/, "", t); return t }
function cidr_pfx(c,   t)  { t = c; sub(/^[^\/]*\//, "", t); return t }

# on_link(addr, prefix, gw): gw is a usable host address inside addr's subnet.
function on_link(a, p, gw,   av, g, size, net, last) {
	av = ipnum(a); g = ipnum(gw)
	size = 2 ^ (32 - p)
	net = int(av / size) * size
	last = net + size - 1
	if (g < net || g > last) return 0
	if (g == av) return 0
	# Neither the gateway nor the host's own address may be the network or broadcast
	# address of a subnet that has more than two addresses.
	if (p <= 30 && (g == net || g == last || av == net || av == last)) return 0
	return 1
}

# gw_on_link(iface, gw): gw lies in the subnet of one of the iface's addresses.
function gw_on_link(n, gw,   k) {
	for (k = 1; k <= vcnt[n]; k++)
		if (on_link(vaddr[n, k], vpfx[n, k], gw)) return 1
	return 0
}

function valid_mac(m,   parts, n, i) {
	n = split(tolower(m), parts, ":")
	if (n != 6) return 0
	for (i = 1; i <= 6; i++)
		if (length(parts[i]) != 2 || parts[i] !~ /^[0-9a-f]+$/) return 0
	return 1
}

# yq(s): single-quote a YAML scalar, doubling embedded single quotes.
function yq(s,   t) { t = s; gsub(/'/, "''", t); return "'" t "'" }

# ---- Wi-Fi credentials from the installer's wpa_supplicant.conf -------------

function kv_value(line,   v) {
	v = line
	sub(/^[ \t]*[A-Za-z_]+[ \t]*=[ \t]*/, "", v)
	sub(/[ \t\r]+$/, "", v)
	return v
}

function add_network(ssid, psk, km,   s, p) {
	if (ssid == "") return
	if (ssid !~ /^".*"$/) {
		warn("A Wi-Fi network with a hex-encoded SSID was not transferred; configure it manually.")
		return
	}
	s = substr(ssid, 2, length(ssid) - 2)
	if (s == "") return
	if (km == "NONE") {
		nn++; nssid[nn] = s; ntype[nn] = "open"; npass[nn] = ""
		return
	}
	if (km != "" && km !~ /WPA-PSK/) {
		if (km ~ /EAP/) warn("802.1X Wi-Fi network \"" s "\" was not transferred; configure it manually.")
		else warn("Wi-Fi network \"" s "\" uses key management \"" km "\", which is not transferred.")
		return
	}
	if (psk == "") {
		warn("Wi-Fi network \"" s "\" has no transferable pre-shared key; it was not transferred.")
		return
	}
	if (psk ~ /^".*"$/) {
		p = substr(psk, 2, length(psk) - 2)
		if (length(p) < 8 || length(p) > 63) {
			warn("Wi-Fi network \"" s "\" has a passphrase outside 8-63 characters; it was not transferred.")
			return
		}
	} else if (length(psk) == 64 && psk ~ /^[0-9A-Fa-f]+$/) {
		p = psk
	} else {
		warn("Wi-Fi network \"" s "\" has an unrecognised PSK format; it was not transferred.")
		return
	}
	nn++; nssid[nn] = s; ntype[nn] = "psk"; npass[nn] = p
}

function parse_wpa(path,   line, innet, ssid, psk, km) {
	innet = 0; ssid = ""; psk = ""; km = ""
	if (path == "" || path == "/dev/null") return
	while ((getline line < path) > 0) {
		sub(/\r$/, "", line)
		if (!innet) {
			if (line ~ /^[ \t]*network[ \t]*=[ \t]*\{/) {
				innet = 1; ssid = ""; psk = ""; km = ""
			}
		} else if (line ~ /^[ \t]*\}/) {
			innet = 0
			add_network(ssid, psk, km)
		} else if (line ~ /^[ \t]*ssid[ \t]*=/) {
			ssid = kv_value(line)
		} else if (line ~ /^[ \t]*psk[ \t]*=/) {
			psk = kv_value(line)
		} else if (line ~ /^[ \t]*key_mgmt[ \t]*=/) {
			km = kv_value(line)
		}
	}
	close(path)
}

# ---- Collect the discovered facts -------------------------------------------

$1 == "L" {
	if (!($2 in lkind)) order[++nif] = $2
	lkind[$2] = $3; lup[$2] = $4; lmac[$2] = $5; lpar[$2] = $6; lvid[$2] = $7
}
$1 == "A" { ac[$2]++; aaddr[$2, ac[$2]] = $3 }
$1 == "R" { rc[$2]++; rto[$2, rc[$2]] = $3; rvia[$2, rc[$2]] = $4; rmet[$2, rc[$2]] = $5 }
$1 == "P" { pname = $2; pgw = $3; psrc = $4 }
$1 == "D" { dns[++nd] = $2 }

# ---- Decide what is configured, then emit ------------------------------------

END {
	if (failed) exit 1
	if (pname == "" || !(pname in lkind))
		fatal("The preferred IPv4 route's interface was not discovered.")

	# Usable addresses per interface.
	for (i = 1; i <= nif; i++) {
		n = order[i]; vcnt[n] = 0
		for (k = 1; k <= ac[n]; k++) {
			c = aaddr[n, k]; a = cidr_addr(c); q = cidr_pfx(c)
			if (!is_ipv4(a) || !is_digits(q) || q + 0 < 1 || q + 0 > 32) {
				warn("Ignoring invalid IPv4 address " c " on " n ".")
				continue
			}
			vcnt[n]++; vaddr[n, vcnt[n]] = a; vpfx[n, vcnt[n]] = q + 0
		}
	}

	# Which interfaces are configured.
	for (i = 1; i <= nif; i++) {
		n = order[i]; kind = lkind[n]
		if (kind != "eth" && kind != "wifi" && kind != "vlan") {
			if (ac[n] > 0)
				warn("Interface " n " (" kind ") has IPv4 addresses, but this script does not configure that kind of link; it is excluded.")
			continue
		}
		if (vcnt[n] == 0) continue
		if (lup[n] != "up") {
			warn("Interface " n " has IPv4 addresses but its link is down; it is excluded.")
			continue
		}
		if (kind == "wifi" && nn == 0) {
			warn("Wi-Fi interface " n " has no transferable credentials; it is excluded and must be configured after installation.")
			continue
		}
		if (kind == "vlan") {
			par = lpar[n]
			if (!(par in lkind) || lkind[par] != "eth") {
				warn("VLAN " n " (ID " lvid[n] ") sits on parent " par ", which is not a supported Ethernet link; it is excluded.")
				continue
			}
			if (!is_digits(lvid[n]) || lvid[n] + 0 < 1 || lvid[n] + 0 > 4094) {
				warn("VLAN " n " has no valid VLAN ID; it is excluded.")
				continue
			}
			needparent[par] = 1
		}
		inc[n] = 1
	}
	# A VLAN parent without an usable address is still declared, so the VLAN
	# has a valid link. It carries no addresses or routes.
	for (n in needparent) {
		if (!inc[n] && lkind[n] == "eth") { inc[n] = 1; parentonly[n] = 1 }
	}

	if (!inc[pname]) {
		why = (lkind[pname] == "wifi" && nn == 0) ? "no transferable Wi-Fi credentials were found" : "unsupported link type " lkind[pname]
		fatal("The preferred IPv4 route uses " pname " (" why "). Refusing to generate a configuration that would lose the installed system's default route.")
	}
	found = 0
	for (k = 1; k <= vcnt[pname]; k++) if (vaddr[pname, k] == psrc) found = 1
	if (!found) fatal("The preferred route's source address " psrc " is not a configured address on " pname ".")

	# Default routes: the preferred route first, then the others.
	primary_added = 0
	for (pass = 1; pass <= 2; pass++) {
		for (i = 1; i <= nif; i++) {
			n = order[i]
			if (!inc[n] || parentonly[n]) continue
			for (r = 1; r <= rc[n]; r++) {
				if (rto[n, r] != "default") continue
				gw = rvia[n, r]
				isprim = (n == pname && gw == pgw)
				if (pass == 1 && !isprim) continue
				if (pass == 2 && isprim) continue
				if (!is_ipv4(gw) || !gw_on_link(n, gw)) {
					if (isprim) fatal("The preferred default gateway " gw " is not on a configured subnet of " pname ".")
					warn("Default route via " gw " on " n " is not on a configured subnet of that interface; it is excluded.")
					continue
				}
				met = rmet[n, r]
				if (met == "") met = 0
				if (!is_digits(met)) {
					warn("Default route on " n " has an invalid metric \"" met "\"; it is excluded.")
					continue
				}
				if (isprim) primary_added = 1
				ndef++
				dif[ndef] = n; dgw[ndef] = gw; dmet[ndef] = met + 0
				hasdef[n] = 1
			}
		}
	}
	if (!primary_added) fatal("The preferred default route was not present on " pname ".")

	# Two default routes with the same metric would be ambiguous. Later routes are
	# moved to the next free metric, so the preferred route keeps its metric.
	for (d = 1; d <= ndef; d++) {
		m = dmet[d]
		while (m in usedm) m += 100
		usedm[m] = 1
		dmet[d] = m
	}

	# Non-default routes (for example DHCP classless routes), when on-link.
	for (i = 1; i <= nif; i++) {
		n = order[i]
		if (!inc[n] || parentonly[n]) continue
		for (r = 1; r <= rc[n]; r++) {
			to = rto[n, r]
			if (to == "default") continue
			if (to !~ /\//) to = to "/32"
			ta = cidr_addr(to); tp = cidr_pfx(to)
			if (!is_ipv4(ta) || !is_digits(tp) || tp + 0 > 32) {
				warn("Route to " to " on " n " is not a valid IPv4 prefix; it is excluded.")
				continue
			}
			gw = rvia[n, r]
			if (!is_ipv4(gw) || !gw_on_link(n, gw)) {
				warn("Route to " to " via " gw " on " n " is not on a configured subnet of that interface; it is excluded.")
				continue
			}
			met = rmet[n, r]
			if (met != "" && !is_digits(met)) {
				warn("Route to " to " on " n " has an invalid metric \"" met "\"; it is excluded.")
				continue
			}
			nx++; xif[nx] = n; xto[nx] = ta "/" (tp + 0); xgw[nx] = gw; xmet[nx] = met
		}
	}

	# DNS servers: global, applied to interfaces that carry a default route.
	ndns = 0
	for (j = 1; j <= nd; j++) {
		s = dns[j]
		if (!is_ipv4(s)) continue
		if (s ~ /^127\./ || s ~ /^169\.254\./ || s == "0.0.0.0" || s == "255.255.255.255") continue
		okdns[++ndns] = s
	}
	if (ndns == 0) fatal("No usable IPv4 DNS server was discovered for the static configuration.")

	# Diagnostics (no secrets).
	for (i = 1; i <= nif; i++) {
		n = order[i]
		if (!inc[n]) continue
		line = "Configured " n " (" lkind[n] ")"
		if (parentonly[n]) line = line " as a VLAN parent without addresses"
		else {
			line = line ": addresses"
			for (k = 1; k <= vcnt[n]; k++) line = line " " vaddr[n, k] "/" vpfx[n, k]
		}
		info(line)
	}
	if (nn > 0) info("Wi-Fi credentials transferred for " nn " network(s); passphrases are not logged.")

	# Warn about every DHCP-derived address.
	for (i = 1; i <= nif; i++) {
		n = order[i]
		if (!inc[n] || parentonly[n]) continue
		mac = (lkind[n] == "vlan") ? lmac[lpar[n]] : lmac[n]
		for (k = 1; k <= vcnt[n]; k++) {
			where = (lkind[n] == "vlan") ? "VLAN " n " (ID " lvid[n] ", parent " lpar[n] ", MAC " mac ")" : "interface " n " (MAC " mac ")"
			warn("DHCP-derived address " vaddr[n, k] "/" vpfx[n, k] " on " where " is now static in /etc/netplan/50-cloud-init.yaml. Reserve " vaddr[n, k] " for MAC " mac " in the DHCP server or IPAM before relying on it; otherwise another host can receive the same address. This script cannot create DHCP reservations.")
		}
	}

	# ---- YAML ----
	neth = 0; nwifi = 0; nvlan = 0
	for (i = 1; i <= nif; i++) {
		n = order[i]
		if (!inc[n]) continue
		if (lkind[n] == "wifi") nwifi++
		else if (lkind[n] == "vlan") nvlan++
		else neth++
	}

	print "# Generated by debian-live-server-target-network.sh from the installer's network state."
	print "# Addresses were DHCP-derived during installation. Reserve them externally (DHCP/IPAM)."
	print "network:"
	print "  version: 2"
	print "  renderer: networkd"
	if (neth > 0) {
		print "  ethernets:"
		for (i = 1; i <= nif; i++) {
			n = order[i]
			if (inc[n] && lkind[n] == "eth") emit_device(n)
		}
	}
	if (nwifi > 0) {
		print "  wifis:"
		for (i = 1; i <= nif; i++) {
			n = order[i]
			if (inc[n] && lkind[n] == "wifi") { emit_device(n); emit_access_points() }
		}
	}
	if (nvlan > 0) {
		print "  vlans:"
		for (i = 1; i <= nif; i++) {
			n = order[i]
			if (inc[n] && lkind[n] == "vlan") emit_vlan(n)
		}
	}
	exit 0
}

# emit_device(n): an ethernets or wifis entry, matched by MAC and named as before.
function emit_device(n) {
	print "    " n ":"
	if (valid_mac(lmac[n])) {
		print "      match:"
		print "        macaddress: " yq(tolower(lmac[n]))
		print "      set-name: " yq(n)
	} else {
		warn("The MAC address of " n " is unavailable; its Netplan entry is matched by name only.")
	}
	print "      dhcp4: no"
	print "      dhcp6: no"
	emit_addresses_and_routes(n)
}

# emit_vlan(n): a vlans entry. VLAN devices take their MAC from the parent.
function emit_vlan(n) {
	print "    " n ":"
	print "      id: " lvid[n] + 0
	print "      link: " yq(lpar[n])
	print "      dhcp4: no"
	print "      dhcp6: no"
	emit_addresses_and_routes(n)
}

function emit_addresses_and_routes(n,   k, d, x, nr) {
	if (parentonly[n]) return
	if (vcnt[n] > 0) {
		print "      addresses:"
		for (k = 1; k <= vcnt[n]; k++) print "        - " vaddr[n, k] "/" vpfx[n, k]
	}
	nr = 0
	for (d = 1; d <= ndef; d++) if (dif[d] == n) nr++
	for (x = 1; x <= nx; x++) if (xif[x] == n) nr++
	if (nr > 0) {
		print "      routes:"
		for (d = 1; d <= ndef; d++) if (dif[d] == n) {
			print "        - to: default"
			print "          via: " dgw[d]
			print "          metric: " dmet[d]
		}
		for (x = 1; x <= nx; x++) if (xif[x] == n) {
			print "        - to: " xto[x]
			print "          via: " xgw[x]
			if (xmet[x] != "") print "          metric: " xmet[x] + 0
		}
	}
	if (hasdef[n]) {
		print "      nameservers:"
		print "        addresses:"
		for (k = 1; k <= ndns; k++) print "          - " okdns[k]
	}
}

# emit_access_points(): Wi-Fi networks from the installer's configuration. The
# passphrase is written only here, into the mode-600 Netplan file.
function emit_access_points(   k) {
	if (nn == 0) return
	print "      access-points:"
	for (k = 1; k <= nn; k++) {
		print "        " yq(nssid[k]) ":"
		if (ntype[k] == "open") print "          {}"
		else print "          password: " yq(npass[k])
	}
}
AWK

if ! awk -v wpa="${WPA_CONF:-/dev/null}" -f "$WORK/render.awk" "$FACTS" > "$WORK/netplan.yaml"; then
	fail "No safe static Netplan configuration could be derived. See $TARGET_ROOT/var/log/debian-live-server-network-discovery.log."
fi
[ -s "$WORK/netplan.yaml" ] || fail "The generated Netplan configuration is empty."

NETPLAN_BACKUP=$WORK/original-netplan
mkdir -m 700 "$NETPLAN_BACKUP"
for _old in "$TARGET_ROOT"/etc/netplan/*.yaml; do
	[ -e "$_old" ] || continue
	cp -a "$_old" "$NETPLAN_BACKUP/" || fail "Cannot back up existing Netplan file $_old."
done
NETPLAN_BACKUP_READY=1

for _old in "$TARGET_ROOT"/etc/netplan/*.yaml; do
	[ -e "$_old" ] || continue
	rm -f "$_old" || fail "Cannot replace existing Netplan file $_old."
done
TMP_NETPLAN=$(mktemp "$TARGET_ROOT/etc/netplan/.10-installer.yaml.XXXXXX") ||
	fail "Cannot create a temporary Netplan file in the installed target."
cat "$WORK/netplan.yaml" > "$TMP_NETPLAN"
chmod 600 "$TMP_NETPLAN"
mv -f "$TMP_NETPLAN" "$NETPLAN_FILE"

if ! in-target netplan generate; then
	fail "Netplan rejected $NETPLAN_FILE in the installed system."
fi

NETPLAN_BACKUP_READY=0
printf 'Installed static Netplan configuration: %s\n' "$NETPLAN_FILE"
NETWORK_HOOK

# Target-only OpenSSH hook. It never modifies the live root.
put 755 config/includes.binary/debian-live-server-target-ssh.sh <<'SSH_HOOK'
#!/bin/sh
#
# debian-live-server-target-ssh.sh
# Apply the requested OpenSSH settings to the installed target only.
#
set -eu
umask 077

TARGET_ROOT=${TARGET_ROOT:-/target}
SSH_DIR=$TARGET_ROOT/etc/ssh
DROPIN_DIR=$SSH_DIR/sshd_config.d
DROPIN=$DROPIN_DIR/50-Debian.conf
TMP=

fail()
{
	printf 'SSH CONFIGURATION ERROR: %s\n' "$*" >&2
	exit 1
}

cleanup()
{
	if [ -n "$TMP" ]; then
		rm -f "$TMP"
	fi
}
trap cleanup 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

[ -d "$TARGET_ROOT/etc" ] ||
	fail "The installed target is not mounted at $TARGET_ROOT."
[ -d "$SSH_DIR" ] && [ ! -L "$SSH_DIR" ] ||
	fail "The target OpenSSH configuration directory is missing or unsafe: $SSH_DIR."
[ -x "$TARGET_ROOT/usr/sbin/sshd" ] ||
	fail "openssh-server is not installed in the target system."
command -v in-target >/dev/null 2>&1 ||
	fail "The Debian Installer in-target command is unavailable."

if [ -L "$DROPIN_DIR" ]; then
	fail "Refusing to write through a symbolic-link SSH drop-in directory: $DROPIN_DIR."
fi
mkdir -p "$DROPIN_DIR"
[ -d "$DROPIN_DIR" ] && [ ! -L "$DROPIN_DIR" ] ||
	fail "Cannot create a safe SSH drop-in directory: $DROPIN_DIR."
[ ! -L "$DROPIN" ] ||
	fail "Refusing to replace a symbolic-link SSH configuration file: $DROPIN."

TMP=$(mktemp "$DROPIN_DIR/.50-Debian.conf.XXXXXX") ||
	fail "Cannot create a temporary SSH configuration file."

cat > "$TMP" <<'SSHD_CONFIG'
PermitRootLogin yes
PasswordAuthentication yes
SSHD_CONFIG
chmod 644 "$TMP"
mv -f "$TMP" "$DROPIN"
TMP=

# Validate the installed daemon configuration and confirm that these settings
# are effective after all included drop-ins have been read.
mkdir -p "$TARGET_ROOT/run/sshd"
chmod 755 "$TARGET_ROOT/run/sshd"
in-target /usr/sbin/sshd -t ||
	fail "The installed OpenSSH configuration failed validation."
EFFECTIVE_CONFIG=$(in-target /usr/sbin/sshd -T 2>/dev/null) ||
	fail "Cannot read the effective installed OpenSSH configuration."
printf '%s\n' "$EFFECTIVE_CONFIG" | grep -Fxq 'permitrootlogin yes' ||
	fail "PermitRootLogin yes is not effective in the installed SSH configuration."
printf '%s\n' "$EFFECTIVE_CONFIG" | grep -Fxq 'passwordauthentication yes' ||
	fail "PasswordAuthentication yes is not effective in the installed SSH configuration."
in-target systemctl enable ssh.service ||
	fail "Could not enable ssh.service in the installed target."

printf 'Wrote, validated and enabled %s\n' "$DROPIN"
SSH_HOOK

put 755 config/includes.binary/debian-live-server-target-grub.sh <<'GRUB_HOOK'
#!/bin/sh
#
# debian-live-server-target-grub.sh
# Apply and regenerate the installed system's GRUB timeout configuration.
#
set -eu
umask 077

TARGET_ROOT=${TARGET_ROOT:-/target}

fail()
{
	printf 'GRUB CONFIGURATION ERROR: %s\n' "$*" >&2
	exit 1
}

[ -d "$TARGET_ROOT/etc" ] || fail "The installed target is not mounted at $TARGET_ROOT."
GRUB_FILE=$TARGET_ROOT/etc/default/grub
[ -f "$GRUB_FILE" ] && [ ! -L "$GRUB_FILE" ] ||
	fail "The installed target's /etc/default/grub is missing or unsafe."
command -v in-target >/dev/null 2>&1 ||
	fail "The Debian Installer in-target command is unavailable."

set_kv()
{
	_file=$1
	_key=$2
	_value=$3
	_tmp=${_file}.tmp.$$
	if grep -q "^${_key}=" "$_file"; then
		sed "s|^${_key}=.*|${_key}=${_value}|" "$_file" > "$_tmp"
		cat "$_tmp" > "$_file"
		rm -f "$_tmp"
	else
		printf '%s=%s\n' "$_key" "$_value" >> "$_file"
	fi
}

set_kv "$GRUB_FILE" GRUB_TIMEOUT 0
set_kv "$GRUB_FILE" GRUB_TIMEOUT_STYLE hidden
set_kv "$GRUB_FILE" GRUB_RECORDFAIL_TIMEOUT 0

grep -Fxq 'GRUB_TIMEOUT=0' "$GRUB_FILE" ||
	fail "GRUB_TIMEOUT=0 was not persisted."
grep -Fxq 'GRUB_TIMEOUT_STYLE=hidden' "$GRUB_FILE" ||
	fail "GRUB_TIMEOUT_STYLE=hidden was not persisted."
grep -Fxq 'GRUB_RECORDFAIL_TIMEOUT=0' "$GRUB_FILE" ||
	fail "GRUB_RECORDFAIL_TIMEOUT=0 was not persisted."

in-target update-grub ||
	fail "update-grub failed in the installed target."

GRUB_CFG=$TARGET_ROOT/boot/grub/grub.cfg
[ -s "$GRUB_CFG" ] ||
	fail "update-grub did not produce /boot/grub/grub.cfg."
grep -Eq '^[[:space:]]*set timeout=0[[:space:]]*$' "$GRUB_CFG" ||
	fail "Generated grub.cfg does not contain the required zero-second timeout."
grep -Eq '^[[:space:]]*set timeout_style=hidden[[:space:]]*$' "$GRUB_CFG" ||
	fail "Generated grub.cfg does not contain the required hidden timeout style."

printf 'Installed GRUB configuration:\n'
grep -E '^(GRUB_TIMEOUT|GRUB_TIMEOUT_STYLE|GRUB_RECORDFAIL_TIMEOUT)=' "$GRUB_FILE"
printf 'Regenerated %s.\n' "$TARGET_ROOT/boot/grub/grub.cfg"
GRUB_HOOK

put 755 config/includes.binary/debian-live-server-target-postinstall.sh <<'POSTINSTALL_HOOK'
#!/bin/sh
#
# debian-live-server-target-postinstall.sh
# Run each target post-install hook independently and retain a combined log.
#
set -u
umask 077

TARGET_ROOT=${TARGET_ROOT:-/target}
HOOK_DIR=${HOOK_DIR:-/cdrom}
LOG=$TARGET_ROOT/var/log/debian-live-server-postinstall.log

[ -d "$TARGET_ROOT/etc" ] || {
	printf 'POST-INSTALL ERROR: target is not mounted at %s.\n' "$TARGET_ROOT" >&2
	exit 1
}
mkdir -p "$TARGET_ROOT/var/log"
exec 3>&1
exec >> "$LOG" 2>&1

failed=0
run_hook()
{
	_name=$1
	_path=$2
	printf '\n===== %s =====\n' "$_name"
	if [ ! -r "$_path" ]; then
		printf 'ERROR: required hook is missing: %s\n' "$_path"
		failed=1
		return
	fi
	if /bin/sh "$_path"; then
		printf 'RESULT: %s completed successfully.\n' "$_name"
	else
		_rc=$?
		printf 'ERROR: %s failed with exit code %s.\n' "$_name" "$_rc"
		failed=1
	fi
}

printf 'Target post-installation started: %s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
run_hook "System configuration" "$HOOK_DIR/debian-live-server-target-system.sh"
if [ -f "$TARGET_ROOT/var/log/debian-live-server-system.log" ]; then
	printf '\n===== Target system configuration detail =====\n'
	cat "$TARGET_ROOT/var/log/debian-live-server-system.log"
fi
run_hook "Network configuration" "$HOOK_DIR/debian-live-server-target-network.sh"
if [ -f "$TARGET_ROOT/var/log/debian-live-server-network-discovery.log" ]; then
	printf '\n===== Network discovery detail =====\n'
	cat "$TARGET_ROOT/var/log/debian-live-server-network-discovery.log"
fi
run_hook "OpenSSH configuration" "$HOOK_DIR/debian-live-server-target-ssh.sh"
run_hook "GRUB configuration" "$HOOK_DIR/debian-live-server-target-grub.sh"

if [ "$failed" -ne 0 ]; then
	printf '\nPOST-INSTALL FAILED. Review %s for individual hook output.\n' "$LOG"
	printf 'Target post-installation failed. See %s\n' "$LOG" >&3
	exit 1
fi

printf '\nAll target post-installation hooks completed successfully.\n'
printf 'Target post-installation completed. Log: %s\n' "$LOG" >&3
POSTINSTALL_HOOK

# Preseed: static answers first, then optional sections driven by the variables above.
PRESEED_FILE=config/includes.binary/preseed.cfg
put 600 "$PRESEED_FILE" <<PRESEED
d-i auto-install/enable boolean true
d-i debconf/priority string critical
d-i debian-installer/locale string $LOCALE
d-i localechooser/supported-locales multiselect $LOCALE
d-i keyboard-configuration/xkb-keymap select $KEYMAP
d-i console-setup/ask_detect boolean false
d-i clock-setup/utc boolean true
d-i time/zone string $TIMEZONE
d-i clock-setup/ntp boolean true

d-i netcfg/enable boolean true
d-i netcfg/disable_autoconfig boolean false
d-i netcfg/choose_interface select auto
d-i netcfg/link_wait_timeout string 10
d-i netcfg/dhcp_timeout string 45
d-i netcfg/dhcpv6_timeout string 15
d-i netcfg/dhcp_failed note
d-i netcfg/dhcp_options select Do not configure the network at this time

d-i hw-detect/load_firmware boolean true
d-i mirror/country string manual
d-i mirror/http/hostname string deb.debian.org
d-i mirror/http/directory string /debian
d-i mirror/http/proxy string
d-i mirror/suite string $CODENAME

# Enable the root account and do not create a regular user.
# Leave the root password unset here so the graphical installer asks for it
# (and its confirmation) interactively.
d-i passwd/root-login boolean true
d-i passwd/make-user boolean false

tasksel tasksel/first multiselect standard, ssh-server
# Target packages. netplan.io renders the static configuration, wpasupplicant is the
# Wi-Fi backend for systemd-networkd, and the two systemd units are enabled by the
# late_command. openssh-server is configured by the separate target-only SSH hook.
d-i pkgsel/include string cloud-init fastfetch netplan.io wpasupplicant systemd-resolved systemd-timesyncd openssh-server
d-i pkgsel/upgrade select none
popularity-contest popularity-contest/participate boolean false

d-i grub-installer/only_debian boolean true
d-i grub-installer/with_other_os boolean true
d-i preseed/late_command string /bin/sh /cdrom/debian-live-server-target-postinstall.sh
d-i finish-install/reboot_in_progress note
PRESEED

if [ -n "$TARGET_HOSTNAME" ]; then
	printf 'd-i netcfg/hostname string %s\n' "$TARGET_HOSTNAME" >> "$PRESEED_FILE"
fi
if [ -n "$TARGET_DOMAIN" ]; then
	printf 'd-i netcfg/get_domain string %s\n' "$TARGET_DOMAIN" >> "$PRESEED_FILE"
fi
if [ -n "$TARGET_DISK" ]; then
	{
		printf 'd-i partman/early_command string if [ ! -b %s ] || [ -e /sys/class/block/%s/partition ]; then exit 1; fi\n' \
			"$TARGET_DISK" "$DISK_NAME"
		printf 'd-i partman-auto/disk string %s\n' "$TARGET_DISK"
		cat <<'PARTMAN'
d-i partman-auto/method string regular
d-i partman-auto/choose_recipe select atomic
d-i partman-partitioning/confirm_write_new_label boolean true
d-i partman-md/device_remove_md boolean true
d-i partman-lvm/device_remove_lvm boolean true
d-i partman/choose_partition select finish
d-i partman/confirm boolean true
d-i partman/confirm_nooverwrite boolean true
PARTMAN
		printf 'd-i grub-installer/bootdev string %s\n' "$TARGET_DISK"
	} >> "$PRESEED_FILE"
fi

# Boot menus. Both are generated from the same variables, so the default entry, timeout
# and kernel parameters agree. The installer is the default; the live session is separate.
ISOLINUX_TIMEOUT=$((BOOT_TIMEOUT_SECONDS * 10))
put 644 config/bootloaders/isolinux/isolinux.cfg <<ISOLINUX
# BIOS boot menu (ISOLINUX). Generated by build-debian-live.sh.
UI vesamenu.c32
DEFAULT installer-gtk
PROMPT 0
TIMEOUT $ISOLINUX_TIMEOUT
MENU TITLE Custom Debian Live

LABEL installer-gtk
	MENU LABEL ^Install Debian (graphical installer)
	MENU DEFAULT
	KERNEL /install/gtk/vmlinuz
	APPEND initrd=/install/gtk/initrd.gz $BOOTAPPEND_INSTALL --- quiet

LABEL live
	MENU LABEL ^Try Debian Live without installing
	KERNEL /live/vmlinuz
	APPEND initrd=/live/initrd.img $BOOTAPPEND_LIVE
ISOLINUX

# live-build generates /boot/grub/grub.cfg for both GRUB BIOS and UEFI from
# config/bootloaders/grub-pc; grub-efi is not the source for this menu.
put 644 config/bootloaders/grub-pc/grub.cfg <<GRUBCFG
# UEFI boot menu (GRUB). Generated by build-debian-live.sh.
if search --no-floppy --set=root --file /live/vmlinuz; then
	set iso_root_found=1
else
	search --no-floppy --set=root --label "$ISO_VOLUME"
fi

set default="installer-gtk"
set timeout=$BOOT_TIMEOUT_SECONDS

menuentry "Install Debian (graphical installer)" --id installer-gtk {
	linux /install/gtk/vmlinuz $BOOTAPPEND_INSTALL --- quiet
	initrd /install/gtk/initrd.gz
}

menuentry "Try Debian Live without installing" --id live {
	linux /live/vmlinuz $BOOTAPPEND_LIVE
	initrd /live/initrd.img
}
GRUBCFG

# ---- Build -------------------------------------------------------------------

lb config --dump > "$WORKSPACE_DIR/lb-config.dump" 2>&1
say "Building Debian Live Standard and its graphical installer."
# Keep restrictive file creation for logs and secrets, but use standard system
# permissions for the generated Debian filesystem and package triggers.
(
	umask 022
	run lb build
)

set --
for _candidate in "$WORKSPACE_DIR"/*.iso; do
	if [ -f "$_candidate" ]; then
		set -- "$@" "$_candidate"
	fi
done
[ "$#" -eq 1 ] || fail "Expected exactly one ISO from live-build; found $#."
# Verify the workspace ISO first. A rejected image must not be published to the
# final output path, otherwise the next invocation refuses to overwrite it.
BUILT_ISO=$1
FINAL_OUTPUT_ISO=$OUTPUT_ISO
OUTPUT_ISO=$BUILT_ISO

ISO_BYTES=$(wc -c < "$OUTPUT_ISO" | tr -d ' ')
[ "$ISO_BYTES" -ge "$MIN_ISO_BYTES" ] || fail "Output ISO is unexpectedly smaller than 100 MiB."

# ---- ISO verification --------------------------------------------------------

iso_report eltorito-report.txt -report_el_torito plain
grep -qi 'El Torito' "$WORKSPACE_DIR/eltorito-report.txt" ||
	fail "No El Torito boot catalog was reported."

# Classify each El Torito image by platform ID: BIOS is 0x00, UEFI is 0xEF. An image that
# cannot be identified is a failure.
EL_CLASSES=$WORKSPACE_DIR/eltorito-platforms.txt
awk '
	/El Torito boot img/ {
		line = $0
		sub(/^.*El Torito boot img[^:]*:/, "", line)
		n = split(line, f, /[ \t]+/)
		plat = "UNKNOWN"
		for (j = 1; j <= n; j++) {
			t = tolower(f[j])
			if (t == "bios" || t == "0x00") { plat = "BIOS"; break }
			if (t == "uefi" || t == "efi" || t == "0xef") { plat = "UEFI"; break }
		}
		print plat "\t" $0
	}' "$WORKSPACE_DIR/eltorito-report.txt" > "$EL_CLASSES"
EL_BIOS=$(grep -c '^BIOS' "$EL_CLASSES" || :)
EL_UEFI=$(grep -c '^UEFI' "$EL_CLASSES" || :)
EL_UNKNOWN=$(grep -c '^UNKNOWN' "$EL_CLASSES" || :)
[ "$EL_UNKNOWN" -eq 0 ] ||
	fail "Cannot identify the platform of $EL_UNKNOWN El Torito boot image(s); see $EL_CLASSES."
[ "$EL_BIOS" -ge 1 ] || fail "The ISO has no BIOS El Torito boot image (platform 0x00)."
[ "$EL_UEFI" -ge 1 ] || fail "The ISO has no UEFI El Torito boot image (platform 0xEF)."

iso_report live-files.txt -ls /live
grep -q 'vmlinuz' "$WORKSPACE_DIR/live-files.txt" || fail "Live kernel is missing from /live."
grep -q 'initrd.img' "$WORKSPACE_DIR/live-files.txt" || fail "Live initrd.img is missing from /live."

iso_report installer-gtk-files.txt -ls /install/gtk
grep -q 'vmlinuz' "$WORKSPACE_DIR/installer-gtk-files.txt" ||
	fail "GTK installer kernel is missing from /install/gtk."
grep -q 'initrd.gz' "$WORKSPACE_DIR/installer-gtk-files.txt" ||
	fail "GTK installer initrd is missing from /install/gtk."

iso_report system-area-report.txt -report_system_area plain

# Exact-copy checks: the files the installer and boot menus rely on must be byte-identical
# to their sources in this build.
ISO_VERIFY_DIR=$WORKSPACE_DIR/iso-verify
mkdir -m 700 "$ISO_VERIFY_DIR"

# verify_iso_copy ISO_PATH SOURCE LABEL: the ISO copy must match SOURCE exactly.
verify_iso_copy()
{
	_vc_tmp=$ISO_VERIFY_DIR/$(basename "$1").check
	iso_extract "$1" "$_vc_tmp"
	_vc_iso=$(sha256_of "$_vc_tmp")
	_vc_src=$(sha256_of "$2")
	rm -f "$_vc_tmp"
	[ "$_vc_iso" = "$_vc_src" ] || fail "$3 on the ISO does not match $2."
}

verify_iso_copy /preseed.cfg config/includes.binary/preseed.cfg "The preseed file"
verify_iso_copy /debian-live-server-target-system.sh config/includes.binary/debian-live-server-target-system.sh "The target system hook"
verify_iso_copy /debian-live-server-target-network.sh config/includes.binary/debian-live-server-target-network.sh "The target network hook"
verify_iso_copy /debian-live-server-target-ssh.sh config/includes.binary/debian-live-server-target-ssh.sh "The target SSH hook"
verify_iso_copy /debian-live-server-target-grub.sh config/includes.binary/debian-live-server-target-grub.sh "The target GRUB hook"
verify_iso_copy /debian-live-server-target-postinstall.sh config/includes.binary/debian-live-server-target-postinstall.sh "The target post-install orchestrator"
verify_iso_copy /sysctl.conf config/includes.binary/sysctl.conf "The sysctl configuration"
# Reject an unrelated GRUB menu even if its boot catalog is UEFI-capable.
verify_iso_copy /boot/grub/grub.cfg config/bootloaders/grub-pc/grub.cfg "The GRUB boot menu"

# The BIOS menu loads the vesamenu module from the ISO, so the module must be present.
iso_extract /isolinux/vesamenu.c32 "$ISO_VERIFY_DIR/vesamenu.c32"
iso_extract /isolinux/isolinux.cfg "$ISO_VERIFY_DIR/isolinux.cfg"
iso_extract /boot/grub/grub.cfg "$ISO_VERIFY_DIR/grub.cfg"

# Menu checks, shared by both menus. Both must boot the installer by default after the
# timeout. The live session must be a separate entry without installer parameters.
# tag names the menu in messages; the menu-specific rules come before these checks.
ENTRY_CHECKS='
		if (index(body["installer-gtk"], "/install/gtk/vmlinuz") == 0 ||
			index(body["installer-gtk"], inst) == 0) {
			print tag " installer entry lacks the installer kernel or its parameters."; bad = 1
		}
		if (index(body["live"], "/live/vmlinuz") == 0 || index(body["live"], live) == 0) {
			print tag " live entry lacks the live kernel or its parameters."; bad = 1
		}
		if (index(body["live"], "preseed/file") > 0) {
			print tag " live entry carries installer parameters."; bad = 1
		}
		exit bad
	}'

# ISOLINUX: keywords are case-insensitive; entries are collected by label.
BIOS_RULES='
	tolower($1) == "label" { cur = $2; next }
	tolower($1) == "default" { def = $2; next }
	tolower($1) == "timeout" { tmo = $2; next }
	cur != "" && (tolower($1) == "kernel" || tolower($1) == "append" || tolower($1) == "initrd") {
		body[cur] = body[cur] " " $0
	}
	END {
		bad = 0
		if (def != "installer-gtk") { print "BIOS menu DEFAULT is \"" def "\"; expected installer-gtk."; bad = 1 }
		if (tmo != want_tmo) { print "BIOS menu TIMEOUT is " tmo " (tenths of a second); expected " want_tmo "."; bad = 1 }'

# GRUB: entries are menuentry blocks identified by --id.
UEFI_RULES='
	/^set default=/ { def = $0 }
	/^set timeout=/ { tmo = $0 }
	/^menuentry / {
		cur = $0; sub(/.*--id[ \t]+/, "", cur); sub(/[ \t].*$/, "", cur); next
	}
	/^}/ { cur = ""; next }
	cur != "" && ($1 == "linux" || $1 == "initrd") { body[cur] = body[cur] " " $0 }
	END {
		bad = 0
		if (def != "set default=\"installer-gtk\"") { print "UEFI menu default is: " def "; expected installer-gtk."; bad = 1 }
		if (tmo != "set timeout=" want_tmo) { print "UEFI menu timeout is: " tmo "; expected " want_tmo "."; bad = 1 }'

if ! awk -v tag=BIOS -v inst="$BOOTAPPEND_INSTALL" -v live="$BOOTAPPEND_LIVE" \
		-v want_tmo="$((BOOT_TIMEOUT_SECONDS * 10))" "$BIOS_RULES$ENTRY_CHECKS" \
		"$ISO_VERIFY_DIR/isolinux.cfg"; then
	fail "The BIOS boot menu on the ISO does not match the required installer-default configuration."
fi

if ! awk -v tag=UEFI -v inst="$BOOTAPPEND_INSTALL" -v live="$BOOTAPPEND_LIVE" \
		-v want_tmo="$BOOT_TIMEOUT_SECONDS" "$UEFI_RULES$ENTRY_CHECKS" \
		"$ISO_VERIFY_DIR/grub.cfg"; then
	fail "The UEFI boot menu on the ISO does not match the required installer-default configuration."
fi

# ---- Artifacts ---------------------------------------------------------------

# Publish only after all ISO checks pass; keep the final filename in provenance.
mv "$OUTPUT_ISO" "$FINAL_OUTPUT_ISO"
OUTPUT_ISO=$FINAL_OUTPUT_ISO
ISO_SHA256=$(sha256_of "$OUTPUT_ISO")
printf '%s  %s\n' "$ISO_SHA256" "$OUTPUT_ISO" > "$ISO_SHA_FILE"
chmod 600 "$ISO_SHA_FILE"

if [ -d "$WORKSPACE_DIR/chroot/var/lib/dpkg" ]; then
	dpkg-query --admindir="$WORKSPACE_DIR/chroot/var/lib/dpkg" \
		-W -f='${binary:Package}\t${Version}\n' \
		> "$WORKSPACE_DIR/live-package-versions.txt" 2>&1
fi

if [ -n "$TARGET_DISK" ]; then AUTO_PARTITIONING=enabled; else AUTO_PARTITIONING=disabled; fi

{
	printf 'Artifact: %s\n' "$OUTPUT_ISO"
	printf 'SHA256: %s\n' "$ISO_SHA256"
	printf 'Size bytes: %s\n' "$ISO_BYTES"
	printf 'Build UTC: %s\n' "$(timestamp)"
	printf 'Build host: %s\n' "$(uname -srm)"
	printf 'Host Debian version: %s\n' "$(cat /etc/debian_version)"
	printf 'Architecture: amd64\n'
	printf 'Requested suite: %s\n' "$DEBIAN_SUITE"
	printf 'Resolved codename: %s\n' "$CODENAME"
	printf 'live-build version: %s\n' "$LB_VERSION"
	printf 'Debian repository: %s\n' "$DEBIAN_MIRROR"
	printf 'Security repository: %s\n' "$SECURITY_MIRROR"
	printf 'Installer network: netcfg (DHCP on the interface chosen by the installer); no build-host network values embedded\n'
	printf 'Installed network: static systemd-networkd Netplan at /etc/netplan/10-installer.yaml when discovery succeeds; otherwise preserve DHCP configuration\n'
	printf 'Automatic partitioning: %s\n' "$AUTO_PARTITIONING"
	printf 'Installer account: root-only (root password entered interactively in the graphical installer)\n'
	printf 'Installed SSH: target-only /etc/ssh/sshd_config.d/50-Debian.conf; root password authentication enabled\n'
	printf 'Installed GRUB: GRUB_TIMEOUT=0, GRUB_TIMEOUT_STYLE=hidden, GRUB_RECORDFAIL_TIMEOUT=0; update-grub executed\n'
	printf 'Target post-install: independent system, network, SSH and GRUB hooks; combined log /var/log/debian-live-server-postinstall.log\n'
	printf 'Installer: Debian Installer GTK, live-installer mode\n'
	printf 'Target packages: cloud-init is installed by pkgsel; its network configuration is disabled to preserve installer-generated Netplan\n'
	printf 'Boot menus: BIOS (ISOLINUX) and UEFI (GRUB); the graphical installer is the default after %s seconds\n' "$BOOT_TIMEOUT_SECONDS"
	printf 'Live session: root console autologin; cloud-init omitted from live root and disabled at live boot; network-online wait masked for live boot only\n'
	printf 'Fastfetch: invoked from /etc/profile in both live and installed systems\n'
	printf 'Installer kernel parameters: %s\n' "$BOOTAPPEND_INSTALL"
	printf 'Preseed: /preseed.cfg on the ISO root, loaded by preseed/file=/cdrom/preseed.cfg\n'
	printf 'El Torito report: %s/eltorito-report.txt\n' "$WORKSPACE_DIR"
	printf 'Build configuration: %s/lb-config.dump\n' "$WORKSPACE_DIR"
	printf 'Live package versions: %s/live-package-versions.txt\n' "$WORKSPACE_DIR"
	printf 'Build log: %s\n' "$LOG_FILE"
	printf '\nThis is a customized derivative, not an official Debian release ISO.\n'
	printf 'Debian repository signatures authenticate repository metadata, not this ISO.\n'
	printf 'The DHCP-derived address must be reserved for the target MAC to avoid conflicts.\n'
} > "$ISO_PROVENANCE"
chmod 600 "$ISO_PROVENANCE"

say "Build completed."
say "ISO:        $OUTPUT_ISO"
say "SHA256:     $ISO_SHA_FILE"
say "Provenance: $ISO_PROVENANCE"
say "Workspace:  $WORKSPACE_DIR"
say "Build log:  $LOG_FILE"
