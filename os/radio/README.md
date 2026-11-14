# RTL8733BS Wi-Fi driver source (GPL-2.0)

The firmware image ships two builds of Realtek's vendor Wi-Fi driver for the RTL8733BS: `8733bs.ko` (station only) and `8733bs_cc.ko` (station + access point, loaded at boot for wireless Android Auto). The driver is licensed **GPL-2.0** (the file headers; `MODULE_LICENSE("GPL")`), and its Wi-Fi firmware is embedded in this source.

- **Upstream**: Realtek's release `rtl8733BS_WiFi_linux_v5.15.17-113-g1924716b3.20250521_COEX20241204-390f` (the tarball's top directory). sha256 of the tarball this tree was unpacked from: `f6fe6b9cba149ae653d239cb88db544bbde8742a32b502fd7c0104617d6bc329`.
- **`src/work/`**: the exact tree the image builds = upstream + `patches/rtl8733bs-linux-7.3.4.patch` (builds against Linux 7.3.4: kbuild flags, the cfg80211 API of Linux 7.x and the per-link CAC state, a local `strncpy`, the driver's SHA-256 helpers renamed `rtw_*` so they don't clash with the kernel's; station-only safeties: no AP/ad-hoc ops and no MAC ACL in that build).
- **`8733bs_cc.ko`**: the same tree, with `../package/rtl8733bs-cc/8733bs-cc-ap-mcc.patch` applied to the build copy (concurrent mode: a second interface for the access point, the AP ops back, Bluetooth coexistence kept on).
- **Build recipe**: the Buildroot packages `../package/rtl8733bs/` and `../package/rtl8733bs-cc/` (kernel modules, their make options in the `.mk` files). They read the source from `/radio/src/work`: mount this `radio/` directory at `/radio` in the build container.

Bluetooth uses the kernel's own `btrtl` driver with `../board/dash/patches/linux/7.3.4/0004-btrtl-add-rtl8733bs.patch`. The Bluetooth firmware is **not** shipped: the Dash cuts it from its own stock firmware at first boot (`bt-fwcutter`).
