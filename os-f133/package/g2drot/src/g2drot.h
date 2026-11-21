/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/* g2drot: the G2D rotation unit's interface (the F133's g2drot module; the dash's port/linux/fb_rot.c holds a copy) */
#ifndef G2DROT_H
#define G2DROT_H
#include <linux/ioctl.h>
#include <linux/types.h>
struct g2drot_fb {	/* the panel's framebuffer as the caller maps it: its pages are the destination */
	__u64 va;
	__u32 len, pitch, width, height;	/* bytes, bytes a row, the panel's own pixels (portrait) */
};
struct g2drot_blit {	/* one area of the landscape canvas: w x h 32-bit pixels at off in the mmap()ed buffer */
	__u32 off, pitch, w, h;
	__s32 x, y;		/* the area on the canvas (the panel turned: canvas w = panel height) */
	__u32 rot;		/* 270: canvas (x, y) is panel (width-1-y, x); 90: panel (y, height-1-x) */
};
#define G2DROT_FB	_IOW('G', 1, struct g2drot_fb)
#define G2DROT_BLIT	_IOW('G', 2, struct g2drot_blit)	/* starts it (after the previous one) and returns */
#define G2DROT_WAIT	_IO('G', 3)				/* until the last one is in the framebuffer */
#endif
