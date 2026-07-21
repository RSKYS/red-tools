#!/bin/sh

# nginx-manager.sh
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

set -u

umask 022

log_info() {
	printf '\033[34m\033[1m[INFO]\033[0m %s\n' "$*"
}

log_success() {
	printf '\033[32m\033[1m[SUCCESS]\033[0m %s\n' "$*"
}

log_warn() {
	printf '\033[33m\033[1m[WARNING]\033[0m %s\n' "$*" >&2
}

log_error() {
	printf '\033[31m\033[1m[ERROR]\033[0m %s\n' "$*" >&2
}

cleanup_temp() {
	if [ -n "${CURRENT_TMP:-}" ] && [ -e "$CURRENT_TMP" ]; then
		rm -f "$CURRENT_TMP" 2> /dev/null || :
	fi
	unset CURRENT_TMP
}

cleanup_validation_dir() {
	if [ -n "${VALIDATION_DIR:-}" ] && [ -d "$VALIDATION_DIR" ]; then
		rm -rf "$VALIDATION_DIR" 2> /dev/null || :
	fi
	unset VALIDATION_DIR
}

fatal() {
	cleanup_temp
	cleanup_validation_dir
	log_error "$*"

	if [ "${TRANSACTION_ACTIVE:-0}" -eq 1 ] \
		&& [ "${ROLLBACK_DONE:-0}" -eq 0 ]; then
		rollback_transaction
	fi

	exit 1
}

run_or_die() {
	rod_description=$1
	shift

	log_info "$rod_description"
	if ! "$@"; then
		fatal "Command failed: $rod_description"
	fi

	unset rod_description
}

confirm() {
	cf_prompt=$1
	cf_default=${2:-no}

	while :; do
		if [ "$cf_default" = yes ]; then
			printf '%s [Y/n]: ' "$cf_prompt"
		else
			printf '%s [y/N]: ' "$cf_prompt"
		fi

		if ! IFS= read -r cf_answer; then
			printf '\n'
			unset cf_prompt cf_default cf_answer
			return 1
		fi

		case $cf_answer in
			'')
				if [ "$cf_default" = yes ]; then
					unset cf_prompt cf_default cf_answer
					return 0
				fi
				unset cf_prompt cf_default cf_answer
				return 1
				;;
			y | Y | yes | YES | Yes)
				unset cf_prompt cf_default cf_answer
				return 0
				;;
			n | N | no | NO | No)
				unset cf_prompt cf_default cf_answer
				return 1
				;;
			*)
				log_warn "Please answer yes or no."
				;;
		esac
	done
}

prompt_ipv6_support() {
	if confirm "Enable IPv6 support?" no; then
		IPV6_ENABLED=1
		IPV6_LISTEN_PREFIX=
	else
		IPV6_ENABLED=0
		IPV6_LISTEN_PREFIX='# '
	fi
}

require_root() {
	if [ "$(id -u)" -ne 0 ]; then
		log_error "This script must be run as root. Re-run it with sudo or from a root shell."
		exit 1
	fi
}

command_exists() {
	command -v "$1" > /dev/null 2>&1
}

reset_internal_state() {
	unset CURRENT_TMP VALIDATION_DIR TRANSACTION_ACTIVE ROLLBACK_DONE BACKUP_DIR MANIFEST \
		PORT_CHECKER MODE DOMAIN CERT_FILE KEY_FILE HTTP_PORT HTTPS_PORT \
		TLS_INTERNAL_PORT BACKEND_PORT TLS_PASSTHROUGH SITE_CONFIG STREAM_CONFIG \
		HTTP_REDIRECT_CONFIG REDIRECT_SNIPPET LOCATION_FRAGMENT UPSTREAM_FRAGMENT \
		WS_MAP_VAR PROMPTED_FILE PROMPTED_PORT SELECTED_FREE_PORT IPV6_ENABLED \
		IPV6_LISTEN_PREFIX STATE_FILE MANAGED_EXISTING MANAGED_INSTALLED_AT \
		MANAGED_LAST_CONFIGURED_AT MANAGED_NODE_ACTION MANAGED_NODE_DESCRIPTION \
		MANAGED_NEW_PORT
}

initialize_runtime_paths() {
	NGINX_CONF_DIR=${NGINX_CONF_DIR:-/etc/nginx}
	NGINX_MAIN_CONF=${NGINX_MAIN_CONF:-"$NGINX_CONF_DIR/nginx.conf"}
	NGINX_BIN=${NGINX_BIN:-nginx}
	STATE_FILE=${NGINX_RPM_STATE_FILE:-"$NGINX_CONF_DIR/.nginx-manager.conf"}

	if ! validate_absolute_path "$NGINX_CONF_DIR"; then
		fatal "NGINX_CONF_DIR must be an absolute path without spaces or Nginx control characters."
	fi

	if ! validate_absolute_path "$NGINX_MAIN_CONF"; then
		fatal "NGINX_MAIN_CONF must be an absolute path without spaces or Nginx control characters."
	fi

	if ! validate_absolute_path "$STATE_FILE"; then
		fatal "NGINX_RPM_STATE_FILE must be an absolute path without spaces or Nginx control characters."
	fi
}

load_management_state() {
	[ -f "$STATE_FILE" ] || return 1
	[ -r "$STATE_FILE" ] || {
		log_warn "The management state file is unreadable: $STATE_FILE"
		return 1
	}

	lms_manager_id=
	lms_version=
	lms_installed_by_script=
	lms_configured_by_script=
	lms_ipv6=
	lms_conf_dir=
	lms_main_conf=
	lms_installed_at=
	lms_last_configured_at=

	while IFS='=' read -r lms_key lms_value; do
		case $lms_key in
			MANAGER_ID) lms_manager_id=$lms_value ;;
			STATE_VERSION) lms_version=$lms_value ;;
			INSTALLED_BY_SCRIPT) lms_installed_by_script=$lms_value ;;
			CONFIGURED_BY_SCRIPT) lms_configured_by_script=$lms_value ;;
			IPV6_ENABLED) lms_ipv6=$lms_value ;;
			NGINX_CONF_DIR) lms_conf_dir=$lms_value ;;
			NGINX_MAIN_CONF) lms_main_conf=$lms_value ;;
			INSTALLED_AT) lms_installed_at=$lms_value ;;
			LAST_CONFIGURED_AT) lms_last_configured_at=$lms_value ;;
		esac
	done < "$STATE_FILE"

	if [ "$lms_manager_id" != nginx-manager ] \
		|| [ "$lms_version" != 1 ] \
		|| [ "$lms_installed_by_script" != 1 ] \
		|| [ "$lms_configured_by_script" != 1 ] \
		|| [ "$lms_conf_dir" != "$NGINX_CONF_DIR" ] \
		|| [ "$lms_main_conf" != "$NGINX_MAIN_CONF" ]; then
		log_warn "Ignoring invalid or incompatible management state: $STATE_FILE"
		unset lms_manager_id lms_version lms_installed_by_script \
			lms_configured_by_script lms_ipv6 \
			lms_conf_dir lms_main_conf lms_installed_at \
			lms_last_configured_at lms_key lms_value
		return 1
	fi

	case $lms_ipv6 in
		1)
			IPV6_ENABLED=1
			IPV6_LISTEN_PREFIX=
			;;
		0)
			IPV6_ENABLED=0
			IPV6_LISTEN_PREFIX='# '
			;;
		*)
			log_warn "Ignoring management state with an invalid IPv6 setting: $STATE_FILE"
			unset lms_manager_id lms_version lms_installed_by_script \
				lms_configured_by_script lms_ipv6 \
				lms_conf_dir lms_main_conf lms_installed_at \
				lms_last_configured_at lms_key lms_value
			return 1
			;;
	esac

	MANAGED_EXISTING=1
	MANAGED_INSTALLED_AT=$lms_installed_at
	MANAGED_LAST_CONFIGURED_AT=$lms_last_configured_at

	unset lms_manager_id lms_version lms_installed_by_script \
		lms_configured_by_script lms_ipv6 \
		lms_conf_dir lms_main_conf lms_installed_at \
		lms_last_configured_at lms_key lms_value
	return 0
}

write_management_state() {
	if [ -z "${MANAGED_INSTALLED_AT:-}" ]; then
		MANAGED_INSTALLED_AT=$(date -u '+%Y-%m-%dT%H:%M:%SZ')
	fi
	MANAGED_LAST_CONFIGURED_AT=$(date -u '+%Y-%m-%dT%H:%M:%SZ')

	make_temp_for "$STATE_FILE"

	cat > "$CURRENT_TMP" <<- MANAGEMENT_STATE
		MANAGER_ID=nginx-manager
		STATE_VERSION=1
		INSTALLED_BY_SCRIPT=1
		CONFIGURED_BY_SCRIPT=1
		IPV6_ENABLED=$IPV6_ENABLED
		NGINX_CONF_DIR=$NGINX_CONF_DIR
		NGINX_MAIN_CONF=$NGINX_MAIN_CONF
		INSTALLED_AT=$MANAGED_INSTALLED_AT
		LAST_CONFIGURED_AT=$MANAGED_LAST_CONFIGURED_AT
	MANAGEMENT_STATE

	chmod 0600 "$CURRENT_TMP" \
		|| fatal "Could not secure the management state temporary file."

	if [ -e "$STATE_FILE" ] || [ -L "$STATE_FILE" ]; then
		wms_backup=$(backup_name_for "$STATE_FILE")
		cp -p -P "$STATE_FILE" "$wms_backup" \
			|| fatal "Could not back up the existing management state."
		record_change O "$STATE_FILE" "$wms_backup"
		unset wms_backup
	else
		record_change N "$STATE_FILE" ""
	fi

	mv -f "$CURRENT_TMP" "$STATE_FILE" \
		|| fatal "Could not atomically install the management state file."
	unset CURRENT_TMP
	log_success "Updated management state: $STATE_FILE"
}

select_port_checker() {
	if command_exists ss; then
		PORT_CHECKER=ss
	elif command_exists netstat; then
		PORT_CHECKER=netstat
	else
		fatal "Neither ss nor netstat is available for checking listener ports."
	fi
}

validate_existing_managed_runtime() {
	command_exists "$NGINX_BIN" \
		|| fatal "The managed Nginx executable is unavailable: $NGINX_BIN"
	[ -f "$NGINX_MAIN_CONF" ] \
		|| fatal "The managed Nginx configuration is missing: $NGINX_MAIN_CONF"
	command_exists cksum \
		|| fatal "The POSIX cksum utility is required but was not found."
	select_port_checker
}

ensure_support_files() {
	ems_snippet="$NGINX_CONF_DIR/snippets/redirect_443.forcessl.conf"
	if [ ! -f "$ems_snippet" ]; then
		log_warn "The managed HTTPS redirect snippet is missing; recreating it."
		write_redirect_snippet
	fi
	unset ems_snippet
}

managed_conf_files() {
	for mcf_file in \
		"$NGINX_CONF_DIR/conf.d"/redirect_*.conf \
		"$NGINX_CONF_DIR/conf.d"/reverse_*.conf \
		"$NGINX_CONF_DIR/conf.d"/proxy_*.conf \
		"$NGINX_CONF_DIR/stream-conf.d"/redirect_*.conf; do
		[ -f "$mcf_file" ] || continue
		if grep '^[[:space:]]*# Managed' "$mcf_file" > /dev/null 2>&1; then
			printf '%s\n' "$mcf_file"
		fi
	done
	unset mcf_file
}

port_owned_by_nginx() {
	mpobn_port=$1

	case ${PORT_CHECKER:-} in
		ss)
			ss -ltnp 2> /dev/null | awk -v wanted="$mpobn_port" '
				NR > 1 {
					address = $4
					sub(/^.*:/, "", address)
					if (address == wanted && $0 ~ /nginx/) {
						found = 1
					}
				}
				END { exit(found ? 0 : 1) }
			'
			mpobn_status=$?
			;;
		netstat)
			netstat -ltnp 2> /dev/null | awk -v wanted="$mpobn_port" '
				NR > 2 {
					address = $4
					sub(/^.*:/, "", address)
					if (address == wanted && $0 ~ /nginx/) {
						found = 1
					}
				}
				END { exit(found ? 0 : 1) }
			'
			mpobn_status=$?
			;;
		*)
			mpobn_status=1
			;;
	esac

	unset mpobn_port
	if [ "$mpobn_status" -eq 0 ]; then
		unset mpobn_status
		return 0
	fi
	unset mpobn_status
	return 1
}

managed_listener_status() {
	mls_spec=$1
	mls_endpoint=${mls_spec%% *}

	case $mls_endpoint in
		\[*\]:*) mls_port=${mls_endpoint##*:} ;;
		*:*) mls_port=${mls_endpoint##*:} ;;
		*) mls_port=$mls_endpoint ;;
	esac

	case $mls_port in
		'' | *[!0-9]*)
			printf '%s\n' CONFIGURED
			;;
		*)
			if port_owned_by_nginx "$mls_port"; then
				printf '%s\n' ACTIVE
			elif port_in_use "$mls_port"; then
				printf '%s\n' OTHER
			else
				printf '%s\n' INACTIVE
			fi
			;;
	esac

	unset mls_spec mls_endpoint mls_port
}

build_listener_inventory() {
	bmli_output=$1
	: > "$bmli_output" || fatal "Could not create the managed-listener inventory."

	if ! managed_conf_files | while IFS= read -r bmli_file; do
		awk -v source="$bmli_file" '
			function trim(value) {
				sub(/^[[:space:]]+/, "", value)
				sub(/[[:space:]]+$/, "", value)
				return value
			}
			BEGIN {
				in_server = 0
				depth = 0
				listen_count = 0
				server_name = ""
			}
			{
				line = $0
				if (!in_server && line ~ /^[[:space:]]*server[[:space:]]*\{/) {
					in_server = 1
					depth = 0
					listen_count = 0
					server_name = ""
				}

				if (in_server) {
					if (line ~ /^[[:space:]]*listen[[:space:]]+/) {
						value = line
						sub(/^[[:space:]]*listen[[:space:]]+/, "", value)
						sub(/;[[:space:]]*$/, "", value)
						listens[++listen_count] = trim(value)
					}

					if (line ~ /^[[:space:]]*server_name[[:space:]]+/) {
						value = line
						sub(/^[[:space:]]*server_name[[:space:]]+/, "", value)
						sub(/;[[:space:]]*$/, "", value)
						server_name = trim(value)
					}

					opening = line
					closing = line
					open_count = gsub(/\{/, "", opening)
					close_count = gsub(/\}/, "", closing)
					depth += open_count - close_count

					if (depth == 0) {
						if (server_name == "") {
							server_name = "stream/SNI gateway"
						}
						for (listener_index = 1; listener_index <= listen_count; listener_index++) {
							print source "|" listens[listener_index] "|" server_name
							delete listens[listener_index]
						}
						in_server = 0
					}
				}
			}
		' "$bmli_file" >> "$bmli_output" || exit 1
	done; then
		fatal "Could not parse the managed-listener inventory."
	fi

	unset bmli_output bmli_file
}

build_upstream_inventory() {
	bmui_output=$1
	: > "$bmui_output" || fatal "Could not create the managed-upstream inventory."

	if ! managed_conf_files | while IFS= read -r bmui_file; do
		awk -v source="$bmui_file" '
			BEGIN {
				in_upstream = 0
				depth = 0
				upstream_name = ""
			}
			{
				line = $0
				if (!in_upstream && line ~ /^[[:space:]]*upstream[[:space:]]+[A-Za-z0-9_]+[[:space:]]*\{/) {
					value = line
					sub(/^[[:space:]]*upstream[[:space:]]+/, "", value)
					sub(/[[:space:]]*\{.*$/, "", value)
					upstream_name = value
					in_upstream = 1
					depth = 0
				}

				if (in_upstream) {
					if (line ~ /^[[:space:]]*server[[:space:]]+/) {
						value = line
						sub(/^[[:space:]]*server[[:space:]]+/, "", value)
						sub(/;[[:space:]]*$/, "", value)
						print source "|" upstream_name "|" value
					}

					opening = line
					closing = line
					open_count = gsub(/\{/, "", opening)
					close_count = gsub(/\}/, "", closing)
					depth += open_count - close_count
					if (depth == 0) {
						in_upstream = 0
					}
				}
			}
		' "$bmui_file" >> "$bmui_output" || exit 1
	done; then
		fatal "Could not parse the managed-upstream inventory."
	fi

	unset bmui_output bmui_file
}

display_inventory() {
	dmi_listeners="$BACKUP_DIR/managed-listeners.txt"
	dmi_upstreams="$BACKUP_DIR/managed-upstreams.txt"
	build_listener_inventory "$dmi_listeners"
	build_upstream_inventory "$dmi_upstreams"

	printf '\n\033[1mScript-managed Nginx listeners and servers\033[0m\n'

	if [ -s "$dmi_listeners" ]; then
		printf '%-9s %-34s %-30s %s\n' STATUS LISTEN SERVER CONFIGURATION
		printf '%-9s %-34s %-30s %s\n' '---------' '----------------------------------' '------------------------------' '-------------'
		while IFS='|' read -r dmi_file dmi_listen dmi_server; do
			dmi_status=$(managed_listener_status "$dmi_listen")
			printf '%-9s %-34s %-30s %s\n' \
				"$dmi_status" "$dmi_listen" "$dmi_server" "$dmi_file"
		done < "$dmi_listeners"
		unset dmi_file dmi_listen dmi_server dmi_status
	else
		printf 'No active script-managed listener configurations were found.\n'
	fi

	if [ -s "$dmi_upstreams" ]; then
		printf '\n%-34s %-38s %s\n' UPSTREAM SERVER CONFIGURATION
		printf '%-34s %-38s %s\n' '----------------------------------' '--------------------------------------' '-------------'
		while IFS='|' read -r dmi_file dmi_upstream dmi_target; do
			printf '%-34s %-38s %s\n' \
				"$dmi_upstream" "$dmi_target" "$dmi_file"
		done < "$dmi_upstreams"
		unset dmi_file dmi_upstream dmi_target
	fi

	printf '\n'
	unset dmi_listeners dmi_upstreams
}

make_temp_for() {
	ct_target=$1
	ct_dir=$(dirname "$ct_target")
	ct_base=$(basename "$ct_target")
	ct_attempt=0

	cleanup_temp

	while [ "$ct_attempt" -lt 100 ]; do
		ct_candidate="${ct_dir}/.${ct_base}.tmp.$$.$ct_attempt"
		if (
			umask 077
			set -C
			: > "$ct_candidate"
		) 2> /dev/null; then
			CURRENT_TMP=$ct_candidate
			unset ct_target ct_dir ct_base ct_attempt ct_candidate
			return 0
		fi
		ct_attempt=$((ct_attempt + 1))
	done

	unset ct_dir ct_base ct_attempt ct_candidate
	fatal "Could not create a safe temporary file beside $ct_target."
}

record_change() {
	rc_type=$1
	rc_target=$2
	rc_backup=${3:-}

	printf '%s|%s|%s\n' "$rc_type" "$rc_target" "$rc_backup" >> "$MANIFEST" \
		|| fatal "Could not update the transaction manifest."

	unset rc_type rc_target rc_backup
}

backup_name_for() {
	bnf_target=$1
	bnf_stripped=$(printf '%s' "$bnf_target" | sed 's#^/##; s#/#__#g')
	printf '%s/%s\n' "$BACKUP_DIR" "$bnf_stripped"
	unset bnf_target bnf_stripped
}

is_redirect_conf() {
	irc_target=$1

	case $irc_target in
		"$NGINX_CONF_DIR"/conf.d/redirect_*.conf | \
			"$NGINX_CONF_DIR"/stream-conf.d/redirect_*.conf)
			unset irc_target
			return 0
			;;
	esac

	unset irc_target
	return 1
}

is_script_managed_file() {
	ismf_target=$1

	if [ -f "$ismf_target" ] \
		&& grep '^[[:space:]]*# Managed' "$ismf_target" > /dev/null 2>&1; then
		unset ismf_target
		return 0
	fi

	unset ismf_target
	return 1
}

backup_existing_file() {
	bef_target=$1
	bef_backup=$(backup_name_for "$bef_target")

	if ! cp -p -P "$bef_target" "$bef_backup"; then
		unset bef_target bef_backup
		return 1
	fi

	record_change O "$bef_target" "$bef_backup"
	log_success "Rollback backup created: $bef_backup"
	unset bef_target bef_backup
	return 0
}

prepare_for_replace() {
	ptr_target=$1

	if [ -e "$ptr_target" ] || [ -L "$ptr_target" ]; then
		if is_redirect_conf "$ptr_target" \
			&& is_script_managed_file "$ptr_target"; then
			log_warn "Automatically replacing script-managed redirect configuration: $ptr_target"
			log_info "No overwrite confirmation is required; a transactional rollback backup will be created automatically."
			backup_existing_file "$ptr_target" \
				|| fatal "Could not create the automatic rollback backup for $ptr_target."
			unset ptr_target
			return 0
		fi

		log_warn "The existing file is not recognized as an automatically replaceable script-managed redirect configuration: $ptr_target"
		log_warn "Overwriting it may remove manually maintained directives or configuration blocks."
		if ! confirm "Overwrite this file?" no; then
			fatal "Stopped without overwriting $ptr_target."
		fi

		if confirm "Create a rollback backup first?" yes; then
			backup_existing_file "$ptr_target" \
				|| fatal "Could not create the rollback backup for $ptr_target."
		else
			log_warn "No rollback backup will be available for $ptr_target."
			if ! confirm "Continue without a backup?" no; then
				fatal "Stopped before overwriting $ptr_target."
			fi
			record_change X "$ptr_target" ""
		fi
	else
		record_change N "$ptr_target" ""
		log_info "Creating new managed configuration file: $ptr_target"
	fi

	unset ptr_target
}

commit_temp_file() {
	ctf_target=$1
	ctf_mode=$2

	[ -n "${CURRENT_TMP:-}" ] \
		|| fatal "Internal error: no temporary file is ready for $ctf_target."

	if ! chmod "$ctf_mode" "$CURRENT_TMP"; then
		fatal "Could not set permissions on the temporary file for $ctf_target."
	fi

	prepare_for_replace "$ctf_target"

	if ! mv -f "$CURRENT_TMP" "$ctf_target"; then
		fatal "Could not atomically install $ctf_target."
	fi

	unset CURRENT_TMP
	log_success "Installed: $ctf_target"
	unset ctf_target ctf_mode
}

make_validation_dir() {
	mvd_attempt=0
	cleanup_validation_dir

	while [ "$mvd_attempt" -lt 100 ]; do
		mvd_candidate="$BACKUP_DIR/nginx-validation.$$.$mvd_attempt"
		if (
			umask 077
			mkdir "$mvd_candidate"
		) 2> /dev/null; then
			VALIDATION_DIR=$mvd_candidate
			unset mvd_attempt mvd_candidate
			return 0
		fi
		mvd_attempt=$((mvd_attempt + 1))
	done

	unset mvd_attempt mvd_candidate
	return 1
}

copy_validation_tree() {
	cvt_item=
	cvt_base=

	for cvt_item in \
		"$NGINX_CONF_DIR"/* \
		"$NGINX_CONF_DIR"/.[!.]* \
		"$NGINX_CONF_DIR"/..?*; do
		if [ ! -e "$cvt_item" ] && [ ! -L "$cvt_item" ]; then
			continue
		fi

		cvt_base=$(basename "$cvt_item")
		case $cvt_base in
			backups)
				continue
				;;
		esac

		if [ -e "$VALIDATION_DIR/$cvt_base" ] \
			|| [ -L "$VALIDATION_DIR/$cvt_base" ]; then
			continue
		fi

		cp -R -P -p "$cvt_item" "$VALIDATION_DIR/" \
			|| {
				unset cvt_item cvt_base
				return 1
			}
	done

	unset cvt_item cvt_base
	return 0
}

validate_nginx_candidate() {
	vnc_target=$1
	vnc_candidate=$2

	case $vnc_target in
		"$NGINX_CONF_DIR"/*)
			vnc_relative=${vnc_target#"$NGINX_CONF_DIR"/}
			;;
		*)
			log_error "Cannot validate a generated Nginx file outside $NGINX_CONF_DIR: $vnc_target"
			unset vnc_target vnc_candidate
			return 1
			;;
	esac

	if ! make_validation_dir; then
		log_error "Could not create an isolated Nginx validation directory."
		unset vnc_target vnc_candidate vnc_relative
		return 1
	fi

	log_info "Building an isolated validation copy for $vnc_target"
	if ! copy_validation_tree; then
		log_error "Could not copy the current Nginx configuration into the validation directory."
		cleanup_validation_dir
		unset vnc_target vnc_candidate vnc_relative
		return 1
	fi

	vnc_shadow_target="$VALIDATION_DIR/$vnc_relative"
	vnc_shadow_dir=$(dirname "$vnc_shadow_target")
	mkdir -p "$vnc_shadow_dir" \
		|| {
			log_error "Could not prepare the candidate validation path: $vnc_shadow_dir"
			cleanup_validation_dir
			unset vnc_target vnc_candidate vnc_relative \
				vnc_shadow_target vnc_shadow_dir
			return 1
		}

	rm -f "$vnc_shadow_target" \
		|| {
			log_error "Could not replace the shadow copy of the validation target."
			cleanup_validation_dir
			unset vnc_target vnc_candidate vnc_relative \
				vnc_shadow_target vnc_shadow_dir
			return 1
		}

	cp -p "$vnc_candidate" "$vnc_shadow_target" \
		|| {
			log_error "Could not copy the generated candidate into the validation tree."
			cleanup_validation_dir
			unset vnc_target vnc_candidate vnc_relative \
				vnc_shadow_target vnc_shadow_dir
			return 1
		}

	vnc_test_main="$VALIDATION_DIR/nginx-manager-validation.conf"
	if ! awk -v source="$NGINX_CONF_DIR" -v replacement="$VALIDATION_DIR" '
		{
			line = $0
			result = ""
			while ((position = index(line, source)) != 0) {
				result = result substr(line, 1, position - 1) replacement
				line = substr(line, position + length(source))
			}
			print result line
		}
	' "$NGINX_MAIN_CONF" > "$vnc_test_main"; then
		log_error "Could not generate the isolated validation entry point."
		cleanup_validation_dir
		unset vnc_target vnc_candidate vnc_relative \
			vnc_shadow_target vnc_shadow_dir vnc_test_main
		return 1
	fi

	vnc_output="$VALIDATION_DIR/nginx-test-output.txt"
	log_info "Running nginx -t against the generated candidate before installation..."
	if "$NGINX_BIN" -t -c "$vnc_test_main" > "$vnc_output" 2>&1; then
		cat "$vnc_output"
		log_success "Candidate validation passed: $vnc_target"
		cleanup_validation_dir
		unset vnc_target vnc_candidate vnc_relative \
			vnc_shadow_target vnc_shadow_dir vnc_test_main vnc_output
		return 0
	fi

	cat "$vnc_output" >&2
	log_error "Candidate validation failed; the live target was not modified: $vnc_target"
	cleanup_validation_dir
	unset vnc_target vnc_candidate vnc_relative \
		vnc_shadow_target vnc_shadow_dir vnc_test_main vnc_output
	return 1
}

commit_nginx_file() {
	cvnf_target=$1
	cvnf_mode=$2

	[ -n "${CURRENT_TMP:-}" ] \
		|| fatal "Internal error: no generated Nginx candidate is ready for $cvnf_target."

	if ! validate_nginx_candidate "$cvnf_target" "$CURRENT_TMP"; then
		cleanup_temp
		unset cvnf_target cvnf_mode
		return 1
	fi

	commit_temp_file "$cvnf_target" "$cvnf_mode"
	unset cvnf_target cvnf_mode
	return 0
}

rollback_transaction() {
	[ "${TRANSACTION_ACTIVE:-0}" -eq 1 ] || return 0
	[ "${ROLLBACK_DONE:-0}" -eq 0 ] || return 0

	ROLLBACK_DONE=1
	log_warn "Rolling back managed configuration changes..."

	if [ -f "${MANIFEST:-}" ]; then
		while IFS='|' read -r rb_type rb_target rb_backup; do
			case $rb_type in
				O)
					if [ -e "$rb_backup" ] || [ -L "$rb_backup" ]; then
						rm -f "$rb_target" 2> /dev/null || :
						if cp -p -P "$rb_backup" "$rb_target"; then
							log_warn "Restored: $rb_target"
						else
							log_error "Failed to restore: $rb_target"
						fi
					fi
					;;
				N)
					if rm -f "$rb_target" 2> /dev/null; then
						log_warn "Removed newly created file: $rb_target"
					else
						log_error "Failed to remove new file: $rb_target"
					fi
					;;
				M)
					if [ -e "$rb_backup" ] || [ -L "$rb_backup" ]; then
						rm -f "$rb_target" 2> /dev/null || :
						if mv "$rb_backup" "$rb_target"; then
							log_warn "Restored moved item: $rb_target"
						else
							log_error "Failed to restore moved item: $rb_target"
						fi
					fi
					;;
				X)
					log_error "Cannot automatically restore unbacked file: $rb_target"
					;;
			esac
		done < "$MANIFEST"
		unset rb_type rb_target rb_backup
	fi

	TRANSACTION_ACTIVE=0
}

handle_signal() {
	printf '\n' >&2
	log_error "Interrupted."
	cleanup_temp
	cleanup_validation_dir
	rollback_transaction
	exit 130
}

trap 'handle_signal' HUP INT TERM

init_transaction() {
	it_timestamp=$(date '+%Y%m%d-%H%M%S')
	BACKUP_DIR="$NGINX_CONF_DIR/backups/reverse-proxy-$it_timestamp"
	MANIFEST="$BACKUP_DIR/manifest.txt"
	unset it_timestamp

	run_or_die "Creating transaction backup directory" mkdir -p "$BACKUP_DIR"
	run_or_die "Securing transaction backup directory" chmod 0700 "$BACKUP_DIR"
	: > "$MANIFEST" || fatal "Could not create $MANIFEST."
	chmod 0600 "$MANIFEST" || fatal "Could not secure $MANIFEST."

	ROLLBACK_DONE=0
	TRANSACTION_ACTIVE=1
}

remove_default_site() {
	rds_default="$NGINX_CONF_DIR/sites-enabled/default"

	if [ ! -e "$rds_default" ] && [ ! -L "$rds_default" ]; then
		log_info "The default enabled site is already absent."
		unset rds_default
		return 0
	fi

	log_warn "Nginx's default enabled site exists: $rds_default"
	if ! confirm "Disable it by moving it into the transaction backup directory?" yes; then
		rm -f "$rds_default" 2> /dev/null || :
		unset rds_default
		return 0
	fi

	rds_backup="$BACKUP_DIR/sites-enabled-default"
	if ! mv "$rds_default" "$rds_backup"; then
		fatal "Could not disable the default Nginx site."
	fi

	record_change M "$rds_default" "$rds_backup"
	log_success "Default site disabled and preserved at $rds_backup"
	unset rds_default rds_backup
}

bootstrap_certificates() {
	ebc_directory="$NGINX_CONF_DIR/certs"
	ebc_certificate="$ebc_directory/cert.pem"
	ebc_private_key="$ebc_directory/cert.key"

	if [ -f "$ebc_certificate" ] && [ -f "$ebc_private_key" ]; then
		log_info "Preserving the existing bootstrap certificate pair in $ebc_directory."
		unset ebc_directory ebc_certificate ebc_private_key
		return 0
	fi

	if [ -e "$ebc_certificate" ] || [ -L "$ebc_certificate" ] \
		|| [ -e "$ebc_private_key" ] || [ -L "$ebc_private_key" ]; then
		fatal "The bootstrap certificate pair is incomplete; refusing to overwrite existing material in $ebc_directory."
	fi

	command_exists openssl \
		|| fatal "OpenSSL is required to generate the bootstrap certificate pair."

	ebc_workspace="$BACKUP_DIR/bootstrap-certificate"
	ebc_config="$ebc_workspace/openssl.cnf"
	ebc_staged_certificate="$ebc_workspace/cert.pem"
	ebc_staged_private_key="$ebc_workspace/cert.key"

	run_or_die "Creating the bootstrap-certificate workspace" mkdir -p "$ebc_workspace"
	run_or_die "Securing the bootstrap-certificate workspace" chmod 0700 "$ebc_workspace"

	cat > "$ebc_config" <<- BOOTSTRAP_CONFIG
		[req]
		prompt = no
		distinguished_name = distinguished_name
		x509_extensions = extensions

		[distinguished_name]
		CN = localhost

		[extensions]
		basicConstraints = critical,CA:FALSE
		keyUsage = critical,digitalSignature,keyEncipherment
		extendedKeyUsage = serverAuth
		subjectAltName = @subject_alt_names

		[subject_alt_names]
		DNS.1 = localhost
		IP.1 = 127.0.0.1
		IP.2 = ::1
	BOOTSTRAP_CONFIG

	chmod 0600 "$ebc_config" \
		|| fatal "Could not secure the bootstrap OpenSSL configuration."

	log_info "Generating a 10-year local bootstrap certificate in $ebc_directory"
	if ! (
		umask 077
		openssl req -x509 -nodes -days 3650 -sha256 \
			-newkey rsa:2048 -config "$ebc_config" \
			-keyout "$ebc_staged_private_key" \
			-out "$ebc_staged_certificate"
	); then
		fatal "Could not generate the bootstrap certificate pair."
	fi

	chmod 0600 "$ebc_staged_private_key" \
		|| fatal "Could not secure the generated bootstrap private key."
	chmod 0644 "$ebc_staged_certificate" \
		|| fatal "Could not set permissions on the generated bootstrap certificate."

	record_change N "$ebc_private_key" ""
	mv "$ebc_staged_private_key" "$ebc_private_key" \
		|| fatal "Could not install the generated bootstrap private key."

	record_change N "$ebc_certificate" ""
	mv "$ebc_staged_certificate" "$ebc_certificate" \
		|| fatal "Could not install the generated bootstrap certificate."

	log_success "Bootstrap certificate installed: $ebc_certificate"
	log_success "Bootstrap private key installed: $ebc_private_key"

	unset ebc_directory ebc_certificate ebc_private_key ebc_workspace \
		ebc_config ebc_staged_certificate ebc_staged_private_key
}

ensure_directories() {
	for ed_directory in \
		"$NGINX_CONF_DIR/conf.d" \
		"$NGINX_CONF_DIR/certs" \
		"$NGINX_CONF_DIR/snippets" \
		"$NGINX_CONF_DIR/stream-conf.d" \
		"$NGINX_CONF_DIR/backups"; do
		if [ ! -d "$ed_directory" ]; then
			run_or_die "Creating $ed_directory" mkdir -p "$ed_directory"
		fi
		run_or_die "Setting permissions on $ed_directory" chmod 0755 "$ed_directory"
	done
	unset ed_directory
}

install_packages() {
	command_exists apt \
		|| fatal "The apt command was not found. This script supports Debian/Ubuntu systems."

	run_or_die "Updating APT package indexes" apt update
	run_or_die "Installing Nginx, OpenSSL, and the stream module" \
		apt install -y nginx openssl libnginx-mod-stream

	if ! command_exists ss && ! command_exists netstat; then
		log_warn "Neither ss nor netstat is installed; installing iproute2 to provide ss."
		run_or_die "Installing iproute2" apt install -y iproute2
	fi

	select_port_checker

	command_exists cksum \
		|| fatal "The POSIX cksum utility is required but was not found."
}

write_forced_ssl_snippet() {
	wfss_https_port=$1
	wfss_target="$NGINX_CONF_DIR/snippets/redirect_${wfss_https_port}.forcessl.conf"
	make_temp_for "$wfss_target"

	if [ "$wfss_https_port" -eq 443 ]; then
		cat > "$CURRENT_TMP" <<- SNIPPET
			# Managed reusable HTTP-to-HTTPS redirect for public domain listeners.
			# Include only from an HTTP server block with a strict server_name.
			return 301 https://\$host\$request_uri;
		SNIPPET
	else
		cat > "$CURRENT_TMP" <<- SNIPPET
			# Managed reusable HTTP-to-HTTPS redirect for custom port $wfss_https_port.
			# Include only from an HTTP server block with a strict server_name.
			return 301 https://\$host:$wfss_https_port\$request_uri;
		SNIPPET
	fi

	commit_temp_file "$wfss_target" 0644
	REDIRECT_SNIPPET=$wfss_target
	unset wfss_https_port wfss_target
}

write_redirect_snippet() {
	write_forced_ssl_snippet 443
	unset REDIRECT_SNIPPET
}

write_redirect_80() {
	wbr_target="$NGINX_CONF_DIR/conf.d/redirect_80.conf"
	make_temp_for "$wbr_target"

	cat > "$CURRENT_TMP" <<- REDIRECT_80
		# Managed public HTTP catch-all.
		# Unknown Host headers are closed without returning content.
		server {
			listen 80 default_server;
			${IPV6_LISTEN_PREFIX}listen [::]:80 default_server;
			server_name _;

			server_tokens off;
			return 444;
		}
	REDIRECT_80

	commit_nginx_file "$wbr_target" 0644 \
		|| fatal "The generated public HTTP redirect configuration failed validation."
	unset wbr_target
}

validate_domain() {
	vd_candidate=$1

	[ -n "$vd_candidate" ] || {
		unset vd_candidate
		return 1
	}
	[ "${#vd_candidate}" -le 253 ] || {
		unset vd_candidate
		return 1
	}

	case $vd_candidate in
		*[!A-Za-z0-9.-]* | .* | *. | *..*)
			unset vd_candidate
			return 1
			;;
		*.*) ;;
		*)
			unset vd_candidate
			return 1
			;;
	esac

	vd_old_ifs=$IFS
	IFS='.'
	set -- $vd_candidate
	IFS=$vd_old_ifs

	for vd_label; do
		[ -n "$vd_label" ] || {
			unset vd_candidate vd_old_ifs vd_label
			return 1
		}
		[ "${#vd_label}" -le 63 ] || {
			unset vd_candidate vd_old_ifs vd_label
			return 1
		}
		case $vd_label in
			-* | *-)
				unset vd_candidate vd_old_ifs vd_label
				return 1
				;;
		esac
	done

	unset vd_candidate vd_old_ifs vd_label
	return 0
}

validate_hostname() {
	vh_candidate=$1

	[ -n "$vh_candidate" ] || {
		unset vh_candidate
		return 1
	}
	[ "${#vh_candidate}" -le 253 ] || {
		unset vh_candidate
		return 1
	}

	case $vh_candidate in
		*[!A-Za-z0-9.-]* | .* | *. | *..*)
			unset vh_candidate
			return 1
			;;
	esac

	vh_old_ifs=$IFS
	IFS='.'
	set -- $vh_candidate
	IFS=$vh_old_ifs

	for vh_label; do
		[ -n "$vh_label" ] || {
			unset vh_candidate vh_old_ifs vh_label
			return 1
		}
		[ "${#vh_label}" -le 63 ] || {
			unset vh_candidate vh_old_ifs vh_label
			return 1
		}
		case $vh_label in
			-* | *-)
				unset vh_candidate vh_old_ifs vh_label
				return 1
				;;
		esac
	done

	unset vh_candidate vh_old_ifs vh_label
	return 0
}

validate_upstream_hostname() {
	vuh_candidate=$1

	[ -n "$vuh_candidate" ] || {
		unset vuh_candidate
		return 1
	}
	[ "${#vuh_candidate}" -le 253 ] || {
		unset vuh_candidate
		return 1
	}

	case $vuh_candidate in
		*[!A-Za-z0-9._-]* | .* | *. | *..*)
			unset vuh_candidate
			return 1
			;;
	esac

	vuh_old_ifs=$IFS
	IFS='.'
	set -- $vuh_candidate
	IFS=$vuh_old_ifs

	for vuh_label; do
		[ -n "$vuh_label" ] || {
			unset vuh_candidate vuh_old_ifs vuh_label
			return 1
		}
		[ "${#vuh_label}" -le 63 ] || {
			unset vuh_candidate vuh_old_ifs vuh_label
			return 1
		}
		case $vuh_label in
			-* | *-)
				unset vuh_candidate vuh_old_ifs vuh_label
				return 1
				;;
		esac
	done

	unset vuh_candidate vuh_old_ifs vuh_label
	return 0
}

validate_ipv4() {
	vi_candidate=$1
	vi_old_ifs=$IFS
	IFS='.'
	set -- $vi_candidate
	IFS=$vi_old_ifs

	[ "$#" -eq 4 ] || {
		unset vi_candidate vi_old_ifs
		return 1
	}

	for vi_octet; do
		case $vi_octet in
			'' | *[!0-9]*)
				unset vi_candidate vi_old_ifs vi_octet
				return 1
				;;
		esac
		[ "$vi_octet" -le 255 ] 2> /dev/null || {
			unset vi_candidate vi_old_ifs vi_octet
			return 1
		}
	done

	unset vi_candidate vi_old_ifs vi_octet
	return 0
}

validate_ipv6() {
	v6_candidate=$1

	awk -v address="$v6_candidate" '
		function valid_ipv4(value, parts, count, i) {
			count = split(value, parts, ".")
			if (count != 4) {
				return 0
			}
			for (i = 1; i <= 4; i++) {
				if (parts[i] !~ /^[0-9]+$/ ||
					parts[i] + 0 < 0 || parts[i] + 0 > 255) {
					return 0
				}
			}
			return 1
		}
		BEGIN {
			if (address !~ /^[0-9A-Fa-f:.]+$/ || index(address, ":") == 0 ||
				index(address, ":::") != 0) {
				exit 1
			}

			copy = address
			compressed = 0
			while ((position = index(copy, "::")) != 0) {
				compressed++
				copy = substr(copy, position + 2)
			}
			if (compressed > 1) {
				exit 1
			}

			if (substr(address, 1, 1) == ":" &&
				substr(address, 1, 2) != "::") {
				exit 1
			}
			if (substr(address, length(address), 1) == ":" &&
				substr(address, length(address) - 1, 2) != "::") {
				exit 1
			}

			count = split(address, groups, ":")
			logical_groups = 0

			for (i = 1; i <= count; i++) {
				if (groups[i] == "") {
					continue
				}

				if (index(groups[i], ".") != 0) {
					if (i != count || !valid_ipv4(groups[i])) {
						exit 1
					}
					logical_groups += 2
					continue
				}

				if (groups[i] !~ /^[0-9A-Fa-f]{1,4}$/) {
					exit 1
				}
				logical_groups++
			}

			if (compressed == 1) {
				exit(logical_groups < 8 ? 0 : 1)
			}
			exit(logical_groups == 8 ? 0 : 1)
		}
	'
	v6_status=$?
	unset v6_candidate

	if [ "$v6_status" -eq 0 ]; then
		unset v6_status
		return 0
	fi

	unset v6_status
	return 1
}

validate_proxy_host() {
	vph_candidate=$1

	case $vph_candidate in
		*[!0-9.]*)
			if validate_upstream_hostname "$vph_candidate"; then
				unset vph_candidate
				return 0
			fi
			;;
		*)
			if validate_ipv4 "$vph_candidate"; then
				unset vph_candidate
				return 0
			fi
			;;
	esac

	unset vph_candidate
	return 1
}

prompt_domain() {
	pd_prompt=$1

	while :; do
		printf '%s: ' "$pd_prompt"
		if ! IFS= read -r pd_input; then
			fatal "Input ended before a domain was provided."
		fi

		pd_input=$(printf '%s' "$pd_input" \
			| tr 'ABCDEFGHIJKLMNOPQRSTUVWXYZ' 'abcdefghijklmnopqrstuvwxyz')

		if validate_domain "$pd_input"; then
			DOMAIN=$pd_input
			unset pd_prompt pd_input
			return 0
		fi

		log_warn "Invalid domain. Use a DNS name such as example.com: at least one dot, no spaces, labels containing only letters, digits, and hyphens, and no leading/trailing hyphens."
	done
}

validate_absolute_path() {
	vasp_candidate=$1

	case $vasp_candidate in
		/*) ;;
		*)
			unset vasp_candidate
			return 1
			;;
	esac

	case $vasp_candidate in
		*[!A-Za-z0-9_./+,:=@%-]* | *//* | */../* | */..)
			unset vasp_candidate
			return 1
			;;
	esac

	unset vasp_candidate
	return 0
}

prompt_existing_file() {
	pef_label=$1

	while :; do
		printf '%s: ' "$pef_label"
		if ! IFS= read -r pef_candidate; then
			fatal "Input ended before a file path was provided."
		fi

		if ! validate_absolute_path "$pef_candidate"; then
			log_warn "Enter an absolute path using safe path characters and no spaces."
			continue
		fi

		if [ ! -f "$pef_candidate" ]; then
			log_warn "File not found: $pef_candidate"
			continue
		fi

		if [ ! -r "$pef_candidate" ]; then
			log_warn "File is not readable: $pef_candidate"
			continue
		fi

		PROMPTED_FILE=$pef_candidate
		unset pef_label pef_candidate
		return 0
	done
}

select_certificate_paths() {
	scp_found=

	for scp_pair in \
		"$NGINX_CONF_DIR/../letsencrypt/live/$DOMAIN/fullchain.pem|$NGINX_CONF_DIR/../letsencrypt/live/$DOMAIN/privkey.pem" \
		"$NGINX_CONF_DIR/certs/$DOMAIN/fullchain.pem|$NGINX_CONF_DIR/certs/$DOMAIN/privkey.pem" \
		"$NGINX_CONF_DIR/certs/$DOMAIN.crt|$NGINX_CONF_DIR/certs/$DOMAIN.key" \
		"$NGINX_CONF_DIR/certs/$DOMAIN.cer|$NGINX_CONF_DIR/certs/$DOMAIN.key" \
		"/etc/certs/$DOMAIN/fullchain.pem|/etc/certs/$DOMAIN/privkey.pem" \
		"/etc/certs/$DOMAIN/$DOMAIN.crt|/etc/certs/$DOMAIN/$DOMAIN.key" \
		"/etc/certs/$DOMAIN/$DOMAIN.cer|/etc/certs/$DOMAIN/$DOMAIN.key" \
		"$NGINX_CONF_DIR/ssl/$DOMAIN.crt|$NGINX_CONF_DIR/ssl/$DOMAIN.key" \
		"$NGINX_CONF_DIR/ssl/$DOMAIN.cer|$NGINX_CONF_DIR/ssl/$DOMAIN.key" \
		"/etc/ssl/$DOMAIN/$DOMAIN.crt|/etc/ssl/$DOMAIN/$DOMAIN.key"; do
		scp_cert=${scp_pair%%|*}
		scp_key=${scp_pair#*|}

		if [ -f "$scp_cert" ] && [ -r "$scp_cert" ] \
			&& [ -f "$scp_key" ] && [ -r "$scp_key" ]; then
			scp_found=$scp_pair
			break
		fi
	done

	if [ -n "$scp_found" ]; then
		scp_cert=${scp_found%%|*}
		scp_key=${scp_found#*|}
		log_success "Detected certificate: $scp_cert"
		log_success "Detected private key: $scp_key"

		if confirm "Use these detected certificate files?" yes; then
			CERT_FILE=$scp_cert
			KEY_FILE=$scp_key
			unset scp_found scp_pair scp_cert scp_key
			return 0
		fi
	else
		log_warn "No complete certificate/key pair was auto-detected for $DOMAIN."
	fi

	prompt_existing_file "Enter the full SSL certificate/full-chain path"
	CERT_FILE=$PROMPTED_FILE
	unset PROMPTED_FILE

	prompt_existing_file "Enter the full SSL private-key path"
	KEY_FILE=$PROMPTED_FILE
	unset PROMPTED_FILE

	[ "$CERT_FILE" != "$KEY_FILE" ] \
		|| fatal "The certificate and private key paths must be different files."

	unset scp_found scp_pair scp_cert scp_key
}

validate_certificate_pair() {
	if ! command_exists openssl; then
		log_warn "OpenSSL is unavailable; certificate structure and key matching will be checked later by nginx -t."
		return 0
	fi

	if ! openssl x509 -in "$CERT_FILE" -noout > /dev/null 2>&1; then
		fatal "The selected certificate is not a readable X.509 certificate: $CERT_FILE"
	fi

	vcp_cert_public="$BACKUP_DIR/certificate-public-key.pem"
	vcp_key_public="$BACKUP_DIR/private-key-public-key.pem"

	if ! openssl x509 -in "$CERT_FILE" -pubkey -noout 2> /dev/null \
		| openssl pkey -pubin -outform PEM > "$vcp_cert_public" 2> /dev/null; then
		fatal "Could not extract the public key from $CERT_FILE."
	fi

	if ! openssl pkey -in "$KEY_FILE" -pubout -outform PEM -passin pass: \
		> "$vcp_key_public" 2> /dev/null; then
		fatal "The private key is invalid, unreadable, or encrypted: $KEY_FILE"
	fi

	if ! cmp "$vcp_cert_public" "$vcp_key_public" > /dev/null 2>&1; then
		fatal "The certificate and private key do not match."
	fi

	rm -f "$vcp_cert_public" "$vcp_key_public"
	unset vcp_cert_public vcp_key_public

	if ! openssl x509 -in "$CERT_FILE" -checkend 604800 -noout > /dev/null 2>&1; then
		log_warn "The selected certificate is expired or will expire within seven days."
		if ! confirm "Continue with this certificate anyway?" no; then
			fatal "Stopped because the certificate is expired or near expiry."
		fi
	fi

	log_success "Certificate and private key validation passed."
}

validate_port() {
	vp_candidate=$1

	case $vp_candidate in
		'' | *[!0-9]*)
			unset vp_candidate
			return 1
			;;
	esac

	[ "$vp_candidate" -ge 1 ] 2> /dev/null || {
		unset vp_candidate
		return 1
	}
	[ "$vp_candidate" -le 65535 ] 2> /dev/null || {
		unset vp_candidate
		return 1
	}

	unset vp_candidate
	return 0
}

port_in_use() {
	piu_port=$1

	case ${PORT_CHECKER:-} in
		ss)
			ss -ltn 2> /dev/null | awk -v wanted="$piu_port" '
				NR > 1 {
					address = $4
					sub(/^.*:/, "", address)
					if (address == wanted) {
						found = 1
					}
				}
				END { exit(found ? 0 : 1) }
			'
			piu_status=$?
			;;
		netstat)
			netstat -ltn 2> /dev/null | awk -v wanted="$piu_port" '
				NR > 2 {
					address = $4
					sub(/^.*:/, "", address)
					if (address == wanted) {
						found = 1
					}
				}
				END { exit(found ? 0 : 1) }
			'
			piu_status=$?
			;;
		*)
			unset piu_port
			fatal "No listener-port checker is configured."
			;;
	esac

	unset piu_port
	if [ "$piu_status" -eq 0 ]; then
		unset piu_status
		return 0
	fi

	unset piu_status
	return 1
}

prompt_port() {
	pp_label=$1
	pp_default=$2

	while :; do
		printf '%s [%s]: ' "$pp_label" "$pp_default"
		if ! IFS= read -r pp_candidate; then
			fatal "Input ended before a port was provided."
		fi
		[ -n "$pp_candidate" ] || pp_candidate=$pp_default

		if ! validate_port "$pp_candidate"; then
			log_warn "Enter a numeric TCP port from 1 through 65535."
			continue
		fi

		if port_in_use "$pp_candidate"; then
			log_warn "TCP port $pp_candidate currently has a listening socket."
			if ! confirm "Continue only if this listener belongs to the Nginx configuration being replaced?" no; then
				continue
			fi
		fi

		PROMPTED_PORT=$pp_candidate
		unset pp_label pp_default pp_candidate
		return 0
	done
}

random_u32() {
	if [ -r /dev/urandom ] && command_exists od; then
		ru_value=$(od -An -N4 -tu4 /dev/urandom 2> /dev/null \
			| tr -d '[:space:]')
		case $ru_value in
			'' | *[!0-9]*)
				ru_value=
				;;
		esac

		if [ -n "$ru_value" ]; then
			printf '%s\n' "$ru_value"
			unset ru_value
			return 0
		fi
		unset ru_value
	fi

	awk 'BEGIN { srand(); printf "%.0f\n", rand() * 4294967295 }'
}

find_random_port() {
	frfp_minimum=$1
	frfp_maximum=$2
	frfp_span=$((frfp_maximum - frfp_minimum + 1))
	frfp_attempt=0

	while [ "$frfp_attempt" -lt 4096 ]; do
		frfp_random=$(random_u32)
		frfp_candidate=$((frfp_minimum + (frfp_random % frfp_span)))

		if ! port_in_use "$frfp_candidate"; then
			SELECTED_FREE_PORT=$frfp_candidate
			unset frfp_minimum frfp_maximum frfp_span frfp_attempt \
				frfp_random frfp_candidate
			return 0
		fi

		frfp_attempt=$((frfp_attempt + 1))
		unset frfp_random frfp_candidate
	done

	unset frfp_minimum frfp_maximum frfp_span frfp_attempt
	fatal "Could not find a free TCP port in the requested range."
}

domain_checksum() {
	dc_domain=$1
	printf '%s\n' "$dc_domain" | cksum | awk '{print $1}'
	unset dc_domain
}

bounded_domain_token() {
	bdt_domain=$1
	bdt_maximum=$2
	bdt_safe=$(printf '%s' "$bdt_domain" | sed 's/[.-]/_/g')

	if [ "${#bdt_safe}" -le "$bdt_maximum" ]; then
		printf '%s\n' "$bdt_safe"
		unset bdt_domain bdt_maximum bdt_safe
		return 0
	fi

	bdt_checksum=$(domain_checksum "$bdt_domain")
	bdt_prefix_length=$((bdt_maximum - ${#bdt_checksum} - 1))
	[ "$bdt_prefix_length" -ge 1 ] \
		|| fatal "Internal error: bounded identifier length is too small."

	bdt_prefix=$(printf '%s' "$bdt_safe" | cut -c "1-$bdt_prefix_length")
	printf '%s_%s\n' "$bdt_prefix" "$bdt_checksum"

	unset bdt_domain bdt_maximum bdt_safe bdt_checksum \
		bdt_prefix_length bdt_prefix
}

websocket_map_variable() {
	wmv_domain=$1
	wmv_checksum=$(domain_checksum "$wmv_domain")
	printf 'map_%s\n' "$wmv_checksum"
	unset wmv_domain wmv_checksum
}

stream_upstream_name() {
	sun_domain=$1
	sun_public_port=$2
	sun_safe=$(printf '%s' "$sun_domain" | sed 's/[.-]/_/g')
	sun_prefix=$(printf '%s' "$sun_safe" | cut -c '1-36')
	printf 'sni_%s_%s\n' "$sun_public_port" "$sun_prefix"
	unset sun_domain sun_public_port sun_safe sun_prefix
}

ensure_stream_include() {
	[ -f "$NGINX_MAIN_CONF" ] \
		|| fatal "$NGINX_MAIN_CONF does not exist after Nginx installation."

	if grep '^[[:space:]]*include[[:space:]]*/.*stream-conf\.d/\*\.conf[[:space:]]*;' \
		"$NGINX_MAIN_CONF" > /dev/null 2>&1; then
		log_info "The stream configuration include is already present in nginx.conf."
		return 0
	fi

	make_temp_for "$NGINX_MAIN_CONF"

	awk -v include_path="$NGINX_CONF_DIR/stream-conf.d/*.conf" '
		BEGIN {
			inserted = 0
			pending_stream = 0
		}
		{
			line = $0

			if (!inserted && line ~ /^[[:space:]]*stream[[:space:]]*\{/) {
				if (line ~ /}[[:space:]]*$/) {
					sub(/}[[:space:]]*$/,
						"\n    include " include_path ";\n}", line)
					print line
				} else {
					print line
					print "    include " include_path ";"
				}
				inserted = 1
				next
			}

			if (!inserted && line ~ /^[[:space:]]*stream[[:space:]]*$/) {
				print line
				pending_stream = 1
				next
			}

			if (!inserted && pending_stream) {
				print line
				if (line ~ /^[[:space:]]*\{/) {
					print "    include " include_path ";"
					inserted = 1
					pending_stream = 0
				}
				next
			}

			print line
		}
		END {
			if (!inserted) {
				print ""
				print "# Managed SNI/TCP routing configuration."
				print "stream {"
				print "    include " include_path ";"
				print "}"
			}
		}
	' "$NGINX_MAIN_CONF" > "$CURRENT_TMP" \
		|| fatal "Could not add the stream include to nginx.conf."

	commit_temp_file "$NGINX_MAIN_CONF" 0644
}

write_domain_redirect_80() {
	wdr_target="$NGINX_CONF_DIR/conf.d/redirect_80.conf"
	make_temp_for "$wdr_target"

	cat > "$CURRENT_TMP" <<- DOMAIN_REDIRECT_80
		# Managed public HTTP catch-all.
		# Unknown Host headers are closed without returning content.
		server {
			listen 80 default_server;
			${IPV6_LISTEN_PREFIX}listen [::]:80 default_server;
			server_name _;

			server_tokens off;
			return 444;
		}

		# Public HTTP redirect for $DOMAIN.
		server {
			listen 80;
			${IPV6_LISTEN_PREFIX}listen [::]:80;
			server_name $DOMAIN;

			server_tokens off;
			include $NGINX_CONF_DIR/snippets/redirect_443.forcessl.conf;
		}
	DOMAIN_REDIRECT_80

	commit_nginx_file "$wdr_target" 0644 \
		|| fatal "The generated domain redirect configuration failed validation."
	HTTP_REDIRECT_CONFIG=$wdr_target
	unset wdr_target
}

write_custom_redirect_config() {
	wcrc_target="$NGINX_CONF_DIR/conf.d/redirect_${HTTP_PORT}.conf"
	make_temp_for "$wcrc_target"

	cat > "$CURRENT_TMP" <<- CUSTOM_REDIRECT
		# Managed custom-port HTTP redirect for $DOMAIN.
		# Unknown Host headers on port $HTTP_PORT are closed without content.
		server {
			listen $HTTP_PORT default_server;
			${IPV6_LISTEN_PREFIX}listen [::]:$HTTP_PORT default_server;
			server_name _;

			server_tokens off;
			return 444;
		}

		server {
			listen $HTTP_PORT;
			${IPV6_LISTEN_PREFIX}listen [::]:$HTTP_PORT;
			server_name $DOMAIN;

			if (\$host != "$DOMAIN") {
				return 444;
			}

			server_tokens off;
			include $REDIRECT_SNIPPET;
		}
	CUSTOM_REDIRECT

	commit_nginx_file "$wcrc_target" 0644 \
		|| fatal "The generated custom-port redirect configuration failed validation."
	HTTP_REDIRECT_CONFIG=$wcrc_target
	unset wcrc_target
}

find_stream_route_upstream() {
	fsru_source=$1
	fsru_domain=$2
	fsru_map_variable=$3

	FOUND_STREAM_UPSTREAM=$(awk -v wanted_domain="$fsru_domain" \
		-v wanted_map_variable="$fsru_map_variable" '
		function trim(value) {
			sub(/^[[:space:]]+/, "", value)
			sub(/[[:space:]]+$/, "", value)
			return value
		}
		function map_variable_from_line(line, value) {
			value = line
			sub(/^[[:space:]]*map[[:space:]]+\$ssl_preread_server_name[[:space:]]+\$/, "", value)
			sub(/[[:space:]]*\{.*$/, "", value)
			return trim(value)
		}
		BEGIN {
			in_map = 0
			depth = 0
		}
		{
			line = $0
			if (!in_map &&
				line ~ /^[[:space:]]*map[[:space:]]+\$ssl_preread_server_name[[:space:]]+\$/ &&
				map_variable_from_line(line) == wanted_map_variable) {
				in_map = 1
				depth = 0
			}

			if (in_map) {
				value = trim(line)
				if (value != "" && value !~ /^#/ && value !~ /^default[[:space:]]+/) {
					key = value
					sub(/[[:space:]].*$/, "", key)
					if (key == wanted_domain) {
						target = value
						sub(/^[^[:space:]]+[[:space:]]+/, "", target)
						sub(/;[[:space:]]*$/, "", target)
						print target
						exit
					}
				}

				opening = line
				closing = line
				open_count = gsub(/\{/, "", opening)
				close_count = gsub(/\}/, "", closing)
				depth += open_count - close_count
				if (depth == 0) {
					in_map = 0
				}
			}
		}
	' "$fsru_source")

	unset fsru_source fsru_domain fsru_map_variable
	if [ -n "$FOUND_STREAM_UPSTREAM" ]; then
		return 0
	fi

	unset FOUND_STREAM_UPSTREAM
	return 1
}

merge_stream_sni_candidate() {
	mssc_source=$1
	mssc_domain=$2
	mssc_upstream=$3
	mssc_backend_port=$4
	mssc_map_variable=$5
	mssc_reject_upstream=$6
	mssc_output=$7

	awk -v wanted_domain="$mssc_domain" \
		-v wanted_upstream="$mssc_upstream" \
		-v wanted_backend_port="$mssc_backend_port" \
		-v wanted_map_variable="$mssc_map_variable" \
		-v reject_upstream="$mssc_reject_upstream" '
		function trim(value) {
			sub(/^[[:space:]]+/, "", value)
			sub(/[[:space:]]+$/, "", value)
			return value
		}
		function upstream_name_from_line(line, value) {
			value = line
			sub(/^[[:space:]]*upstream[[:space:]]+/, "", value)
			sub(/[[:space:]]*\{.*$/, "", value)
			return trim(value)
		}
		function map_variable_from_line(line, value) {
			value = line
			sub(/^[[:space:]]*map[[:space:]]+\$ssl_preread_server_name[[:space:]]+\$/, "", value)
			sub(/[[:space:]]*\{.*$/, "", value)
			return trim(value)
		}
		function emit_upstream() {
			print "upstream " wanted_upstream " {"
			print "    server 127.0.0.1:" wanted_backend_port ";"
			print "}"
			print ""
		}
		BEGIN {
			in_map = 0
			map_depth = 0
			map_found = 0
			route_written = 0
			skip_upstream = 0
			upstream_depth = 0
			upstream_written = 0
		}
		{
			line = $0

			if (skip_upstream) {
				opening = line
				closing = line
				open_count = gsub(/\{/, "", opening)
				close_count = gsub(/\}/, "", closing)
				upstream_depth += open_count - close_count
				if (upstream_depth == 0) {
					emit_upstream()
					skip_upstream = 0
					upstream_written = 1
				}
				next
			}

			if (!in_map &&
				line ~ /^[[:space:]]*map[[:space:]]+\$ssl_preread_server_name[[:space:]]+\$/ &&
				map_variable_from_line(line) == wanted_map_variable) {
				in_map = 1
				map_found = 1
				map_depth = 0
				print line

				opening = line
				closing = line
				open_count = gsub(/\{/, "", opening)
				close_count = gsub(/\}/, "", closing)
				map_depth += open_count - close_count
				next
			}

			if (in_map) {
				opening = line
				closing = line
				open_count = gsub(/\{/, "", opening)
				close_count = gsub(/\}/, "", closing)

				if (map_depth + open_count - close_count == 0) {
					if (!route_written) {
						print "    " wanted_domain " " wanted_upstream ";"
						route_written = 1
					}
					print line
					in_map = 0
					map_depth = 0
					next
				}

				value = trim(line)
				if (value != "" && value !~ /^#/ && value !~ /^default[[:space:]]+/) {
					key = value
					sub(/[[:space:]].*$/, "", key)
					if (key == wanted_domain) {
						print "    " wanted_domain " " wanted_upstream ";"
						route_written = 1
						map_depth += open_count - close_count
						next
					}
				}

				print line
				map_depth += open_count - close_count
				next
			}

			if (!upstream_written &&
				line ~ /^[[:space:]]*upstream[[:space:]]+/ &&
				upstream_name_from_line(line) == wanted_upstream) {
				skip_upstream = 1
				upstream_depth = 0

				opening = line
				closing = line
				open_count = gsub(/\{/, "", opening)
				close_count = gsub(/\}/, "", closing)
				upstream_depth += open_count - close_count
				if (upstream_depth == 0) {
					emit_upstream()
					skip_upstream = 0
					upstream_written = 1
				}
				next
			}

			if (!upstream_written &&
				((line ~ /^[[:space:]]*upstream[[:space:]]+/ &&
				  upstream_name_from_line(line) == reject_upstream) ||
				 line ~ /^[[:space:]]*server[[:space:]]*\{/)) {
				emit_upstream()
				upstream_written = 1
			}

			print line
		}
		END {
			if (!upstream_written) {
				emit_upstream()
				upstream_written = 1
			}
			if (!map_found || !route_written || !upstream_written) {
				exit 2
			}
		}
	' "$mssc_source" > "$mssc_output"
	mssc_status=$?

	unset mssc_source mssc_domain mssc_upstream mssc_backend_port \
		mssc_map_variable mssc_reject_upstream mssc_output
	if [ "$mssc_status" -eq 0 ]; then
		unset mssc_status
		return 0
	fi

	unset mssc_status
	return 1
}

write_stream_sni_config() {
	wssc_public_port=${1:-443}
	wssc_target="$NGINX_CONF_DIR/stream-conf.d/redirect_${wssc_public_port}.conf"
	wssc_upstream=$(stream_upstream_name "$DOMAIN" "$wssc_public_port")
	wssc_map_variable="redirect_${wssc_public_port}_tls_backend"
	wssc_reject_upstream="redirect_${wssc_public_port}_reject"

	if [ "${TLS_PASSTHROUGH:-0}" -eq 1 ]; then
		wssc_backend_port=$BACKEND_PORT
	else
		wssc_backend_port=$TLS_INTERNAL_PORT
	fi

	if [ -f "$wssc_target" ] && find_stream_route_upstream "$wssc_target" "$DOMAIN" "$wssc_map_variable"; then
		wssc_upstream=$FOUND_STREAM_UPSTREAM
		unset FOUND_STREAM_UPSTREAM
	fi

	make_temp_for "$wssc_target"

	if [ -f "$wssc_target" ]; then
		if ! is_script_managed_file "$wssc_target"; then
			cleanup_temp
			fatal "Refusing to merge into an unmanaged TLS stream configuration: $wssc_target"
		fi

		if ! merge_stream_sni_candidate "$wssc_target" "$DOMAIN" \
			"$wssc_upstream" "$wssc_backend_port" "$wssc_map_variable" \
			"$wssc_reject_upstream" "$CURRENT_TMP"; then
			cleanup_temp
			fatal "Could not safely merge $DOMAIN into the existing TLS stream configuration: $wssc_target"
		fi

		log_info "Preserving existing SNI routes in $wssc_target and updating only $DOMAIN."
	else
		cat > "$CURRENT_TMP" <<- STREAM_CONF
			# Managed public TLS/SNI gateway on port $wssc_public_port.
			# Each configured SNI name is retained until its managed upstream is explicitly removed.

			map \$ssl_preread_server_name \$$wssc_map_variable {
				default $wssc_reject_upstream;
				$DOMAIN $wssc_upstream;
			}

			upstream $wssc_upstream {
				server 127.0.0.1:$wssc_backend_port;
			}

			# Unknown or absent SNI is rejected through a local discard endpoint.
			upstream $wssc_reject_upstream {
				server 127.0.0.1:9;
			}

			server {
				listen $wssc_public_port;
				${IPV6_LISTEN_PREFIX}listen [::]:$wssc_public_port;

				ssl_preread on;
				proxy_pass \$$wssc_map_variable;
				proxy_connect_timeout 2s;
				proxy_timeout 1h;
			}
		STREAM_CONF
	fi

	commit_nginx_file "$wssc_target" 0644 \
		|| fatal "The generated TLS stream configuration failed validation."
	STREAM_CONFIG=$wssc_target
	unset wssc_public_port wssc_target wssc_upstream wssc_map_variable \
		wssc_reject_upstream wssc_backend_port
}

validate_location_path() {
	vlp_candidate=$1

	if [ -n "$vlp_candidate" ]; then
		unset vlp_candidate
		return 0
	fi

	unset vlp_candidate
	return 1
}

validate_proxy_url() {
	vpu_candidate=$1

	case $vpu_candidate in
		http://*)
			vpu_rest=${vpu_candidate#http://}
			;;
		https://*)
			vpu_rest=${vpu_candidate#https://}
			;;
		*)
			unset vpu_candidate
			return 1
			;;
	esac

	case $vpu_candidate in
		*[!A-Za-z0-9_./:@?\&=%+,\[\]-]*)
			unset vpu_candidate vpu_rest
			return 1
			;;
	esac

	vpu_authority=${vpu_rest%%/*}
	case $vpu_authority in
		*\?*)
			vpu_authority=${vpu_authority%%\?*}
			;;
	esac

	[ -n "$vpu_authority" ] || {
		unset vpu_candidate vpu_rest vpu_authority
		return 1
	}

	case $vpu_authority in
		*@* | *\&* | *=* | *,*)
			unset vpu_candidate vpu_rest vpu_authority
			return 1
			;;
	esac

	case $vpu_authority in
		\[*\]*)
			vpu_host=${vpu_authority#\[}
			vpu_host=${vpu_host%%\]*}
			vpu_suffix=${vpu_authority#*\]}

			if ! validate_ipv6 "$vpu_host"; then
				unset vpu_candidate vpu_rest vpu_authority \
					vpu_host vpu_suffix
				return 1
			fi

			case $vpu_suffix in
				'') ;;
				:*)
					vpu_port=${vpu_suffix#:}
					if ! validate_port "$vpu_port"; then
						unset vpu_candidate vpu_rest vpu_authority \
							vpu_host vpu_suffix vpu_port
						return 1
					fi
					unset vpu_port
					;;
				*)
					unset vpu_candidate vpu_rest vpu_authority \
						vpu_host vpu_suffix
					return 1
					;;
			esac
			;;
		*:*)
			case $vpu_authority in
				*:*:*)
					unset vpu_candidate vpu_rest vpu_authority
					return 1
					;;
			esac

			vpu_host=${vpu_authority%%:*}
			vpu_port=${vpu_authority#*:}
			if ! validate_port "$vpu_port"; then
				unset vpu_candidate vpu_rest vpu_authority \
					vpu_host vpu_port
				return 1
			fi
			unset vpu_port

			if ! validate_proxy_host "$vpu_host"; then
				unset vpu_candidate vpu_rest vpu_authority vpu_host
				return 1
			fi
			;;
		*)
			vpu_host=$vpu_authority
			if ! validate_proxy_host "$vpu_host"; then
				unset vpu_candidate vpu_rest vpu_authority vpu_host
				return 1
			fi
			;;
	esac

	unset vpu_candidate vpu_rest vpu_authority vpu_host vpu_suffix
	return 0
}

append_common_proxy_headers() {
	acph_fragment=$1
	acph_forwarded_port=$2
	acph_timeout=${3:-60s}

	cat >> "$acph_fragment" <<- PROXY_HEADERS
		proxy_http_version 1.1;
		proxy_set_header Host \$http_host;
		proxy_set_header X-Forwarded-Host \$http_host;
		proxy_set_header X-Forwarded-Port $acph_forwarded_port;
		proxy_set_header X-Real-IP \$remote_addr;
		proxy_set_header X-Forwarded-For \$proxy_add_x_forwarded_for;
		proxy_set_header X-Forwarded-Proto \$scheme;
		proxy_connect_timeout 10s;
		proxy_send_timeout $acph_timeout;
		proxy_read_timeout $acph_timeout;
	PROXY_HEADERS

	unset acph_fragment acph_forwarded_port acph_timeout
}

extract_proxy_upstreams() {
	enpu_source=$1

	awk '
		{
			remainder = $0
			while (match(remainder, /proxy_pass[[:space:]]+https?:\/\/[^;[:space:]]+/)) {
				value = substr(remainder, RSTART, RLENGTH)
				sub(/^proxy_pass[[:space:]]+https?:\/\//, "", value)
				sub(/\/.*/, "", value)

				if (value ~ /^[A-Za-z_][A-Za-z0-9_-]*$/ && value != "localhost") {
					print value
				}

				remainder = substr(remainder, RSTART + RLENGTH)
			}
		}
	' "$enpu_source" | awk '!seen[$0]++'

	unset enpu_source
}

upstream_defined_elsewhere() {
	ude_name=$1
	ude_excluded_target=$2

	ude_match=$(find "$NGINX_CONF_DIR" -type f \
		! -path "$NGINX_CONF_DIR/backups/*" \
		! -path "$ude_excluded_target" \
		-exec awk -v wanted="$ude_name" '
			/^[[:space:]]*upstream[[:space:]]+/ {
				value = $0
				sub(/^[[:space:]]*upstream[[:space:]]+/, "", value)
				sub(/[[:space:]]*\{.*/, "", value)
				if (value == wanted) {
					print FILENAME
					exit
				}
			}
		' {} \; 2> /dev/null | sed -n '1p')

	if [ -n "$ude_match" ]; then
		log_success "Named upstream $ude_name is already defined in $ude_match"
		unset ude_name ude_excluded_target ude_match
		return 0
	fi

	unset ude_name ude_excluded_target ude_match
	return 1
}

normalize_upstream_server() {
	nus_value=$1
	nus_value=$(printf '%s\n' "$nus_value" \
		| sed 's/^[[:space:]]*//; s/[[:space:]]*$//; s/^server[[:space:]][[:space:]]*//; s/;[[:space:]]*$//')

	case $nus_value in
		'' | *'{'* | *'}'* | *';'*)
			unset nus_value
			return 1
			;;
	esac

	NORMALIZED_UPSTREAM_SERVER=$nus_value
	unset nus_value
	return 0
}

collect_required_upstreams() {
	cru_locations=$1
	cru_target=$2
	UPSTREAM_FRAGMENT="$BACKUP_DIR/http-upstreams.conf"
	cru_names="$BACKUP_DIR/proxy-pass-upstreams.txt"

	: > "$UPSTREAM_FRAGMENT" \
		|| fatal "Could not create the HTTP-upstream workspace."
	chmod 0600 "$UPSTREAM_FRAGMENT" \
		|| fatal "Could not secure the HTTP-upstream workspace."

	extract_proxy_upstreams "$cru_locations" > "$cru_names" \
		|| fatal "Could not inspect custom proxy_pass directives for named upstreams."
	chmod 0600 "$cru_names" \
		|| fatal "Could not secure the named-upstream inventory."

	if [ ! -s "$cru_names" ]; then
		rm -f "$cru_names"
		unset cru_locations cru_target cru_names
		return 0
	fi

	while IFS= read -r cru_name <&3; do
		[ -n "$cru_name" ] || continue

		if upstream_defined_elsewhere "$cru_name" "$cru_target"; then
			continue
		fi

		log_warn "Custom proxy_pass references named upstream '$cru_name', but no matching HTTP-level upstream block will remain after replacing $cru_target."
		log_info "A named proxy_pass target must be declared outside all server and location blocks."

		while :; do
			printf "Backend server for upstream %s (for example 127.0.0.1:8080): " "$cru_name"
			if ! IFS= read -r cru_server; then
				fatal "Input ended before a server was provided for upstream $cru_name."
			fi

			if normalize_upstream_server "$cru_server"; then
				cru_server=$NORMALIZED_UPSTREAM_SERVER
				unset NORMALIZED_UPSTREAM_SERVER
				break
			fi

			log_warn "Enter one Nginx upstream server value without braces or embedded semicolons, such as 127.0.0.1:8080."
		done

		cat >> "$UPSTREAM_FRAGMENT" <<- HTTP_UPSTREAM
			# Managed HTTP upstream inferred from custom proxy_pass directives.
			# UPSTREAM: $cru_name
			upstream $cru_name {
				server $cru_server;
			}

		HTTP_UPSTREAM

		log_success "Added HTTP upstream $cru_name -> $cru_server"
		unset cru_name cru_server
	done 3< "$cru_names"

	rm -f "$cru_names"
	unset cru_locations cru_target cru_names
	return 0
}

collect_custom_parameters() {
	ccp_output=$1

	while :; do
		: > "$ccp_output" \
			|| fatal "Could not create the custom-parameter workspace."
		chmod 0600 "$ccp_output" \
			|| fatal "Could not secure the custom-parameter workspace."

		printf '\nPaste raw Nginx directives exactly as they should appear inside the location block.\n'
		printf 'Single-line semicolon-separated and multi-line formats are both supported.\n'
		printf 'Examples:\n'
		printf '  proxy_pass http://api_backend; proxy_set_header Host $http_host;\n'
		printf '  proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;\n'
		printf 'Enter END on a line by itself when finished.\n\n'

		ccp_lines=0
		while :; do
			if ! IFS= read -r ccp_line; then
				fatal "Input ended before the custom Nginx directives were completed with END."
			fi

			if [ "$ccp_line" = END ]; then
				break
			fi

			printf '%s\n' "$ccp_line" >> "$ccp_output" \
				|| fatal "Could not save the custom Nginx directives."
			ccp_lines=$((ccp_lines + 1))
		done

		if [ "$ccp_lines" -gt 0 ] \
			&& grep '[^[:space:]]' "$ccp_output" > /dev/null 2>&1; then
			if [ "$ccp_lines" -eq 1 ]; then
				log_success "Captured one custom directive line; semicolon-separated directives will be parsed directly by Nginx."
			else
				log_success "Captured $ccp_lines custom directive lines for Nginx validation."
			fi
			unset ccp_output ccp_lines ccp_line
			return 0
		fi

		log_warn "At least one non-empty Nginx directive is required. Enter the directives again."
		unset ccp_lines ccp_line
	done
}

collect_custom_locations() {
	ccl_forwarded_port=$1
	LOCATION_FRAGMENT="$BACKUP_DIR/location-blocks.conf"
	: > "$LOCATION_FRAGMENT" \
		|| fatal "Could not create the location-block workspace."
	chmod 0600 "$LOCATION_FRAGMENT" \
		|| fatal "Could not secure the location-block workspace."

	printf '\n\033[1mCustom location examples:\033[0m\n'
	printf '  Static files: location / { root /var/www/site; try_files $uri $uri/ =404; }\n'
	printf '  HTTP proxy:   location /api/ { proxy_pass http://127.0.0.1:3001; ... }\n'
	printf '  WebSocket:    location /ws/ { proxy_pass http://127.0.0.1:3002; Upgrade headers... }\n'
	printf '  Regex match:  location ~ ^/(api) { ... }\n'
	printf '  PHP regex:    location ~* \.php$ { ... }\n\n'

	ccl_count=0

	while :; do
		while :; do
			printf 'Location match after the location keyword (for example /, ~ ^/(api), or ~* \.php$): '
			if ! IFS= read -r ccl_path; then
				fatal "Input ended while collecting location blocks."
			fi

			if ! validate_location_path "$ccl_path"; then
				log_warn "The location match cannot be empty. Prefix, exact, named, regex, and other Nginx-supported location forms are accepted and will be checked by nginx -t."
				continue
			fi

			if grep -F "# LOCATION: $ccl_path" "$LOCATION_FRAGMENT" \
				> /dev/null 2>&1; then
				log_warn "That exact location match has already been configured."
				continue
			fi
			break
		done

		printf '\nSelect the location type:\n'
		printf '  1) Static files\n'
		printf '  2) Standard HTTP/HTTPS reverse proxy\n'
		printf '  3) WebSocket reverse proxy\n'
		printf '  4) Custom parameters\n'

		while :; do
			printf 'Choice [1-4]: '
			if ! IFS= read -r ccl_type; then
				fatal "Input ended while selecting a location type."
			fi
			case $ccl_type in
				1 | 2 | 3 | 4)
					break
					;;
				*)
					log_warn "Enter 1, 2, 3, or 4."
					;;
			esac
		done

		printf '\n# LOCATION: %s\n' "$ccl_path" >> "$LOCATION_FRAGMENT"

		case $ccl_type in
			1)
				while :; do
					printf 'Absolute static root path [/var/www/site]: '
					if ! IFS= read -r ccl_static_root; then
						fatal "Input ended while reading the static root."
					fi
					[ -n "$ccl_static_root" ] \
						|| ccl_static_root=/var/www/site

					if validate_absolute_path "$ccl_static_root"; then
						break
					fi
					log_warn "Enter an absolute path with no spaces or Nginx control characters."
				done

				printf 'Index filename [index.html]: '
				if ! IFS= read -r ccl_index; then
					fatal "Input ended while reading the index filename."
				fi
				[ -n "$ccl_index" ] || ccl_index=index.html
				case $ccl_index in
					*[!A-Za-z0-9_.-]* | '')
						fatal "Unsafe index filename: $ccl_index"
						;;
				esac

				cat >> "$LOCATION_FRAGMENT" <<- STATIC_LOCATION
					location $ccl_path {
						root $ccl_static_root;
						index $ccl_index;
						try_files \$uri \$uri/ =404;
						autoindex off;
					}
				STATIC_LOCATION

				unset ccl_static_root ccl_index
				;;
			2 | 3)
				while :; do
					printf 'Upstream URL (for example http://127.0.0.1:3001): '
					if ! IFS= read -r ccl_upstream; then
						fatal "Input ended while reading the upstream URL."
					fi
					if validate_proxy_url "$ccl_upstream"; then
						break
					fi
					log_warn "Use a valid http:// or https:// URL with a domain, IPv4 address, or bracketed IPv6 address; credentials and Nginx control characters are rejected."
				done

				printf '    location %s {\n' "$ccl_path" >> "$LOCATION_FRAGMENT"
				printf '        proxy_pass %s;\n' "$ccl_upstream" \
					>> "$LOCATION_FRAGMENT"

				if [ "$ccl_type" -eq 3 ]; then
					append_common_proxy_headers \
						"$LOCATION_FRAGMENT" "$ccl_forwarded_port" 3600s
				else
					append_common_proxy_headers \
						"$LOCATION_FRAGMENT" "$ccl_forwarded_port" 60s
				fi

				if [ "$ccl_type" -eq 3 ]; then
					cat >> "$LOCATION_FRAGMENT" <<- WEBSOCKET_HEADERS
						proxy_set_header Upgrade \$http_upgrade;
						proxy_set_header Connection \$$WS_MAP_VAR;
						proxy_buffering off;
						proxy_cache off;
					WEBSOCKET_HEADERS
				fi

				printf '    }\n' >> "$LOCATION_FRAGMENT"
				unset ccl_upstream
				;;
			4)
				ccl_custom_parameters="$BACKUP_DIR/custom-location-parameters.conf"
				collect_custom_parameters "$ccl_custom_parameters"

				printf '    location %s {\n' "$ccl_path" >> "$LOCATION_FRAGMENT"
				sed 's/^/        /' "$ccl_custom_parameters" \
					>> "$LOCATION_FRAGMENT" \
					|| fatal "Could not append the custom Nginx directives."
				printf '    }\n' >> "$LOCATION_FRAGMENT"
				rm -f "$ccl_custom_parameters"
				log_success "Custom parameters added for location $ccl_path; the complete candidate will now be checked by nginx -t."
				unset ccl_custom_parameters
				;;
		esac

		ccl_count=$((ccl_count + 1))
		log_success "Added location block: location $ccl_path"
		unset ccl_path ccl_type

		if ! confirm "Add another location block?" no; then
			break
		fi
		printf '\n'
	done

	[ "$ccl_count" -gt 0 ] \
		|| fatal "At least one location block is required."

	unset ccl_forwarded_port ccl_count
}

prompt_raw_tls_backend_port() {
	while :; do
		printf 'Desired raw TLS backend port [3000]: '
		if ! IFS= read -r prtbp_candidate; then
			fatal "Input ended before a raw TLS backend port was provided."
		fi
		[ -n "$prtbp_candidate" ] || prtbp_candidate=3000

		if validate_port "$prtbp_candidate"; then
			BACKEND_PORT=$prtbp_candidate
			log_success "Selected raw TLS backend port: $BACKEND_PORT"
			unset prtbp_candidate
			return 0
		fi

		log_warn "Enter a numeric TCP port from 1 through 65535."
	done
}

choose_proxy_layout() {
	cpl_forwarded_port=$1

	printf '\nSelect how this HTTPS virtual host should serve traffic:\n'
	printf '  1) Proxy every request to one manually selected backend port\n'
	printf '  2) Build custom static/proxy/WebSocket/custom-parameter location blocks interactively\n'

	while :; do
		printf 'Choice [1]: '
		if ! IFS= read -r cpl_choice; then
			fatal "Input ended before a proxy layout was selected."
		fi
		[ -n "$cpl_choice" ] || cpl_choice=1

		case $cpl_choice in
			1)
				while :; do
					printf 'Desired backend port [3000]: '
					if ! IFS= read -r cpl_backend_port; then
						fatal "Input ended before a backend port was provided."
					fi
					[ -n "$cpl_backend_port" ] || cpl_backend_port=3000

					if validate_port "$cpl_backend_port"; then
						BACKEND_PORT=$cpl_backend_port
						unset cpl_backend_port
						break
					fi

					log_warn "Enter a numeric TCP port from 1 through 65535."
				done

				LOCATION_FRAGMENT="$BACKUP_DIR/location-blocks.conf"
				: > "$LOCATION_FRAGMENT" \
					|| fatal "Could not create the proxy workspace."
				chmod 0600 "$LOCATION_FRAGMENT" \
					|| fatal "Could not secure the proxy workspace."

				cat > "$LOCATION_FRAGMENT" <<- SINGLE_PROXY
					location / {
						proxy_pass http://127.0.0.1:$BACKEND_PORT;
				SINGLE_PROXY

				append_common_proxy_headers \
					"$LOCATION_FRAGMENT" "$cpl_forwarded_port" 3600s

				cat >> "$LOCATION_FRAGMENT" <<- SINGLE_PROXY_END
					proxy_set_header Upgrade \$http_upgrade;
					proxy_set_header Connection \$$WS_MAP_VAR;
							}
				SINGLE_PROXY_END

				log_success "Selected backend port: $BACKEND_PORT"
				unset cpl_forwarded_port cpl_choice
				return 0
				;;
			2)
				collect_custom_locations "$cpl_forwarded_port"
				unset cpl_forwarded_port cpl_choice
				return 0
				;;
			*)
				log_warn "Enter 1 or 2."
				;;
		esac
	done
}

write_https_site_config() {
	whsc_listen=$1
	whsc_target=$2

	collect_required_upstreams "$LOCATION_FRAGMENT" "$whsc_target"
	make_temp_for "$whsc_target"

	if [ -s "$UPSTREAM_FRAGMENT" ]; then
		cat "$UPSTREAM_FRAGMENT" >> "$CURRENT_TMP" \
			|| fatal "Could not append generated HTTP upstream blocks to $whsc_target."
	fi

	cat >> "$CURRENT_TMP" <<- HTTPS_HEADER
		# Managed HTTPS reverse-proxy virtual host for $DOMAIN.
		# Host validation is intentionally strict.

		map \$http_upgrade \$$WS_MAP_VAR {
			default upgrade;
			'' close;
		}

		server {
			$whsc_listen
			server_name $DOMAIN;

			if (\$host != "$DOMAIN") {
				return 444;
			}

			server_tokens off;

			ssl_certificate $CERT_FILE;
			ssl_certificate_key $KEY_FILE;
			ssl_protocols TLSv1.2 TLSv1.3;
			ssl_session_timeout 1d;
			ssl_session_cache shared:SSL:10m;
			ssl_session_tickets off;

			client_max_body_size 16m;
			keepalive_timeout 65s;

			add_header Strict-Transport-Security "max-age=31536000" always;
			add_header X-Content-Type-Options "nosniff" always;
			add_header X-Frame-Options "SAMEORIGIN" always;
			add_header Referrer-Policy "strict-origin-when-cross-origin" always;

	HTTPS_HEADER

	cat "$LOCATION_FRAGMENT" >> "$CURRENT_TMP" \
		|| fatal "Could not append location blocks to $whsc_target."
	printf '}\n' >> "$CURRENT_TMP"

	if ! commit_nginx_file "$whsc_target" 0644; then
		unset whsc_listen whsc_target
		return 1
	fi

	SITE_CONFIG=$whsc_target
	unset whsc_listen whsc_target
	return 0
}

write_custom_config() {
	wcmc_target=$1

	collect_required_upstreams "$LOCATION_FRAGMENT" "$wcmc_target"
	make_temp_for "$wcmc_target"

	if [ -s "$UPSTREAM_FRAGMENT" ]; then
		cat "$UPSTREAM_FRAGMENT" >> "$CURRENT_TMP" \
			|| fatal "Could not append generated HTTP upstream blocks to $wcmc_target."
	fi

	cat >> "$CURRENT_TMP" <<- CUSTOM_CONFIG
		# Managed custom-port HTTP/HTTPS reverse proxy for $DOMAIN.
		# This file does not alter public port-80 or port-443 routing.

		map \$http_upgrade \$$WS_MAP_VAR {
			default upgrade;
			'' close;
		}

		server {
			listen $HTTP_PORT;
			${IPV6_LISTEN_PREFIX}listen [::]:$HTTP_PORT;
			server_name $DOMAIN;

			if (\$host != "$DOMAIN") {
				return 444;
			}

			server_tokens off;
			return 301 https://\$host:$HTTPS_PORT\$request_uri;
		}

		server {
			listen $HTTPS_PORT ssl;
			${IPV6_LISTEN_PREFIX}listen [::]:$HTTPS_PORT ssl;
			server_name $DOMAIN;

			if (\$host != "$DOMAIN") {
				return 444;
			}

			server_tokens off;

			ssl_certificate $CERT_FILE;
			ssl_certificate_key $KEY_FILE;
			ssl_protocols TLSv1.2 TLSv1.3;
			ssl_session_timeout 1d;
			ssl_session_cache shared:SSL:10m;
			ssl_session_tickets off;

			client_max_body_size 16m;
			keepalive_timeout 65s;

			add_header Strict-Transport-Security "max-age=31536000" always;
			add_header X-Content-Type-Options "nosniff" always;
			add_header X-Frame-Options "SAMEORIGIN" always;
			add_header Referrer-Policy "strict-origin-when-cross-origin" always;

	CUSTOM_CONFIG

	cat "$LOCATION_FRAGMENT" >> "$CURRENT_TMP" \
		|| fatal "Could not append custom location blocks."
	printf '}\n' >> "$CURRENT_TMP"

	if ! commit_nginx_file "$wcmc_target" 0644; then
		unset wcmc_target
		return 1
	fi

	SITE_CONFIG=$wcmc_target
	unset wcmc_target
	return 0
}

configure_domain_mode() {
	MODE=domain

	prompt_domain "Enter the public domain to route on ports 80 and 443"

	if confirm "Use Nginx-managed certificates?" yes; then
		TLS_PASSTHROUGH=0
		select_certificate_paths
		validate_certificate_pair

		find_random_port 10000 19999
		TLS_INTERNAL_PORT=$SELECTED_FREE_PORT
		unset SELECTED_FREE_PORT

		WS_MAP_VAR=$(websocket_map_variable "$DOMAIN")
		log_success "Selected internal TLS listener port: $TLS_INTERNAL_PORT"

		choose_proxy_layout 443
	else
		TLS_PASSTHROUGH=1
		prompt_raw_tls_backend_port
	fi

	ensure_stream_include
	write_domain_redirect_80
	write_stream_sni_config 443

	if [ "$TLS_PASSTHROUGH" -eq 0 ]; then
		cdm_token=$(bounded_domain_token "$DOMAIN" 80)
		cdm_target="$NGINX_CONF_DIR/conf.d/reverse_${cdm_token}.conf"
		cdm_listen="listen 127.0.0.1:$TLS_INTERNAL_PORT ssl;"

		while ! write_https_site_config "$cdm_listen" "$cdm_target"; do
			log_warn "The HTTPS virtual-host candidate was rejected by nginx -t."
			if ! confirm "Re-enter the proxy layout and location parameters?" yes; then
				fatal "Stopped without installing an invalid HTTPS virtual-host configuration."
			fi
			choose_proxy_layout 443
		done

		log_success "The HTTPS virtual-host configuration passed validation and was installed."
		unset cdm_token cdm_target cdm_listen
	fi

	unset WS_MAP_VAR LOCATION_FRAGMENT UPSTREAM_FRAGMENT
}

configure_custom_port_mode() {
	MODE=custom

	prompt_port "Custom HTTP redirect port" 2080
	HTTP_PORT=$PROMPTED_PORT
	unset PROMPTED_PORT

	while :; do
		prompt_port "Custom HTTPS reverse-proxy port" 2443
		HTTPS_PORT=$PROMPTED_PORT
		unset PROMPTED_PORT

		if [ "$HTTPS_PORT" = "$HTTP_PORT" ]; then
			log_warn "The HTTP and HTTPS ports must differ."
			unset HTTPS_PORT
			continue
		fi
		break
	done

	prompt_domain "Enter the domain clients will use on these custom ports"

	if confirm "Use Nginx-managed certificates?" yes; then
		TLS_PASSTHROUGH=0
		WS_MAP_VAR=$(websocket_map_variable "$DOMAIN")

		select_certificate_paths
		validate_certificate_pair
		choose_proxy_layout "$HTTPS_PORT"

		ccpm_token=$(bounded_domain_token "$DOMAIN" 72)
		ccpm_target="$NGINX_CONF_DIR/conf.d/proxy_${ccpm_token}_${HTTP_PORT}_${HTTPS_PORT}.conf"

		while ! write_custom_config "$ccpm_target"; do
			log_warn "The custom-port HTTP/HTTPS candidate was rejected by nginx -t."
			if ! confirm "Re-enter the proxy layout and location parameters?" yes; then
				fatal "Stopped without installing an invalid custom-port configuration."
			fi
			choose_proxy_layout "$HTTPS_PORT"
		done

		log_success "The custom-port HTTP/HTTPS configuration passed validation and was installed."
		unset ccpm_token ccpm_target WS_MAP_VAR LOCATION_FRAGMENT UPSTREAM_FRAGMENT
	else
		TLS_PASSTHROUGH=1
		prompt_raw_tls_backend_port
		ensure_stream_include
		write_forced_ssl_snippet "$HTTPS_PORT"
		write_custom_redirect_config
		write_stream_sni_config "$HTTPS_PORT"
	fi
}

has_managed_configurations() {
	hmc_first=$(managed_conf_files | sed -n '1p')

	if [ -n "$hmc_first" ]; then
		unset hmc_first
		return 0
	fi

	unset hmc_first
	return 1
}

build_management_inventory() {
	bmi_output=$1
	bmi_listeners="$BACKUP_DIR/manage-listeners.txt"
	bmi_upstreams="$BACKUP_DIR/manage-upstreams.txt"

	build_listener_inventory "$bmi_listeners"
	build_upstream_inventory "$bmi_upstreams"

	: > "$bmi_output" \
		|| fatal "Could not create the configuration-management inventory."

	awk -F '|' '{ print "listener|" $1 "|" $2 "|" $3 }' \
		"$bmi_listeners" >> "$bmi_output" \
		|| fatal "Could not add managed listeners to the configuration-management inventory."
	awk -F '|' '{ print "upstream|" $1 "|" $2 "|" $3 }' \
		"$bmi_upstreams" >> "$bmi_output" \
		|| fatal "Could not add managed upstreams to the configuration-management inventory."

	chmod 0600 "$bmi_output" \
		|| fatal "Could not secure the configuration-management inventory."

	unset bmi_output bmi_listeners bmi_upstreams
}

extract_endpoint_port() {
	eep_value=$1
	eep_endpoint=${eep_value%% *}

	case $eep_endpoint in
		\[*\]:*) eep_port=${eep_endpoint##*:} ;;
		*:*) eep_port=${eep_endpoint##*:} ;;
		*) eep_port=$eep_endpoint ;;
	esac

	if validate_port "$eep_port"; then
		EXTRACTED_PORT=$eep_port
		unset eep_value eep_endpoint eep_port
		return 0
	fi

	unset eep_value eep_endpoint eep_port
	return 1
}

change_listener_port() {
	prlp_old_port=$1

	while :; do
		printf 'New listener port (current %s): ' "$prlp_old_port"
		if ! IFS= read -r prlp_candidate; then
			fatal "Input ended before a replacement listener port was provided."
		fi

		if ! validate_port "$prlp_candidate"; then
			log_warn "Enter a numeric TCP port from 1 through 65535."
			continue
		fi

		if [ "$prlp_candidate" = "$prlp_old_port" ]; then
			log_warn "The replacement listener port must differ from the current port."
			continue
		fi

		if port_in_use "$prlp_candidate"; then
			log_warn "TCP port $prlp_candidate currently has a listening socket."
			if ! confirm "Use this listener port anyway?" no; then
				continue
			fi
		fi

		MANAGED_NEW_PORT=$prlp_candidate
		unset prlp_old_port prlp_candidate
		return 0
	done
}

change_upstream_port() {
	prup_old_port=$1

	while :; do
		printf 'New backend port (current %s): ' "$prup_old_port"
		if ! IFS= read -r prup_candidate; then
			fatal "Input ended before a replacement backend port was provided."
		fi

		if ! validate_port "$prup_candidate"; then
			log_warn "Enter a numeric TCP port from 1 through 65535."
			continue
		fi

		if [ "$prup_candidate" = "$prup_old_port" ]; then
			log_warn "The replacement backend port must differ from the current port."
			continue
		fi

		MANAGED_NEW_PORT=$prup_candidate
		unset prup_old_port prup_candidate
		return 0
	done
}

finalize_selected_file() {
	csmf_target=$1
	csmf_mode=$2

	[ -n "${CURRENT_TMP:-}" ] \
		|| fatal "Internal error: no managed configuration candidate is ready for $csmf_target."

	if ! is_script_managed_file "$csmf_target"; then
		cleanup_temp
		log_error "The selected file is no longer recognized as script-managed: $csmf_target"
		unset csmf_target csmf_mode
		return 1
	fi

	if ! validate_nginx_candidate "$csmf_target" "$CURRENT_TMP"; then
		cleanup_temp
		unset csmf_target csmf_mode
		return 1
	fi

	chmod "$csmf_mode" "$CURRENT_TMP" \
		|| fatal "Could not set permissions on the managed configuration candidate for $csmf_target."

	log_warn "Updating the selected script-managed configuration: $csmf_target"
	log_info "No overwrite confirmation is required because this node was selected explicitly; a transactional rollback backup will be created."
	backup_existing_file "$csmf_target" \
		|| fatal "Could not create the rollback backup for $csmf_target."

	mv -f "$CURRENT_TMP" "$csmf_target" \
		|| fatal "Could not atomically install the updated managed configuration: $csmf_target"

	unset CURRENT_TMP
	log_success "Updated managed configuration: $csmf_target"
	unset csmf_target csmf_mode
	return 0
}

replace_listener_port() {
	rlpc_source=$1
	rlpc_old_port=$2
	rlpc_new_port=$3
	rlpc_output=$4

	awk -v old_port="$rlpc_old_port" -v new_port="$rlpc_new_port" '
		function replace_port_tokens(text, pattern, matched, leading, trailing,
			before, after) {
			pattern = "(^|[^0-9])" old_port "([^0-9]|$)"
			while (match(text, pattern)) {
				matched = substr(text, RSTART, RLENGTH)
				leading = ""
				trailing = ""
				if (substr(matched, 1, 1) !~ /[0-9]/) {
					leading = substr(matched, 1, 1)
				}
				if (substr(matched, length(matched), 1) !~ /[0-9]/) {
					trailing = substr(matched, length(matched), 1)
				}
				before = substr(text, 1, RSTART - 1)
				after = substr(text, RSTART + RLENGTH)
				text = before leading new_port trailing after
			}
			return text
		}
		function replace_listen_endpoint(line, prefix, value, token, suffix,
			separator_position, endpoint_prefix) {
			match(line, /^[[:space:]]*listen[[:space:]]+/)
			prefix = substr(line, 1, RLENGTH)
			value = substr(line, RLENGTH + 1)
			separator_position = match(value, /[[:space:];]/)
			if (separator_position == 0) {
				token = value
				suffix = ""
			} else {
				token = substr(value, 1, separator_position - 1)
				suffix = substr(value, separator_position)
			}

			if (token == old_port) {
				token = new_port
			} else if (length(token) > length(old_port) + 1 &&
				substr(token, length(token) - length(old_port), 1) == ":" &&
				substr(token, length(token) - length(old_port) + 1) == old_port) {
				endpoint_prefix = substr(token, 1, length(token) - length(old_port))
				token = endpoint_prefix new_port
			}

			return prefix token suffix
		}
		{
			line = $0
			if (line ~ /^[[:space:]]*listen[[:space:]]+/) {
				line = replace_listen_endpoint(line)
			} else if (line ~ /return[[:space:]]+30[1278][[:space:]]+https:\/\// ||
				line ~ /proxy_set_header[[:space:]]+X-Forwarded-Port[[:space:]]+/ ||
				line ~ /redirect_[0-9]+_/ || line ~ /sni_[0-9]+_/) {
				line = replace_port_tokens(line)
			}
			print line
		}
	' "$rlpc_source" > "$rlpc_output"
	rlpc_status=$?

	unset rlpc_source rlpc_old_port rlpc_new_port rlpc_output
	if [ "$rlpc_status" -eq 0 ]; then
		unset rlpc_status
		return 0
	fi
	unset rlpc_status
	return 1
}

replace_upstream_port_candidate() {
	rupc_source=$1
	rupc_name=$2
	rupc_target=$3
	rupc_old_port=$4
	rupc_new_port=$5
	rupc_output=$6

	awk -v wanted_name="$rupc_name" -v wanted_target="$rupc_target" \
		-v old_port="$rupc_old_port" -v new_port="$rupc_new_port" '
		function trim(value) {
			sub(/^[[:space:]]+/, "", value)
			sub(/[[:space:]]+$/, "", value)
			return value
		}
		function replace_port_tokens(text, pattern, matched, leading, trailing,
			before, after) {
			pattern = "(^|[^0-9])" old_port "([^0-9]|$)"
			while (match(text, pattern)) {
				matched = substr(text, RSTART, RLENGTH)
				leading = ""
				trailing = ""
				if (substr(matched, 1, 1) !~ /[0-9]/) {
					leading = substr(matched, 1, 1)
				}
				if (substr(matched, length(matched), 1) !~ /[0-9]/) {
					trailing = substr(matched, length(matched), 1)
				}
				before = substr(text, 1, RSTART - 1)
				after = substr(text, RSTART + RLENGTH)
				text = before leading new_port trailing after
			}
			return text
		}
		BEGIN {
			in_upstream = 0
			depth = 0
			matched_server = 0
		}
		{
			line = $0
			if (!in_upstream && line ~ /^[[:space:]]*upstream[[:space:]]+/) {
				name = line
				sub(/^[[:space:]]*upstream[[:space:]]+/, "", name)
				sub(/[[:space:]]*\{.*$/, "", name)
				if (name == wanted_name) {
					in_upstream = 1
					depth = 0
				}
			}

			if (in_upstream && line ~ /^[[:space:]]*server[[:space:]]+/) {
				value = line
				sub(/^[[:space:]]*server[[:space:]]+/, "", value)
				sub(/;[[:space:]]*$/, "", value)
				value = trim(value)
				if (value == wanted_target) {
					line = replace_port_tokens(line)
					matched_server = 1
				}
			}

			print line

			if (in_upstream) {
				opening = $0
				closing = $0
				open_count = gsub(/\{/, "", opening)
				close_count = gsub(/\}/, "", closing)
				depth += open_count - close_count
				if (depth == 0) {
					in_upstream = 0
				}
			}
		}
		END { exit(matched_server ? 0 : 2) }
	' "$rupc_source" > "$rupc_output"
	rupc_status=$?

	unset rupc_source rupc_name rupc_target rupc_old_port rupc_new_port rupc_output
	if [ "$rupc_status" -eq 0 ]; then
		unset rupc_status
		return 0
	fi
	unset rupc_status
	return 1
}

remove_listener_candidate() {
	rlc_source=$1
	rlc_listen=$2
	rlc_server=$3
	rlc_output=$4

	awk -v wanted_listen="$rlc_listen" -v wanted_server="$rlc_server" '
		function trim(value) {
			sub(/^[[:space:]]+/, "", value)
			sub(/[[:space:]]+$/, "", value)
			return value
		}
		BEGIN {
			in_server = 0
			depth = 0
			removed = 0
		}
		{
			line = $0
			if (!in_server && line ~ /^[[:space:]]*server[[:space:]]*\{/) {
				in_server = 1
				depth = 0
				block = ""
				listen_match = 0
				server_name = ""
			}

			if (in_server) {
				block = block line ORS

				if (line ~ /^[[:space:]]*listen[[:space:]]+/) {
					value = line
					sub(/^[[:space:]]*listen[[:space:]]+/, "", value)
					sub(/;[[:space:]]*$/, "", value)
					if (trim(value) == wanted_listen) {
						listen_match = 1
					}
				}

				if (line ~ /^[[:space:]]*server_name[[:space:]]+/) {
					value = line
					sub(/^[[:space:]]*server_name[[:space:]]+/, "", value)
					sub(/;[[:space:]]*$/, "", value)
					server_name = trim(value)
				}

				opening = line
				closing = line
				open_count = gsub(/\{/, "", opening)
				close_count = gsub(/\}/, "", closing)
				depth += open_count - close_count

				if (depth == 0) {
					name_match = (wanted_server == "stream/SNI gateway") \
						? (server_name == "") : (server_name == wanted_server)
					if (!removed && listen_match && name_match) {
						removed = 1
					} else {
						printf "%s", block
					}
					in_server = 0
				}
			} else {
				print line
			}
		}
		END {
			if (in_server) {
				printf "%s", block
			}
			exit(removed ? 0 : 2)
		}
	' "$rlc_source" > "$rlc_output"
	rlc_status=$?

	unset rlc_source rlc_listen rlc_server rlc_output
	if [ "$rlc_status" -eq 0 ]; then
		unset rlc_status
		return 0
	fi
	unset rlc_status
	return 1
}

remove_upstream_candidate() {
	ruc_source=$1
	ruc_name=$2
	ruc_output=$3

	awk -v wanted_name="$ruc_name" '
		function trim(value) {
			sub(/^[[:space:]]+/, "", value)
			sub(/[[:space:]]+$/, "", value)
			return value
		}
		BEGIN {
			in_upstream = 0
			depth = 0
			removed = 0
		}
		{
			line = $0
			if (!in_upstream && line ~ /^[[:space:]]*upstream[[:space:]]+/) {
				name = line
				sub(/^[[:space:]]*upstream[[:space:]]+/, "", name)
				sub(/[[:space:]]*\{.*$/, "", name)
				name = trim(name)
				if (!removed && name == wanted_name) {
					in_upstream = 1
					depth = 0
				}
			}

			if (in_upstream) {
				opening = line
				closing = line
				open_count = gsub(/\{/, "", opening)
				close_count = gsub(/\}/, "", closing)
				depth += open_count - close_count
				if (depth == 0) {
					in_upstream = 0
					removed = 1
				}
				next
			}

			value = trim(line)
			if (value != "" && value !~ /^#/ &&
				value !~ /^default[[:space:]]+/ &&
				value ~ /^[^[:space:]]+[[:space:]]+[^[:space:];]+;[[:space:]]*$/) {
				target = value
				sub(/^[^[:space:]]+[[:space:]]+/, "", target)
				sub(/;[[:space:]]*$/, "", target)
				if (target == wanted_name) {
					next
				}
			}

			print line
		}
		END { exit(removed ? 0 : 2) }
	' "$ruc_source" > "$ruc_output"
	ruc_status=$?

	unset ruc_source ruc_name ruc_output
	if [ "$ruc_status" -eq 0 ]; then
		unset ruc_status
		return 0
	fi
	unset ruc_status
	return 1
}

modify_managed_listener() {
	ml_file=$1
	ml_listen=$2
	ml_server=$3

	if ! extract_endpoint_port "$ml_listen"; then
		log_error "Could not determine the numeric port from managed listener: $ml_listen"
		unset ml_file ml_listen ml_server
		return 1
	fi
	ml_old_port=$EXTRACTED_PORT
	unset EXTRACTED_PORT

	while :; do
		change_listener_port "$ml_old_port"
		ml_new_port=$MANAGED_NEW_PORT
		unset MANAGED_NEW_PORT

		make_temp_for "$ml_file"
		replace_listener_port "$ml_file" "$ml_old_port" \
			"$ml_new_port" "$CURRENT_TMP" \
			|| fatal "Could not generate the listener-port replacement candidate."

		log_warn "Every port-aware reference to $ml_old_port in $ml_file will be updated to $ml_new_port so related redirects, forwarded-port headers, and generated identifiers remain consistent."
		if finalize_selected_file "$ml_file" 0644; then
			MODE=manage
			MANAGED_NODE_ACTION="Changed listener port $ml_old_port to $ml_new_port"
			MANAGED_NODE_DESCRIPTION="$ml_server in $ml_file"
			unset ml_file ml_listen ml_server ml_old_port ml_new_port
			return 0
		fi

		log_warn "The listener-port change was rejected by nginx -t; the live configuration was not modified."
		if ! confirm "Try a different listener port?" yes; then
			unset ml_file ml_listen ml_server ml_old_port ml_new_port
			return 1
		fi
		unset ml_new_port
	done
}

modify_managed_upstream() {
	mmu_file=$1
	mmu_name=$2
	mmu_target=$3

	if ! extract_endpoint_port "$mmu_target"; then
		log_error "Could not determine the numeric backend port from upstream target: $mmu_target"
		unset mmu_file mmu_name mmu_target
		return 1
	fi
	mmu_old_port=$EXTRACTED_PORT
	unset EXTRACTED_PORT

	while :; do
		change_upstream_port "$mmu_old_port"
		mmu_new_port=$MANAGED_NEW_PORT
		unset MANAGED_NEW_PORT

		make_temp_for "$mmu_file"
		if ! replace_upstream_port_candidate "$mmu_file" "$mmu_name" \
			"$mmu_target" "$mmu_old_port" "$mmu_new_port" "$CURRENT_TMP"; then
			cleanup_temp
			log_error "Could not locate the selected upstream server directive in $mmu_file."
			unset mmu_file mmu_name mmu_target mmu_old_port mmu_new_port
			return 1
		fi

		if finalize_selected_file "$mmu_file" 0644; then
			MODE=manage
			MANAGED_NODE_ACTION="Changed upstream port $mmu_old_port to $mmu_new_port"
			MANAGED_NODE_DESCRIPTION="$mmu_name in $mmu_file"
			unset mmu_file mmu_name mmu_target mmu_old_port mmu_new_port
			return 0
		fi

		log_warn "The upstream-port change was rejected by nginx -t; the live configuration was not modified."
		if ! confirm "Try a different backend port?" yes; then
			unset mmu_file mmu_name mmu_target mmu_old_port mmu_new_port
			return 1
		fi
		unset mmu_new_port
	done
}

remove_managed_listener() {
	rml_file=$1
	rml_listen=$2
	rml_server=$3

	log_warn "Removing this listener removes its complete server block from $rml_file."
	log_warn "Related redirects or upstreams in other blocks are preserved and must remain valid."
	if ! confirm "Remove listener '$rml_listen' for '$rml_server'?" no; then
		unset rml_file rml_listen rml_server
		return 1
	fi

	make_temp_for "$rml_file"
	if ! remove_listener_candidate "$rml_file" "$rml_listen" \
		"$rml_server" "$CURRENT_TMP"; then
		cleanup_temp
		log_error "Could not locate the selected listener server block in $rml_file."
		unset rml_file rml_listen rml_server
		return 1
	fi

	if ! finalize_selected_file "$rml_file" 0644; then
		log_warn "Nginx rejected the removal, usually because another managed node still depends on this server block."
		unset rml_file rml_listen rml_server
		return 1
	fi

	MODE=manage
	MANAGED_NODE_ACTION="Removed listener $rml_listen"
	MANAGED_NODE_DESCRIPTION="$rml_server from $rml_file"
	unset rml_file rml_listen rml_server
	return 0
}

remove_managed_upstream() {
	rmu_file=$1
	rmu_name=$2
	rmu_target=$3

	log_warn "Removing this node removes the complete upstream '$rmu_name' block from $rmu_file."
	log_warn "Matching SNI map routes in the same managed file are removed with it; unrelated routes and upstreams are preserved."
	log_warn "Nginx validation will prevent removal while any other remaining directive still references it."
	if ! confirm "Remove upstream '$rmu_name'?" no; then
		unset rmu_file rmu_name rmu_target
		return 1
	fi

	make_temp_for "$rmu_file"
	if ! remove_upstream_candidate "$rmu_file" "$rmu_name" "$CURRENT_TMP"; then
		cleanup_temp
		log_error "Could not locate upstream '$rmu_name' in $rmu_file."
		unset rmu_file rmu_name rmu_target
		return 1
	fi

	if ! finalize_selected_file "$rmu_file" 0644; then
		log_warn "Nginx rejected the upstream removal because the resulting configuration is not valid."
		unset rmu_file rmu_name rmu_target
		return 1
	fi

	MODE=manage
	MANAGED_NODE_ACTION="Removed upstream $rmu_name"
	MANAGED_NODE_DESCRIPTION="$rmu_target from $rmu_file"
	unset rmu_file rmu_name rmu_target
	return 0
}

manage_existing_confs() {
	mec_inventory="$BACKUP_DIR/manage-nodes.txt"
	build_management_inventory "$mec_inventory"
	mec_total=$(awk 'END { print NR + 0 }' "$mec_inventory")

	if [ "$mec_total" -eq 0 ]; then
		log_warn "No script-managed listener, server, or upstream nodes are currently available to manage."
		unset mec_inventory mec_total
		return 2
	fi

	while :; do
		printf '\n\033[1mManage existing configurations\033[0m\n'
		awk -F '|' '
			$1 == "listener" {
				printf "  %d) Listener  %-28s Server: %-28s %s\n", NR, $3, $4, $2
			}
			$1 == "upstream" {
				printf "  %d) Upstream  %-28s Target: %-28s %s\n", NR, $3, $4, $2
			}
		' "$mec_inventory"
		printf '  0) Return to the previous menu\n'

		printf 'Select a managed node [0-%s]: ' "$mec_total"
		if ! IFS= read -r mec_choice; then
			fatal "Input ended before a managed node was selected."
		fi

		case $mec_choice in
			'' | *[!0-9]*)
				log_warn "Enter a number from 0 through $mec_total."
				continue
				;;
			0)
				unset mec_inventory mec_total mec_choice
				return 2
				;;
		esac

		if [ "$mec_choice" -gt "$mec_total" ]; then
			log_warn "Enter a number from 0 through $mec_total."
			continue
		fi

		mec_line=$(sed -n "${mec_choice}p" "$mec_inventory")
		mec_old_ifs=$IFS
		IFS='|'
		set -- $mec_line
		IFS=$mec_old_ifs
		mec_type=$1
		mec_file=$2
		mec_name=$3
		mec_detail=$4

		printf '\nSelected %s: %s\n' "$mec_type" "$mec_name"
		printf '  1) Modify its port\n'
		printf '  2) Remove this node\n'
		printf '  3) Choose another node\n'

		while :; do
			printf 'Action [1-3]: '
			if ! IFS= read -r mec_action; then
				fatal "Input ended before a management action was selected."
			fi

			case $mec_action in
				1)
					if [ "$mec_type" = listener ]; then
						if modify_managed_listener "$mec_file" "$mec_name" "$mec_detail"; then
							unset mec_inventory mec_total mec_choice mec_line mec_old_ifs \
								mec_type mec_file mec_name mec_detail mec_action
							return 0
						fi
					else
						if modify_managed_upstream "$mec_file" "$mec_name" "$mec_detail"; then
							unset mec_inventory mec_total mec_choice mec_line mec_old_ifs \
								mec_type mec_file mec_name mec_detail mec_action
							return 0
						fi
					fi
					log_warn "No managed configuration was changed."
					break
					;;
				2)
					if [ "$mec_type" = listener ]; then
						if remove_managed_listener "$mec_file" "$mec_name" "$mec_detail"; then
							unset mec_inventory mec_total mec_choice mec_line mec_old_ifs \
								mec_type mec_file mec_name mec_detail mec_action
							return 0
						fi
					else
						if remove_managed_upstream "$mec_file" "$mec_name" "$mec_detail"; then
							unset mec_inventory mec_total mec_choice mec_line mec_old_ifs \
								mec_type mec_file mec_name mec_detail mec_action
							return 0
						fi
					fi
					log_warn "No managed configuration was removed."
					break
					;;
				3)
					break
					;;
				*)
					log_warn "Enter 1, 2, or 3."
					;;
			esac
		done

		unset mec_choice mec_line mec_old_ifs mec_type mec_file mec_name \
			mec_detail mec_action
	done
}

add_new_configuration() {
	printf '\n\033[1mAdd new configuration\033[0m\n'
	printf '  1) Public ports 80 and 443 with domain and SNI routing\n'
	printf '  2) Isolated custom HTTP and HTTPS ports\n'

	while :; do
		printf 'Select the new configuration type [1-2]: '
		if ! IFS= read -r anc_choice; then
			fatal "Input ended before a new configuration type was selected."
		fi

		case $anc_choice in
			1)
				configure_domain_mode
				unset anc_choice
				return 0
				;;
			2)
				write_redirect_80
				configure_custom_port_mode
				unset anc_choice
				return 0
				;;
			*)
				log_warn "Enter 1 or 2."
				;;
		esac
	done
}

choose_next_action() {
	if [ "${MANAGED_EXISTING:-0}" -ne 1 ] \
		|| ! has_managed_configurations; then
		if [ "${MANAGED_EXISTING:-0}" -eq 1 ]; then
			log_warn "Management state exists, but no script-managed listener or upstream configuration files were found."
		fi
		add_new_configuration
		return 0
	fi

	while :; do
		printf 'Choose what to do next:\n\n  1) Manage existing configurations — select a listener, server, or upstream shown above to modify its port or remove that node'
		printf '\n  2) Add new configuration — create a new public 80/443 or custom-port setup without changing an existing node\n\n'
		printf 'Select an action [1-2]: '
		if ! IFS= read -r cna_choice; then
			fatal "Input ended before the next action was selected."
		fi

		case $cna_choice in
			1)
				manage_existing_confs
				cna_status=$?
				if [ "$cna_status" -eq 0 ]; then
					unset cna_choice cna_status
					return 0
				fi
				unset cna_status
				printf '\n'
				;;
			2)
				add_new_configuration
				unset cna_choice
				return 0
				;;
			*)
				log_warn "Enter 1 or 2."
				;;
		esac
	done
}

test_nginx_conf() {
	log_info "Testing the complete Nginx configuration..."

	if tnc_output=$("$NGINX_BIN" -t -c "$NGINX_MAIN_CONF" 2>&1); then
		printf '%s\n' "$tnc_output"
		log_success "nginx -t completed successfully."
		unset tnc_output
		return 0
	fi

	printf '%s\n' "$tnc_output" >&2
	log_error "The generated Nginx configuration is invalid."
	unset tnc_output
	rollback_transaction

	log_info "Testing the restored configuration..."
	if tnc_restored=$("$NGINX_BIN" -t -c "$NGINX_MAIN_CONF" 2>&1); then
		printf '%s\n' "$tnc_restored"
		log_success "Rollback restored a valid Nginx configuration."
	else
		printf '%s\n' "$tnc_restored" >&2
		log_error "The restored configuration also fails nginx -t; inspect existing Nginx files manually."
	fi

	unset tnc_restored
	exit 1
}

restart_nginx() {
	log_info "Restarting Nginx..."

	rn_restarted=0

	if command_exists systemctl; then
		if systemctl restart nginx; then
			rn_restarted=1
			log_success "Nginx restarted successfully with systemctl."
			printf '\n'
			systemctl --no-pager --full status nginx || :
		fi
	fi

	if [ "$rn_restarted" -eq 0 ] && command_exists service; then
		if service nginx restart; then
			rn_restarted=1
			log_success "Nginx restarted successfully with service."
			printf '\n'
			service nginx status || :
		fi
	fi

	if [ "$rn_restarted" -eq 1 ]; then
		unset rn_restarted
		return 0
	fi

	unset rn_restarted
	if ! command_exists systemctl && ! command_exists service; then
		fatal "Neither systemctl nor service is available to restart Nginx."
	fi

	log_error "Nginx failed to restart; rolling back managed changes."
	rollback_transaction

	if "$NGINX_BIN" -t -c "$NGINX_MAIN_CONF" > /dev/null 2>&1; then
		if command_exists systemctl; then
			systemctl restart nginx > /dev/null 2>&1 || :
		elif command_exists service; then
			service nginx restart > /dev/null 2>&1 || :
		fi
	fi

	fatal "Nginx restart failed. Previous managed files were restored where backups were available."
}

print_summary() {
	printf '\n\033[1mConfiguration summary\033[0m\n'

	if [ "${MODE:-}" = manage ]; then
		printf '  Mode:                 Manage existing configurations\n'
		printf '  Action:               %s\n' "$MANAGED_NODE_ACTION"
		printf '  Managed node:         %s\n' "$MANAGED_NODE_DESCRIPTION"
		printf '  Transaction backups: %s\n\n' "$BACKUP_DIR"
		unset MODE MANAGED_NODE_ACTION MANAGED_NODE_DESCRIPTION
		return 0
	fi

	printf '  Mode:                 %s\n' "$MODE"
	printf '  domain:             %s\n' "$DOMAIN"

	if [ "${TLS_PASSTHROUGH:-0}" -eq 1 ]; then
		printf '  TLS handling:         Raw TLS passthrough\n'
	else
		printf '  TLS handling:         Nginx-managed certificates\n'
		printf '  Certificate:          %s\n' "$CERT_FILE"
		printf '  Private key:          %s\n' "$KEY_FILE"
		printf '  Site configuration:   %s\n' "$SITE_CONFIG"
	fi

	if [ "$MODE" = domain ]; then
		printf '  Public HTTP:          80\n'
		printf '  Public HTTPS/SNI:     443\n'
		if [ "${TLS_PASSTHROUGH:-0}" -eq 0 ]; then
			printf '  Internal TLS port:    %s\n' "$TLS_INTERNAL_PORT"
		fi
		if [ -n "${HTTP_REDIRECT_CONFIG:-}" ]; then
			printf '  HTTP redirect config: %s\n' "$HTTP_REDIRECT_CONFIG"
		fi
		printf '  Stream configuration: %s\n' "$STREAM_CONFIG"
	else
		printf '  Custom HTTP port:     %s\n' "$HTTP_PORT"
		printf '  Custom HTTPS port:    %s\n' "$HTTPS_PORT"
		if [ "${TLS_PASSTHROUGH:-0}" -eq 1 ]; then
			printf '  Forced-SSL snippet:   %s\n' "$REDIRECT_SNIPPET"
			printf '  HTTP redirect config: %s\n' "$HTTP_REDIRECT_CONFIG"
			printf '  Stream configuration: %s\n' "$STREAM_CONFIG"
		fi
	fi

	if [ -n "${BACKEND_PORT:-}" ]; then
		if [ "${TLS_PASSTHROUGH:-0}" -eq 1 ]; then
			printf '  Selected backend:     tls://127.0.0.1:%s\n' "$BACKEND_PORT"
			log_warn "Start your TLS backend service on 127.0.0.1:$BACKEND_PORT before sending production traffic."
		else
			printf '  Selected backend:     http://127.0.0.1:%s\n' "$BACKEND_PORT"
			log_warn "Start your backend service on 127.0.0.1:$BACKEND_PORT before sending production traffic."
		fi
	fi

	printf '  Transaction backups: %s\n\n' "$BACKUP_DIR"

	unset MODE DOMAIN CERT_FILE KEY_FILE SITE_CONFIG STREAM_CONFIG \
		HTTP_REDIRECT_CONFIG REDIRECT_SNIPPET TLS_INTERNAL_PORT HTTP_PORT \
		HTTPS_PORT BACKEND_PORT TLS_PASSTHROUGH
}

main() {
	require_root
	reset_internal_state
	initialize_runtime_paths

	if load_management_state; then
		printf '\033[1mNginx Reverse-Proxy Manager\033[0m\n'
		log_success "Detected an existing Nginx instance managed by this script."
		validate_existing_managed_runtime
		ensure_directories
		init_transaction
		ensure_support_files
	else
		MANAGED_EXISTING=0
		prompt_ipv6_support
		printf '\033[1mNginx Reverse-Proxy Manager\033[0m\n'
		printf 'This installer will install packages and modify Nginx configuration files.\n\n'

		if ! confirm "Proceed with package installation and transactional Nginx changes?" no; then
			log_warn "No changes were made."
			unset NGINX_CONF_DIR NGINX_MAIN_CONF NGINX_BIN STATE_FILE \
				IPV6_ENABLED IPV6_LISTEN_PREFIX MANAGED_EXISTING
			exit 0
		fi

		install_packages
		ensure_directories
		init_transaction
		remove_default_site
		bootstrap_certificates
		write_redirect_snippet
	fi

	display_inventory
	choose_next_action

	write_management_state
	test_nginx_conf
	restart_nginx

	TRANSACTION_ACTIVE=0
	cleanup_temp
	cleanup_validation_dir
	print_summary

	unset TRANSACTION_ACTIVE ROLLBACK_DONE MANIFEST BACKUP_DIR \
		PORT_CHECKER NGINX_CONF_DIR NGINX_MAIN_CONF NGINX_BIN \
		IPV6_ENABLED IPV6_LISTEN_PREFIX STATE_FILE MANAGED_EXISTING \
		MANAGED_INSTALLED_AT MANAGED_LAST_CONFIGURED_AT TLS_PASSTHROUGH \
		HTTP_REDIRECT_CONFIG REDIRECT_SNIPPET MANAGED_NODE_ACTION \
		MANAGED_NODE_DESCRIPTION MANAGED_NEW_PORT

	log_success "Nginx reverse-proxy configuration completed."
}

if [ "${NGINX_RPM_NO_MAIN:-0}" != 1 ]; then
	main "$@"
fi
