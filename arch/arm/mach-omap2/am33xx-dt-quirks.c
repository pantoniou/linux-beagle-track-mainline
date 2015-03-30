/*
 * arch/arm/mach-omap2/am33xx-dt-quirks.c
 *
 * AM33xx variant DT quirks
 *
 * Copyright (C) 2015 Konsulko Group
 *
 * Author:
 *	Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <asm/io.h>
#include <asm/delay.h>

#include "am33xx-dt-quirks.h"

/*
 * The board IDs for am33xx board are in an I2C EEPROM
 * We are very early in the boot process so we have to
 * read the EEPROM directly without using the I2C layer.
 *
 * Note that we rely on the bootloader setting up the muxes
 * (which is the case for u-boot).
 */

/* I2C Status Register (OMAP_I2C_STAT): */
#define OMAP_I2C_STAT_XDR	(1 << 14)	/* TX Buffer draining */
#define OMAP_I2C_STAT_RDR	(1 << 13)	/* RX Buffer draining */
#define OMAP_I2C_STAT_BB	(1 << 12)	/* Bus busy */
#define OMAP_I2C_STAT_ROVR	(1 << 11)	/* Receive overrun */
#define OMAP_I2C_STAT_XUDF	(1 << 10)	/* Transmit underflow */
#define OMAP_I2C_STAT_AAS	(1 << 9)	/* Address as slave */
#define OMAP_I2C_STAT_BF	(1 << 8)	/* Bus Free */
#define OMAP_I2C_STAT_XRDY	(1 << 4)	/* Transmit data ready */
#define OMAP_I2C_STAT_RRDY	(1 << 3)	/* Receive data ready */
#define OMAP_I2C_STAT_ARDY	(1 << 2)	/* Register access ready */
#define OMAP_I2C_STAT_NACK	(1 << 1)	/* No ack interrupt enable */
#define OMAP_I2C_STAT_AL	(1 << 0)	/* Arbitration lost int ena */

/* I2C Configuration Register (OMAP_I2C_CON): */
#define OMAP_I2C_CON_EN		(1 << 15)	/* I2C module enable */
#define OMAP_I2C_CON_BE		(1 << 14)	/* Big endian mode */
#define OMAP_I2C_CON_OPMODE_HS	(1 << 12)	/* High Speed support */
#define OMAP_I2C_CON_STB	(1 << 11)	/* Start byte mode (master) */
#define OMAP_I2C_CON_MST	(1 << 10)	/* Master/slave mode */
#define OMAP_I2C_CON_TRX	(1 << 9)	/* TX/RX mode (master only) */
#define OMAP_I2C_CON_XA		(1 << 8)	/* Expand address */
#define OMAP_I2C_CON_RM		(1 << 2)	/* Repeat mode (master only) */
#define OMAP_I2C_CON_STP	(1 << 1)	/* Stop cond (master only) */
#define OMAP_I2C_CON_STT	(1 << 0)	/* Start condition (master) */

/* register definitions */
#define I2C_REVNB_LO		0x00
#define I2C_REVNB_HI		0x04
#define I2C_SYSC		0x10
#define I2C_IRQSTATUS_RAW	0x24
#define I2C_IRQSTATUS		0x28
#define I2C_IRQENABLE_SET	0x2C
#define I2C_IRQENABLE_CLR	0x30
#define I2C_WE			0x34
#define I2C_DMARXENABLE_SET	0x38
#define I2C_DMATXENABLE_SET	0x3C
#define I2C_DMARXENABLE_CLR	0x40
#define I2C_DMATXENABLE_CLR	0x44
#define I2C_DMARXWAKE_EN	0x48
#define I2C_DMATXWAKE_EN	0x4C
#define I2C_SYSS		0x90
#define I2C_BUF			0x94
#define I2C_CNT			0x98
#define I2C_DATA		0x9C
#define I2C_CON			0xA4
#define I2C_OA			0xA8
#define I2C_SA			0xAC
#define I2C_PSC			0xB0
#define I2C_SCLL		0xB4
#define I2C_SCLH		0xB8
#define I2C_SYSTEST		0xBC
#define I2C_BUFSTAT		0xC0
#define I2C_OA1			0xC4
#define I2C_OA2			0xC8
#define I2C_OA3			0xCC
#define I2C_ACTOA		0xD0
#define I2C_SBLOCK		0xD4

static inline void i2c_reg_write(void __iomem *base, unsigned int reg, u16 val)
{
	writew_relaxed(val, base + reg);
}

static inline u16 i2c_reg_read(void __iomem *base, unsigned int reg)
{
	return readw_relaxed(base + reg);
}

static void flush_fifo(void __iomem *base)
{
	u16 stat;

	while ((stat = i2c_reg_read(base, I2C_IRQSTATUS_RAW))
			& OMAP_I2C_STAT_RRDY) {
		(void)i2c_reg_read(base, I2C_DATA);
		i2c_reg_write(base, I2C_IRQSTATUS, OMAP_I2C_STAT_RRDY);
		udelay(1000);
	};
}

static void wait_delay(void __iomem *base)
{
	udelay((10000000 / 100000) * 2);
}

static int wait_for_bb(void __iomem *base)
{
	int timeout;
	u16 stat;

	timeout = 1000;
	while (((stat = i2c_reg_read(base, I2C_IRQSTATUS_RAW)) &
				OMAP_I2C_STAT_BB) && timeout--) {
		i2c_reg_write(base, I2C_IRQSTATUS, stat);
		wait_delay(base);
	}

	if (timeout <= 0) {
		pr_err("%s: Timeout while waiting for bus\n", __func__);
		return -1;
	}
	i2c_reg_write(base, I2C_IRQSTATUS, 0xffff);
	return 0;
}

static u16 wait_for_event(void __iomem *base)
{
	u16 status;
	int timeout = 10000;

	do {
		wait_delay(base);
		status = i2c_reg_read(base, I2C_IRQSTATUS_RAW);
	} while (!(status & (OMAP_I2C_STAT_ROVR | OMAP_I2C_STAT_XUDF |
		    OMAP_I2C_STAT_XRDY | OMAP_I2C_STAT_RRDY |
		    OMAP_I2C_STAT_ARDY | OMAP_I2C_STAT_NACK |
		    OMAP_I2C_STAT_AL)) &&
			timeout--);

	if (timeout <= 0) {
		pr_err("%s: Timeout status=%04x\n", __func__, status);
		i2c_reg_write(base, I2C_IRQSTATUS, 0xffff);
		status = 0;
	}

	return status;
}

static int i2c_init(void __iomem *base, u16 psc, u16 scll, u16 sclh)
{
	int timeout;

	/* init */
	if (i2c_reg_read(base, I2C_CON) & OMAP_I2C_CON_EN) {
		i2c_reg_write(base, I2C_CON, 0);
		for (timeout = 0; timeout <= 50000; timeout += 1000)
			udelay(1000);
	}

	/* soft reset */
	i2c_reg_write(base, I2C_SYSC, 0x02);
	udelay(1000);
	i2c_reg_write(base, I2C_CON, OMAP_I2C_CON_EN);

	timeout = 1000;
	while (!(i2c_reg_read(base, I2C_SYSS) & 0x0001) && timeout--) {
		if (timeout <= 0) {
			pr_err("%s: Timeout in soft reset\n", __func__);
			return -ENODEV;
		}
		udelay(1000);
	}

	i2c_reg_write(base, I2C_CON, 0x0000);
	i2c_reg_write(base, I2C_PSC, psc);
	i2c_reg_write(base, I2C_SCLL, scll);
	i2c_reg_write(base, I2C_SCLH, sclh);
	i2c_reg_write(base, I2C_CON, 0x8000);
	udelay(1000);

	i2c_reg_write(base, I2C_OA, 1);

	/* flush fifo */
	flush_fifo(base);
	i2c_reg_write(base, I2C_IRQSTATUS, 0xffff);

	return 0;
}

static int i2c_read(void __iomem *base, u8 chip, u16 addr,
		unsigned int alen,
		void *buffer, int len)
{
	u8 *s = buffer;
	unsigned int i;
	u16 status;
	int timeout, ret;

	if (alen < 0 || len < 0 || !buffer || alen > 2 ||
			(addr + len > 0x10000))
		return -EINVAL;

	if (wait_for_bb(base) != 0) {
		pr_err("%s: wait for bb fail\n", __func__);
		return -ENODEV;
	}

	ret = -EINVAL;
	i2c_reg_write(base, I2C_SA, chip);

	if (alen) {
		i2c_reg_write(base, I2C_CNT, alen);

		/* Stop - Start (P-S) */
		i2c_reg_write(base, I2C_CON, OMAP_I2C_CON_EN |
			OMAP_I2C_CON_MST | OMAP_I2C_CON_STT |
			OMAP_I2C_CON_STP | OMAP_I2C_CON_TRX);
		while (alen > 0) {
			status = wait_for_event(base);
			if (status == 0 || (status & OMAP_I2C_STAT_NACK)) {
				ret = -ENODEV;
				pr_err("%s: error waiting for addr ACK\n",
						__func__);
				goto out;
			}
			if (status & OMAP_I2C_STAT_XRDY) {
				alen--;
				i2c_reg_write(base, I2C_DATA,
					(addr >> (8 * alen)) & 0xff);
				i2c_reg_write(base, I2C_IRQSTATUS,
						OMAP_I2C_STAT_XRDY);
			}
		}

		/* poll ARDY for last byte going out */
		timeout = 1000;
		do {
			status = wait_for_event(base);
			if (status & OMAP_I2C_STAT_ARDY)
				break;
			udelay(1000);
		} while (timeout--);

		if (timeout <= 0) {
			ret = -ENODEV;
			pr_err("%s: timeout waiting for ARDY\n",
					__func__);
			goto out;
		}

		i2c_reg_write(base, I2C_IRQSTATUS, OMAP_I2C_STAT_ARDY);

		wait_delay(base);
	}

	i2c_reg_write(base, I2C_CNT, len);

	i2c_reg_write(base, I2C_CON, OMAP_I2C_CON_EN | OMAP_I2C_CON_MST |
		OMAP_I2C_CON_STT | OMAP_I2C_CON_STP);

	i = 0;
	while (i < len) {
		status = wait_for_event(base);
		if (status == 0 || (status & OMAP_I2C_STAT_NACK)) {
			ret = -ENODEV;
			pr_err("%s: error waiting for data ACK\n",
					__func__);
			goto out;
		}
		if (status & OMAP_I2C_STAT_RRDY) {
			*s++ = (u8)i2c_reg_read(base, I2C_DATA);
			i2c_reg_write(base, I2C_IRQSTATUS,
					OMAP_I2C_STAT_RRDY);
			i++;
		}
		if (status & OMAP_I2C_STAT_ARDY) {
			i2c_reg_write(base, I2C_IRQSTATUS,
					OMAP_I2C_STAT_ARDY);
			break;
		}
	}

	if (i < len)
		pr_err("%s: short read (%u < %u)\n", __func__, i, len);
	ret = i;

out:
	flush_fifo(base);
	i2c_reg_write(base, I2C_IRQSTATUS, 0xffff);
	return ret;
}

struct am335x_baseboard_id {
	u8 magic[4];
	u8 name[8];
	u8 version[4];
	u8 serial[12];
	u8 config[32];
	u8 mac_addr[3][6];
};

static int beaglebone_read_header(struct am335x_baseboard_id *hdr)
{
	void __iomem *base;
	u32 magic;
	int ret;

	/* map I2C0 */
	base = ioremap((phys_addr_t)0x44E0B000, 0x1000);
	if (base == NULL) {
		pr_err("%s: failed to ioremap\n", __func__);
		return -ENOMEM;
	}

	ret = i2c_init(base, 0x0000, 0x00ea, 0x00ea);
	if (ret != 0) {
		pr_err("%s: i2c_init failed\n", __func__);
		return ret;
	}

	/* the EEPROM is at 0x50 */
	ret = i2c_read(base, 0x50, 0, 2, hdr, sizeof(*hdr));
	if (ret != sizeof(*hdr)) {
		pr_err("%s: Failed to read EEPROM\n", __func__);
		ret = ret >= 0 ? -EINVAL : ret;
		goto out;
	}

	print_hex_dump(KERN_DEBUG, "EEPROM: ", DUMP_PREFIX_OFFSET,
			16, 1, hdr, sizeof(*hdr), true);

	/* magic value is LE */
	magic = ((u32)hdr->magic[3] << 24) | ((u32)hdr->magic[2] << 16) |
		((u32)hdr->magic[1] << 8) | hdr->magic[0];
	if (magic != 0xEE3355AA) {
		pr_err("%s: Bad EEPROM (0x%08x) %02x %02x %02x %02x\n",
				__func__, magic,
				hdr->magic[0], hdr->magic[1],
				hdr->magic[2], hdr->magic[3]);
		ret = -EINVAL;
		goto out;
	}

	ret = 0;

out:
	iounmap(base);
	return ret;
}

/* find whether a command line argument exists */
static const char *command_line_arg(const char *bootargs, const char *what)
{
	const char *s, *e, *p;
	int len;

	len = strlen(what);
	s = bootargs;
	e = bootargs + strlen(bootargs);
	for (; s < e && (p = strstr(s, what)) != NULL; s += len) {

		/* if not the first arguments a space must precede */
		if (p > bootargs && p[-1] != ' ')
			continue;

		/* skip over what */
		p += len;

		/* if not the last argument a space must follow */
		if (p < e && *p != ' ')
			continue;

		return p;
	}

	return NULL;
}

static void __init beaglebone_dt_quirk(void)
{
	struct am335x_baseboard_id header;
	struct device_node *np, *revnp, *child, *optnp, *chosen;
	struct property *prop;
	char name[8 + 1];
	const char *detected_board_id, *board_id, *bootargs;
	int i, ret;
	phandle ph;

	np = NULL;
	revnp = NULL;
	optnp = NULL;
	chosen = NULL;

	/* beaglebone quirks */
	np = of_find_compatible_node(NULL, NULL, "ti,am33xx-bone-quirk");
	if (!np || !of_device_is_available(np))
		goto out;

	revnp = of_get_child_by_name(np, "revs");
	if (!revnp) {
		pr_err("%s: no revs node at %pO\n", __func__, np);
		goto out;
	}
	child = NULL;
	optnp = NULL;
	chosen = NULL;

	ret = beaglebone_read_header(&header);
	if (ret != 0) {
		pr_err("%s: Failed to read EEPROM\n", __func__);
		goto out;
	}
	memcpy(name, header.name, sizeof(header.name));
	name[sizeof(header.name)] = '\0';

	detected_board_id = name;

	pr_debug("%s: Finding quirks for board_id=%s\n", __func__,
			detected_board_id);
	for_each_child_of_node(revnp, child) {
		if (!of_property_read_string(child, "board-id", &board_id) &&
			!strcmp(board_id, detected_board_id))
			goto found;
	}
	pr_warn("%s: No quirks for board_id=%s\n", __func__, detected_board_id);
	goto out;
found:
	pr_debug("%s: Applying quirks for board_id=%s\n", __func__,
			board_id);
	ret = 0;
	for (i = 0; of_property_read_u32_index(child,
				"board-apply", i, &ph) == 0; i++) {

		ret = of_quirk_apply_by_phandle(ph);
		if (ret != 0)
			break;
	}
	if (ret != 0) {
		pr_err("%s: Failed to apply quirk at %pO\n", __func__, child);
		goto out;
	}

	optnp = of_get_child_by_name(child, "options");
	chosen = of_find_node_by_path("/chosen");
	bootargs = NULL;
	if (chosen) {
		if (of_property_read_string(chosen, "bootargs", &bootargs))
			bootargs = NULL;
		of_node_put(chosen);
	}
	if (optnp && bootargs) {
		/* iterate on properties */
		for_each_property_of_node(optnp, prop) {
			if (!strcmp(prop->name, "name"))
				continue;

			if (command_line_arg(bootargs, prop->name))
				i = 0;
			else
				i = 1;
			if (of_property_read_u32_index(optnp,
						prop->name, i, &ph) != 0) {
				pr_err("%s: Failed to get phandle at %pO/%s\n",
						__func__, optnp, prop->name);
				continue;
			}
			ret = of_quirk_apply_by_phandle(ph);
			if (ret != 0)
				break;
		}
	}

out:
	of_node_put(optnp);
	of_node_put(child);
	of_node_put(revnp);
	of_node_put(np);
}

void __init am33xx_dt_quirk(void)
{
	/* Manually issue calls for each supported board variant */

	/* the beaglebone is the one supported board for now */
	beaglebone_dt_quirk();
}
