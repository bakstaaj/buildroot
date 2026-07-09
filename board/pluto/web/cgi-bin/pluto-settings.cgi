#!/bin/sh

SETTINGS_DIR=/mnt/jffs2/pluto-web
SETTINGS_FILE=$SETTINGS_DIR/settings.conf
APPLY=/usr/sbin/pluto-web-apply-settings

echo "Content-Type: application/json"
echo "Cache-Control: no-store"
echo

json_escape_stream() {
	awk '
		BEGIN { ORS = "" }
		{
			if (NR > 1) printf "\\n"
			gsub(/\\/, "\\\\")
			gsub(/"/, "\\\"")
			gsub(/\t/, "\\t")
			gsub(/\r/, "\\r")
			gsub(/[\001-\010\013\014\016-\037]/, "")
			printf "%s", $0
		}
	'
}

json_escape() {
	printf '%s' "$1" | json_escape_stream
}

json_pair() {
	printf '"%s":"%s"' "$1" "$(json_escape "$2")"
}

find_iio_device() {
	for dev in /sys/bus/iio/devices/iio:device*; do
		[ -r "$dev/name" ] || continue
		[ "$(cat "$dev/name" 2>/dev/null)" = "$1" ] && {
			echo "$dev"
			return 0
		}
	done
	return 1
}

attr() {
	[ -n "$1" ] && [ -r "$1/$2" ] && cat "$1/$2" 2>/dev/null
}

is_uint() {
	case "$1" in
		""|*[!0-9]*)
			return 1
			;;
	esac
	return 0
}

uint_range() {
	is_uint "$1" || return 1
	awk -v value="$1" -v min="$2" -v max="$3" 'BEGIN { exit !(value >= min && value <= max) }'
}

is_decimal() {
	case "$1" in
		""|*[!0-9.+-]*)
			return 1
			;;
	esac
	return 0
}

valid_key_value() {
	key=$1
	value=$2
	case "$key" in
		rx_lo_hz|tx_lo_hz)
			uint_range "$value" 70000000 6000000000
			;;
		rx_sample_rate_hz|tx_sample_rate_hz)
			uint_range "$value" 520000 61440000
			;;
		rx_rf_bandwidth_hz|tx_rf_bandwidth_hz)
			uint_range "$value" 200000 56000000
			;;
		rx_gain_control_mode)
			case "$value" in manual|slow_attack|fast_attack|hybrid) return 0 ;; *) return 1 ;; esac
			;;
		rx_hardwaregain_db|tx_hardwaregain_db)
			is_decimal "$value"
			;;
		ensm_mode)
			case "$value" in sleep|alert|fdd|pinctrl|rx|tx) return 0 ;; *) return 1 ;; esac
			;;
		*)
			return 1
			;;
	esac
}

decode_token() {
	printf '%s' "$1" | sed 's/+/ /g'
}

save_post() {
	[ -d /mnt/jffs2 ] || {
		printf '{"ok":false,"error":"jffs2_not_mounted"}\n'
		return
	}
	mkdir -p "$SETTINGS_DIR" || {
		printf '{"ok":false,"error":"settings_dir_failed"}\n'
		return
	}

	body=$(dd bs=1 count="${CONTENT_LENGTH:-0}" 2>/dev/null)
	tmp=$SETTINGS_FILE.tmp
	: > "$tmp" || {
		printf '{"ok":false,"error":"settings_write_failed"}\n'
		return
	}

	old_ifs=$IFS
	IFS='&'
	for pair in $body; do
		key=${pair%%=*}
		value=${pair#*=}
		key=$(decode_token "$key")
		value=$(decode_token "$value")
		[ "$key" = "$value" ] && continue
		if valid_key_value "$key" "$value"; then
			printf '%s=%s\n' "$key" "$value" >> "$tmp"
		fi
	done
	IFS=$old_ifs

	mv "$tmp" "$SETTINGS_FILE" || {
		printf '{"ok":false,"error":"settings_commit_failed"}\n'
		return
	}
	chmod 0600 "$SETTINGS_FILE" 2>/dev/null

	apply_status=skipped
	if [ -x "$APPLY" ]; then
		if "$APPLY"; then
			apply_status=ok
		else
			apply_status=failed
		fi
	fi
	printf '{"ok":true,"apply":"%s"}\n' "$apply_status"
}

print_settings() {
	phy=$(find_iio_device ad9361-phy)
	printf '{'
	printf '"ok":true,'
	printf '"persisted":{'
	first=1
	if [ -r "$SETTINGS_FILE" ]; then
		while IFS='=' read -r key value; do
			valid_key_value "$key" "$value" || continue
			[ $first -eq 0 ] && printf ','
			json_pair "$key" "$value"
			first=0
		done < "$SETTINGS_FILE"
	fi
	printf '},'
	printf '"current":{'
	json_pair rx_lo_hz "$(attr "$phy" out_altvoltage0_RX_LO_frequency)"; printf ','
	json_pair tx_lo_hz "$(attr "$phy" out_altvoltage1_TX_LO_frequency)"; printf ','
	json_pair rx_sample_rate_hz "$(attr "$phy" in_voltage_sampling_frequency)"; printf ','
	json_pair tx_sample_rate_hz "$(attr "$phy" out_voltage_sampling_frequency)"; printf ','
	json_pair rx_rf_bandwidth_hz "$(attr "$phy" in_voltage_rf_bandwidth)"; printf ','
	json_pair tx_rf_bandwidth_hz "$(attr "$phy" out_voltage_rf_bandwidth)"; printf ','
	json_pair rx_gain_control_mode "$(attr "$phy" in_voltage0_gain_control_mode)"; printf ','
	json_pair rx_hardwaregain_db "$(attr "$phy" in_voltage0_hardwaregain)"; printf ','
	json_pair tx_hardwaregain_db "$(attr "$phy" out_voltage0_hardwaregain)"; printf ','
	json_pair ensm_mode "$(attr "$phy" ensm_mode)"
	printf '},'
	printf '"schema":['
	printf '{"key":"rx_lo_hz","label":"RX LO","unit":"Hz","min":"70000000","max":"6000000000"},'
	printf '{"key":"tx_lo_hz","label":"TX LO","unit":"Hz","min":"70000000","max":"6000000000"},'
	printf '{"key":"rx_sample_rate_hz","label":"RX sample rate","unit":"SPS","min":"520000","max":"61440000"},'
	printf '{"key":"tx_sample_rate_hz","label":"TX sample rate","unit":"SPS","min":"520000","max":"61440000"},'
	printf '{"key":"rx_rf_bandwidth_hz","label":"RX RF bandwidth","unit":"Hz","min":"200000","max":"56000000"},'
	printf '{"key":"tx_rf_bandwidth_hz","label":"TX RF bandwidth","unit":"Hz","min":"200000","max":"56000000"},'
	printf '{"key":"rx_gain_control_mode","label":"RX gain mode","unit":"manual|slow_attack|fast_attack|hybrid"},'
	printf '{"key":"rx_hardwaregain_db","label":"RX hardware gain","unit":"dB"},'
	printf '{"key":"tx_hardwaregain_db","label":"TX hardware gain","unit":"dB"},'
	printf '{"key":"ensm_mode","label":"ENSM mode","unit":"sleep|alert|fdd|pinctrl|rx|tx"}'
	printf ']'
	printf '}\n'
}

case "$REQUEST_METHOD" in
	POST)
		save_post
		;;
	*)
		print_settings
		;;
esac
