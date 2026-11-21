// SPDX-License-Identifier: GPL-2.0
/*
 * g2drot: the dash's flush through the G2D's rotation unit (Allwinner D1/D1s/F133: 0x05410000, as the stock Melis's
 * g2d_probe; registers from Allwinner's g2d_rcq: RT-Thread's sunxi-hal g2d_top_type.h, g2d_rotate.c). The dash draws a
 * landscape canvas for a portrait panel: the unit turns each drawn area into the framebuffer (~6 ns a pixel, with the
 * cache work: 750 MB/s in and out) while the CPU draws the next one; the CPU's own rotation took 26-42 ns a pixel.
 *   open()       powers the unit: the CCU's bus, module (300 MHz) and MBUS gates, its reset (CCU 0x63C bit 16)
 *                released before any of its registers is touched (an unclocked block can hang the bus)
 *   mmap()       the caller's draw buffers: physically contiguous (CMA), cached
 *   G2DROT_FB    the destination: the caller's mapping of the framebuffer, resolved to its (contiguous) pages
 *   G2DROT_BLIT  one area: waits for the previous job, readies the caches, starts this one, returns
 *   G2DROT_WAIT  until the last job is done (the unit's done bit polled: no interrupt needed)
 * The unit (version 0x0110 here): 90 and 270 degrees of 32-bit pixels, any width and height, any source pitch; the
 * destination's pitch must be a multiple of 8 bytes (an odd pixel count garbled its rows; never wrote outside them).
 * One user at a time (root). A job that runs past 100 ms resets the unit and reports -ETIMEDOUT (the dash falls back
 * to its own rotation).
 * ponytail: the framebuffer's pages are taken as fixed (DRM's fbdev buffer lives as long as the system); a mode
 * change that reallocates it would need a reference held on it.
 */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/uaccess.h>
#include "g2drot.h"

#define CCU_BASE	0x02001000
#define CCU_G2D_BGR	0x63c	/* bit 0 bus gate (the clock framework's), bit 16 reset (1 = released) */
#define G2D_BASE	0x05410000
#define G2D_SIZE	0x30000
#define TOP_SCLK	0x00	/* bit 0 mixer, bit 1 rotate: module clock gates */
#define TOP_HCLK	0x04	/* their bus gates */
#define TOP_RST		0x08	/* their resets, 1 = released */
#define TOP_VER		0x10	/* bit 8: a rotate unit */
#define ROT		0x28000
#define ROT_CTL		(ROT + 0x00)	/* bit 0 enable, 4-5 quarter turns clockwise, 31 start */
#define ROT_INT		(ROT + 0x04)	/* bit 0 done (write 1 to clear), bit 16 its interrupt enable */
#define ROT_IFMT	(ROT + 0x20)	/* 0: ARGB8888 */
#define ROT_ISIZE	(ROT + 0x24)	/* (h-1) << 16 | (w-1) */
#define ROT_IPITCH0	(ROT + 0x30)
#define ROT_ILADD0	(ROT + 0x40)
#define ROT_IHADD0	(ROT + 0x44)
#define ROT_OSIZE	(ROT + 0x84)
#define ROT_OPITCH0	(ROT + 0x90)
#define ROT_OLADD0	(ROT + 0xa0)
#define ROT_OHADD0	(ROT + 0xa4)
#define BUF_MAX		(8 << 20)	/* the first frame drawn whole (the canvas: 1920 x 720 x 4) */

static void __iomem *ccu, *g2d;
static struct clk *c_bus, *c_mod, *c_mbus;
static struct platform_device *pdev;
static DEFINE_MUTEX(lock);
static bool opened, busy;
static struct page *buf;	/* the caller's draw buffers */
static size_t buf_len;
static dma_addr_t buf_dma;
static phys_addr_t fb_phys;	/* the framebuffer (0: not named yet) */
static u32 fb_len, fb_pitch, fb_w, fb_h;

/* the clocks on and the reset released (true), or all off again: its registers are touched only after true */
static bool clocks_on(void)
{
	if (clk_prepare_enable(c_bus))
		return false;
	writel(readl(ccu + CCU_G2D_BGR) | BIT(16), ccu + CCU_G2D_BGR);
	udelay(10);
	if (clk_prepare_enable(c_mod))
		goto bus;
	if (clk_prepare_enable(c_mbus))
		goto mod;
	if ((readl(ccu + CCU_G2D_BGR) & 0x10001) == 0x10001)
		return true;
	clk_disable_unprepare(c_mbus);
mod:
	clk_disable_unprepare(c_mod);
bus:
	writel(readl(ccu + CCU_G2D_BGR) & ~BIT(16), ccu + CCU_G2D_BGR);
	clk_disable_unprepare(c_bus);
	return false;
}

static void clocks_off(void)
{
	clk_disable_unprepare(c_mbus);
	clk_disable_unprepare(c_mod);
	writel(readl(ccu + CCU_G2D_BGR) & ~BIT(16), ccu + CCU_G2D_BGR);
	clk_disable_unprepare(c_bus);
}

static bool unit_on(void)
{
	if (!clocks_on())
		return false;
	writel(3, g2d + TOP_SCLK);
	writel(3, g2d + TOP_HCLK);
	writel(3, g2d + TOP_RST);
	return true;
}

static void unit_off(void)
{
	writel(0, g2d + ROT_INT);
	writel(0, g2d + TOP_RST);
	writel(0, g2d + TOP_HCLK);
	writel(0, g2d + TOP_SCLK);
	clocks_off();
}

/* the running job: done (0), or 100 ms gone (the unit reset, -ETIMEDOUT) */
static int job_wait(void)
{
	for (int i = 0; busy && i < 1500; i++) {
		if (readl(g2d + ROT_INT) & 1) {
			writel(BIT(16) | 1, g2d + ROT_INT);
			busy = false;
			return 0;
		}
		if (i < 20)
			udelay(5);	/* a strip's job is a few hundred us: look closely first */
		else
			usleep_range(50, 100);
	}
	if (!busy)
		return 0;
	pr_err("g2drot: job not done in 100 ms: rotate unit reset\n");
	writel(1, g2d + TOP_RST);
	writel(3, g2d + TOP_RST);
	busy = false;
	return -ETIMEDOUT;
}

/* The unit reads the source from memory and writes the framebuffer there, past the CPU's cache: the draw buffer's
 * dirty lines are written back first, and any line of the destination the cache holds dropped, else the unit reads
 * stale pixels and a dirty framebuffer line evicted later puts old pixels over its picture (DRM's fbdev mapping is
 * cached here: the display engine's node sits outside the soc node that says dma-noncoherent). g2dtest saw both: with
 * no clean every area came out stale; with no drop, the areas whose lines the CPU had read. Small areas line by line
 * (by physical address: th.dcache.cpa, cipa), big ones by the whole cache (th.dcache.ciall, 32 KiB): line by line, a
 * strip of the Settings list took twice as long to start (0.7 against 0.36 ms).
 * ponytail: T-Head C906 cache instructions, so this module runs on that core only; dma_sync_* for any other. */
static void caches_for(phys_addr_t src, size_t n, phys_addr_t dst, u32 rows, u32 row_bytes)
{
	u32 per_row = (row_bytes + 2 * L1_CACHE_BYTES - 2) / L1_CACHE_BYTES;
	phys_addr_t a, e;

	if (n / L1_CACHE_BYTES + (u64)rows * per_row > 512) {	/* more lines than the cache holds */
		asm volatile(".long 0x0030000b" : : : "memory");	/* th.dcache.ciall */
	} else {
		for (a = src & ~(phys_addr_t)(L1_CACHE_BYTES - 1), e = src + n; a < e; a += L1_CACHE_BYTES)
			asm volatile("mv a0, %0\n\t.long 0x0295000b" : : "r"(a) : "a0", "memory");	/* th.dcache.cpa */
		for (u32 r = 0; r < rows; r++)
			for (a = (dst + (u64)r * fb_pitch) & ~(phys_addr_t)(L1_CACHE_BYTES - 1),
			     e = dst + (u64)r * fb_pitch + row_bytes; a < e; a += L1_CACHE_BYTES)
				asm volatile("mv a0, %0\n\t.long 0x02b5000b" : : "r"(a) : "a0", "memory");	/* th.dcache.cipa */
	}
	asm volatile(".long 0x0190000b" : : : "memory");	/* th.sync.s: done before the unit starts */
}

/* the pages looked up under mmap_lock alone, then lock taken to name them: mmap() is called with mmap_lock held and
 * takes lock, so lock is never held while waiting for mmap_lock */
static int set_fb(struct g2drot_fb *f)
{
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma;
	unsigned long first, pfn, a;
	int r = -EINVAL;

	if (!f->len || f->pitch % 8 || f->pitch < (u64)f->width * 4 || (u64)f->pitch * f->height > f->len)
		return -EINVAL;
	mmap_read_lock(mm);
	vma = vma_lookup(mm, f->va);
	if (!vma || !(vma->vm_flags & (VM_IO | VM_PFNMAP)) || f->va + f->len > vma->vm_end)
		goto out;
	for (a = f->va & PAGE_MASK; a < f->va + f->len; a += PAGE_SIZE) {	/* every page: one contiguous run */
		if (follow_pfn(vma, a, &pfn))
			goto out;
		if (a == (f->va & PAGE_MASK))
			first = pfn;
		else if (pfn != first + ((a - (f->va & PAGE_MASK)) >> PAGE_SHIFT))
			goto out;
	}
	r = 0;
out:
	mmap_read_unlock(mm);
	if (r)
		return r;
	mutex_lock(&lock);
	fb_phys = PFN_PHYS(first) + (f->va & ~PAGE_MASK);
	fb_len = f->len, fb_pitch = f->pitch, fb_w = f->width, fb_h = f->height;
	mutex_unlock(&lock);
	return 0;
}

static int blit(struct g2drot_blit *b)
{
	u64 src_end = (u64)b->off + (u64)(b->h - 1) * b->pitch + (u64)b->w * 4, off;
	phys_addr_t dst;
	int deg, r;

	if (!fb_phys || !buf || b->w < 1 || b->h < 1 || b->w > 8192 || b->h > 8192 || b->off % 4 || b->pitch % 4 ||
	    b->pitch < b->w * 4 || src_end > buf_len || b->x < 0 || b->y < 0 || b->x + b->w > fb_h || b->y + b->h > fb_w)
		return -EINVAL;
	if (b->rot == 270) {	/* a quarter turn clockwise: canvas row y is panel column width-1-y */
		deg = 1;
		off = (u64)b->x * fb_pitch + (u64)(fb_w - b->y - b->h) * 4;
	} else if (b->rot == 90) {
		deg = 3;
		off = (u64)(fb_h - b->x - b->w) * fb_pitch + (u64)b->y * 4;
	} else {
		return -EINVAL;
	}
	if (off + (u64)(b->w - 1) * fb_pitch + (u64)b->h * 4 > fb_len)	/* its w rows of h pixels: in the framebuffer */
		return -EINVAL;
	dst = fb_phys + off;
	r = job_wait();
	if (r)
		return r;
	caches_for(buf_dma + b->off, src_end - b->off, dst, b->w, b->h * 4);
	writel(1 | deg << 4, g2d + ROT_CTL);
	writel(0, g2d + ROT_IFMT);
	writel((b->h - 1) << 16 | (b->w - 1), g2d + ROT_ISIZE);
	writel(b->pitch, g2d + ROT_IPITCH0);
	writel(lower_32_bits(buf_dma + b->off), g2d + ROT_ILADD0);
	writel(upper_32_bits(buf_dma + b->off) & 0xff, g2d + ROT_IHADD0);
	writel((b->w - 1) << 16 | (b->h - 1), g2d + ROT_OSIZE);
	writel(fb_pitch, g2d + ROT_OPITCH0);
	writel(lower_32_bits(dst), g2d + ROT_OLADD0);
	writel(upper_32_bits(dst) & 0xff, g2d + ROT_OHADD0);
	writel(BIT(16) | 1, g2d + ROT_INT);	/* (IRQ 105 is not requested: the PLIC keeps it masked) */
	writel(readl(g2d + ROT_CTL) | BIT(31), g2d + ROT_CTL);
	busy = true;
	return 0;
}

static long g2drot_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct g2drot_blit b;
	struct g2drot_fb f;
	long r;

	switch (cmd) {
	case G2DROT_FB:
		if (copy_from_user(&f, (void __user *)arg, sizeof(f)))
			return -EFAULT;
		return set_fb(&f);	/* takes lock itself, after mmap_lock is let go */
	case G2DROT_BLIT:
		if (copy_from_user(&b, (void __user *)arg, sizeof(b)))
			return -EFAULT;
		mutex_lock(&lock);
		r = blit(&b);
		mutex_unlock(&lock);
		return r;
	case G2DROT_WAIT:
		mutex_lock(&lock);
		r = job_wait();
		mutex_unlock(&lock);
		return r;
	}
	return -ENOTTY;
}

static int g2drot_mmap(struct file *file, struct vm_area_struct *vma)
{
	size_t len = vma->vm_end - vma->vm_start;
	int r = -EBUSY;

	if (vma->vm_pgoff || !len || len > BUF_MAX)
		return -EINVAL;
	mutex_lock(&lock);
	if (buf)
		goto out;
	r = -ENOMEM;
	buf = dma_alloc_pages(&pdev->dev, len, &buf_dma, DMA_TO_DEVICE, GFP_KERNEL);
	if (!buf)
		goto out;
	buf_len = len;
	r = dma_mmap_pages(&pdev->dev, vma, len, buf);	/* cached, as any memory the CPU draws in */
	if (r) {
		dma_free_pages(&pdev->dev, len, buf, buf_dma, DMA_TO_DEVICE);
		buf = NULL;
	}
out:
	mutex_unlock(&lock);
	return r;
}

static int g2drot_open(struct inode *inode, struct file *file)
{
	int r = 0;

	mutex_lock(&lock);
	if (opened)
		r = -EBUSY;
	else if (!unit_on())
		r = -EIO;
	else
		opened = true;
	mutex_unlock(&lock);
	return r;
}

static int g2drot_release(struct inode *inode, struct file *file)	/* after the last mapping is gone too */
{
	mutex_lock(&lock);
	job_wait();
	unit_off();
	if (buf)
		dma_free_pages(&pdev->dev, buf_len, buf, buf_dma, DMA_TO_DEVICE);
	buf = NULL, fb_phys = 0, opened = false;
	mutex_unlock(&lock);
	return 0;
}

static const struct file_operations g2drot_fops = {
	.owner = THIS_MODULE,
	.open = g2drot_open,
	.release = g2drot_release,
	.mmap = g2drot_mmap,
	.unlocked_ioctl = g2drot_ioctl,
};

static struct miscdevice g2drot_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "g2drot",
	.fops = &g2drot_fops,
	.mode = 0600,
};

static struct clk *ccu_clk(struct device_node *np, int idx)
{
	struct of_phandle_args a = { .np = np, .args_count = 1, .args = { idx } };

	return of_clk_get_from_provider(&a);
}

static void put_all(void)
{
	if (!IS_ERR_OR_NULL(pdev))
		platform_device_unregister(pdev);
	if (g2d)
		iounmap(g2d);
	if (ccu)
		iounmap(ccu);
	if (!IS_ERR_OR_NULL(c_bus))
		clk_put(c_bus);
	if (!IS_ERR_OR_NULL(c_mod))
		clk_put(c_mod);
	if (!IS_ERR_OR_NULL(c_mbus))
		clk_put(c_mbus);
}

static int __init g2drot_init(void)
{
	struct device_node *np = of_find_compatible_node(NULL, NULL, "allwinner,sun20i-d1-ccu");
	u32 ver;
	int r;

	if (!np)
		return -ENODEV;
	c_bus = ccu_clk(np, 32);	/* CLK_BUS_G2D */
	c_mod = ccu_clk(np, 31);	/* CLK_G2D */
	c_mbus = ccu_clk(np, 53);	/* CLK_MBUS_G2D */
	of_node_put(np);
	ccu = ioremap(CCU_BASE, 0x1000);
	g2d = ioremap(G2D_BASE, G2D_SIZE);
	pdev = platform_device_register_simple("g2drot", -1, NULL, 0);
	r = -ENODEV;
	if (IS_ERR(c_bus) || IS_ERR(c_mod) || IS_ERR(c_mbus) || !ccu || !g2d || IS_ERR(pdev))
		goto fail;
	pdev->dev.dma_coherent = false;	/* the C906 does not snoop: riscv's devices default to coherent */
	r = dma_coerce_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (r)
		goto fail;
	clk_set_rate(c_mod, 300000000);
	r = -ENODEV;
	if (!clocks_on())	/* the version word, with the unit clocked and out of reset */
		goto fail;
	ver = readl(g2d + TOP_VER);
	clocks_off();
	if (!(ver >> 8 & 1)) {
		pr_err("g2drot: no rotate unit (version %08x)\n", ver);
		goto fail;
	}
	r = misc_register(&g2drot_misc);
	if (r)
		goto fail;
	pr_info("g2drot: G2D %04x, rotate unit at %08x\n", ver >> 16, G2D_BASE + ROT);
	return 0;
fail:
	put_all();
	return r;
}

static void __exit g2drot_exit(void)
{
	misc_deregister(&g2drot_misc);
	put_all();
}

module_init(g2drot_init);
module_exit(g2drot_exit);
MODULE_DESCRIPTION("G2D rotation for the dash's flush (Allwinner D1/F133)");
MODULE_LICENSE("GPL");
