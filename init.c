/*
 * (c) Copyright 2002-2010, Ralink Technology, Inc.
 * Copyright (C) 2014 Felix Fietkau <nbd@openwrt.org>
 * Copyright (C) 2015 Jakub Kicinski <kubakici@wp.pl>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "mt7601u.h"
#include "eeprom.h"
#include "trace.h"
#include "mcu.h"

static void
mt7601u_set_wlan_state(struct mt7601u_dev *dev, u32 val, bool enable)
{
	int i;

	/* TODO: we don't turn off WLAN_CLK because that makes the device
	 *	 not respond properly to the probe path.
	 *	 In case anyone (PSM?) wants to use this function we can
	 *	 bring the clock stuff back and fixup the probe path.
	 *
	 *	 MT_WLAN_FUN_CTRL_WLAN_CLK_EN | MT_WLAN_FUN_CTRL_PCIE_CLK_REQ
	 */

	if (enable)
		val |= (MT_WLAN_FUN_CTRL_WLAN_EN |
			MT_WLAN_FUN_CTRL_WLAN_CLK_EN);
	else
		val &= ~(MT_WLAN_FUN_CTRL_WLAN_EN);

	mt7601u_wr(dev, MT_WLAN_FUN_CTRL, val);
	udelay(20);

	dev->wlan_ctrl = val;

	if (!enable)
		return;

	for (i = 200; i; i--) {
		val = mt7601u_rr(dev, MT_CMB_CTRL);

		if (val & MT_CMB_CTRL_XTAL_RDY && val & MT_CMB_CTRL_PLL_LD)
			break;

		udelay(20);
	}

	/* Note: vendor driver tries to disable/enable wlan here and retry
	 *       but the code which does it is so buggy it must have never
	 *       triggered, so don't bother.
	 */
	if (!i)
		printk("Error: PLL and XTAL check failed!\n");
}

static void mt7601u_chip_onoff(struct mt7601u_dev *dev, bool enable, bool reset)
{
	u32 val;

	mutex_lock(&dev->hw_atomic_mutex);

	val = mt7601u_rr(dev, MT_WLAN_FUN_CTRL);

	if (reset) {
		val |= MT_WLAN_FUN_CTRL_GPIO_OUT_EN;
		val &= ~MT_WLAN_FUN_CTRL_FRC_WL_ANT_SEL;

		if (val & MT_WLAN_FUN_CTRL_WLAN_EN) {
			val |= (MT_WLAN_FUN_CTRL_WLAN_RESET |
				MT_WLAN_FUN_CTRL_WLAN_RESET_RF);
			mt7601u_wr(dev, MT_WLAN_FUN_CTRL, val);
			udelay(20);

			val &= ~(MT_WLAN_FUN_CTRL_WLAN_RESET |
				 MT_WLAN_FUN_CTRL_WLAN_RESET_RF);
		}
	}

	mt7601u_wr(dev, MT_WLAN_FUN_CTRL, val);
	udelay(20);

	mt7601u_set_wlan_state(dev, val, enable);

	mutex_unlock(&dev->hw_atomic_mutex);
}

u8 mt7601u_bbp_rr(struct mt7601u_dev *dev, u8 offset)
{
	u32 val;
	u8 ret = 0xff;

	if (!(dev->wlan_ctrl & MT_WLAN_FUN_CTRL_WLAN_EN)) {
		printk("Error: %s wlan not enabled\n", __func__);
		return 0xff;
	}
	if (test_bit(MT7601U_STATE_REMOVED, &dev->state))
		return 0xff;

	mutex_lock(&dev->reg_atomic_mutex);

	if (!mt76_poll(dev, MT_BBP_CSR_CFG, MT_BBP_CSR_CFG_BUSY, 0, 1000)) {
		printk("Error: BBP busy\n");
		goto out;
	}

	val = MT76_SET(MT_BBP_CSR_CFG_REG_NUM, offset) |
		MT_BBP_CSR_CFG_READ |
		MT_BBP_CSR_CFG_RW_MODE |
		MT_BBP_CSR_CFG_BUSY;
	mt7601u_wr(dev, MT_BBP_CSR_CFG, val);

	if (!mt76_poll(dev, MT_BBP_CSR_CFG, MT_BBP_CSR_CFG_BUSY, 0, 1000)) {
		printk("Error: BBP busy after cmd\n");
		goto out;
	}

	val = mt7601u_rr(dev, MT_BBP_CSR_CFG);
	if (MT76_GET(MT_BBP_CSR_CFG_REG_NUM, val) != offset) {
		printk("Error: BBP reg changed!?\n");
		goto out;
	}
	ret = MT76_GET(MT_BBP_CSR_CFG_VAL, val);
out:
	mutex_unlock(&dev->reg_atomic_mutex);

	trace_bbp_read(offset, ret);
	return ret;
}

void mt7601u_bbp_wr(struct mt7601u_dev *dev, u8 offset, u8 val)
{
	if (!(dev->wlan_ctrl & MT_WLAN_FUN_CTRL_WLAN_EN)) {
		printk("Error: %s wlan not enabled\n", __func__);
		return;
	}
	if (test_bit(MT7601U_STATE_REMOVED, &dev->state))
		return;

	mutex_lock(&dev->reg_atomic_mutex);

	if (!mt76_poll(dev, MT_BBP_CSR_CFG, MT_BBP_CSR_CFG_BUSY, 0, 1000)) {
		printk("Error: BBP busy\n");
		goto out;
	}

	mt7601u_wr(dev, MT_BBP_CSR_CFG,
		   val | MT76_SET(MT_BBP_CSR_CFG_REG_NUM, offset) |
		   MT_BBP_CSR_CFG_RW_MODE | MT_BBP_CSR_CFG_BUSY);
out:
	mutex_unlock(&dev->reg_atomic_mutex);
	trace_bbp_write(offset, val);
}

u8 mt7601u_bbp_rmw(struct mt7601u_dev *dev, u8 offset, u8 mask, u8 val)
{
	u8 reg = mt7601u_bbp_rr(dev, offset);
	val |= reg & ~mask;
	mt7601u_bbp_wr(dev, offset, val);
	return val;
}

u8 mt7601u_bbp_rmc(struct mt7601u_dev *dev, u8 offset, u8 mask, u8 val)
{
	u8 reg = mt7601u_bbp_rr(dev, offset);
	val |= reg & ~mask;
	if (reg != val)
		mt7601u_bbp_wr(dev, offset, val);
	return val;
}

static inline int bbp_ready(struct mt7601u_dev *dev)
{
	const u8 val = mt7601u_bbp_rr(dev, MT_BBP_REG_VERSION);
	return val && ~val;
}

static int mt7601u_chip_init_bbp(struct mt7601u_dev *dev)
{
	static const struct mt76_reg_pair vals[] = {
		{   1, 0x04 },	{   4, 0x40 },	{  20, 0x06 },	{  31, 0x08 },
		/* CCK Tx Control */
		{ 178, 0xff },
		/* AGC/Sync controls */
		{  66, 0x14 },	{  68, 0x8b },	{  69, 0x12 },	{  70, 0x09 },
		{  73, 0x11 },	{  75, 0x60 },	{  76, 0x44 },	{  84, 0x9a },
		{  86, 0x38 },	{  91, 0x07 },	{  92, 0x02 },
		/* Rx Path Controls */
		{  99, 0x50 },	{ 101, 0x00 },	{ 103, 0xc0 },	{ 104, 0x92 },
		{ 105, 0x3c },	{ 106, 0x03 },	{ 128, 0x12 },
		/* Change RXWI content: Gain Report */
		{ 142, 0x04 },	{ 143, 0x37 },
		/* Change RXWI content: Antenna Report */
		{ 142, 0x03 },	{ 143, 0x99 },
 		/* Calibration Index Register */
		/* CCK Receiver Control */
		{ 160, 0xeb },	{ 161, 0xc4 },	{ 162, 0x77 },	{ 163, 0xf9 },
		{ 164, 0x88 },	{ 165, 0x80 },	{ 166, 0xff },	{ 167, 0xe4 },
		/* Added AGC controls - these AGC/GLRT registers are accessed
		 * through R195 and R196.  */
		{ 195, 0x00 },	{ 196, 0x00 },
		{ 195, 0x01 },	{ 196, 0x04 },
		{ 195, 0x02 },	{ 196, 0x20 },
		{ 195, 0x03 },	{ 196, 0x0a },
		{ 195, 0x06 },	{ 196, 0x16 },
		{ 195, 0x07 },	{ 196, 0x05 },
		{ 195, 0x08 },	{ 196, 0x37 },
		{ 195, 0x0a },	{ 196, 0x15 },
		{ 195, 0x0b },	{ 196, 0x17 },
		{ 195, 0x0c },	{ 196, 0x06 },
		{ 195, 0x0d },	{ 196, 0x09 },
		{ 195, 0x0e },	{ 196, 0x05 },
		{ 195, 0x0f },	{ 196, 0x09 },
		{ 195, 0x10 },	{ 196, 0x20 },
		{ 195, 0x20 },	{ 196, 0x17 },
		{ 195, 0x21 },	{ 196, 0x06 },
		{ 195, 0x22 },	{ 196, 0x09 },
		{ 195, 0x23 },	{ 196, 0x17 },
		{ 195, 0x24 },	{ 196, 0x06 },
		{ 195, 0x25 },	{ 196, 0x09 },
		{ 195, 0x26 },	{ 196, 0x17 },
		{ 195, 0x27 },	{ 196, 0x06 },
		{ 195, 0x28 },	{ 196, 0x09 },
		{ 195, 0x29 },	{ 196, 0x05 },
		{ 195, 0x2a },	{ 196, 0x09 },
		{ 195, 0x80 },	{ 196, 0x8b },
		{ 195, 0x81 },	{ 196, 0x12 },
		{ 195, 0x82 },	{ 196, 0x09 },
		{ 195, 0x83 },	{ 196, 0x17 },
		{ 195, 0x84 },	{ 196, 0x11 },
		{ 195, 0x85 },	{ 196, 0x00 },
		{ 195, 0x86 },	{ 196, 0x00 },
		{ 195, 0x87 },	{ 196, 0x18 },
		{ 195, 0x88 },	{ 196, 0x60 },
		{ 195, 0x89 },	{ 196, 0x44 },
		{ 195, 0x8a },	{ 196, 0x8b },
		{ 195, 0x8b },	{ 196, 0x8b },
		{ 195, 0x8c },	{ 196, 0x8b },
		{ 195, 0x8d },	{ 196, 0x8b },
		{ 195, 0x8e },	{ 196, 0x09 },
		{ 195, 0x8f },	{ 196, 0x09 },
		{ 195, 0x90 },	{ 196, 0x09 },
		{ 195, 0x91 },	{ 196, 0x09 },
		{ 195, 0x92 },	{ 196, 0x11 },
		{ 195, 0x93 },	{ 196, 0x11 },
		{ 195, 0x94 },	{ 196, 0x11 },
		{ 195, 0x95 },	{ 196, 0x11 },
		/* PPAD */
		{  47, 0x80 },	{  60, 0x80 },	{ 150, 0xd2 },	{ 151, 0x32 },
		{ 152, 0x23 },	{ 153, 0x41 },	{ 154, 0x00 },	{ 155, 0x4f },
		{ 253, 0x7e },	{ 195, 0x30 },	{ 196, 0x32 },	{ 195, 0x31 },
		{ 196, 0x23 },	{ 195, 0x32 },	{ 196, 0x45 },	{ 195, 0x35 },
		{ 196, 0x4a },	{ 195, 0x36 },	{ 196, 0x5a },	{ 195, 0x37 },
		{ 196, 0x5a },
	};

	return mt7601u_write_reg_pairs(dev, MT7601U_MCU_MEMMAP_BBP,
				       vals, ARRAY_SIZE(vals));
}

static int mt7601u_init_bbp(struct mt7601u_dev *dev)
{
	static const struct mt76_reg_pair vals[] = {
		{  65,	0x2c },
		{  66,	0x38 },
		{  68,	0x0b },
		{  69,	0x12 },
		{  70,	0x0a },
		{  73,	0x10 },
		{  81,	0x37 },
		{  82,	0x62 },
		{  83,	0x6a },
		{  84,	0x99 },
		{  86,	0x00 },
		{  91,	0x04 },
		{  92,	0x00 },
		{ 103,	0x00 },
		{ 105,	0x05 },
		{ 106,	0x35 },
	};
	int i;

	for (i = 20; i && !bbp_ready(dev); i--)
		;
	if (!i) {
		printk("Error: BBP is not ready\n");
		return -EIO;
	}

	for (i = 0; i < ARRAY_SIZE(vals); i++)
		mt7601u_bbp_wr(dev, vals[i].reg, vals[i].value);

	return mt7601u_chip_init_bbp(dev);
}

static void
mt76_init_beacon_offsets(struct mt7601u_dev *dev)
{
	u16 base = MT_BEACON_BASE;
	u32 regs[4] = {};
	int i;

	for (i = 0; i < 16; i++) {
		u16 addr = dev->beacon_offsets[i];

		regs[i / 4] |= ((addr - base) / 64) << (8 * (i % 4));
	}

	for (i = 0; i < 4; i++)
		mt7601u_wr(dev, MT_BCN_OFFSET(i), regs[i]);
}

static void mt7601u_reset_csr_bbp(struct mt7601u_dev *dev)
{
	mt7601u_wr(dev, MT_MAC_SYS_CTRL, (MT_MAC_SYS_CTRL_RESET_CSR |
					  MT_MAC_SYS_CTRL_RESET_BBP));
	mt7601u_wr(dev, MT_USB_DMA_CFG, 0);
	msleep(1);
	mt7601u_wr(dev, MT_MAC_SYS_CTRL, 0);
}

static int mt7601u_write_mac_initvals(struct mt7601u_dev *dev)
{
#define MAX_AGGREGATION_SIZE    3840 /* TODO: drop this */
	static const struct mt76_reg_pair vals[] = {
		{ MT_LEGACY_BASIC_RATE,		0x0000013f },
		{ MT_HT_BASIC_RATE,		0x00008003 },
		{ MT_MAC_SYS_CTRL,		0x00000000 },
		{ MT_RX_FILTR_CFG,		0x00017f97 },
		{ MT_BKOFF_SLOT_CFG,		0x00000209 },
		{ MT_TX_SW_CFG0,		0x00000000 },
		{ MT_TX_SW_CFG1,		0x00080606 },
		{ MT_TX_LINK_CFG,		0x00001020 },
		{ MT_TX_TIMEOUT_CFG,		0x000a2090 },
		{ MT_MAX_LEN_CFG,		MAX_AGGREGATION_SIZE | 0x00001000 },
		{ MT_PBF_TX_MAX_PCNT,		0x1fbf1f1f },
		{ MT_PBF_RX_MAX_PCNT,		0x0000009f },
		{ MT_TX_RETRY_CFG,		0x47d01f0f },
		{ MT_AUTO_RSP_CFG,		0x00000013 },
		{ MT_CCK_PROT_CFG,		0x05740003 },
		{ MT_OFDM_PROT_CFG,		0x05740003 },
		{ MT_MM40_PROT_CFG,		0x03f44084 },
		{ MT_GF20_PROT_CFG,		0x01744004 },
		{ MT_GF40_PROT_CFG,		0x03f44084 },
		{ MT_MM20_PROT_CFG,		0x01744004 },
		{ MT_TXOP_CTRL_CFG,		0x0000583f },
		{ MT_TX_RTS_CFG,		0x01092b20 },
		{ MT_EXP_ACK_TIME,		0x002400ca },
		{ MT_TXOP_HLDR_ET, 		0x00000002 },
		{ MT_XIFS_TIME_CFG,		0x33a41010 },
		{ MT_PWR_PIN_CFG,		0x00000000 },
	};

	return mt7601u_write_reg_pairs(dev, MT_MCU_MEMMAP_OFFSET,
				       vals, ARRAY_SIZE(vals));
}

static int mt7601u_write_chip_mac_initvals(struct mt7601u_dev *dev)
{
	int ret;
	static const struct mt76_reg_pair vals[] = {
		{ MT_TSO_CTRL, 			0x00006050 },
		{ MT_BCN_OFFSET(0),		0x18100800 }, /*< TODO: did I */
		{ MT_BCN_OFFSET(1),		0x38302820 }, /*<  not just   */
		{ MT_PBF_SYS_CTRL,		0x00080c00 }, /*   init that? */
		{ MT_PBF_CFG,			0x7f723c1f },
		{ MT_FCE_PSE_CTRL,		0x00000001 },
		{ MT_PAUSE_ENABLE_CONTROL1,	0x00000000 },
		{ MT_TX0_RF_GAIN_CORR,		0x003b0005 },
		{ MT_TX0_RF_GAIN_ATTEN,		0x00006900 },
		{ MT_TX0_BB_GAIN_ATTEN,		0x00000400 },
		{ MT_TX_ALC_VGA3,		0x00060006 },
		{ MT_TX_SW_CFG0,		0x00000402 },
		{ MT_TX_SW_CFG1,		0x00000000 },
		{ MT_TX_SW_CFG2,		0x00000000 },
		{ MT_HEADER_TRANS_CTRL_REG,	0x00000000 },
		{ MT_FCE_CSO,			0x0000030f },
		{ MT_FCE_PARAMETERS,		0x00256f0f },
	};

	ret = mt7601u_write_reg_pairs(dev, MT_MCU_MEMMAP_OFFSET,
				      vals, ARRAY_SIZE(vals));
	if (ret)
		return ret;

	/* TODO: redundant - initvals already released the reset. */
	mt76_clear(dev, MT_MAC_SYS_CTRL, (MT_MAC_SYS_CTRL_RESET_CSR |
					  MT_MAC_SYS_CTRL_RESET_BBP));
	mt7601u_wr(dev, MT_AUX_CLK_CFG, 0);

	return 0;
}

/* TODO: this is embarrassingly dumb code. */
static int mt7601u_init_wcid_mem(struct mt7601u_dev *dev)
{
	u32 *vals;
	int i, ret;

	vals = kmalloc(sizeof(*vals) * 128 * 2, GFP_KERNEL);

	for (i = 0; i < 128; i++)  {
		vals[i * 2] = 0xffffffff;
		vals[i * 2 + 1] = 0x00ffffff;
	}

	ret = mt7601u_burst_write_regs(dev, MT_WCID_ADDR_BASE, vals, 128 * 2);

	kfree(vals);

	return ret;
}

static int mt7601u_init_key_mem(struct mt7601u_dev *dev)
{
	u32 vals[4] = {};

	return mt7601u_burst_write_regs(dev, MT_SKEY_MODE_BASE_0,
					vals, ARRAY_SIZE(vals));
}

static int mt7601u_init_wcid_attr_mem(struct mt7601u_dev *dev)
{
	u32 *vals;
	int i, ret;

	vals = kmalloc(sizeof(*vals) * 256, GFP_KERNEL);

	for (i = 0; i < 256; i++)
		vals[i] = 1;

	ret = mt7601u_burst_write_regs(dev, MT_WCID_ATTR_BASE, vals, 128 * 2);

	kfree(vals);

	return ret;
}

static void mt7601u_reset_counters(struct mt7601u_dev *dev)
{
	mt7601u_rr(dev, MT_RX_STA_CNT0);
	mt7601u_rr(dev, MT_RX_STA_CNT1);
	mt7601u_rr(dev, MT_RX_STA_CNT2);
	mt7601u_rr(dev, MT_TX_STA_CNT0);
	mt7601u_rr(dev, MT_TX_STA_CNT1);
	mt7601u_rr(dev, MT_TX_STA_CNT2);
}

static void mt7601u_set_default_edca(struct mt7601u_dev *dev)
{
	static const struct edca_cfg {
		u8 aifs;
		u8 cw_min;
		u8 cw_max;
		u8 txop;
	} params[4] = {
		{ .aifs = 3,	.cw_min = 4,	.cw_max = 6,	.txop = 0 },
		{ .aifs = 7,	.cw_min = 4,	.cw_max = 10,	.txop = 0 },
		{ .aifs = 1,	.cw_min = 3,	.cw_max = 4,	.txop = 94 },
		{ .aifs = 1,	.cw_min = 2,	.cw_max = 3,	.txop = 47 }
	};
	u32 val;
	int i;

	for (i = 0; i < 2; i++) {
		val = MT76_SET(MT_EDCA_CFG_TXOP, params[i].txop) |
			MT76_SET(MT_EDCA_CFG_AIFSN, params[i].aifs) |
			MT76_SET(MT_EDCA_CFG_CWMIN, params[i].cw_min) |
			MT76_SET(MT_EDCA_CFG_CWMAX, params[i].cw_max);
		mt76_wr(dev, MT_EDCA_CFG_AC(i), val);
	}

	val = MT76_SET(MT_EDCA_CFG_TXOP, (params[2].txop * 6) / 10) |
	      MT76_SET(MT_EDCA_CFG_AIFSN, params[2].aifs + 1) |
	      MT76_SET(MT_EDCA_CFG_CWMIN, params[2].cw_min) |
	      MT76_SET(MT_EDCA_CFG_CWMAX, params[2].cw_max);
	mt76_wr(dev, MT_EDCA_CFG_AC(2), val);
	val = MT76_SET(MT_EDCA_CFG_TXOP, params[3].txop) |
		MT76_SET(MT_EDCA_CFG_AIFSN, params[3].aifs) |
		MT76_SET(MT_EDCA_CFG_CWMIN, params[3].cw_min) |
		MT76_SET(MT_EDCA_CFG_CWMAX, params[3].cw_max);
	mt76_wr(dev, MT_EDCA_CFG_AC(3), val);

	mt76_wr(dev, MT_WMM_TXOP(0), (u32)params[0].txop |
		((u32)params[1].txop << 16));
	mt76_wr(dev, MT_WMM_TXOP(2), ((u32)params[2].txop * 6) / 10 |
		((u32)params[3].txop << 16));

	val = 0;
	for (i = 3; i >= 0 ; i--)
		val = (val << 4) | params[i].cw_min;
	val -= 0x1000;
	mt76_wr(dev, MT_WMM_CWMIN, val);

	val = 0;
	for (i = 3; i >= 0 ; i--)
		val = (val << 4) | params[i].cw_max;
	mt76_wr(dev, MT_WMM_CWMAX, val);

	val = 0x0293;
	mt76_wr(dev, MT_WMM_AIFSN, val);
}

int mt7601u_mac_start(struct mt7601u_dev *dev)
{
	/* TODO: move counter clears here */

	mt7601u_wr(dev, MT_MAC_SYS_CTRL, MT_MAC_SYS_CTRL_ENABLE_TX);

	if (!mt76_poll(dev, MT_WPDMA_GLO_CFG, MT_WPDMA_GLO_CFG_TX_DMA_BUSY |
		       MT_WPDMA_GLO_CFG_RX_DMA_BUSY, 0, 200000))
		return -ETIMEDOUT;

	dev->rxfilter = MT_RX_FILTR_CFG_CRC_ERR |
		MT_RX_FILTR_CFG_PHY_ERR | MT_RX_FILTR_CFG_PROMISC |
		MT_RX_FILTR_CFG_VER_ERR | MT_RX_FILTR_CFG_DUP |
		MT_RX_FILTR_CFG_CFACK | MT_RX_FILTR_CFG_CFEND |
		MT_RX_FILTR_CFG_ACK | MT_RX_FILTR_CFG_CTS |
		MT_RX_FILTR_CFG_RTS | MT_RX_FILTR_CFG_PSPOLL |
		MT_RX_FILTR_CFG_BA | MT_RX_FILTR_CFG_CTRL_RSV;
	mt7601u_wr(dev, MT_RX_FILTR_CFG, dev->rxfilter);

	mt7601u_wr(dev, MT_MAC_SYS_CTRL,
		   MT_MAC_SYS_CTRL_ENABLE_TX | MT_MAC_SYS_CTRL_ENABLE_RX);

	/* TODO: drop this, IRQs are not there any more. */
	mt7601u_rr(dev, MT_EDCA_CFG_BASE);

	if (!mt76_poll(dev, MT_WPDMA_GLO_CFG, MT_WPDMA_GLO_CFG_TX_DMA_BUSY |
		       MT_WPDMA_GLO_CFG_RX_DMA_BUSY, 0, 50))
		return -ETIMEDOUT;

	if (!mt76_poll(dev, MT_WPDMA_GLO_CFG, MT_WPDMA_GLO_CFG_TX_DMA_BUSY |
		       MT_WPDMA_GLO_CFG_RX_DMA_BUSY, 0, 50))
		return -ETIMEDOUT;

	return 0;
}

static void mt7601u_mac_stop_hw(struct mt7601u_dev *dev)
{
	int i, ok;

	if (test_bit(MT7601U_STATE_REMOVED, &dev->state))
		return;

	mt76_clear(dev, MT_BEACON_TIME_CFG, MT_BEACON_TIME_CFG_TIMER_EN |
		   MT_BEACON_TIME_CFG_SYNC_MODE | MT_BEACON_TIME_CFG_TBTT_EN |
		   MT_BEACON_TIME_CFG_BEACON_TX);

	if (!mt76_poll(dev, MT_USB_DMA_CFG, MT_USB_DMA_CFG_TX_BUSY, 0, 1000))
		printk("Error: TX DMA did not stop!\n");

	/* Page count on TxQ */
	i = 200;
	while (i-- && ((mt76_rr(dev, 0x0438) & 0xffffffff) ||
		       (mt76_rr(dev, 0x0a30) & 0x000000ff) ||
		       (mt76_rr(dev, 0x0a34) & 0x00ff00ff)))
		msleep(10);

	if (!mt76_poll(dev, MT_MAC_STATUS, MT_MAC_STATUS_TX, 0, 1000))
		printk("Error: MAC TX did not stop!\n");

	mt76_clear(dev, MT_MAC_SYS_CTRL, MT_MAC_SYS_CTRL_ENABLE_RX);

	/* Page count on RxQ */
	ok = 0;
	i = 200;
	while (i--) {
		if ((mt76_rr(dev, 0x0430) & 0x00ff0000) ||
		    (mt76_rr(dev, 0x0a30) & 0xffffffff) ||
		    (mt76_rr(dev, 0x0a34) & 0xffffffff))
			ok++;
		if (ok > 6)
			break;

		msleep(1);
		/* TODO: vendor does rx and cmd processing here. */
	}

	if (!mt76_poll(dev, MT_MAC_STATUS, MT_MAC_STATUS_RX, 0, 1000))
		printk("Error: MAC RX did not stop!\n");

	/* TODO: drain RX queue */

	if (!mt76_poll(dev, MT_USB_DMA_CFG, MT_USB_DMA_CFG_RX_BUSY, 0, 1000))
		printk("Error: RX DMA did not stop!\n");
}

void mt7601u_mac_stop(struct mt7601u_dev *dev)
{
	mt7601u_mac_stop_hw(dev);
	flush_workqueue(dev->stat_wq);
}

static void mt7601u_stop_hardware(struct mt7601u_dev *dev)
{
	mt7601u_chip_onoff(dev, false, false);
}

int mt7601u_init_hardware(struct mt7601u_dev *dev)
{
	static const u16 beacon_offsets[16] = {
		/* 512 byte per beacon */
		0xc000,
		0xc200,
		0xc400,
		0xc600,
		0xc800,
		0xca00,
		0xcc00,
		0xce00,
		0xd000,
		0xd200,
		0xd400,
		0xd600,
		0xd800,
		0xda00,
		0xdc00,
		0xde00
	};
	int ret;
	u32 val;

	dev->beacon_offsets = beacon_offsets;

	mt7601u_chip_onoff(dev, true, false);

	ret = mt7601u_wait_asic_ready(dev);
	if (ret)
		goto err;

	ret = mt7601u_mcu_init(dev);
	if (ret)
		goto err;

	if (!mt76_poll_msec(dev, MT_WPDMA_GLO_CFG,
			    MT_WPDMA_GLO_CFG_TX_DMA_BUSY |
			    MT_WPDMA_GLO_CFG_RX_DMA_BUSY, 0, 100)) {
		ret = -EIO;
		goto err;
	}

	/* Wait for ASIC ready after FW load. */
	ret = mt7601u_wait_asic_ready(dev);
	if (ret)
		goto err;

	mt7601u_reset_csr_bbp(dev);

	val = MT76_SET(MT_USB_DMA_CFG_RX_BULK_AGG_TOUT, 0x80) | /* TODO: val */
		MT76_SET(MT_USB_DMA_CFG_RX_BULK_AGG_LMT, 21) |  /* TODO: val */
		MT_USB_DMA_CFG_RX_BULK_EN | MT_USB_DMA_CFG_TX_BULK_EN;
	if (dev->in_max_packet == 512)
		val |= MT_USB_DMA_CFG_RX_BULK_AGG_EN;
	mt7601u_wr(dev, MT_USB_DMA_CFG, val);

	val |= MT_USB_DMA_CFG_UDMA_RX_WL_DROP;
	mt7601u_wr(dev, MT_USB_DMA_CFG, val);
	val &= ~MT_USB_DMA_CFG_UDMA_RX_WL_DROP;
	mt7601u_wr(dev, MT_USB_DMA_CFG, val);

	ret = mt7601u_mcu_cmd_init(dev);
	if (ret)
		goto err;

	ret = mt7601u_dma_init(dev);
	if (ret)
		goto err_mcu;

	ret = mt7601u_write_mac_initvals(dev);
	if (ret)
		goto err_rx;

	mt76_init_beacon_offsets(dev);

	ret = mt7601u_write_chip_mac_initvals(dev);
	if (ret)
		goto err_rx;

	if (!mt76_poll_msec(dev, MT_MAC_STATUS,
			    MT_MAC_STATUS_TX | MT_MAC_STATUS_RX, 0, 100)) {
		ret = -EIO;
		goto err_rx;
	}

	ret = mt7601u_init_bbp(dev);
	if (ret)
		goto err_rx;

	/* TODO: this is set to sth else in initvals */
	mt76_set(dev, MT_MAX_LEN_CFG, 0x3fff);

	ret = mt7601u_init_wcid_mem(dev);
	if (ret)
		goto err_rx;

	/* TODO: we will just do it again few lines below... */
	mt7601u_reset_counters(dev);

	ret = mt7601u_init_key_mem(dev);
	if (ret)
		goto err_rx;

	ret = mt7601u_init_wcid_attr_mem(dev);
	if (ret)
		goto err_rx;

	mt76_clear(dev, MT_BEACON_TIME_CFG, (MT_BEACON_TIME_CFG_TIMER_EN |
					     MT_BEACON_TIME_CFG_SYNC_MODE |
					     MT_BEACON_TIME_CFG_TBTT_EN |
					     MT_BEACON_TIME_CFG_BEACON_TX));

	mt7601u_reset_counters(dev);

	mt7601u_rmw(dev, MT_USB_CYC_CFG, 0xffffff00, 0x1e);

	mt7601u_wr(dev, MT_TXOP_CTRL_CFG, 0x583f);

	ret = mt7601u_eeprom_init(dev);
	if (ret)
		goto err_rx;

	ret = mt7601u_phy_init(dev);
	if (ret)
		goto err_rx;

	mt7601u_set_rx_path(dev, 0);
	mt7601u_set_tx_dac(dev, 0);

	mt7601u_mac_set_ctrlch(dev, false);
	mt7601u_bbp_set_ctrlch(dev, false); /* Note: *not* in vendor driver */
	mt7601u_bbp_set_bw(dev, MT_BW_20);
	mt7601u_set_default_edca(dev);

	return 0;

err_rx:
	mt7601u_dma_cleanup(dev);
err_mcu:
	mt7601u_mcu_cmd_deinit(dev);
err:
	mt7601u_chip_onoff(dev, false, false);
	return ret;
}

void mt7601u_cleanup(struct mt7601u_dev *dev)
{
	mt7601u_stop_hardware(dev);
	mt7601u_dma_cleanup(dev);
	mt7601u_mcu_cmd_deinit(dev);
}

struct mt7601u_dev *mt7601u_alloc_device(struct device *pdev)
{
	struct ieee80211_hw *hw;
	struct mt7601u_dev *dev;

	hw = ieee80211_alloc_hw(sizeof(*dev), &mt7601u_ops);
	if (!hw)
		return NULL;

	dev = hw->priv;
	dev->dev = pdev;
	dev->hw = hw;
	mutex_init(&dev->vendor_req_mutex);
	mutex_init(&dev->reg_atomic_mutex);
	mutex_init(&dev->hw_atomic_mutex);
	mutex_init(&dev->mutex);
	spin_lock_init(&dev->tx_lock);
	spin_lock_init(&dev->rx_lock);
	spin_lock_init(&dev->lock);
	spin_lock_init(&dev->last_beacon.lock);
	atomic_set(&dev->avg_ampdu_len, 1);

	dev->stat_wq = alloc_workqueue("mt7601u", WQ_UNBOUND, 0);
	if (!dev->stat_wq) {
		ieee80211_free_hw(hw);
		return NULL;
	}

	return dev;
}

static const struct ieee80211_iface_limit if_limits[] = {
	{
		.max = 1,
		.types = BIT(NL80211_IFTYPE_ADHOC)
	}, {
		.max = 8,
		.types = BIT(NL80211_IFTYPE_STATION) |
#ifdef CONFIG_MAC80211_MESH
			 BIT(NL80211_IFTYPE_MESH_POINT) |
#endif
			 BIT(NL80211_IFTYPE_AP)
	 },
};

static const struct ieee80211_iface_combination if_comb[] = {
	{
		.limits = if_limits,
		.n_limits = ARRAY_SIZE(if_limits),
		.max_interfaces = 8,
		.num_different_channels = 1,
		.beacon_int_infra_match = true,
	}
};

#define CHAN2G(_idx, _freq) {			\
	.band = IEEE80211_BAND_2GHZ,		\
	.center_freq = (_freq),			\
	.hw_value = (_idx),			\
	.max_power = 30,			\
}

static const struct ieee80211_channel mt76_channels_2ghz[] = {
	CHAN2G(1, 2412),
	CHAN2G(2, 2417),
	CHAN2G(3, 2422),
	CHAN2G(4, 2427),
	CHAN2G(5, 2432),
	CHAN2G(6, 2437),
	CHAN2G(7, 2442),
	CHAN2G(8, 2447),
	CHAN2G(9, 2452),
	CHAN2G(10, 2457),
	CHAN2G(11, 2462),
	CHAN2G(12, 2467),
	CHAN2G(13, 2472),
	CHAN2G(14, 2484),
};

#define CCK_RATE(_idx, _rate) {					\
	.bitrate = _rate,					\
	.flags = IEEE80211_RATE_SHORT_PREAMBLE,			\
	.hw_value = (MT_PHY_TYPE_CCK << 8) | _idx,		\
	.hw_value_short = (MT_PHY_TYPE_CCK << 8) | (8 + _idx),	\
}

#define OFDM_RATE(_idx, _rate) {				\
	.bitrate = _rate,					\
	.hw_value = (MT_PHY_TYPE_OFDM << 8) | _idx,		\
	.hw_value_short = (MT_PHY_TYPE_OFDM << 8) | _idx,	\
}

static struct ieee80211_rate mt76_rates[] = {
	CCK_RATE(0, 10),
	CCK_RATE(1, 20),
	CCK_RATE(2, 55),
	CCK_RATE(3, 110),
	OFDM_RATE(0, 60),
	OFDM_RATE(1, 90),
	OFDM_RATE(2, 120),
	OFDM_RATE(3, 180),
	OFDM_RATE(4, 240),
	OFDM_RATE(5, 360),
	OFDM_RATE(6, 480),
	OFDM_RATE(7, 540),
};

static int
mt76_init_sband(struct mt76_dev *dev, struct ieee80211_supported_band *sband,
		const struct ieee80211_channel *chan, int n_chan,
		struct ieee80211_rate *rates, int n_rates)
{
	struct ieee80211_sta_ht_cap *ht_cap;
	void *chanlist;
	int size;

	size = n_chan * sizeof(*chan);
	chanlist = devm_kmemdup(dev->dev, chan, size, GFP_KERNEL);
	if (!chanlist)
		return -ENOMEM;

	sband->channels = chanlist;
	sband->n_channels = n_chan;
	sband->bitrates = rates;
	sband->n_bitrates = n_rates;

	ht_cap = &sband->ht_cap;
	ht_cap->ht_supported = true;
	ht_cap->cap = IEEE80211_HT_CAP_SUP_WIDTH_20_40 |
		      IEEE80211_HT_CAP_GRN_FLD |
		      IEEE80211_HT_CAP_SGI_20 |
		      IEEE80211_HT_CAP_SGI_40 |
		      (1 << IEEE80211_HT_CAP_RX_STBC_SHIFT);

	ht_cap->mcs.rx_mask[0] = 0xff;
	ht_cap->mcs.rx_mask[4] = 0x1;
	ht_cap->mcs.tx_params = IEEE80211_HT_MCS_TX_DEFINED;
	ht_cap->ampdu_factor = IEEE80211_HT_MAX_AMPDU_64K;
	ht_cap->ampdu_density = IEEE80211_HT_MPDU_DENSITY_2;

	dev->chandef.chan = &sband->channels[0];

	return 0;
}

static int
mt76_init_sband_2g(struct mt76_dev *dev)
{
	dev->sband_2g = devm_kzalloc(dev->dev, sizeof(*dev->sband_2g),
				     GFP_KERNEL);
	dev->hw->wiphy->bands[IEEE80211_BAND_2GHZ] = dev->sband_2g;

	WARN_ON(dev->ee->reg.start - 1 + dev->ee->reg.num >
		ARRAY_SIZE(mt76_channels_2ghz));

	return mt76_init_sband(dev, dev->sband_2g,
			       &mt76_channels_2ghz[dev->ee->reg.start - 1],
			       dev->ee->reg.num,
			       mt76_rates, ARRAY_SIZE(mt76_rates));
}

int mt7601u_register_device(struct mt7601u_dev *dev)
{
	struct ieee80211_hw *hw = dev->hw;
	struct wiphy *wiphy = hw->wiphy;
	int ret;

	/* TODO: this should disappear after WCID clean up */
	/* Reserve WCID 0 for mcast - thanks to this APs WCID will go to
	 * entry no. 1 like in the vendor driver.
	 */
	dev->wcid_mask[0] |= 1;

	/* init fake wcid for monitor interfaces */
	dev->mon_wcid = devm_kmalloc(dev->dev, sizeof(*dev->mon_wcid),
				     GFP_KERNEL);
	if (!dev->mon_wcid)
		return -ENOMEM;
	dev->mon_wcid->idx = 0xff;
	dev->mon_wcid->hw_key_idx = -1;

	SET_IEEE80211_DEV(hw, dev->dev);

	hw->queues = 4;
	hw->flags = IEEE80211_HW_SIGNAL_DBM |
		    IEEE80211_HW_PS_NULLFUNC_STACK |
		    IEEE80211_HW_SUPPORTS_HT_CCK_RATES |
		    IEEE80211_HW_AMPDU_AGGREGATION |
#ifdef MAC80211_IS_PATCHED
		    IEEE80211_HW_TX_STATS_EVERY_MPDU |
#endif
		    IEEE80211_HW_SUPPORTS_RC_TABLE;
	hw->max_rates = 1;
	hw->max_report_rates = 7;
	hw->max_rate_tries = 1;

	hw->sta_data_size = sizeof(struct mt76_sta);
	hw->vif_data_size = sizeof(struct mt76_vif);
	//hw->txq_data_size = sizeof(struct mt76_txq);

	SET_IEEE80211_PERM_ADDR(hw, dev->macaddr);

	wiphy->features |= NL80211_FEATURE_ACTIVE_MONITOR;

	wiphy->interface_modes =
		BIT(NL80211_IFTYPE_STATION) |
		BIT(NL80211_IFTYPE_AP) |
#ifdef CONFIG_MAC80211_MESH
		BIT(NL80211_IFTYPE_MESH_POINT) |
#endif
		BIT(NL80211_IFTYPE_ADHOC);

	wiphy->iface_combinations = if_comb;
	wiphy->n_iface_combinations = ARRAY_SIZE(if_comb);

	ret = mt76_init_sband_2g(dev);
	if (ret)
		return ret;

	INIT_DELAYED_WORK(&dev->mac_work, mt7601u_mac_work);
	INIT_DELAYED_WORK(&dev->stat_work, mt7601u_tx_stat);

	ret = ieee80211_register_hw(hw);
	if (ret)
		return ret;

	mt7601u_init_debugfs(dev);

	return 0;
}
