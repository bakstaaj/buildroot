#!/bin/sh

echo "Content-Type: application/json"
echo "Cache-Control: no-store"
echo

json_escape() {
	printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g; s/	/ /g'
}

json_pair() {
	printf '"%s":"%s"' "$1" "$(json_escape "$2")"
}

read_file() {
	[ -r "$1" ] && sed -n '1p' "$1" 2>/dev/null
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

df_line() {
	df -k "$1" 2>/dev/null | awk 'NR==2 {printf "%s,%s,%s,%s", $2,$3,$4,$5}' || true
}

net_stat() {
	[ -r "/sys/class/net/$1/statistics/$2" ] && cat "/sys/class/net/$1/statistics/$2" 2>/dev/null
}

ip_addr() {
	/sbin/ifconfig "$1" 2>/dev/null | awk '/inet addr:/ {sub(/addr:/, "", $2); print $2; exit}'
}

iface_mode() {
	awk -v iface="$1" '
		$1 == "iface" && $2 == iface { print $4; found=1; exit }
		END { if (!found) print "unknown" }
	' /etc/network/interfaces 2>/dev/null
}

iface_operstate() {
	[ -r "/sys/class/net/$1/operstate" ] && cat "/sys/class/net/$1/operstate" 2>/dev/null
}

iface_carrier() {
	[ -r "/sys/class/net/$1/carrier" ] && cat "/sys/class/net/$1/carrier" 2>/dev/null
}

lease_count() {
	lease_file=$1
	[ -r "$lease_file" ] || {
		echo 0
		return
	}
	grep -c '^[0-9a-fA-F][0-9a-fA-F]:.*[0-9][0-9]*$' "$lease_file" 2>/dev/null || echo 0
}

lease_clients() {
	lease_file=$1
	[ -r "$lease_file" ] || return 0
	awk '
		/^[0-9a-fA-F][0-9a-fA-F]:/ {
			if (out != "") out = out ";"
			out = out $1 "," $2
		}
		END { print out }
	' "$lease_file" 2>/dev/null
}

mem_value() {
	awk -v key="$1:" '$1 == key {print $2}' /proc/meminfo 2>/dev/null || true
}

phy=$(find_iio_device ad9361-phy)
rx0_rssi=$(attr "$phy" in_voltage0_rssi)
rx1_rssi=$(attr "$phy" in_voltage1_rssi)

printf '{'
printf '"system":{'
json_pair hostname "$(hostname 2>/dev/null)"; printf ','
json_pair uptime_seconds "$(awk '{print int($1)}' /proc/uptime 2>/dev/null)"; printf ','
json_pair loadavg "$(cat /proc/loadavg 2>/dev/null)"; printf ','
json_pair mem_total_kb "$(mem_value MemTotal)"; printf ','
json_pair mem_available_kb "$(mem_value MemAvailable)"; printf ','
json_pair root_df_kb "$(df_line /)"; printf ','
json_pair jffs2_df_kb "$(df_line /mnt/jffs2)"; printf ','
json_pair versions "$(tr '\n' ';' < /opt/VERSIONS 2>/dev/null)"
printf '},'

printf '"network":{'
json_pair usb0_ip "$(ip_addr usb0)"; printf ','
json_pair usb0_rx_bytes "$(net_stat usb0 rx_bytes)"; printf ','
json_pair usb0_tx_bytes "$(net_stat usb0 tx_bytes)"; printf ','
json_pair eth0_ip "$(ip_addr eth0)"; printf ','
json_pair eth0_mode "$(iface_mode eth0)"; printf ','
json_pair eth0_operstate "$(iface_operstate eth0)"; printf ','
json_pair eth0_carrier "$(iface_carrier eth0)"; printf ','
json_pair eth0_rx_bytes "$(net_stat eth0 rx_bytes)"; printf ','
json_pair eth0_tx_bytes "$(net_stat eth0 tx_bytes)"; printf ','
json_pair eth0_dhcp_server_active "$([ -f /etc/udhcpd-eth0.conf ] && echo 1 || echo 0)"; printf ','
json_pair eth0_dhcp_lease_count "$(lease_count /var/lib/misc/udhcpd-eth0.leases)"; printf ','
json_pair eth0_dhcp_clients "$(lease_clients /var/lib/misc/udhcpd-eth0.leases)"
printf '},'

printf '"radio":{'
json_pair phy_path "$phy"; printf ','
json_pair ensm_mode "$(attr "$phy" ensm_mode)"; printf ','
json_pair rx_lo_hz "$(attr "$phy" out_altvoltage0_RX_LO_frequency)"; printf ','
json_pair tx_lo_hz "$(attr "$phy" out_altvoltage1_TX_LO_frequency)"; printf ','
json_pair rx_sample_rate_hz "$(attr "$phy" in_voltage_sampling_frequency)"; printf ','
json_pair tx_sample_rate_hz "$(attr "$phy" out_voltage_sampling_frequency)"; printf ','
json_pair rx_rf_bandwidth_hz "$(attr "$phy" in_voltage_rf_bandwidth)"; printf ','
json_pair tx_rf_bandwidth_hz "$(attr "$phy" out_voltage_rf_bandwidth)"; printf ','
json_pair rx_gain_control_mode_ch0 "$(attr "$phy" in_voltage0_gain_control_mode)"; printf ','
json_pair rx_gain_control_mode_ch1 "$(attr "$phy" in_voltage1_gain_control_mode)"; printf ','
json_pair rx_hardwaregain_db_ch0 "$(attr "$phy" in_voltage0_hardwaregain)"; printf ','
json_pair rx_hardwaregain_db_ch1 "$(attr "$phy" in_voltage1_hardwaregain)"; printf ','
json_pair tx_hardwaregain_db_ch0 "$(attr "$phy" out_voltage0_hardwaregain)"; printf ','
json_pair tx_hardwaregain_db_ch1 "$(attr "$phy" out_voltage1_hardwaregain)"; printf ','
json_pair rx_rssi_ch0 "$rx0_rssi"; printf ','
json_pair rx_rssi_ch1 "$rx1_rssi"; printf ','
json_pair rx_channel0_present "$([ -e "$phy/in_voltage0_hardwaregain" ] && echo 1 || echo 0)"; printf ','
json_pair rx_channel1_present "$([ -e "$phy/in_voltage1_hardwaregain" ] && echo 1 || echo 0)"; printf ','
json_pair tx_channel0_present "$([ -e "$phy/out_voltage0_hardwaregain" ] && echo 1 || echo 0)"; printf ','
json_pair tx_channel1_present "$([ -e "$phy/out_voltage1_hardwaregain" ] && echo 1 || echo 0)"; printf ','
json_pair temperature_raw "$(attr "$phy" temp0_input)"
printf '}'
printf '}\n'
