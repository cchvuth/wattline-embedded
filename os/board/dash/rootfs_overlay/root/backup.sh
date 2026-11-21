# R0 backup helpers: cardA(), bp() and b4(). Source it: . /root/backup.sh
# The route: r0mount; D=/dev/mmcblkN (the MMC-type node); b4 /mnt/backup; then the hashes.
r0mount() {                                # mount card D's data partition (p2, label DASHDATA) read-write at /mnt/backup
  for d in /sys/block/mmcblk[0-9]; do [ "$(cat $d/device/type)" = SD ] && S=/dev/${d##*/}p2; done
  [ -b "$S" ] || { echo 'no SD card partition 2'; return 1; }
  mkdir -p /mnt/backup && mount -t vfat "$S" /mnt/backup && df -h /mnt/backup
}
emmcnode() { for d in /sys/block/mmcblk[0-9]; do [ "$(cat $d/device/type)" = MMC ] && echo /dev/${d##*/}; done; }
cardA() {                                  # usage: cardA [MOUNTPOINT]   default /mnt/extsd
  MP=${1:-/mnt/extsd}
  CA=$(awk -v m="$MP" '$2==m{print $1}' /proc/mounts)
  [ -n "$CA" ] || { echo "card A is not mounted at $MP"; return 1; }
  CN=$(basename $CA | sed 's/p[0-9]*$//')
  [ "$(cat /sys/block/$CN/device/type 2>/dev/null)" = SD ] || { echo 'the mounted device is not an SD card'; return 1; }
  awk -v m="$MP" '$2==m{print $3}' /proc/mounts | grep -qx vfat || { echo 'card A is not vfat'; return 1; }
  FREEKB=$(df -Pk "$MP" | awk 'NR==2{print $4}')
}
bp() {
  for p in boot0 boot1; do
    [ -b ${1}$p ] || continue
    dd if=${1}$p of=$2/$p.bin bs=1k || return 1
    sync; echo 3 > /proc/sys/vm/drop_caches
    a=$(dd if=${1}$p bs=1k 2>/dev/null | sha256sum); sync; echo 3 > /proc/sys/vm/drop_caches; c=$(dd if=${1}$p bs=1k 2>/dev/null | sha256sum)
    [ "$a" = "$c" ] && [ "$a" = "$(sha256sum < $2/$p.bin)" ] || { echo "$p: the three reads differ"; return 1; }
  done
}
b4() {                                         # usage: b4 [MOUNTPOINT]   default /mnt/extsd (stock); in R0 /mnt/backup
  MP=${1:-/mnt/extsd}
  D=${D:-/dev/mmcblk0}                          # [check] the eMMC's: size, CID, has ${D}boot0
  N=${D##*/}; [ "$(cat /sys/block/$N/device/type 2>/dev/null)" = MMC ] || { echo "$D is not an eMMC"; return 1; }
  SECT=$(cat /sys/block/$N/size)
  cardA $MP || return 1
  case "$CA" in /dev/${N}*) echo 'card A is the eMMC'; return 1;; esac
  [ "$FREEKB" -ge $((SECT/2 + 262144)) ] || { echo 'card A too small'; return 1; }
  O=$MP/backup; mkdir -p $O || return 1
  echo $SECT > $O/sectors.txt; cat /sys/block/$N/device/cid > $O/cid.txt; cat /sys/block/$N/device/csd > $O/csd.txt
  mount -t debugfs none /sys/kernel/debug 2>/dev/null
  cat /sys/kernel/debug/mmc*/mmc*:0001/ext_csd > $O/ext_csd.hex
  [ "$(tr -d ' \n' < $O/ext_csd.hex | wc -c)" -eq 1024 ] || { echo 'EXT_CSD missing or short: stop (see the note below)'; return 1; }
  ls /dev/mmcblk*gp* 2>/dev/null && { echo 'general-purpose partitions exist: dump them too'; return 1; }
  i=0; MB=$(( (SECT + 2047) / 2048 ))
  while [ $((i*1024)) -lt $MB ]; do
    dd if=$D of=$O/emmc.part$(printf %02d $i) bs=1M skip=$((i*1024)) count=1024 || return 1
    i=$((i+1))
  done
  bp $D $O || return 1
  cat /proc/mtd                                 # anything listed? dump each: cat /dev/mtdN > $O/mtdN.bin (NOR); nanddump for NAND; then read each node twice, each read after `sync; echo 3 > /proc/sys/vm/drop_caches`: the two sha256sum values and the hash of the dump file must be equal, as bp() does
  sync
}
