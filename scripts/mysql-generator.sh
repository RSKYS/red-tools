#!/bin/sh

# mysql-generator.sh
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

set -eu

umask 077
export LC_ALL=C
export DEBIAN_FRONTEND=noninteractive

MARIADB_RELEASE='11.8.8'
PHPMYADMIN_ADDRESS='127.0.0.1'
PHPMYADMIN_SERVICE='phpmyadmin-local'
PHPMYADMIN_RUNTIME='/run/phpmyadmin-local'

STATE_DIR='/var/lib/phpmyadmin-manager'
BACKUP_DIR="$STATE_DIR/backups"
STATUS_FILE="$STATE_DIR/status"
PACKAGE_FILE="$STATE_DIR/packages-installed-by-script"
DATABASE_FILE="$STATE_DIR/database-name"
USER_FILE="$STATE_DIR/database-user"
DEBCONF_BACKUP="$STATE_DIR/phpmyadmin-debconf-selections"

REPOSITORY_LIST="/etc/apt/sources.list.d/mariadb-$MARIADB_RELEASE.list"
REPOSITORY_KEY='/usr/share/keyrings/mariadb-keyring.gpg'
MARIADB_BIND_CONFIG='/etc/mysql/mariadb.conf.d/99-local-bind.cnf'
PHPMYADMIN_CONFIG='/etc/phpmyadmin/config.inc.php'
PHPMYADMIN_LINK='/usr/share/phpmyadmin/config.inc.php'
PHPMYADMIN_SERVICE_FILE="/etc/systemd/system/$PHPMYADMIN_SERVICE.service"

fail() {
	printf 'Error: %s\n' "$*" >&2
	exit 1
}

info() {
	printf '[ * ] %s\n' "$*"
}

warn() {
	printf '[ ! ] %s\n' "$*" >&2
}

require_command() {
	command -v "$1" > /dev/null 2>&1 || fail "Required command not found: $1"
}

is_package_installed() {
	dpkg-query -W -f='${db:Status-Abbrev}' "$1" 2> /dev/null \
		| grep '^ii ' > /dev/null 2>&1
}

service_unit_exists() {
	service_name=$1
	systemctl list-unit-files "$service_name.service" --no-legend 2> /dev/null \
		| awk '{ print $1 }' \
		| grep -Fx "$service_name.service" > /dev/null 2>&1
}

random_hex() {
	# $1 is the number of random bytes. Output is lowercase hexadecimal.
	dd if=/dev/urandom bs=1 count="$1" 2> /dev/null \
		| od -A n -t x1 \
		| tr -d ' \n'
}

generate_password() {
	while :; do
		password=$(tr -dc 'A-Za-z0-9!@#%&_=+^-' < /dev/urandom \
			| dd bs=1 count=20 2> /dev/null)

		[ "${#password}" -eq 20 ] || continue

		case $password in *[A-Z]*) ;; *) continue ;; esac
		case $password in *[a-z]*) ;; *) continue ;; esac
		case $password in *[0-9]*) ;; *) continue ;; esac
		case $password in
			*'!'* | *'@'* | *'#'* | *'%'* | *'&'* | *'_'* | *'='* | *'+'* | *'^'* | *'-'*) ;;
			*) continue ;;
		esac

		printf '%s\n' "$password"
		return 0
	done
}

urlencode() {
	# Percent-encode an ASCII string according to RFC 3986.
	value=$1
	encoded=''

	while [ -n "$value" ]; do
		remainder=${value#?}
		character=${value%"$remainder"}
		value=$remainder

		case $character in
			[A-Za-z0-9._~-])
				encoded=${encoded}${character}
				;;
			*)
				hexadecimal=$(printf '%s' "$character" \
					| od -A n -t x1 \
					| tr -d ' \n' \
					| tr 'abcdef' 'ABCDEF')
				encoded=${encoded}%${hexadecimal}
				;;
		esac
	done

	printf '%s' "$encoded"
}

prompt_identifier() {
	prompt=$1
	maximum_length=$2

	while :; do
		printf '%s' "$prompt" >&2
		IFS= read -r identifier || fail 'Input ended unexpectedly.'

		case $identifier in
			'')
				printf 'Value cannot be empty.\n' >&2
				continue
				;;
			*[!A-Za-z0-9_]*)
				printf 'Use only ASCII letters, digits, and underscores.\n' >&2
				continue
				;;
		esac

		if [ "${#identifier}" -gt "$maximum_length" ]; then
			printf 'Value must not exceed %s characters.\n' "$maximum_length" >&2
			continue
		fi

		printf '%s\n' "$identifier"
		return 0
	done
}

prompt_yes_no() {
	question=$1
	default_answer=$2

	while :; do
		case $default_answer in
			yes) suffix='[Y/n]' ;;
			no) suffix='[y/N]' ;;
			*) fail 'Invalid yes/no default.' ;;
		esac

		printf '%s %s ' "$question" "$suffix" >&2
		IFS= read -r answer || fail 'Input ended unexpectedly.'

		case $answer in
			'')
				[ "$default_answer" = yes ] && return 0
				return 1
				;;
			y | Y | yes | YES | Yes) return 0 ;;
			n | N | no | NO | No) return 1 ;;
			*) printf 'Enter yes or no.\n' >&2 ;;
		esac
	done
}

ensure_root_and_platform() {
	[ "$(id -u)" -eq 0 ] || fail 'Run this script as root.'
	[ -r /etc/os-release ] || fail '/etc/os-release is unavailable.'

	require_command apt-get
	require_command dpkg
	require_command dpkg-query
	require_command systemctl
}

# Back up a regular file or symbolic link. Original absence is also recorded.
backup_path() {
	path=$1
	key=$2
	type_file="$BACKUP_DIR/$key.type"

	mkdir -p "$BACKUP_DIR"

	if [ -L "$path" ]; then
		printf '%s\n' 'symlink' > "$type_file"
		readlink "$path" > "$BACKUP_DIR/$key.target"
	elif [ -f "$path" ]; then
		printf '%s\n' 'file' > "$type_file"
		cp -p "$path" "$BACKUP_DIR/$key.file"
	elif [ -e "$path" ]; then
		fail "Refusing to replace unsupported path type: $path"
	else
		printf '%s\n' 'absent' > "$type_file"
	fi
}

restore_path() {
	path=$1
	key=$2
	type_file="$BACKUP_DIR/$key.type"

	[ -f "$type_file" ] || return 0
	saved_type=$(cat "$type_file")

	rm -f "$path"
	mkdir -p "$(dirname "$path")"

	case $saved_type in
		absent) ;;
		file)
			cp -p "$BACKUP_DIR/$key.file" "$path"
			;;
		symlink)
			saved_target=$(cat "$BACKUP_DIR/$key.target")
			ln -s "$saved_target" "$path"
			;;
		*)
			fail "Invalid backup metadata for $path"
			;;
	esac
}

record_service_state() {
	service_name=$1
	key=$2

	if service_unit_exists "$service_name"; then
		printf '%s\n' 'present' > "$STATE_DIR/$key.presence"
		systemctl is-enabled "$service_name" > "$STATE_DIR/$key.enabled" 2> /dev/null || true
		systemctl is-active "$service_name" > "$STATE_DIR/$key.active" 2> /dev/null || true
	else
		printf '%s\n' 'absent' > "$STATE_DIR/$key.presence"
		printf '%s\n' 'disabled' > "$STATE_DIR/$key.enabled"
		printf '%s\n' 'inactive' > "$STATE_DIR/$key.active"
	fi
}

restore_service_state() {
	service_name=$1
	key=$2

	service_unit_exists "$service_name" || return 0

	presence=$(cat "$STATE_DIR/$key.presence" 2> /dev/null || printf '%s' 'absent')
	enabled_state=$(cat "$STATE_DIR/$key.enabled" 2> /dev/null || printf '%s' 'disabled')
	active_state=$(cat "$STATE_DIR/$key.active" 2> /dev/null || printf '%s' 'inactive')

	if [ "$presence" = absent ]; then
		systemctl disable "$service_name" > /dev/null 2>&1 || true
		systemctl stop "$service_name" > /dev/null 2>&1 || true
		return 0
	fi

	case $enabled_state in
		enabled | enabled-runtime)
			systemctl enable "$service_name" > /dev/null 2>&1 || true
			;;
		masked | masked-runtime)
			systemctl mask "$service_name" > /dev/null 2>&1 || true
			;;
		disabled)
			systemctl disable "$service_name" > /dev/null 2>&1 || true
			;;
		*)
			# Static, indirect, generated, alias, and linked units need no action.
			;;
	esac

	case $active_state in
		active)
			systemctl restart "$service_name" > /dev/null 2>&1 \
				|| systemctl start "$service_name" > /dev/null 2>&1 \
				|| true
			;;
		*)
			systemctl stop "$service_name" > /dev/null 2>&1 || true
			;;
	esac
}

write_package_ownership() {
	: > "$PACKAGE_FILE"

	for package in \
		ca-certificates \
		curl \
		gnupg \
		debconf-utils \
		iproute2 \
		mariadb-server \
		mariadb-client \
		php-cli \
		php-mysql \
		php-mbstring \
		php-zip \
		php-gd \
		php-curl \
		php-xml \
		php-bz2 \
		php-intl \
		phpmyadmin; do
		if ! is_package_installed "$package"; then
			printf '%s\n' "$package" >> "$PACKAGE_FILE"
		fi
	done
}

preflight_existing_database_server() {
	if is_package_installed mysql-server \
		|| is_package_installed mysql-community-server \
		|| is_package_installed percona-server-server; then
		fail 'A MySQL/Percona server is already installed. Remove it or use an isolated host.'
	fi

	if is_package_installed mariadb-server; then
		existing_version=$(mariadbd --version 2> /dev/null || true)
		case $existing_version in
			*"Distrib $MARIADB_RELEASE"* | *"$MARIADB_RELEASE-MariaDB"*) ;;
			*)
				fail "An existing MariaDB server is not version $MARIADB_RELEASE: ${existing_version:-unknown}"
				;;
		esac
	fi
}

database_exists() {
	database_name=$1
	mariadb --protocol=socket --batch --skip-column-names \
		-e "SELECT SCHEMA_NAME FROM information_schema.SCHEMATA WHERE SCHEMA_NAME = '$database_name';" \
		2> /dev/null | grep -Fx "$database_name" > /dev/null 2>&1
}

user_exists() {
	database_user=$1
	mariadb --protocol=socket --batch --skip-column-names \
		-e "SELECT User FROM mysql.user WHERE User = '$database_user' AND Host IN ('127.0.0.1', 'localhost');" \
		2> /dev/null | grep -Fx "$database_user" > /dev/null 2>&1
}

select_new_database_name() {
	while :; do
		selected_database=$(prompt_identifier 'Database name: ' 64)
		if database_exists "$selected_database"; then
			warn "Database '$selected_database' already exists. Choose another name."
			continue
		fi
		printf '%s\n' "$selected_database"
		return 0
	done
}

select_new_database_user() {
	while :; do
		selected_user=$(prompt_identifier 'Database username: ' 32)
		if user_exists "$selected_user"; then
			warn "User '$selected_user'@'127.0.0.1' already exists. Choose another name."
			continue
		fi
		printf '%s\n' "$selected_user"
		return 0
	done
}

save_phpmyadmin_debconf() {
	if command -v debconf-get-selections > /dev/null 2>&1; then
		debconf-get-selections 2> /dev/null \
			| awk '$1 == "phpmyadmin"' > "$DEBCONF_BACKUP"
	else
		: > "$DEBCONF_BACKUP"
	fi
}

restore_phpmyadmin_debconf() {
	if [ -s "$DEBCONF_BACKUP" ] \
		&& command -v debconf-set-selections > /dev/null 2>&1; then
		debconf-set-selections < "$DEBCONF_BACKUP"
	elif [ ! -s "$DEBCONF_BACKUP" ] \
		&& command -v debconf-communicate > /dev/null 2>&1; then
		printf '%s\n' 'PURGE' \
			| debconf-communicate phpmyadmin > /dev/null 2>&1 \
			|| true
	fi
}

port_is_in_use() {
	port=$1
	port_hex=$(awk -v port="$port" 'BEGIN { printf "%04X", port }')

	awk -v local_port=":$port_hex" '
		$2 ~ local_port "$" && $4 == "0A" { found = 1; exit }
		END { exit found ? 0 : 1 }
	' /proc/net/tcp /proc/net/tcp6 2> /dev/null
}

prompt_installation_ports() {
	printf '%s' 'phpMyAdmin port: ' >&2
	IFS= read -r PHPMYADMIN_PORT || fail 'Input ended unexpectedly.'

	printf '%s' 'MariaDB port[3306]: ' >&2
	IFS= read -r MARIADB_PORT || fail 'Input ended unexpectedly.'
	MARIADB_PORT=${MARIADB_PORT:-3306}

	case $PHPMYADMIN_PORT in
		'' | *[!0-9]*) fail 'phpMyAdmin Port must be an integer from 1 to 65535.' ;;
	esac
	[ "$PHPMYADMIN_PORT" -ge 1 ] 2> /dev/null \
		&& [ "$PHPMYADMIN_PORT" -le 65535 ] 2> /dev/null \
		|| fail 'phpMyAdmin Port must be an integer from 1 to 65535.'

	case $MARIADB_PORT in
		'' | *[!0-9]*) fail 'MariaDB Port must be an integer from 1 to 65535.' ;;
	esac
	[ "$MARIADB_PORT" -ge 1 ] 2> /dev/null \
		&& [ "$MARIADB_PORT" -le 65535 ] 2> /dev/null \
		|| fail 'MariaDB Port must be an integer from 1 to 65535.'

	if port_is_in_use "$PHPMYADMIN_PORT"; then
		warn "phpMyAdmin Port $PHPMYADMIN_PORT is currently in use."
	else
		info "phpMyAdmin Port $PHPMYADMIN_PORT is currently available."
	fi

	if port_is_in_use "$MARIADB_PORT"; then
		warn "MariaDB Port $MARIADB_PORT is currently in use."
	else
		info "MariaDB Port $MARIADB_PORT is currently available."
	fi
}

port_is_in_use() {
	port=$1
	ss -H -ltn "sport = :$port" 2> /dev/null | grep . > /dev/null 2>&1
}

write_phpmyadmin_service() {
	cat > "$PHPMYADMIN_SERVICE_FILE" <<- SERVICE
		[Unit]
		Description=Local phpMyAdmin HTTP service
		After=network.target mariadb.service
		Wants=mariadb.service

		[Service]
		Type=simple
		User=www-data
		Group=www-data
		RuntimeDirectory=phpmyadmin-local
		RuntimeDirectoryMode=0700
		ExecStartPre=/usr/bin/install -d -m 0700 $PHPMYADMIN_RUNTIME/tmp $PHPMYADMIN_RUNTIME/sessions
		Environment=PHP_CLI_SERVER_WORKERS=4
		ExecStart=/usr/bin/php -d expose_php=0 -d display_errors=0 -d log_errors=1 -d session.use_strict_mode=1 -d session.cookie_httponly=1 -d session.cookie_samesite=Strict -d session.save_path=$PHPMYADMIN_RUNTIME/sessions -d upload_tmp_dir=$PHPMYADMIN_RUNTIME/tmp -d sys_temp_dir=$PHPMYADMIN_RUNTIME/tmp -d upload_max_filesize=16M -d post_max_size=16M -d max_execution_time=120 -d memory_limit=256M -S $PHPMYADMIN_ADDRESS:$PHPMYADMIN_PORT -t /usr/share/phpmyadmin
		Restart=on-failure
		RestartSec=2s
		TimeoutStopSec=15s
		UMask=0077

		NoNewPrivileges=true
		PrivateDevices=true
		PrivateTmp=true
		ProtectSystem=strict
		ProtectHome=true
		ProtectKernelTunables=true
		ProtectKernelModules=true
		ProtectControlGroups=true
		RestrictSUIDSGID=true
		RestrictRealtime=true
		LockPersonality=true
		CapabilityBoundingSet=
		AmbientCapabilities=
		RestrictAddressFamilies=AF_UNIX AF_INET
		ReadWritePaths=$PHPMYADMIN_RUNTIME

		[Install]
		WantedBy=multi-user.target
	SERVICE

	chmod 0644 "$PHPMYADMIN_SERVICE_FILE"
}

deploy() {
	info "MariaDB $MARIADB_RELEASE"
	ensure_root_and_platform

	if [ -e "$STATE_DIR" ]; then
		fail "A previous or partial deployment is recorded in $STATE_DIR. Run '$0 uninstall' first."
	fi

	. /etc/os-release

	VERSION=${ID:-}
	codename=${VERSION_CODENAME:-}

	case $VERSION in
		debian | ubuntu) ;;
		*) fail 'Only Debian and Ubuntu are supported.' ;;
	esac

	[ -n "$codename" ] || fail 'Unable to determine the distribution codename.'

	require_command apt-cache
	preflight_existing_database_server

	ARCH=$(dpkg --print-architecture)
	[ -n "$ARCH" ] || fail 'Unable to determine the Debian architecture.'

	mkdir -p "$BACKUP_DIR"
	chmod 0700 "$STATE_DIR" "$BACKUP_DIR"
	printf '%s\n' 'deploying' > "$STATUS_FILE"

	backup_path "$REPOSITORY_LIST" 'repository-list'
	backup_path "$REPOSITORY_KEY" 'repository-key'
	backup_path "$MARIADB_BIND_CONFIG" 'mariadb-bind-config'
	backup_path "$PHPMYADMIN_CONFIG" 'phpmyadmin-config'
	backup_path "$PHPMYADMIN_LINK" 'phpmyadmin-link'
	backup_path "$PHPMYADMIN_SERVICE_FILE" 'phpmyadmin-service-file'

	write_package_ownership
	record_service_state 'mariadb' 'mariadb-service'
	record_service_state "$PHPMYADMIN_SERVICE" 'phpmyadmin-service'

	info "Detected $VERSION $codename ($ARCH)"
	info 'Installing repository prerequisites'

	apt-get update
	apt-get install -y --no-install-recommends \
		ca-certificates \
		curl \
		gnupg \
		debconf-utils \
		iproute2

	save_phpmyadmin_debconf

	REPOSITORY_BASE="https://mirror.mva-n.net/mariadb/mariadb-$MARIADB_RELEASE/repo/$VERSION"
	RELEASE_URL="$REPOSITORY_BASE/dists/$codename/Release"

	curl -fsS --connect-timeout 15 --max-time 60 -o /dev/null "$RELEASE_URL" \
		|| fail "MariaDB $MARIADB_RELEASE has no repository for $VERSION/$codename."

	info 'Importing the MariaDB repository signing key'
	KEY_SOURCE="$STATE_DIR/MariaDB-PublicKey"
	KEY_NEW="$STATE_DIR/mariadb-keyring.gpg.new"

	curl -fsS --connect-timeout 15 --max-time 60 \
		-o "$KEY_SOURCE" \
		https://mirror.mva-n.net/mariadb/PublicKey

	rm -f "$KEY_NEW"
	gpg --batch --yes --dearmor --output "$KEY_NEW" "$KEY_SOURCE"
	chmod 0644 "$KEY_NEW"
	mv -f "$KEY_NEW" "$REPOSITORY_KEY"
	rm -f "$KEY_SOURCE"

	KEY_FINGERPRINT=$(gpg --show-keys --with-colons "$REPOSITORY_KEY" 2> /dev/null \
		| awk -F: '$1 == "fpr" { print $10; exit }')

	[ "$KEY_FINGERPRINT" = '177F4010FE56CA3336300305F1656F24C74CD1D8' ] \
		|| fail "Unexpected MariaDB signing-key fingerprint: ${KEY_FINGERPRINT:-missing}"

	printf '%s\n' \
		"deb [arch=$ARCH signed-by=/usr/share/keyrings/mariadb-keyring.gpg] https://mirror.mva-n.net/mariadb/mariadb-$MARIADB_RELEASE/repo/$VERSION $codename main" \
		> "$REPOSITORY_LIST"
	chmod 0644 "$REPOSITORY_LIST"

	info 'Refreshing APT metadata'
	apt-get update

	MARIADB_CANDIDATE=$(apt-cache policy mariadb-server \
		| awk '/Candidate:/ { print $2; exit }')

	case $MARIADB_CANDIDATE in
		*"$MARIADB_RELEASE"*) ;;
		*) fail "APT candidate is not MariaDB $MARIADB_RELEASE: ${MARIADB_CANDIDATE:-missing}" ;;
	esac

	printf '%s\n' 'phpmyadmin phpmyadmin/reconfigure-webserver multiselect' \
		| debconf-set-selections
	printf '%s\n' 'phpmyadmin phpmyadmin/dbconfig-install boolean false' \
		| debconf-set-selections

	info 'Installing MariaDB, PHP CLI, and phpMyAdmin'
	apt-get install -y --no-install-recommends \
		mariadb-server \
		mariadb-client \
		php-cli \
		php-mysql \
		php-mbstring \
		php-zip \
		php-gd \
		php-curl \
		php-xml \
		php-bz2 \
		php-intl \
		phpmyadmin

	require_command mariadb
	require_command mariadb-admin
	require_command mariadbd
	require_command php
	require_command curl
	require_command ss

	MARIADB_VERSION=$(mariadbd --version 2> /dev/null || true)
	case $MARIADB_VERSION in
		*"Distrib $MARIADB_RELEASE"* | *"$MARIADB_RELEASE-MariaDB"*) ;;
		*) fail "Installed server is not MariaDB $MARIADB_RELEASE: ${MARIADB_VERSION:-unknown}" ;;
	esac

	info "Binding MariaDB to 127.0.0.1:$MARIADB_PORT"
	mkdir -p /etc/mysql/mariadb.conf.d
	cat > "$MARIADB_BIND_CONFIG" <<- MARIADB
		[mariadbd]
		bind-address = 127.0.0.1
		port = $MARIADB_PORT
		skip-name-resolve
	MARIADB
	chmod 0644 "$MARIADB_BIND_CONFIG"

	systemctl enable mariadb > /dev/null 2>&1
	systemctl restart mariadb
	mariadb-admin --protocol=socket ping > /dev/null 2>&1 \
		|| fail 'MariaDB did not become ready.'

	[ -d /usr/share/phpmyadmin ] \
		|| fail 'The phpMyAdmin document root was not installed.'

	BLOWFISH_HEX=$(random_hex 32)
	[ "${#BLOWFISH_HEX}" -eq 64 ] || fail 'Unable to generate phpMyAdmin secret.'
	php -r 'exit(function_exists("sodium_hex2bin") ? 0 : 1);' \
		|| fail 'The PHP Sodium extension required by phpMyAdmin is unavailable.'

	info 'Writing secure phpMyAdmin configuration'
	mkdir -p /etc/phpmyadmin
	cat > "$PHPMYADMIN_CONFIG" <<- PHPMYADMIN
		<?php
		declare(strict_types=1);

		\$cfg['blowfish_secret'] = sodium_hex2bin('$BLOWFISH_HEX');

		\$i = 0;
		++\$i;
		\$cfg['Servers'][\$i]['verbose'] = 'Local MariaDB';
		\$cfg['Servers'][\$i]['host'] = '127.0.0.1';
		\$cfg['Servers'][\$i]['port'] = '$MARIADB_PORT';
		\$cfg['Servers'][\$i]['connect_type'] = 'tcp';
		\$cfg['Servers'][\$i]['auth_type'] = 'cookie';
		\$cfg['Servers'][\$i]['compress'] = false;
		\$cfg['Servers'][\$i]['AllowNoPassword'] = false;

		\$cfg['AllowArbitraryServer'] = false;
		\$cfg['CookieSameSite'] = 'Strict';
		\$cfg['LoginCookieRecall'] = false;
		\$cfg['LoginCookieValidity'] = 1800;
		\$cfg['TempDir'] = '$PHPMYADMIN_RUNTIME/tmp';
		\$cfg['ShowPhpInfo'] = false;
		\$cfg['ShowChgPassword'] = true;
	PHPMYADMIN
	chmod 0640 "$PHPMYADMIN_CONFIG"
	chown root:www-data "$PHPMYADMIN_CONFIG"

	rm -f "$PHPMYADMIN_LINK"
	ln -s "$PHPMYADMIN_CONFIG" "$PHPMYADMIN_LINK"

	php -l "$PHPMYADMIN_CONFIG" > /dev/null \
		|| fail 'Generated phpMyAdmin configuration is invalid.'

	if port_is_in_use "$PHPMYADMIN_PORT"; then
		fail "TCP port $PHPMYADMIN_PORT is already in use. Uninstall the partial deployment before retrying."
	fi

	info "Creating phpMyAdmin loopback service on $PHPMYADMIN_ADDRESS:$PHPMYADMIN_PORT"
	write_phpmyadmin_service
	systemctl daemon-reload
	systemctl enable "$PHPMYADMIN_SERVICE" > /dev/null 2>&1
	systemctl restart "$PHPMYADMIN_SERVICE"

	listener_attempt=0
	while ! port_is_in_use "$PHPMYADMIN_PORT"; do
		listener_attempt=$((listener_attempt + 1))

		if systemctl is-failed --quiet "$PHPMYADMIN_SERVICE" \
			|| [ "$listener_attempt" -ge 15 ]; then
			systemctl status "$PHPMYADMIN_SERVICE" --no-pager >&2 || true
			fail "phpMyAdmin did not start listening on $PHPMYADMIN_ADDRESS:$PHPMYADMIN_PORT."
		fi

		sleep 1
	done

	HEALTH_FILE="$STATE_DIR/phpmyadmin-health.html"
	if ! curl -fsS \
		--retry 10 \
		--retry-connrefused \
		--retry-delay 1 \
		--connect-timeout 3 \
		--max-time 15 \
		-o "$HEALTH_FILE" \
		"http://$PHPMYADMIN_ADDRESS:$PHPMYADMIN_PORT/"; then
		systemctl status "$PHPMYADMIN_SERVICE" --no-pager >&2 || true
		fail "phpMyAdmin did not respond on $PHPMYADMIN_ADDRESS:$PHPMYADMIN_PORT."
	fi

	if ! grep -i 'phpMyAdmin' "$HEALTH_FILE" > /dev/null 2>&1; then
		fail "The service on $PHPMYADMIN_ADDRESS:$PHPMYADMIN_PORT did not return a phpMyAdmin page."
	fi
	rm -f "$HEALTH_FILE"

	app=$(select_new_database_name)
	user=$(select_new_database_user)
	DB_PASSWORD=$(generate_password)
	[ "${#DB_PASSWORD}" -eq 20 ] || fail 'Unable to generate a strong database password.'

	# Record ownership before SQL execution so a partial deployment remains removable.
	printf '%s\n' "$app" > "$DATABASE_FILE"
	printf '%s\n' "$user" > "$USER_FILE"

	info "Creating database '$app' and user '$user'"
	mariadb --protocol=socket <<- DATABASE
		CREATE DATABASE \`$app\`
		    CHARACTER SET utf8mb4
		    COLLATE utf8mb4_unicode_ci;
		CREATE USER '$user'@'127.0.0.1' IDENTIFIED BY '$DB_PASSWORD';
		CREATE USER '$user'@'localhost' IDENTIFIED BY '$DB_PASSWORD';
		GRANT ALL PRIVILEGES ON \`$app\`.* TO '$user'@'127.0.0.1';
		GRANT ALL PRIVILEGES ON \`$app\`.* TO '$user'@'localhost';
		FLUSH PRIVILEGES;
	DATABASE

	MYSQL_PWD=$DB_PASSWORD mariadb --protocol=TCP \
		--host=127.0.0.1 \
		--port=$MARIADB_PORT \
		--user="$user" \
		--database="$app" \
		--execute='SELECT 1;' > /dev/null 2>&1 \
		|| fail 'The generated database credentials failed verification.'

	printf '%s\n' 'installed' > "$STATUS_FILE"
	ENCODED_PASSWORD=$(urlencode "$DB_PASSWORD")

	info 'Installation completed successfully.'
	info "phpMyAdmin is listening only at http://$PHPMYADMIN_ADDRESS:$PHPMYADMIN_PORT/"
	printf 'phpMyAdmin username: %s\n' "$user"
	printf '\nDatabase URL: mysql://%s:%s@127.0.0.1:%s/%s\n' "$user" "$DB_PASSWORD" "$MARIADB_PORT" "$app"
	printf 'Raw password: %s\n' "$ENCODED_PASSWORD"
}

read_managed_name() {
	file=$1
	label=$2

	if [ -f "$file" ]; then
		managed_value=$(cat "$file")
		case $managed_value in
			'' | *[!A-Za-z0-9_]*)
				fail "Invalid $label recorded in $file"
				;;
		esac
		printf '%s\n' "$managed_value"
		return 0
	fi

	return 1
}

uninstall() {
	ensure_root_and_platform

	if [ ! -d "$STATE_DIR" ]; then
		fail "No managed installation state was found at $STATE_DIR. Nothing was removed."
	fi

	managed_database=''
	managed_user=''
	remove_database=no
	remove_user=no
	purge_packages=no

	if managed_database=$(read_managed_name "$DATABASE_FILE" 'database name'); then
		if prompt_yes_no "Remove database '$managed_database' and all of its data?" no; then
			remove_database=yes
		fi
	else
		warn 'No managed database name is recorded; no database will be dropped.'
	fi

	if managed_user=$(read_managed_name "$USER_FILE" 'database username'); then
		if prompt_yes_no "Remove MariaDB user '$managed_user'@'127.0.0.1'?" no; then
			remove_user=yes
		fi
	else
		warn 'No managed database username is recorded; no database user will be dropped.'
	fi

	if [ -s "$PACKAGE_FILE" ]; then
		if prompt_yes_no 'Purge packages installed by this script?' yes; then
			purge_packages=yes
		fi
	fi

	if [ "$remove_database" = yes ] || [ "$remove_user" = yes ]; then
		require_command mariadb

		systemctl start mariadb > /dev/null 2>&1 \
			|| fail 'MariaDB could not be started for database/user removal.'
		mariadb-admin --protocol=socket ping > /dev/null 2>&1 \
			|| fail 'MariaDB is not ready for database/user removal.'

		if [ "$remove_database" = yes ]; then
			info "Dropping database '$managed_database'"
			mariadb --protocol=socket \
				-e "DROP DATABASE IF EXISTS \`$managed_database\`;"
		else
			info "Keeping database '$managed_database'"
		fi

		if [ "$remove_user" = yes ]; then
			info "Dropping loopback users for '$managed_user'"
			mariadb --protocol=socket \
				-e "DROP USER IF EXISTS '$managed_user'@'127.0.0.1', '$managed_user'@'localhost'; FLUSH PRIVILEGES;"
		else
			info "Keeping loopback users for '$managed_user'"
		fi
	fi

	info 'Stopping the managed phpMyAdmin service'
	systemctl disable "$PHPMYADMIN_SERVICE" > /dev/null 2>&1 || true
	systemctl stop "$PHPMYADMIN_SERVICE" > /dev/null 2>&1 || true

	info 'Removing managed configuration and restoring previous files'
	restore_path "$PHPMYADMIN_LINK" 'phpmyadmin-link'
	restore_path "$PHPMYADMIN_CONFIG" 'phpmyadmin-config'
	restore_path "$PHPMYADMIN_SERVICE_FILE" 'phpmyadmin-service-file'
	restore_path "$MARIADB_BIND_CONFIG" 'mariadb-bind-config'
	restore_path "$REPOSITORY_LIST" 'repository-list'
	restore_path "$REPOSITORY_KEY" 'repository-key'
	restore_phpmyadmin_debconf

	systemctl daemon-reload

	if [ "$purge_packages" = yes ]; then
		packages_to_purge=''

		while IFS= read -r package; do
			[ -n "$package" ] || continue
			if is_package_installed "$package"; then
				packages_to_purge="$packages_to_purge $package"
			fi
		done < "$PACKAGE_FILE"

		if [ -n "$packages_to_purge" ]; then
			info 'Purging packages installed by this script'
			# Package names originate only from write_package_ownership().
			# shellcheck disable=SC2086
			apt-get purge -y --auto-remove $packages_to_purge
		fi
	fi

	restore_service_state 'mariadb' 'mariadb-service'
	restore_service_state "$PHPMYADMIN_SERVICE" 'phpmyadmin-service'

	info 'Refreshing APT metadata after repository removal'
	apt-get update

	rm -rf "$STATE_DIR"

	info 'Uninstallation completed successfully.'
	if [ "$remove_database" != yes ] && [ -n "$managed_database" ]; then
		info "Database retained: $managed_database"
	fi
	if [ "$remove_user" != yes ] && [ -n "$managed_user" ]; then
		info "Database loopback users retained: $managed_user"
	fi
}

usage() {
	cat <<- USAGE
		Usage: $0 [deploy|install|uninstall|remove]

		  deploy, install    Install and configure MariaDB $MARIADB_RELEASE and
		                     phpMyAdmin on $PHPMYADMIN_ADDRESS:$PHPMYADMIN_PORT.
		  uninstall, remove  Restore previous settings and optionally remove the
		                     managed database, user, and packages.

		With no argument, the script asks which operation to perform.
	USAGE
}

select_action() {
	printf '%s\n' 'Choose an operation:' >&2
	printf '%s\n' '  1) Deploy/install' >&2
	printf '%s\n' '  2) Uninstall/remove' >&2
	printf '%s' 'Selection [1/2]: ' >&2
	IFS= read -r selection || fail 'Input ended unexpectedly.'

	case $selection in
		1) printf '%s\n' 'deploy' ;;
		2) printf '%s\n' 'uninstall' ;;
		*) fail 'Invalid selection.' ;;
	esac
}

ACTION=${1:-}
if [ -z "$ACTION" ]; then
	ACTION=$(select_action)
fi

case $ACTION in
	deploy | install | --deploy | --install)
		prompt_installation_ports
		deploy
		;;
	uninstall | remove | --uninstall | --remove)
		uninstall
		;;
	-h | --help | help)
		usage
		;;
	*)
		usage >&2
		exit 2
		;;
esac
