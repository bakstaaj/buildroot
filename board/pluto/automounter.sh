#!/bin/sh

destdir=/media

is_ignored_dev()
{
	case "$1" in
		mmcblk[0-9]p1)
			return 0
			;;
		*)
			return 1
			;;
	esac
}

my_umount()
{
	if grep -qs "^/dev/$1 " /proc/mounts ; then
		umount "$(mount_point "$1")";
		echo heartbeat > /sys/class/leds/led0:green/trigger
	fi

	if [ "$(mount_point "$1")" != "$destdir" ]; then
		[ -d "$(mount_point "$1")" ] && rmdir "$(mount_point "$1")"
	fi
}

mount_point()
{
	case "$1" in
		mmcblk[0-9]p2)
			echo "$destdir"
			;;
		*)
			echo "${destdir}/$1"
			;;
	esac
}

do_mount()
{
	local errno
	local err
	local opts

	errno=0
	case "$1" in
		mmcblk*)
			/usr/sbin/pluto-sdcard-prepare "$1"
			return $?
			;;
		*)
			opts=sync
			;;
	esac

	for I in $(seq 5)
	do
		err=$(mount -t auto -o "${opts}" "/dev/$1" "${destdir}/$1" 2>&1)
		errno=$?

		# If we get a "Device or resource busy" error, retry again in a
		# little bit, otherwise just return immediately.
		if ! echo "${err}" | grep -q "Device or resource busy"
		then
			return ${errno}
		fi

		sleep .25
	done

	echo "${err}" >&2
	return ${errno}
}

my_mount()
{
	mkdir -p "$(mount_point "$1")" || exit 1

	if ! do_mount $1; then
		# failed to mount, clean up mountpoint
		if [ "$(mount_point "$1")" != "$destdir" ]; then
			rmdir "$(mount_point "$1")"
		fi
		exit 1
	fi

	echo default-on > /sys/class/leds/led0:green/trigger

	for i in "$(mount_point "$1")"/runme??* ;do

	# Ignore dangling symlinks (if any).
	[ ! -f "$i" ] && continue

	case "$i" in
		*.sh)
		# Source shell script for speed.
		(
			trap - INT QUIT TSTP
			set start
			. $i
		)
		;;
		*)
		# No sh extension, so fork subprocess.
		$i start
		;;
	esac
	done
}

case "${ACTION}" in
add|"")
	is_ignored_dev "${MDEV}" && exit 0
	my_umount ${MDEV}
	my_mount ${MDEV}
	;;
remove)
	is_ignored_dev "${MDEV}" && exit 0
	my_umount ${MDEV}
	;;
remove_all)
	for i in ${destdir}/??*
	do
		my_umount $(basename $i)
	done
esac
