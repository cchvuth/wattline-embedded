// SPDX-License-Identifier: MIT
/*
 * bt-fwcutter: cut a Realtek Bluetooth firmware patch (and its UART config) out of a binary that embeds them,
 * typically the Bluetooth daemon of the device's own original system. Nothing is redistributed: the files are made
 * on the device, from the device's own software.
 *
 *   bt-fwcutter [-p PROJECT_ID] BINARY FW_OUT CONFIG_OUT
 *
 * No fixed offsets: BINARY is scanned for every "Realtech" (epatch v1) and "RTBTCore" (v2) signature, each candidate
 * is parsed with the layout Linux's drivers/bluetooth/btrtl.c reads, and only one that parses end to end is kept:
 *
 *   v1: "Realtech", le32 fw_version, le16 num_patches, le16 chip_id[n], le16 patch_length[n], le32 patch_offset[n],
 *       the patches (offsets from the file start), then the extension section ending in 51 04 fd 77.
 *   v2: "RTBTCore", u8 fw_version[8], le32 num_sections, sections {le32 opcode, le32 len, data[len]}, then the
 *       extension section (btrtl reads the sections from all but the last 7 bytes).
 *   Extension section, read backwards from its signature as btrtl does: {data, length, opcode} triples; opcode 0 with
 *   length 1 carries the project id (19: RTL8723F / RTL8733B), opcode 0xff ends it.
 *
 * The patch whose project id is PROJECT_ID (default 19) is written to FW_OUT as found. Several patches with that id
 * are accepted only when they are identical.
 *
 * Config (btrtl: le32 0x8723ab55, le16 total_len, entries {le16 offset, u8 len, data[len]}): every config block in
 * BINARY that parses exactly is read and its entry at offset 0x0c (UART: le32 baud word, flow control in data[12]
 * bit 2) taken; all of them must agree. CONFIG_OUT gets only that entry, so it never sets a Bluetooth address.
 *
 * Both outputs are written to a temporary file and renamed into place, the config first: FW_OUT existing means both
 * are complete. Exit 0 written, 1 usage or I/O error, 2 nothing usable found (or ambiguous).
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const uint8_t EXT_SIG[4] = { 0x51, 0x04, 0xfd, 0x77 };
static const uint8_t CFG_MAGIC[4] = { 0x55, 0xab, 0x23, 0x87 };
#define EXT_WINDOW 4096	/* how far past the patch data the extension signature may sit (reference: 63 bytes of padding) */

static uint8_t *d;
static size_t n;

static unsigned le16(size_t p) { return d[p] | d[p + 1] << 8; }
static uint32_t le32(size_t p) { return d[p] | d[p + 1] << 8 | d[p + 2] << 16 | (uint32_t)d[p + 3] << 24; }

/* btrtl's backward walk from the extension signature at sig (start: the patch start). Project id or -1. */
static int project_id(size_t start, size_t sig, size_t hdr)
{
	size_t p = sig;
	while (p >= start + hdr + 3) {
		uint8_t op = d[--p], len = d[--p], data = d[--p];
		if (op == 0xff)
			return -1;
		if (len == 0)
			return -1;
		if (op == 0 && len == 1)
			return data;
		if (p < len)
			return -1;
		p -= len;
	}
	return -1;
}

/* Parse the patch starting at i. Returns its length (0: not a valid patch); *pid gets the project id. */
static size_t parse_patch(size_t i, int *pid, uint32_t *ver)
{
	size_t end, hdr = 14;	/* end: first byte after the patch data/sections; hdr: btrtl's walk bound (v1 header size, both formats) */
	int v2 = !memcmp(d + i, "RTBTCore", 8);
	if (!v2) {
		if (i + 14 > n)
			return 0;
		unsigned np = le16(i + 12);
		size_t t = i + 14, h = 14 + 8 * (size_t)np;
		if (np == 0 || np > 64 || i + h > n)
			return 0;
		end = i + h;
		for (unsigned k = 0; k < np; k++) {
			size_t len = le16(t + 2 * np + 2 * k), off = le32(t + 4 * np + 4 * k);
			if (len == 0 || off < h || off > n - i || len > n - i - off)
				return 0;
			if (i + off + len > end)
				end = i + off + len;
		}
		*ver = le32(i + 8);
	} else {	/* "RTBTCore" */
		if (i + 20 > n)
			return 0;
		uint32_t ns = le32(i + 16);
		if (ns == 0 || ns > 256)
			return 0;
		end = i + 20;
		for (uint32_t k = 0; k < ns; k++) {
			if (end + 8 > n)
				return 0;
			uint32_t len = le32(end + 4);
			if (len > n - end - 8)
				return 0;
			end += 8 + len;
		}
		*ver = le32(i + 8);
	}
	size_t lim = end + EXT_WINDOW < n ? end + EXT_WINDOW : n;
	uint8_t *s = lim > end ? memmem(d + end, lim - end, EXT_SIG, 4) : NULL;
	if (!s)
		return 0;
	size_t sig = s - d;
	if (v2 && sig + 4 - 7 < end)	/* v2: btrtl drops the last 7 bytes */
		return 0;
	*pid = project_id(i, sig, hdr);
	if (*pid < 0)
		return 0;
	return sig + 4 - i;
}

static int write_file(const char *path, const uint8_t *buf, size_t len)
{
	char tmp[4096];
	if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp)
		return -1;
	FILE *f = fopen(tmp, "wb");
	if (!f)
		return -1;
	int ok = fwrite(buf, 1, len, f) == len && fflush(f) == 0 && fsync(fileno(f)) == 0;
	if (fclose(f) != 0 || !ok || rename(tmp, path) != 0) {
		unlink(tmp);
		return -1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	int want = 19, c;
	while ((c = getopt(argc, argv, "p:")) != -1) {
		if (c != 'p')
			goto usage;
		want = atoi(optarg);
	}
	if (argc - optind != 3) {
usage:
		fprintf(stderr, "usage: bt-fwcutter [-p PROJECT_ID] BINARY FW_OUT CONFIG_OUT\n");
		return 1;
	}
	const char *in = argv[optind], *fw_out = argv[optind + 1], *cfg_out = argv[optind + 2];

	FILE *f = fopen(in, "rb");
	if (!f || fseek(f, 0, SEEK_END) || (long)(n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET)) {
		perror(in);
		return 1;
	}
	if (!(d = malloc(n + 1)) || fread(d, 1, n, f) != n) {
		perror(in);
		return 1;
	}
	fclose(f);

	/* patches */
	size_t fw = 0, fw_len = 0;
	int found = 0;
	for (size_t i = 0; i + 8 <= n; i++) {
		if (memcmp(d + i, "Realtech", 8) && memcmp(d + i, "RTBTCore", 8))
			continue;
		int pid;
		uint32_t ver;
		size_t len = parse_patch(i, &pid, &ver);
		if (!len)
			continue;
		fprintf(stderr, "bt-fwcutter: patch at 0x%zx: %.8s, %zu bytes, project id %d, fw_version 0x%08x\n",
			i, (char *)d + i, len, pid, ver);
		if (pid != want)
			continue;
		if (found++ && (len != fw_len || memcmp(d + i, d + fw, len))) {
			fprintf(stderr, "bt-fwcutter: different patches for project id %d: refusing\n", want);
			return 2;
		}
		fw = i;
		fw_len = len;
	}
	if (!found) {
		fprintf(stderr, "bt-fwcutter: no patch for project id %d in %s\n", want, in);
		return 2;
	}

	/* UART config entry */
	size_t uart = 0, uart_len = 0;
	for (uint8_t *s = d; (s = memmem(s, n - (s - d), CFG_MAGIC, 4)); s++) {
		size_t i = s - d, p = i + 6, u = 0, ul = 0;
		if (p > n)
			break;
		size_t e = p + le16(i + 4);
		if (e > n)
			continue;
		while (p + 3 <= e) {
			size_t len = d[p + 2];
			if (le16(p) == 0x0c && len >= 4)
				u = p + 3, ul = len;
			p += 3 + len;
		}
		if (p != e || !ul)
			continue;
		if (uart_len && (ul != uart_len || memcmp(d + u, d + uart, ul))) {
			fprintf(stderr, "bt-fwcutter: different UART config entries: refusing\n");
			return 2;
		}
		uart = u;
		uart_len = ul;
	}
	if (!uart_len) {
		fprintf(stderr, "bt-fwcutter: no UART config entry in %s\n", in);
		return 2;
	}
	uint8_t cfg[6 + 3 + 255];
	memcpy(cfg, CFG_MAGIC, 4);
	cfg[4] = (3 + uart_len) & 0xff, cfg[5] = (3 + uart_len) >> 8;
	cfg[6] = 0x0c, cfg[7] = 0, cfg[8] = uart_len;
	memcpy(cfg + 9, d + uart, uart_len);

	if (write_file(cfg_out, cfg, 9 + uart_len) || write_file(fw_out, d + fw, fw_len)) {
		perror("bt-fwcutter: write");
		return 1;
	}
	fprintf(stderr, "bt-fwcutter: wrote %s (%zu bytes, project id %d) and %s (UART word 0x%08x, flow control %s)\n",
		fw_out, fw_len, want, cfg_out, le32(uart), uart_len >= 13 && d[uart + 12] & 4 ? "on" : "off");
	return 0;
}
