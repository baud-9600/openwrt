/*
 * LZMA compressed kernel loader for Atheros AR7XXX/AR9XXX based boards
 *
 * Copyright (C) 2011 Gabor Juhos <juhosg@openwrt.org>
 *
 * Some parts of this code was based on the OpenWrt specific lzma-loader
 * for the BCM47xx and ADM5120 based boards:
 *	Copyright (C) 2004 Manuel Novoa III (mjn3@codepoet.org)
 *	Copyright (C) 2005 Mineharu Takahara <mtakahar@yahoo.com>
 *	Copyright (C) 2005 by Oleg I. Vdovikin <oleg@cs.msu.su>
 *
 * The image_header structure has been taken from the U-Boot project.
 *	(C) Copyright 2008 Semihalf
 *	(C) Copyright 2000-2005
 *	Wolfgang Denk, DENX Software Engineering, wd@denx.de.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 */

#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "cache.h"
#include "printf.h"
#include "LzmaDecode.h"

#ifndef CONFIG_FLASH_BASE
#define CONFIG_FLASH_BASE	0x1f000000
#endif

#define AR71XX_FLASH_START	CONFIG_FLASH_BASE
#define AR71XX_FLASH_END	(CONFIG_FLASH_BASE + 0x00e00000)

#define KSEG0			0x80000000
#define KSEG1			0xa0000000

#define KSEG1ADDR(a)		((((unsigned)(a)) & 0x1fffffffU) | KSEG1)

#undef LZMA_DEBUG

#ifdef LZMA_DEBUG
#  define DBG(f, a...)	printf(f, ## a)
#else
#  define DBG(f, a...)	do {} while (0)
#endif

#define IH_MAGIC_OKLI		0x4f4b4c49	/* 'OKLI' */

#define IH_NMLEN		32	/* Image Name Length		*/

/*
 * SPI command-based reads for AR9344 CS1.
 *
 * The AR9344 memory-maps only CS0 at 0x1F000000; CS1 reads require the
 * SPI function registers. For dual-flash boards (e.g. Zyxel NWA5123-AC)
 * where the OKLI payload lives on CS1, we must drive the SPI block
 * ourselves. Behind SPI_FUNC_FS=1 the controller is in "GPIO-ish"
 * command mode and SPI_IOC bit-bangs CLK/DO/CS lines; we read bits back
 * from SPI_RDS[0].
 */
#define SPI_FUNC_FS	0xbf000000	/* function select (bit0=1 → cmd mode) */
#define SPI_FUNC_IOC	0xbf000008	/* I/O control (CS/CLK/DO) */
#define SPI_FUNC_RDS	0xbf00000c	/* read-data shift (bit0 = DI) */

#define SPI_IOC_DO	(1u << 0)
#define SPI_IOC_CLK	(1u << 8)
#define SPI_IOC_CS0	(1u << 16)
#define SPI_IOC_CS1	(1u << 17)
#define SPI_IOC_CS2	(1u << 18)
#define SPI_IOC_CS_ALL	(SPI_IOC_CS0 | SPI_IOC_CS1 | SPI_IOC_CS2)
/* CS line is active-low, so leaving the other CS bits set keeps them inactive. */
#define SPI_CS1_SEL	(SPI_IOC_CS_ALL & ~SPI_IOC_CS1)

#define SPI_CMD_FAST_READ	0x0b

static inline void spi_reg_wr(unsigned long addr, unsigned long val)
{
	*(volatile unsigned long *)addr = val;
}

static inline unsigned long spi_reg_rd(unsigned long addr)
{
	return *(volatile unsigned long *)addr;
}

static void spi_shift_byte(unsigned char byte)
{
	int i;
	for (i = 7; i >= 0; i--) {
		unsigned long base = SPI_CS1_SEL;
		if (byte & (1 << i))
			base |= SPI_IOC_DO;
		spi_reg_wr(SPI_FUNC_IOC, base);                /* CLK low */
		spi_reg_wr(SPI_FUNC_IOC, base | SPI_IOC_CLK);  /* CLK high: shift bit */
	}
}

static unsigned char spi_recv_byte(void)
{
	unsigned char byte = 0;
	int i;
	for (i = 7; i >= 0; i--) {
		spi_reg_wr(SPI_FUNC_IOC, SPI_CS1_SEL);                /* CLK low */
		spi_reg_wr(SPI_FUNC_IOC, SPI_CS1_SEL | SPI_IOC_CLK);  /* CLK high: sample */
		if (spi_reg_rd(SPI_FUNC_RDS) & 1)
			byte |= (1 << i);
	}
	return byte;
}

static void cs1_read(unsigned long offset, void *dst, unsigned long len)
{
	unsigned char *d = dst;
	unsigned long i;

	spi_reg_wr(SPI_FUNC_FS, 1);		/* enter command mode */
	spi_reg_wr(SPI_FUNC_IOC, SPI_IOC_CS_ALL); /* all CS high (idle) */

	/* select CS1 */
	spi_reg_wr(SPI_FUNC_IOC, SPI_CS1_SEL);

	spi_shift_byte(SPI_CMD_FAST_READ);
	spi_shift_byte((offset >> 16) & 0xff);
	spi_shift_byte((offset >> 8) & 0xff);
	spi_shift_byte(offset & 0xff);
	spi_shift_byte(0);			/* dummy */

	for (i = 0; i < len; i++)
		d[i] = spi_recv_byte();

	spi_reg_wr(SPI_FUNC_IOC, SPI_IOC_CS_ALL); /* deassert CS */
	spi_reg_wr(SPI_FUNC_FS, 0);		/* back to memory-mapped mode */
}

typedef struct image_header {
	uint32_t	ih_magic;	/* Image Header Magic Number	*/
	uint32_t	ih_hcrc;	/* Image Header CRC Checksum	*/
	uint32_t	ih_time;	/* Image Creation Timestamp	*/
	uint32_t	ih_size;	/* Image Data Size		*/
	uint32_t	ih_load;	/* Data	 Load  Address		*/
	uint32_t	ih_ep;		/* Entry Point Address		*/
	uint32_t	ih_dcrc;	/* Image Data CRC Checksum	*/
	uint8_t		ih_os;		/* Operating System		*/
	uint8_t		ih_arch;	/* CPU architecture		*/
	uint8_t		ih_type;	/* Image Type			*/
	uint8_t		ih_comp;	/* Compression Type		*/
	uint8_t		ih_name[IH_NMLEN];	/* Image Name		*/
} image_header_t;

/* beyond the image end, size not known in advance */
extern unsigned char workspace[];
extern void board_init(void);

static CLzmaDecoderState lzma_state;
static unsigned char *lzma_data;
static unsigned long lzma_datasize;
static unsigned long lzma_outsize;
static unsigned long kernel_la;

#ifdef CONFIG_KERNEL_CMDLINE
#define kernel_argc	2
static const char kernel_cmdline[] = CONFIG_KERNEL_CMDLINE;
static const char *const kernel_argv[] = {
	NULL,
	kernel_cmdline,
	NULL,
};
#endif /* CONFIG_KERNEL_CMDLINE */

static void halt(void)
{
	printf("\nSystem halted!\n");
	for(;;);
}

static __inline__ unsigned long get_be32(void *buf)
{
	unsigned char *p = buf;

	return (((unsigned long) p[0] << 24) +
	        ((unsigned long) p[1] << 16) +
	        ((unsigned long) p[2] << 8) +
	        (unsigned long) p[3]);
}

static __inline__ unsigned char lzma_get_byte(void)
{
	unsigned char c;

	lzma_datasize--;
	c = *lzma_data++;

	return c;
}

static int lzma_init_props(void)
{
	unsigned char props[LZMA_PROPERTIES_SIZE];
	int res;
	int i;

	/* read lzma properties */
	for (i = 0; i < LZMA_PROPERTIES_SIZE; i++)
		props[i] = lzma_get_byte();

	/* read the lower half of uncompressed size in the header */
	lzma_outsize = ((SizeT) lzma_get_byte()) +
		       ((SizeT) lzma_get_byte() << 8) +
		       ((SizeT) lzma_get_byte() << 16) +
		       ((SizeT) lzma_get_byte() << 24);

	/* skip rest of the header (upper half of uncompressed size) */
	for (i = 0; i < 4; i++)
		lzma_get_byte();

	res = LzmaDecodeProperties(&lzma_state.Properties, props,
					LZMA_PROPERTIES_SIZE);
	return res;
}

static int lzma_decompress(unsigned char *outStream)
{
	SizeT ip, op;
	int ret;

	lzma_state.Probs = (CProb *) workspace;

	ret = LzmaDecode(&lzma_state, lzma_data, lzma_datasize, &ip, outStream,
			 lzma_outsize, &op);

	if (ret != LZMA_RESULT_OK) {
		int i;

		DBG("LzmaDecode error %d at %08x, osize:%d ip:%d op:%d\n",
		    ret, lzma_data + ip, lzma_outsize, ip, op);

		for (i = 0; i < 16; i++)
			DBG("%02x ", lzma_data[ip + i]);

		DBG("\n");
	}

	return ret;
}

#if (LZMA_WRAPPER)
static void lzma_init_data(void)
{
	extern unsigned char _lzma_data_start[];
	extern unsigned char _lzma_data_end[];

	kernel_la = LOADADDR;
	lzma_data = _lzma_data_start;
	lzma_datasize = _lzma_data_end - _lzma_data_start;
}
#else
static void lzma_init_data(void)
{
	struct image_header *hdr = NULL;
	unsigned char *flash_base;
	unsigned long flash_ofs;
	unsigned long kernel_ofs;
	unsigned long kernel_size;

	flash_base = (unsigned char *) KSEG1ADDR(AR71XX_FLASH_START);

	printf("Looking for OpenWrt image... ");

	/*
	 * For CS1 (AR71XX_FLASH_START = 0x1e000000), the memory-mapped
	 * window actually mirrors CS0, so we read via SPI commands into
	 * a RAM staging buffer and point lzma_data at that. For CS0 we
	 * keep the fast memory-mapped read.
	 */
#ifdef CONFIG_FLASH_CS1
	{
		/* Staging buffer well above the loader's own ~128KB footprint
		 * and the kernel load address, in free DDR. */
		static unsigned char *buf = (unsigned char *) 0x82000000;
		unsigned long magic;
		unsigned long hdr_size;

#ifdef CONFIG_FLASH_CS0_FALLBACK
		/*
		 * Some units (e.g. Zyxel NWA5123-AC with bootImage=1 ex
		 * factory) end up with the OpenWrt payload on CS0 instead
		 * of CS1 after ATNZ, because ZLD writes the inactive slot
		 * and that slot sits on the opposite flash. Probe CS0
		 * memory-mapped first, fall back to CS1 if nothing matches.
		 */
		{
			unsigned char *cs0 = (unsigned char *)
				KSEG1ADDR(0x1f000000 + CONFIG_FLASH_CS0_FALLBACK);
			unsigned long cs0_magic = get_be32(cs0);
#ifdef CONFIG_KERNEL_MAGIC
			if (cs0_magic == CONFIG_KERNEL_MAGIC) {
#else
			if (cs0_magic == IH_MAGIC_OKLI) {
#endif
				hdr = (struct image_header *) cs0;
				kernel_size = get_be32(&hdr->ih_size);
				kernel_la = get_be32(&hdr->ih_load);
				printf("found @cs0+%08x, size=%u\n",
				       (unsigned)CONFIG_FLASH_CS0_FALLBACK,
				       (unsigned)kernel_size);
				lzma_data = cs0 + sizeof(struct image_header);
				lzma_datasize = kernel_size;
				return;
			}
		}
#endif /* CONFIG_FLASH_CS0_FALLBACK */

		cs1_read(CONFIG_FLASH_OFFS, buf, sizeof(struct image_header));
		magic = get_be32(buf);
#ifdef CONFIG_KERNEL_MAGIC
		if (magic != CONFIG_KERNEL_MAGIC) {
#else
		if (magic != IH_MAGIC_OKLI) {
#endif
			printf("not found! @cs1+%08x: %02x %02x %02x %02x\n",
			       (unsigned)CONFIG_FLASH_OFFS,
			       buf[0], buf[1], buf[2], buf[3]);
			halt();
		}

		hdr = (struct image_header *) buf;
		hdr_size = sizeof(struct image_header);
		kernel_size = get_be32(&hdr->ih_size);
		kernel_la = get_be32(&hdr->ih_load);

		printf("found @cs1+%08x, size=%u\n",
		       (unsigned)CONFIG_FLASH_OFFS, (unsigned)kernel_size);

		cs1_read(CONFIG_FLASH_OFFS + hdr_size, buf + hdr_size, kernel_size);

		lzma_data = buf + hdr_size;
		lzma_datasize = kernel_size;
		return;
	}
#endif

	for (flash_ofs = CONFIG_FLASH_OFFS;
	     flash_ofs <= (CONFIG_FLASH_OFFS + CONFIG_FLASH_MAX);
	     flash_ofs += CONFIG_FLASH_STEP) {
		unsigned long magic;
		unsigned char *p;

		p = flash_base + flash_ofs;
		magic = get_be32(p);
#ifdef CONFIG_KERNEL_MAGIC
		if (magic == CONFIG_KERNEL_MAGIC) {
#else
		if (magic == IH_MAGIC_OKLI) {
#endif
			hdr = (struct image_header *) p;
			break;
		}
	}

	if (hdr == NULL) {
		printf("not found!\n");
		halt();
	}

	printf("found at 0x%08x\n", flash_base + flash_ofs);

	kernel_ofs = sizeof(struct image_header);
	kernel_size = get_be32(&hdr->ih_size);
	kernel_la = get_be32(&hdr->ih_load);

	lzma_data = flash_base + flash_ofs + kernel_ofs;
	lzma_datasize = kernel_size;
}
#endif /* (LZMA_WRAPPER) */

void loader_main(unsigned long reg_a0, unsigned long reg_a1,
		 unsigned long reg_a2, unsigned long reg_a3)
{
	void (*kernel_entry) (unsigned long, unsigned long, unsigned long,
			      unsigned long);
	int res;

	board_init();

	printf("\n\nOpenWrt kernel loader for AR7XXX/AR9XXX\n");
	printf("Copyright (C) 2011 Gabor Juhos <juhosg@openwrt.org>\n");

	lzma_init_data();

	res = lzma_init_props();
	if (res != LZMA_RESULT_OK) {
		printf("Incorrect LZMA stream properties!\n");
		halt();
	}

	printf("Decompressing kernel... ");

	res = lzma_decompress((unsigned char *) kernel_la);
	if (res != LZMA_RESULT_OK) {
		printf("failed, ");
		switch (res) {
		case LZMA_RESULT_DATA_ERROR:
			printf("data error!\n");
			break;
		default:
			printf("unknown error %d!\n", res);
		}
		halt();
	} else {
		printf("done!\n");
	}

	flush_cache(kernel_la, lzma_outsize);

	printf("Starting kernel at %08x...\n\n", kernel_la);

#ifdef CONFIG_KERNEL_CMDLINE
	reg_a0 = kernel_argc;
	reg_a1 = (unsigned long) kernel_argv;
	reg_a2 = 0;
	reg_a3 = 0;
#endif

	kernel_entry = (void *) kernel_la;
	kernel_entry(reg_a0, reg_a1, reg_a2, reg_a3);
}
