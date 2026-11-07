# R0 over FEL (c.5): sunxi-fel wrote zImage, the DTB and the initramfs to these addresses
setenv bootargs console=ttyS0,115200 earlycon
bootz 0x42000000 0x43300000 0x43000000
