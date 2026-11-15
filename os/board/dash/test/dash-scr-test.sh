#!/bin/sh
# dash-scr-test.sh UBOOT_SRC DASH_CMD: run the real boot script (dash.cmd) in U-Boot's sandbox against a card-like
# disk image (p1 FAT with the factory files + ota.txt, p3 ext4 with /boot/A and /boot/B), once per case, and check
# what it boots and what it leaves in ota.txt. Only bootz and reset are stubbed (to print what would start).
# Needs: a build toolchain, bison, flex, libssl-dev, python3, dosfstools, mtools, e2fsprogs, sfdisk.
set -e
SRC=$1; CMD=$2; W=${W:-/tmp/scrtest}; FAIL=0
mkdir -p $W && cd $W
if [ ! -x u-boot/u-boot ]; then
	rm -rf u-boot && cp -a "$SRC" u-boot
	make -C u-boot -s distclean >/dev/null 2>&1 || true
	make -C u-boot -s sandbox_defconfig >/dev/null
	# nothing here is EFI: drop the capsule signing that would need efitools
	u-boot/scripts/config --file u-boot/.config -d EFI_CAPSULE_AUTHENTICATE -d EFI_CAPSULE_ON_DISK
	make -C u-boot -s olddefconfig >/dev/null
	make -C u-boot -s -j"$(nproc)" NO_SDL=1 >/dev/null 2>&1 || { make -C u-boot -j"$(nproc)" NO_SDL=1 2>&1 | tail -20; exit 1; }
fi

# the script under test, on the sandbox's "host" disk instead of mmc; bootz/reset stubbed to print + stop
sed -e 's/mmc 0:/host 0:/g' \
    -e 's/^\(\t*\)bootz .*/\1echo "RESULT boot slot=${slot}"; exit/' \
    -e 's/ && bootz .*/ \&\& echo "RESULT boot factory" \&\& exit/' \
    -e 's/^reset$/echo "RESULT reset"/' "$CMD" > dash-test.cmd
u-boot/tools/mkimage -A sandbox -O linux -T script -C none -d dash-test.cmd dash.scr >/dev/null

mkcard() {   # mkcard "OTA_TXT" "SLOTS"  (SLOTS: which of A B have their files)
	rm -f card.img p1.img p3.img; rm -rf p3
	dd if=/dev/zero of=p1.img bs=1M count=64 2>/dev/null; mkfs.vfat -F 32 -n BOOT p1.img >/dev/null   # FAT32, as on the card
	for f in zImage dash-r0.dtb rootfs.cpio.uboot; do echo "factory $f" > $f; mcopy -i p1.img $f ::$f; done
	mcopy -i p1.img dash.scr ::dash.scr
	[ -n "$1" ] && printf '%s\n' $1 > ota.txt && mcopy -i p1.img ota.txt ::ota.txt
	mkdir -p p3/boot; for s in $2; do mkdir -p p3/boot/$s; for f in zImage dash-r0.dtb rootfs.cpio.uboot; do echo "slot $s $f" > p3/boot/$s/$f; done; done
	dd if=/dev/zero of=p3.img bs=1M count=16 2>/dev/null; mkfs.ext4 -q -d p3 p3.img
	dd if=/dev/zero of=card.img bs=1M count=84 2>/dev/null
	printf 'start=2048,size=131072,type=c\nstart=133120,size=4096,type=c\nstart=137216,size=32768,type=83\n' | sfdisk -q card.img
	dd if=p1.img of=card.img bs=512 seek=2048 conv=notrunc 2>/dev/null
	dd if=p3.img of=card.img bs=512 seek=137216 conv=notrunc 2>/dev/null
}
run() {   # run NAME "OTA_TXT" "SLOTS" "WANT_RESULT" "WANT_OTA_TXT"
	mkcard "$2" "$3"
	out=$(u-boot/u-boot -c "host bind 0 card.img; setenv kernel_addr_r 0x1000000; setenv fdt_addr_r 0x1800000; setenv ramdisk_addr_r 0x1C00000; setenv scriptaddr 0x1900000; load host 0:1 \${scriptaddr} dash.scr && source \${scriptaddr}" 2>&1)
	got=$(echo "$out" | tr -d '\r' | grep -o 'RESULT .*' | head -1)   # sandbox U-Boot ends lines with CR
	dd if=card.img of=p1.out bs=512 skip=2048 count=131072 2>/dev/null
	ota=$(mtype -i p1.out ::ota.txt 2>/dev/null | tr -d '\0\r' | grep -v '^$' | sort | tr '\n' ' ')
	if [ "$got" = "RESULT $4" ] && [ "$ota" = "$5" ]; then echo "PASS $1"
	else echo "FAIL $1: got [$got] ota.txt [$ota], want [RESULT $4] [$5]"; echo "$out" | grep -E "dash:|Error|error|Unknown" | head -5; FAIL=1; fi
}
#   name                          ota.txt in                              slots   boots            ota.txt after (sorted)
run "no ota.txt: factory"         ""                                      "A"     "boot factory"   ""
run "confirmed slot A"            "slot=A good=A bootcount=0"             "A"     "boot slot=A"    "bootcount=0 good=A slot=A "
run "trial: first start counted"  "slot=B good=A trial=1 bootcount=0"     "A B"   "boot slot=B"    "bootcount=1 good=A slot=B trial=1 "
run "trial: third start"          "slot=B good=A trial=1 bootcount=2"     "A B"   "boot slot=B"    "bootcount=3 good=A slot=B trial=1 "
run "trial failed: back to good"  "slot=B good=A trial=1 bootcount=3"     "A B"   "boot slot=A"    "bootcount=0 failed=B good=A slot=A "
run "trial failed, good=factory"  "slot=A trial=1 bootcount=3"            "A"     "boot factory"   "bootcount=0 failed=A "
run "slot files missing: factory" "slot=B good=B bootcount=0"             "A"     "boot factory"   "bootcount=0 good=B slot=B "
run "trial w/o bootcount"         "slot=A trial=1"                        "A"     "boot slot=A"    "bootcount=1 slot=A trial=1 "
run "trial keeps failed"          "slot=B good=A trial=1 failed=A"        "A B"   "boot slot=B"    "bootcount=1 failed=A good=A slot=B trial=1 "   # dash-update drops it
[ $FAIL = 0 ] && echo "ALL PASS" || { echo "SOME FAILED"; exit 1; }
