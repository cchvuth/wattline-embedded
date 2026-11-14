# Wattline — embedded T113 head unit

- `os/`  — Buildroot firmware (kernel, packages, overlays; R0–R4)
- `app/` — LVGL head-unit application (dashboard UI, wireless Android Auto receiver)

See the [latest release](https://github.com/cchvuth/wattline-embedded-t113/releases) to download a card image and burn it (`sdcard.img.xz`). The stock eMMC is never touched — pull the card and the stock firmware boots.

## Android Auto certificate

Wireless Android Auto requires a **Google-signed head-unit certificate**: the phone pins Google's Android Auto CA, so a self-signed certificate is rejected at the TLS handshake ("communication error 7"). Supply your own `aa.crt` / `aa.key` (PEM).

Place them on the Dash at — and they persist across reboot **and** reflash (`/data` is a separate partition):

```
/data/wattline/aa.crt    # PEM certificate (leaf; append the CA chain if the phone wants it)
/data/wattline/aa.key    # PEM private key   (chmod 600)
```

How to put the files there:

- **Over SSH** (enable SSH in **Settings → Advanced**, pair a key):
  ```
  scp aa.crt aa.key root@<dash-ip>:/data/wattline/
  ```
- **Or** power off, take the card out, and mount its data partition (ext4, label `DASHDATA2`) on a computer, then copy the files into `wattline/`.

If no cert is present the Dash generates a self-signed placeholder so the on-screen **Android Auto** button still appears; a real certificate replaces it. The button is in the dashboard's bottom-right — tapping it starts Android Auto.
