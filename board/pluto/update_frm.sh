#!/bin/sh

#set -x

source /etc/device_config

FRM_FILE="$1"
CONFIG_DIR=`dirname "${FRM_FILE}"`
jffs2conf="${CONFIG_DIR}/config.frm"
JFFS2_BASE=1179648
JFFS2_CONFIG_ERROR="${CONFIG_DIR}/FAILED_JFFS2_CONFIG_ERROR"

flash_indication_on() {
	echo timer > /sys/class/leds/led0:green/trigger
	echo 40 > /sys/class/leds/led0:green/delay_off
	echo 40 > /sys/class/leds/led0:green/delay_on
}

flash_indication_off() {
	echo heartbeat > /sys/class/leds/led0:green/trigger
}

jffs2_config_error() {
	echo "$1" > ${JFFS2_CONFIG_ERROR}
	echo "$1"
}

validate_jffs2_config() {
	rm -f ${JFFS2_CONFIG_ERROR}
	CONFIG_JFFS2_SIZE=

	if [ ! -s "${jffs2conf}" ]; then
		jffs2_config_error "Missing ${jffs2conf}. Install firmware with a valid config.frm file."
		return 1
	fi

	sed -e 's/[[:space:]]//g' \
	    -e '/^#/d' \
	    -e '/^$/d' \
	    "${jffs2conf}" > /opt/jffs2_config.active

	if grep -qv '^JFFS2_SIZE_MIB=[1-5]$' /opt/jffs2_config.active; then
		jffs2_config_error "Invalid config.frm entry. Use exactly one uncommented JFFS2_SIZE_MIB=1..5 line."
		rm -f /opt/jffs2_config.active
		return 1
	fi

	active_count=`grep -c '^JFFS2_SIZE_MIB=[1-5]$' /opt/jffs2_config.active`
	if [ "${active_count}" != "1" ]; then
		jffs2_config_error "Invalid config.frm. Exactly one JFFS2_SIZE_MIB option must be uncommented."
		rm -f /opt/jffs2_config.active
		return 1
	fi

	CONFIG_JFFS2_SIZE=`sed -n 's/^JFFS2_SIZE_MIB=//p' /opt/jffs2_config.active`
	rm -f /opt/jffs2_config.active
	return 0
}

set_jffs2_layout_values() {
	case "$1" in
		1)
			JFFS2_NVMFS_SIZE=917504
			JFFS2_FIT_OFFSET=2097152
			JFFS2_FIT_PARTITION_SIZE=31457280
			;;
		2)
			JFFS2_NVMFS_SIZE=2097152
			JFFS2_FIT_OFFSET=3276800
			JFFS2_FIT_PARTITION_SIZE=30277632
			;;
		3)
			JFFS2_NVMFS_SIZE=3145728
			JFFS2_FIT_OFFSET=4325376
			JFFS2_FIT_PARTITION_SIZE=29229056
			;;
		4)
			JFFS2_NVMFS_SIZE=4194304
			JFFS2_FIT_OFFSET=5373952
			JFFS2_FIT_PARTITION_SIZE=28180480
			;;
		5)
			JFFS2_NVMFS_SIZE=5242880
			JFFS2_FIT_OFFSET=6422528
			JFFS2_FIT_PARTITION_SIZE=27131904
			;;
	esac
}

mtd_size_dec() {
	mtd_name="$1"
	mtd_size_hex=`awk -v name="\"${mtd_name}\"" '$4 == name { print $2 }' /proc/mtd`
	echo $((0x${mtd_size_hex}))
}

mtd_erasesize_dec() {
	mtd_name="$1"
	mtd_erasesize_hex=`awk -v name="\"${mtd_name}\"" '$4 == name { print $3 }' /proc/mtd`
	echo $((0x${mtd_erasesize_hex}))
}

prepare_jffs2_layout() {
	current_nvmfs_size=`mtd_size_dec qspi-nvmfs`
	current_fit_offset=$((JFFS2_BASE + current_nvmfs_size))

	if [ "${JFFS2_FIT_OFFSET}" -lt "${current_fit_offset}" ]; then
		jffs2_config_error "Cannot shrink /mnt/jffs2 in one firmware install. Select the current or a larger size, reboot, then shrink in a second install."
		return 1
	fi

	extra_jffs2_size=$((JFFS2_FIT_OFFSET - current_fit_offset))
	if [ "${JFFS2_NVMFS_SIZE}" != "${current_nvmfs_size}" ]; then
		umount /mnt/jffs2 2>/dev/null
		flash_erase -j /dev/mtd2 0 0 || return 1

		if [ "${extra_jffs2_size}" -gt 0 ]; then
			erasesize=`mtd_erasesize_dec qspi-linux`
			erase_count=$((extra_jffs2_size / erasesize))
			flash_erase -j /dev/mtd3 0 ${erase_count} || return 1
		fi
	fi

	FIRMWARE_SEEK_BYTES=$((JFFS2_FIT_OFFSET - current_fit_offset))
	return 0
}

handle_frimware_frm () {
	FILE="$1"
	MAGIC="$2"
	md5=`tail -c 33 ${FILE}`
	head -c -33 ${FILE} > /opt/firmware.frm
	FRM_SIZE_DEC=`cat /opt/firmware.frm | wc -c | xargs`
	FRM_SIZE=`printf "%X\n" ${FRM_SIZE_DEC}`
	frm=`md5sum /opt/firmware.frm | cut -d ' ' -f 1`
	if [ "$frm" = "$md5" ]
	then
		validate_jffs2_config || exit 1
		if ! grep -q "${MAGIC}" /opt/firmware.frm; then
			echo "Failed"
			rm -f /opt/firmware.frm
			exit 1
		fi
		set_jffs2_layout_values "${CONFIG_JFFS2_SIZE}"
		if [ "${FRM_SIZE_DEC}" -gt "${JFFS2_FIT_PARTITION_SIZE}" ]; then
			jffs2_config_error "Firmware image is too large for the selected JFFS2 layout."
			exit 1
		fi

		flash_indication_on
		prepare_jffs2_layout || exit 1
		seek_blocks=$((FIRMWARE_SEEK_BYTES / 65536))
		dd if=/opt/firmware.frm of=/dev/mtdblock3 bs=64k seek=${seek_blocks} conv=notrunc && \
			fw_setenv fit_size ${FRM_SIZE} && \
			fw_setenv jffs2_size_mib ${CONFIG_JFFS2_SIZE} && echo "Done" || echo "Failed"
		flash_indication_off
		rm -f /opt/firmware.frm
		sync
		exit 0
	else
		rm -f /opt/firmware.frm
		echo Failed Checksum error: $frm $md5
		exit 1
	fi
}



case "${FRM_FILE}" in
	*.frm)
		if [ -f "${FRM_FILE}" ] && [ -s "${FRM_FILE}" ]; then
			handle_frimware_frm "${FRM_FILE}" "${FRM_MAGIC}"
		fi
		;;
esac

echo "Failed"
exit 1
