# dash.scr: the Dash's boot, with over-the-air updates and automatic rollback (dash-update).
#
# The card's boot partition (p1, FAT) holds the factory image: zImage, dash-r0.dtb, rootfs.cpio.uboot. OTA never
# rewrites it, so it is always there to fall back to. Updates live on /data (p3, ext4) as /boot/A and /boot/B,
# each the same three files. The state is ota.txt on p1, `name=value` lines that dash-update and this script edit:
#   slot       what to boot: A, B, or empty = the factory image
#   good       the last slot that proved itself (dash-update confirm), or empty = the factory image
#   trial      1 while `slot` is new and unproven
#   bootcount  starts of the slot on trial so far
#   failed     the slot last given up on (set here at a rollback; dash-update stops auto-installing that version)
# A trial not confirmed after 3 starts goes back to `good`. A slot whose files do not load, or whose kernel does not
# start, falls through to the factory image at once (and counts as one of the 3).
# Compile: mkimage -A arm -O linux -T script -C none -d dash.cmd dash.scr   (U-Boot's bootcmd sources it from p1)
#
# Addresses: this script runs at ${scriptaddr}, so ota.txt goes through ${kernel_addr_r} (before the kernel is loaded).

setenv slot
setenv good
setenv trial
setenv bootcount
setenv failed
if fatload mmc 0:1 ${kernel_addr_r} ota.txt; then env import -t ${kernel_addr_r} ${filesize}; fi

if test -n "${trial}"; then
	if test -z "${bootcount}"; then setenv bootcount 0; fi
	setexpr bootcount ${bootcount} + 1
	if test ${bootcount} -gt 3; then
		echo "dash: slot ${slot} not confirmed after 3 starts: back to slot ${good} (none = the factory image)"
		setenv failed ${slot}
		setenv slot ${good}
		setenv trial
		setenv bootcount 0
	fi
	env export -t ${kernel_addr_r} slot good trial bootcount failed
	# a start that cannot be counted is no trial: a version that never comes up would start for ever, never rolled back
	fatwrite mmc 0:1 ${kernel_addr_r} ota.txt ${filesize} || setenv slot ${good}
fi

setenv initrd_high 0xffffffff
if test -n "${slot}"; then
	setenv bootargs console=ttyS0,115200 earlycon panic=5 dash.slot=${slot}
	if ext4load mmc 0:3 ${kernel_addr_r} /boot/${slot}/zImage && ext4load mmc 0:3 ${fdt_addr_r} /boot/${slot}/dash-r0.dtb && ext4load mmc 0:3 ${ramdisk_addr_r} /boot/${slot}/rootfs.cpio.uboot; then
		echo "dash: starting slot ${slot}"
		bootz ${kernel_addr_r} ${ramdisk_addr_r} ${fdt_addr_r}
	fi
	echo "dash: slot ${slot} did not start: factory image"
fi
setenv bootargs console=ttyS0,115200 earlycon panic=5 dash.slot=
load mmc 0:1 ${kernel_addr_r} zImage && load mmc 0:1 ${fdt_addr_r} dash-r0.dtb && load mmc 0:1 ${ramdisk_addr_r} rootfs.cpio.uboot && bootz ${kernel_addr_r} ${ramdisk_addr_r} ${fdt_addr_r}
echo "dash: the factory image did not start"
sleep 3
reset
