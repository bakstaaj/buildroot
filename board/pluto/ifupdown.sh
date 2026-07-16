#!/bin/sh

case "${ACTION}" in
add|"")
	ifconfig ${MDEV} up
	if [ "${MDEV}" = "eth0" ]; then
		/usr/sbin/pluto-eth-fallback ${MDEV} >/dev/null 2>&1 &
	else
		ifup ${MDEV}
		echo $(ip -f inet -o addr show ${MDEV}|cut -d\  -f 7 | cut -d/ -f 1) > /opt/ipaddr-${MDEV}
	fi
	;;
remove)
	if [ "${MDEV}" = "eth0" ]; then
		start-stop-daemon -K -q -p /var/run/udhcpd-eth0.pid 2>/dev/null
		start-stop-daemon -K -q -p /var/run/udhcpc-eth0.pid 2>/dev/null
		ifconfig ${MDEV} down
	else
		ifdown ${MDEV}
	fi
	;;
esac
