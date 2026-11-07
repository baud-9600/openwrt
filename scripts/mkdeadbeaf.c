// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * mkdeadbeaf - Build Zyxel "deadbeaf" firmware images
 *
 * Used by: NWA5123-AC and similar Zyxel NWA access points
 *
 * Header layout (652 bytes, but header_size field = 648):
 *   - pre-header:    108 bytes (includes magic, sizes, board, num_sections)
 *   - section_entry: 272 bytes (kernel)
 *   - section_entry: 272 bytes (rootfs)
 *
 * The header_size field stores 648 (= 652 - 4, Zyxel "+4 quirk").
 * The ZLD bootloader uses header_size as the data start offset, so data
 * begins at byte 648.  The last 4 bytes of the s1.pad overlap with the
 * first 4 bytes of the kernel sub-header; both are zeros, so no conflict.
 *
 * Section 0 (kernel): size = KERNEL_SUB_HDR_SIZE + kernel_size.
 *   checksum covers the kernel data only (excluding the 512-byte sub-header).
 *
 * Checksums use MD5 word-sum: sum of 4 big-endian 32-bit words from the
 * MD5 digest, mod 2^32 (returns 1 if result is 0).
 *
 * Copyright (C) 2024 OpenWrt.org
 */

#include <byteswap.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "md5.h"

#if __BYTE_ORDER == __BIG_ENDIAN
#define cpu_to_be16(x) (x)
#define cpu_to_be32(x) (x)
#elif __BYTE_ORDER == __LITTLE_ENDIAN
#define cpu_to_be16(x) bswap_16(x)
#define cpu_to_be32(x) bswap_32(x)
#else
#error "unsupported endianness"
#endif

#define DEADBEAF_MAGIC		0xDEADBEAF
#define PRE_HEADER_SIZE		108
#define SECTION_ENTRY_SIZE	272
#define KERNEL_SUB_HDR_SIZE	512
/*
 * DATA_OFFSET is where the ZLD bootloader begins reading section data.
 * It equals the value stored in the header_size field (648), which is 4 less
 * than the actual struct end (652).  The last 4 bytes of the s1 pad overlap
 * with the first 4 bytes of the kernel sub-header; both are zeros.
 */
#define HEADER_SIZE		(PRE_HEADER_SIZE + 2 * SECTION_ENTRY_SIZE)
#define DATA_OFFSET		(HEADER_SIZE - 4)
#define FILENAME_LEN		64
#define PARTITION_LEN		64
#define BLOCKSIZE		0x10000	/* 64k flash erase block */

/* Default kernel flash offset for NWA5123-AC */
#define DEFAULT_KERNEL_OFFSET	0x00180000

/* Default model_id (0 = unset, pass -m to override) */
#define DEFAULT_MODEL_ID	0

struct deadbeaf_header {
	uint32_t master_crc;
	uint32_t magic;
	uint32_t type;
	uint32_t model_id;
	uint32_t reserved1;
	uint32_t reserved2;
	uint32_t file_size;
	uint16_t header_size;
	uint16_t pre_header_size;
	uint32_t flags;
	uint8_t  pad1[4];
	char     version[32];
	char     board[32];
	uint16_t num_sections;
	uint16_t pad2;
} __attribute__((packed));

struct section_entry {
	uint32_t size;
	uint32_t checksum;
	uint32_t flash_offset;
	char     filename[FILENAME_LEN];
	char     partition[PARTITION_LEN];
	uint8_t  pad[SECTION_ENTRY_SIZE - 12 - FILENAME_LEN - PARTITION_LEN];
} __attribute__((packed));

static const char *progname;

static uint32_t md5ws(const void *data, size_t len)
{
	MD5_CTX ctx;
	uint8_t digest[16];
	uint32_t w[4], sum;
	int i;

	MD5_Init(&ctx);
	MD5_Update(&ctx, data, len);
	MD5_Final(digest, &ctx);

	for (i = 0; i < 4; i++)
		w[i] = ((uint32_t)digest[i * 4] << 24) |
		       ((uint32_t)digest[i * 4 + 1] << 16) |
		       ((uint32_t)digest[i * 4 + 2] << 8) |
		       ((uint32_t)digest[i * 4 + 3]);

	sum = (w[0] + w[1] + w[2] + w[3]) & 0xFFFFFFFF;
	return sum ? sum : 1;
}

static void *map_file(const char *path, size_t *size)
{
	struct stat st;
	FILE *fp;
	void *buf;

	if (stat(path, &st)) {
		fprintf(stderr, "%s: stat %s: %s\n", progname, path,
			strerror(errno));
		return NULL;
	}
	*size = st.st_size;

	buf = malloc(*size);
	if (!buf) {
		fprintf(stderr, "%s: malloc(%zu): %s\n", progname, *size,
			strerror(errno));
		return NULL;
	}

	fp = fopen(path, "rb");
	if (!fp) {
		fprintf(stderr, "%s: open %s: %s\n", progname, path,
			strerror(errno));
		free(buf);
		return NULL;
	}

	if (fread(buf, 1, *size, fp) != *size) {
		fprintf(stderr, "%s: short read on %s\n", progname, path);
		fclose(fp);
		free(buf);
		return NULL;
	}

	fclose(fp);
	return buf;
}

static void usage(int status)
{
	fprintf(stderr,
		"Usage: %s [OPTIONS]\n"
		"  -B <board>     board name (required)\n"
		"  -k <file>      kernel image (required)\n"
		"  -r <file>      rootfs image (required)\n"
		"  -o <file>      output file (required)\n"
		"  -v <version>   firmware version string (default: 1.00.00)\n"
		"  -K <offset>    kernel flash offset (default: 0x%x)\n"
		"  -m <model_id>  model ID (default: 0)\n"
		"  -h             show this help\n",
		progname, DEFAULT_KERNEL_OFFSET);
	exit(status);
}

int main(int argc, char *argv[])
{
	struct deadbeaf_header hdr;
	struct section_entry s0, s1;
	uint8_t sub_hdr[KERNEL_SUB_HDR_SIZE];
	const char *board = NULL, *kernel_path = NULL;
	const char *rootfs_path = NULL, *output_path = NULL;
	const char *version = "1.00.00";
	uint32_t kernel_offset = DEFAULT_KERNEL_OFFSET;
	uint32_t model_id = DEFAULT_MODEL_ID;
	uint32_t rootfs_offset;
	void *kernel_data, *rootfs_data, *file_buf;
	size_t kernel_size, rootfs_size, file_size;
	char name_buf[FILENAME_LEN];
	FILE *fp;
	int opt, i;

	progname = argv[0];

	while ((opt = getopt(argc, argv, "B:k:r:o:v:K:m:h")) != -1) {
		switch (opt) {
		case 'B':
			board = optarg;
			break;
		case 'k':
			kernel_path = optarg;
			break;
		case 'r':
			rootfs_path = optarg;
			break;
		case 'o':
			output_path = optarg;
			break;
		case 'v':
			version = optarg;
			break;
		case 'K':
			kernel_offset = strtoul(optarg, NULL, 0);
			break;
		case 'm':
			model_id = strtoul(optarg, NULL, 0);
			break;
		case 'h':
			usage(0);
		default:
			usage(1);
		}
	}

	if (!board || !kernel_path || !rootfs_path || !output_path) {
		fprintf(stderr, "%s: missing required arguments\n", progname);
		usage(1);
	}

	kernel_data = map_file(kernel_path, &kernel_size);
	if (!kernel_data)
		return 1;

	rootfs_data = map_file(rootfs_path, &rootfs_size);
	if (!rootfs_data)
		return 1;

	/* Rootfs follows kernel on flash, aligned to erase block.
	 * The flash section for the kernel holds the sub-header + kernel data. */
	rootfs_offset = kernel_offset +
		((KERNEL_SUB_HDR_SIZE + kernel_size + BLOCKSIZE - 1) /
		 BLOCKSIZE) * BLOCKSIZE;

	/* Total file: data at DATA_OFFSET + sub-header + kernel + rootfs */
	file_size = DATA_OFFSET + KERNEL_SUB_HDR_SIZE + kernel_size + rootfs_size;

	/* Build pre-header */
	memset(&hdr, 0, sizeof(hdr));
	hdr.master_crc    = 0; /* filled below */
	hdr.magic         = cpu_to_be32(DEADBEAF_MAGIC);
	hdr.type          = cpu_to_be32(0x01000002);
	hdr.model_id      = cpu_to_be32(model_id);
	hdr.reserved1     = cpu_to_be32(0xFFFFFFFF);
	hdr.reserved2     = cpu_to_be32(0xFFFFFFFF);
	hdr.file_size     = cpu_to_be32(file_size);
	hdr.header_size   = cpu_to_be16(DATA_OFFSET); /* ZLD reads data at this offset */
	hdr.pre_header_size = cpu_to_be16(PRE_HEADER_SIZE);
	hdr.flags         = cpu_to_be32(0x01100000);
	strncpy(hdr.version, version, sizeof(hdr.version) - 1);
	strncpy(hdr.board, board, sizeof(hdr.board) - 1);
	hdr.num_sections  = cpu_to_be16(2);

	/* Build section entry 0 (kernel) */
	memset(&s0, 0, sizeof(s0));
	s0.size         = cpu_to_be32(KERNEL_SUB_HDR_SIZE + kernel_size);
	/* Checksum covers kernel data only, NOT the 512-byte sub-header */
	s0.checksum     = cpu_to_be32(md5ws(kernel_data, kernel_size));
	s0.flash_offset = cpu_to_be32(kernel_offset);

	/* filename: "kernel<board_lowercase>.bin" */
	memset(name_buf, 0, sizeof(name_buf));
	snprintf(name_buf, sizeof(name_buf), "kernel");
	i = strlen(name_buf);
	strncpy(name_buf + i, board, sizeof(name_buf) - i - 5);
	for (; name_buf[i]; i++)
		if (name_buf[i] >= 'A' && name_buf[i] <= 'Z')
			name_buf[i] += 'a' - 'A';
	strncat(name_buf, ".bin", sizeof(name_buf) - strlen(name_buf) - 1);
	strncpy(s0.filename, name_buf, FILENAME_LEN - 1);
	strncpy(s0.partition, "kernel", PARTITION_LEN - 1);

	/* Build section entry 1 (rootfs) */
	memset(&s1, 0, sizeof(s1));
	s1.size         = cpu_to_be32(rootfs_size);
	s1.checksum     = cpu_to_be32(md5ws(rootfs_data, rootfs_size));
	s1.flash_offset = cpu_to_be32(rootfs_offset);
	strncpy(s1.filename, "squashfs_ro.img", FILENAME_LEN - 1);
	strncpy(s1.partition, "zldfs", PARTITION_LEN - 1);

	/* Kernel sub-header: all zeros */
	memset(sub_hdr, 0, sizeof(sub_hdr));

	/* Assemble complete file for master checksum */
	file_buf = malloc(file_size);
	if (!file_buf) {
		fprintf(stderr, "%s: malloc(%zu) failed\n", progname, file_size);
		return 1;
	}

	/*
	 * Write header structs into a zero-initialised buffer.  The header
	 * occupies 652 bytes but data_offset (= header_size field) = 648, so
	 * the last 4 bytes of s1.pad overlap with the first 4 bytes of the
	 * sub-header.  Both are zero; memset(file_buf, 0) + the copies below
	 * produce the correct layout without any special handling.
	 */
	memset(file_buf, 0, file_size);
	memcpy(file_buf, &hdr, sizeof(hdr));
	memcpy((uint8_t *)file_buf + PRE_HEADER_SIZE, &s0, sizeof(s0));
	memcpy((uint8_t *)file_buf + PRE_HEADER_SIZE + SECTION_ENTRY_SIZE,
	       &s1, sizeof(s1));
	/* sub-header and payload begin at DATA_OFFSET (= 648) */
	memcpy((uint8_t *)file_buf + DATA_OFFSET, sub_hdr, sizeof(sub_hdr));
	memcpy((uint8_t *)file_buf + DATA_OFFSET + KERNEL_SUB_HDR_SIZE,
	       kernel_data, kernel_size);
	memcpy((uint8_t *)file_buf + DATA_OFFSET + KERNEL_SUB_HDR_SIZE +
	       kernel_size, rootfs_data, rootfs_size);

	/* Master checksum: md5ws(file[4:]) */
	{
		uint32_t master = md5ws((uint8_t *)file_buf + 4, file_size - 4);
		((struct deadbeaf_header *)file_buf)->master_crc =
			cpu_to_be32(master);
	}

	/* Write output */
	fp = fopen(output_path, "wb");
	if (!fp) {
		fprintf(stderr, "%s: open %s: %s\n", progname, output_path,
			strerror(errno));
		return 1;
	}

	if (fwrite(file_buf, 1, file_size, fp) != file_size) {
		fprintf(stderr, "%s: write error\n", progname);
		fclose(fp);
		return 1;
	}

	fclose(fp);
	free(file_buf);
	free(kernel_data);
	free(rootfs_data);

	fprintf(stderr,
		"%s: %s kernel=%zu rootfs=%zu total=%zu "
		"kernel_off=0x%x rootfs_off=0x%x\n",
		progname, board, kernel_size, rootfs_size, file_size,
		kernel_offset, rootfs_offset);

	return 0;
}
