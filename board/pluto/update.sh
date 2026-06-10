#!/bin/sh

source /etc/device_config

file=/sys/kernel/config/usb_gadget/composite_gadget/functions/mass_storage.0/lun.0/file
bootimage=/mnt/msd/boot.frm
conf=/mnt/msd/config.txt
jffs2conf=/mnt/msd/config.frm
img=/opt/vfat.img

JFFS2_BASE=1179648
JFFS2_CONFIG_ERROR=/mnt/msd/FAILED_JFFS2_CONFIG_ERROR

ini_parser() {
 FILE=$1
 SECTION=$2
 eval `sed -e 's/[[:space:]]*\=[[:space:]]*/=/g' \
    -e 's/[#;\`].*$//' \
    -e 's/[[:space:]]*$//' \
    -e 's/^[[:space:]]*//' \
    -e "s/^\(.*\)=\([^\"']*\)$/\1=\"\2\"/" \
   < $FILE \
    | sed -n -e "/^\[$SECTION\]/,/^\s*\[/{/^[^;].*\=.*/p;}"`
}

reset() {
	echo "REBOOT/RESET using Watchdog timeout"
	flash_indication_off
	sync
	device_reboot reset
	sleep 10
}

dfu() {
	echo "Entering DFU mode using SW Reset"
	flash_indication_off
	sync
	device_reboot sf
}

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
	do_reset=0
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

make_diagnostic_report () {
	FILE=$1
	cat  /opt/VERSIONS /etc/os-release /var/log/messages /proc/cpuinfo /proc/interrupts /proc/iomem /proc/meminfo /proc/cmdline /sys/kernel/debug/clk/clk_summary > ${FILE}
	grep -r "" /sys/kernel/debug/regmap/ >> ${FILE} 2>&1
	iio_info >> ${FILE} 2>&1
	ifconfig -a >> ${FILE} 2>&1
	mount >> ${FILE} 2>&1
	top -b -n1  >> ${FILE} 2>&1
	fw_printenv >> ${FILE} 2>&1
	unix2dos ${FILE}
}

calibrate () {
	/usr/sbin/ad936x_ref_cal -e $1 ad9361-phy
	if [ $? -eq 0 ]; then

		for dev in /sys/bus/iio/devices/*; do
			[ `cat ${dev}/name` == "ad9361-phy" ] && DEV_NAME=`basename ${dev}`
		done
		NEW_XO=`cat /sys/bus/iio/devices/${DEV_NAME}/xo_correction`
		flash_indication_on
		fw_setenv xo_correction $NEW_XO
		flash_indication_off

		sed -i -e "s/^xo_correction .*$/xo_correction = $NEW_XO/" -e "s/^calibrate .*$/calibrate = 0/" $conf
	else
		sed -i -e "s/^calibrate .*$/calibrate = 0/" $conf
	fi
}

process_ini() {
	FILE=$1

	ini_parser $FILE "NETWORK"
	ini_parser $FILE "WLAN"
	ini_parser $FILE "SYSTEM"
	ini_parser $FILE "USB_ETHERNET"

	rm -f /mnt/msd/SUCCESS_ENV_UPDATE /mnt/msd/FAILED_INVALID_UBOOT_ENV /mnt/msd/CAL_STATUS


	fw_printenv qspiboot
	if [ $? -eq 0 ]; then
		flash_indication_on
		echo "hostname $hostname" > /opt/fw_set.tmp
		echo "ipaddr $ipaddr" >> /opt/fw_set.tmp
		echo "ipaddr_host $ipaddr_host" >> /opt/fw_set.tmp
		echo "netmask $netmask" >> /opt/fw_set.tmp
		echo "ssid_wlan $ssid_wlan" >> /opt/fw_set.tmp
		echo "ipaddr_wlan $ipaddr_wlan" >> /opt/fw_set.tmp
		echo "pwd_wlan $pwd_wlan" >> /opt/fw_set.tmp
		echo "xo_correction $xo_correction" >> /opt/fw_set.tmp
		echo "udc_handle_suspend $udc_handle_suspend" >> /opt/fw_set.tmp
		echo "usb_ethernet_mode $usb_ethernet_mode" >> /opt/fw_set.tmp
		echo "ipaddr_eth $ipaddr_eth" >> /opt/fw_set.tmp
		echo "netmask_eth $netmask_eth" >> /opt/fw_set.tmp
		fw_setenv -s /opt/fw_set.tmp
		rm /opt/fw_set.tmp
		flash_indication_off
		touch /mnt/msd/SUCCESS_ENV_UPDATE
	else
		touch /mnt/msd/FAILED_INVALID_UBOOT_ENV
	fi

	ini_parser $FILE "ACTIONS"

	if [ "$reset" == "1" ]
	then
		reset
	fi

	if [ "$dfu" == "1" ]
	then
		dfu
	fi

	if [ "$diagnostic_report" == "1" ]
	then
		make_diagnostic_report /mnt/msd/diagnostic_report
	fi

	if [ "$calibrate" -gt "70000000" ]
	then
		calibrate $calibrate > /mnt/msd/CAL_STATUS
	fi

	echo here_1: > /mnt/msd/status

	if [ "$revert_passwd" == "1" ]
	then
		echo here $revert_passwd >> /mnt/msd/status
		rm -f /mnt/jffs2/etc/passwd /mnt/jffs2/etc/group /mnt/jffs2/etc/password.md5 /mnt/jffs2/etc/shadow 2>&1 >> /mnt/msd/status
		echo here_2: >> /mnt/msd/status
	fi

	md5sum $FILE > /opt/config.md5
}

handle_boot_frm () {
	FILE="$1"
	rm -f /mnt/msd/BOOT_SUCCESS /mnt/msd/BOOT_FAILED /mnt/msd/FAILED_MTD_PARTITION_ERROR /mnt/msd/FAILED_BOOT_CHSUM_ERROR
	head -2 /proc/mtd | sed 's/00010000/00001000/g' > /opt/mtd

	md5=`tail -c 33 ${FILE}`
	head -c -33 ${FILE} > /opt/boot_and_env_and_mtdinfo.bin

	tail -c 1024 /opt/boot_and_env_and_mtdinfo.bin | head -2 > /opt/mtd-info.txt
	head -c -1024 /opt/boot_and_env_and_mtdinfo.bin > /opt/boot_and_env.bin

	tail -c 131072 /opt/boot_and_env.bin > /opt/u-boot-env.bin
	head -c -131072 /opt/boot_and_env.bin > /opt/boot.bin

	frm=`md5sum /opt/boot_and_env_and_mtdinfo.bin | cut -d ' ' -f 1`

	if [ "$frm" = "$md5" ]
	then
		diff -w /opt/mtd /opt/mtd-info.txt
		if [ $? -eq 0 ]; then
			flash_indication_on
			dd if=/opt/boot.bin of=/dev/mtdblock0 bs=64k && dd if=/opt/u-boot-env.bin of=/dev/mtdblock1 bs=64k && do_reset=1 && touch /mnt/msd/BOOT_SUCCESS || touch /mnt/msd/BOOT_FAILED
			flash_indication_off
		else
			cat /opt/mtd /opt/mtd-info.txt > /mnt/msd/FAILED_MTD_PARTITION_ERROR
			do_reset=0
		fi
	else
		echo $md5 $frm >  /mnt/msd/FAILED_BOOT_CHSUM_ERROR
		do_reset=0
	fi

	rm -f ${FILE} /opt/boot_and_env_and_mtdinfo.bin /opt/mtd-info.txt /opt/boot_and_env.bin /opt/u-boot-env.bin /opt/boot.bin /opt/mtd
}



handle_frimware_frm () {
	FILE="$1"
	MAGIC="$2"
	rm -f /mnt/msd/SUCCESS /mnt/msd/FAILED /mnt/msd/FAILED_FIRMWARE_CHSUM_ERROR ${JFFS2_CONFIG_ERROR}
	md5=`tail -c 33 ${FILE}`
	head -c -33 ${FILE} > /opt/firmware.frm
	FRM_SIZE_DEC=`cat /opt/firmware.frm | wc -c | xargs`
	FRM_SIZE=`printf "%X\n" ${FRM_SIZE_DEC}`
	frm=`md5sum /opt/firmware.frm | cut -d ' ' -f 1`
	if [ "$frm" = "$md5" ]
	then
		if validate_jffs2_config && grep -q "${MAGIC}" /opt/firmware.frm; then
			set_jffs2_layout_values "${CONFIG_JFFS2_SIZE}"
			if [ "${FRM_SIZE_DEC}" -gt "${JFFS2_FIT_PARTITION_SIZE}" ]; then
				jffs2_config_error "Firmware image is too large for the selected JFFS2 layout."
				touch /mnt/msd/FAILED
			else
				flash_indication_on
				if prepare_jffs2_layout; then
					seek_blocks=$((FIRMWARE_SEEK_BYTES / 65536))
					dd if=/opt/firmware.frm of=/dev/mtdblock3 bs=64k seek=${seek_blocks} conv=notrunc && \
						fw_setenv fit_size ${FRM_SIZE} && \
						fw_setenv jffs2_size_mib ${CONFIG_JFFS2_SIZE} && \
						do_reset=1 && touch /mnt/msd/SUCCESS || touch /mnt/msd/FAILED
				else
					touch /mnt/msd/FAILED
				fi
				flash_indication_off
			fi
		else
			touch /mnt/msd/FAILED
		fi
	else
		echo $frm $md5 > /mnt/msd/FAILED_FIRMWARE_CHSUM_ERROR
		do_reset=0
	fi

	rm -f ${FILE} /opt/firmware.frm
	sync
}

while [ 1 ]
do
 if [[ -r ${file} ]]
  then
    lun=`cat $file`
    if [ ${#lun} -eq 0 ]
    then
	do_reset=0
	losetup /dev/loop7 $img -o 512
	mkdir -p /mnt/msd
	mount /dev/loop7 /mnt/msd

	if [[ -s /mnt/msd/$TARGET-fw-*.zip ]]
	then
		mv /mnt/msd/$TARGET-fw-*.zip /opt/
		firmware_base=`basename ${FIRMWARE}`
		unzip -o /opt/$TARGET-fw-*.zip ${firmware_base} boot.frm -d /mnt/msd
		if [[ ! -s ${jffs2conf} ]]; then
			unzip -o /opt/$TARGET-fw-*.zip config.frm -d /mnt/msd
		fi
		rm /opt/$TARGET-fw-*.zip
	fi

	if [[ -s ${FIRMWARE} ]]
	then
		handle_frimware_frm "${FIRMWARE}" "${FRM_MAGIC}"
	fi

	if [[ -s ${bootimage} ]]
	then
		handle_boot_frm "${bootimage}"
	fi

	md5sum -c /opt/config.md5 || process_ini $conf

	if [ "$TARGET" == "m2k" ]; then
		if [[ -s /mnt/msd/${CALIBFILENAME} ]]; then
			md5sum -c /opt/${CALIBFILENAME}.md5
			if [ $? -ne 0 ]; then
				cp  /mnt/msd/${CALIBFILENAME} /mnt/jffs2/${CALIBFILENAME} && do_reset=1
			fi

		else
			rm /mnt/jffs2/${CALIBFILENAME} && do_reset=1
		fi

		if [[ -s /mnt/msd/${CALIBFILENAME_FACTORY} ]]; then
			if [[ ! -s /mnt/jffs2/${CALIBFILENAME_FACTORY} ]]; then
				cp /mnt/msd/${CALIBFILENAME_FACTORY} /mnt/jffs2/${CALIBFILENAME_FACTORY}
					do_reset=1
			fi
		fi

		if [[ -s /mnt/msd/${CALIBFILENAME_TEMP_LUT} ]]; then
			cp /mnt/msd/${CALIBFILENAME_TEMP_LUT} /mnt/jffs2/${CALIBFILENAME_TEMP_LUT}
			do_reset=1
		fi
	fi

	if [[ $do_reset = 1 ]]
	then
		reset
	fi

	cp /opt/ipaddr-* /mnt/msd 2>/dev/null

	umount /mnt/msd
	#losetup -d /dev/loop7
	echo $img > $file
	flash_indication_off
	sleep 1
    fi
fi

sleep 1

done

exit 1
