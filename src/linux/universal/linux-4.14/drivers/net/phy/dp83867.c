/*
 * Driver for the Texas Instruments DP83867 PHY
 *
 * Copyright (C) 2015 Texas Instruments Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/ethtool.h>
#include <linux/kernel.h>
#include <linux/mii.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy.h>

#include <dt-bindings/net/ti-dp83867.h>

#define DP83867_PHY_ID		0x2000a231
#define DP83867_DEVADDR		0x1f

#define MII_DP83867_PHYCTRL	0x10
#define MII_DP83867_MICR	0x12
#define MII_DP83867_ISR		0x13
#define DP83867_CTRL		0x1f
#define DP83867_CFG3		0x1e

/* Extended Registers */
#define DP83867_CFG4            0x0031
#define DP83867_CFG4_SGMII_ANEG_MASK (BIT(5) | BIT(6))
#define DP83867_CFG4_SGMII_ANEG_TIMER_11MS   (3 << 5)
#define DP83867_CFG4_SGMII_ANEG_TIMER_800US  (2 << 5)
#define DP83867_CFG4_SGMII_ANEG_TIMER_2US    (1 << 5)
#define DP83867_CFG4_SGMII_ANEG_TIMER_16MS   (0 << 5)

#define DP83867_RGMIICTL	0x0032
#define DP83867_STRAP_STS1	0x006E
#define DP83867_RGMIIDCTL	0x0086
#define DP83867_IO_MUX_CFG	0x0170
#define DP83867_10M_SGMII_CFG   0x016F
#define DP83867_10M_SGMII_RATE_ADAPT_MASK BIT(7)

#define DP83867_SW_RESET	BIT(15)
#define DP83867_SW_RESTART	BIT(14)

/* MICR Interrupt bits */
#define MII_DP83867_MICR_AN_ERR_INT_EN		BIT(15)
#define MII_DP83867_MICR_SPEED_CHNG_INT_EN	BIT(14)
#define MII_DP83867_MICR_DUP_MODE_CHNG_INT_EN	BIT(13)
#define MII_DP83867_MICR_PAGE_RXD_INT_EN	BIT(12)
#define MII_DP83867_MICR_AUTONEG_COMP_INT_EN	BIT(11)
#define MII_DP83867_MICR_LINK_STS_CHNG_INT_EN	BIT(10)
#define MII_DP83867_MICR_FALSE_CARRIER_INT_EN	BIT(8)
#define MII_DP83867_MICR_SLEEP_MODE_CHNG_INT_EN	BIT(4)
#define MII_DP83867_MICR_WOL_INT_EN		BIT(3)
#define MII_DP83867_MICR_XGMII_ERR_INT_EN	BIT(2)
#define MII_DP83867_MICR_POL_CHNG_INT_EN	BIT(1)
#define MII_DP83867_MICR_JABBER_INT_EN		BIT(0)

/* RGMIICTL bits */
#define DP83867_RGMII_TX_CLK_DELAY_EN		BIT(1)
#define DP83867_RGMII_RX_CLK_DELAY_EN		BIT(0)

/* STRAP_STS1 bits */
#define DP83867_STRAP_STS1_RESERVED		BIT(11)

/* PHY CTRL bits */
#define DP83867_PHYCR_FIFO_DEPTH_SHIFT		14
#define DP83867_PHYCR_FIFO_DEPTH_MASK		(3 << 14)
#define DP83867_PHYCR_RESERVED_MASK		BIT(11)

/* RGMIIDCTL bits */
#define DP83867_RGMII_TX_CLK_DELAY_SHIFT	4

/* IO_MUX_CFG bits */
#define DP83867_IO_MUX_CFG_IO_IMPEDANCE_CTRL	0x1f

#define DP83867_IO_MUX_CFG_IO_IMPEDANCE_MAX	0x0
#define DP83867_IO_MUX_CFG_IO_IMPEDANCE_MIN	0x1f
#define DP83867_IO_MUX_CFG_CLK_O_DISABLE	BIT(6)
#define DP83867_IO_MUX_CFG_CLK_O_SEL_MASK	(0x1f << 8)
#define DP83867_IO_MUX_CFG_CLK_O_SEL_SHIFT	8

/* CFG3 bits */
#define DP83867_CFG3_INT_OE			BIT(7)
#define DP83867_CFG3_ROBUST_AUTO_MDIX		BIT(9)

/* CFG4 bits */
#define DP83867_CFG4_PORT_MIRROR_EN              BIT(0)

enum {
	DP83867_PORT_MIRROING_KEEP,
	DP83867_PORT_MIRROING_EN,
	DP83867_PORT_MIRROING_DIS,
};

struct dp83867_private {
	int rx_id_delay;
	int tx_id_delay;
	int fifo_depth;
	int io_impedance;
	int port_mirroring;
	bool rxctrl_strap_quirk;
	bool set_clk_output;
	u32 clk_output_sel;
};

static int dp83867_ack_interrupt(struct phy_device *phydev)
{
	int err = phy_read(phydev, MII_DP83867_ISR);

	if (err < 0)
		return err;

	return 0;
}

static int dp83867_config_intr(struct phy_device *phydev)
{
	int micr_status;

	if (phydev->interrupts == PHY_INTERRUPT_ENABLED) {
		micr_status = phy_read(phydev, MII_DP83867_MICR);
		if (micr_status < 0)
			return micr_status;

		micr_status |=
			(MII_DP83867_MICR_AN_ERR_INT_EN |
			MII_DP83867_MICR_SPEED_CHNG_INT_EN |
			MII_DP83867_MICR_AUTONEG_COMP_INT_EN |
			MII_DP83867_MICR_LINK_STS_CHNG_INT_EN |
			MII_DP83867_MICR_DUP_MODE_CHNG_INT_EN |
			MII_DP83867_MICR_SLEEP_MODE_CHNG_INT_EN);

		return phy_write(phydev, MII_DP83867_MICR, micr_status);
	}

	micr_status = 0x0;
	return phy_write(phydev, MII_DP83867_MICR, micr_status);
}

static int dp83867_config_port_mirroring(struct phy_device *phydev)
{
	struct dp83867_private *dp83867 =
		(struct dp83867_private *)phydev->priv;
	u16 val;

	val = phy_read_mmd(phydev, DP83867_DEVADDR, DP83867_CFG4);

	if (dp83867->port_mirroring == DP83867_PORT_MIRROING_EN)
		val |= DP83867_CFG4_PORT_MIRROR_EN;
	else
		val &= ~DP83867_CFG4_PORT_MIRROR_EN;

	phy_write_mmd(phydev, DP83867_DEVADDR, DP83867_CFG4, val);

	return 0;
}

#ifdef CONFIG_OF_MDIO
static int dp83867_of_init(struct phy_device *phydev)
{
	struct dp83867_private *dp83867 = phydev->priv;
	struct device *dev = &phydev->mdio.dev;
	struct device_node *of_node = dev->of_node;
	int ret;

	if (!of_node)
		return -ENODEV;

	dp83867->io_impedance = -EINVAL;

	/* Optional configuration */
	ret = of_property_read_u32(of_node, "ti,clk-output-sel",
				   &dp83867->clk_output_sel);
	/* If not set, keep default */
	if (!ret) {
		dp83867->set_clk_output = true;
		/* Valid values are 0 to DP83867_CLK_O_SEL_REF_CLK or
		 * DP83867_CLK_O_SEL_OFF.
		 */
		if (dp83867->clk_output_sel > DP83867_CLK_O_SEL_REF_CLK &&
		    dp83867->clk_output_sel != DP83867_CLK_O_SEL_OFF) {
			phydev_err(phydev, "ti,clk-output-sel value %u out of range\n",
				   dp83867->clk_output_sel);
			return -EINVAL;
		}
	}

	if (of_property_read_bool(of_node, "ti,max-output-impedance"))
		dp83867->io_impedance = DP83867_IO_MUX_CFG_IO_IMPEDANCE_MAX;
	else if (of_property_read_bool(of_node, "ti,min-output-impedance"))
		dp83867->io_impedance = DP83867_IO_MUX_CFG_IO_IMPEDANCE_MIN;

	dp83867->rxctrl_strap_quirk = of_property_read_bool(of_node,
					"ti,dp83867-rxctrl-strap-quirk");

	ret = of_property_read_u32(of_node, "ti,rx-internal-delay",
				   &dp83867->rx_id_delay);
	if (ret &&
	    (phydev->interface == PHY_INTERFACE_MODE_RGMII_ID ||
	     phydev->interface == PHY_INTERFACE_MODE_RGMII_RXID))
		return ret;

	ret = of_property_read_u32(of_node, "ti,tx-internal-delay",
				   &dp83867->tx_id_delay);
	if (ret &&
	    (phydev->interface == PHY_INTERFACE_MODE_RGMII_ID ||
	     phydev->interface == PHY_INTERFACE_MODE_RGMII_TXID))
		return ret;

	if (of_property_read_bool(of_node, "enet-phy-lane-swap"))
		dp83867->port_mirroring = DP83867_PORT_MIRROING_EN;

	if (of_property_read_bool(of_node, "enet-phy-lane-no-swap"))
		dp83867->port_mirroring = DP83867_PORT_MIRROING_DIS;

	return of_property_read_u32(of_node, "ti,fifo-depth",
				   &dp83867->fifo_depth);
}
#else
static int dp83867_of_init(struct phy_device *phydev)
{
	return 0;
}
#endif /* CONFIG_OF_MDIO */

static int dp83867_config_init(struct phy_device *phydev)
{
	struct dp83867_private *dp83867;
	int ret, val, bs;
	u16 delay;

	if (!phydev->priv) {
		dp83867 = devm_kzalloc(&phydev->mdio.dev, sizeof(*dp83867),
				       GFP_KERNEL);
		if (!dp83867)
			return -ENOMEM;

		phydev->priv = dp83867;
		ret = dp83867_of_init(phydev);
		if (ret)
			return ret;
	} else {
		dp83867 = (struct dp83867_private *)phydev->priv;
	}

	/* RX_DV/RX_CTRL strapped in mode 1 or mode 2 workaround */
	if (dp83867->rxctrl_strap_quirk) {
		val = phy_read_mmd(phydev, DP83867_DEVADDR, DP83867_CFG4);
		val &= ~BIT(7);
		phy_write_mmd(phydev, DP83867_DEVADDR, DP83867_CFG4, val);
	}

	if (phy_interface_is_rgmii(phydev)) {
		val = phy_read(phydev, MII_DP83867_PHYCTRL);
		if (val < 0)
			return val;
		val &= ~DP83867_PHYCR_FIFO_DEPTH_MASK;
		val |= (dp83867->fifo_depth << DP83867_PHYCR_FIFO_DEPTH_SHIFT);

		/* The code below checks if "port mirroring" N/A MODE4 has been
		 * enabled during power on bootstrap.
		 *
		 * Such N/A mode enabled by mistake can put PHY IC in some
		 * internal testing mode and disable RGMII transmission.
		 *
		 * In this particular case one needs to check STRAP_STS1
		 * register's bit 11 (marked as RESERVED).
		 */

		bs = phy_read_mmd(phydev, DP83867_DEVADDR, DP83867_STRAP_STS1);
		if (bs & DP83867_STRAP_STS1_RESERVED)
			val &= ~DP83867_PHYCR_RESERVED_MASK;

		ret = phy_write(phydev, MII_DP83867_PHYCTRL, val);
		if (ret)
			return ret;

		/* Set up RGMII delays */
		val = phy_read_mmd(phydev, DP83867_DEVADDR, DP83867_RGMIICTL);

		val &= ~(DP83867_RGMII_TX_CLK_DELAY_EN | DP83867_RGMII_RX_CLK_DELAY_EN);
		if (phydev->interface == PHY_INTERFACE_MODE_RGMII_ID)
			val |= (DP83867_RGMII_TX_CLK_DELAY_EN | DP83867_RGMII_RX_CLK_DELAY_EN);

		if (phydev->interface == PHY_INTERFACE_MODE_RGMII_TXID)
			val |= DP83867_RGMII_TX_CLK_DELAY_EN;

		if (phydev->interface == PHY_INTERFACE_MODE_RGMII_RXID)
			val |= DP83867_RGMII_RX_CLK_DELAY_EN;

		phy_write_mmd(phydev, DP83867_DEVADDR, DP83867_RGMIICTL, val);

		delay = (dp83867->rx_id_delay |
			(dp83867->tx_id_delay << DP83867_RGMII_TX_CLK_DELAY_SHIFT));

		phy_write_mmd(phydev, DP83867_DEVADDR, DP83867_RGMIIDCTL,
			      delay);

		if (dp83867->io_impedance >= 0) {
			val = phy_read_mmd(phydev, DP83867_DEVADDR,
					   DP83867_IO_MUX_CFG);

			val &= ~DP83867_IO_MUX_CFG_IO_IMPEDANCE_CTRL;
			val |= dp83867->io_impedance &
			       DP83867_IO_MUX_CFG_IO_IMPEDANCE_CTRL;

			phy_write_mmd(phydev, DP83867_DEVADDR,
				      DP83867_IO_MUX_CFG, val);
		}
	}

	if (phydev->interface == PHY_INTERFACE_MODE_SGMII) {
		/* For support SPEED_10 in SGMII mode
		 * DP83867_10M_SGMII_RATE_ADAPT bit
		 * has to be cleared by software. That
		 * does not affect SPEED_100 and
		 * SPEED_1000.
		 */
		val = phy_read_mmd(phydev, DP83867_DEVADDR,
				   DP83867_10M_SGMII_CFG);
		val &= ~DP83867_10M_SGMII_RATE_ADAPT_MASK;
		ret = phy_write_mmd(phydev, DP83867_DEVADDR,
				    DP83867_10M_SGMII_CFG, val);

		if (ret)
			return ret;

		/* After reset SGMII Autoneg timer is set to 2us (bits 6 and 5
		 * are 01). That is not enough to finalize autoneg on some
		 * devices. Increase this timer duration to maximum 16ms.
		 */
		val = phy_read_mmd(phydev, DP83867_DEVADDR, DP83867_CFG4);
		val &= ~DP83867_CFG4_SGMII_ANEG_MASK;
		val |= DP83867_CFG4_SGMII_ANEG_TIMER_16MS;
		ret = phy_write_mmd(phydev, DP83867_DEVADDR, DP83867_CFG4, val);

		if (ret)
			return ret;
	}

	val = phy_read(phydev, DP83867_CFG3);
	/* Enable Interrupt output INT_OE in CFG3 register */
	if (phy_interrupt_is_valid(phydev))
		val |= DP83867_CFG3_INT_OE;

	val |= DP83867_CFG3_ROBUST_AUTO_MDIX;
	phy_write(phydev, DP83867_CFG3, val);

	if (dp83867->port_mirroring != DP83867_PORT_MIRROING_KEEP)
		dp83867_config_port_mirroring(phydev);

	/* Clock output selection if muxing property is set */
	if (dp83867->set_clk_output) {
		u16 mask = DP83867_IO_MUX_CFG_CLK_O_DISABLE;
		u16 set;

		if (dp83867->clk_output_sel == DP83867_CLK_O_SEL_OFF) {
			set = DP83867_IO_MUX_CFG_CLK_O_DISABLE;
		} else {
			mask |= DP83867_IO_MUX_CFG_CLK_O_SEL_MASK;
			set = dp83867->clk_output_sel <<
			      DP83867_IO_MUX_CFG_CLK_O_SEL_SHIFT;
		}

		val = phy_read_mmd(phydev, DP83867_DEVADDR, DP83867_IO_MUX_CFG);
		val &= ~mask;
		val |= set;
		phy_write_mmd(phydev, DP83867_DEVADDR, DP83867_IO_MUX_CFG, val);
	}

	return 0;
}

static int dp83867_phy_reset(struct phy_device *phydev)
{
	int err;

	err = phy_write(phydev, DP83867_CTRL, DP83867_SW_RESET);
	if (err < 0)
		return err;

	return dp83867_config_init(phydev);
}

static struct phy_driver dp83867_driver[] = {
	{
		.phy_id		= DP83867_PHY_ID,
		.phy_id_mask	= 0xfffffff0,
		.name		= "TI DP83867",
		.features	= PHY_GBIT_FEATURES,
		.flags		= PHY_HAS_INTERRUPT,

		.config_init	= dp83867_config_init,
		.soft_reset	= dp83867_phy_reset,

		/* IRQ related */
		.ack_interrupt	= dp83867_ack_interrupt,
		.config_intr	= dp83867_config_intr,

		.config_aneg	= genphy_config_aneg,
		.read_status	= genphy_read_status,
		.suspend	= genphy_suspend,
		.resume		= genphy_resume,
	},
};
module_phy_driver(dp83867_driver);

static struct mdio_device_id __maybe_unused dp83867_tbl[] = {
	{ DP83867_PHY_ID, 0xfffffff0 },
	{ }
};

MODULE_DEVICE_TABLE(mdio, dp83867_tbl);

MODULE_DESCRIPTION("Texas Instruments DP83867 PHY driver");
MODULE_AUTHOR("Dan Murphy <dmurphy@ti.com");
MODULE_LICENSE("GPL");
