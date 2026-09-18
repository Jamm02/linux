// SPDX-License-Identifier: GPL-2.0
/*
 * TDA54 SCM Configuration syscon driver
 *
 * Copyright (C) 2026 Texas Instruments Incorporated - https://www.ti.com/
 */
#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/soc/ti/k3-socinfo.h>
#include <linux/types.h>

/* Size of a single proxy window into the scm_conf register space */
#define TDA54_SCM_CONF_PROXY_STRIDE		0x4000

#define TDA54_JTAG_ID_REG_OFFSET		0x14

#define TDA54_SCM_CONF_LOCK_KICK0_UNLOCK_VAL	0x68EF3490
#define TDA54_SCM_CONF_LOCK_KICK1_UNLOCK_VAL	0xD172BC5A

#define TDA54_SCM_CONF_INTR_RAW			0x0
#define TDA54_SCM_CONF_INTR_STATUS		0x4
#define TDA54_SCM_CONF_INTR_ENABLE		0x8
#define TDA54_SCM_CONF_INTR_CLEAR		0xc
#define TDA54_SCM_CONF_INTR_ALL			GENMASK(3, 0)

/*
 * Claim registers start at 0x3100. Claim register i holds the claim bit
 * for each of the 32 registers in the (i * TDA54_SCM_CONF_CLAIM_BLOCK_SIZE)
 * .. (i * TDA54_SCM_CONF_CLAIM_BLOCK_SIZE + 0x7c) block, one bit per
 * register, i.e. bit b of claim register i corresponds to the register at
 * offset i * TDA54_SCM_CONF_CLAIM_BLOCK_SIZE + b * 4.
 */
#define TDA54_SCM_CONF_CLAIM_REG_STRIDE		0x4
#define TDA54_SCM_CONF_CLAIM_REGS_PER_WORD	32
#define TDA54_SCM_CONF_CLAIM_BLOCK_SIZE		\
	(TDA54_SCM_CONF_CLAIM_REGS_PER_WORD * 0x4)

#define TDA54_SCM_CONF_FAULT_TYPE_MASK		GENMASK(5, 0)
#define TDA54_SCM_CONF_FAULT_CLEAR_VAL		BIT(0)

/* Per-SoC register offset data (platform data) */
struct tda54_scm_conf_data {
	u32 lock_offset;
	u32 intr_offset;
	u32 fault_offset;
	u32 claim_offset;
	u32 claim_status_offset;
};

static const struct tda54_scm_conf_data tda54_scm_conf_data = {
	.lock_offset		= 0x3008,
	.intr_offset		= 0x3010,
	.fault_offset		= 0x3024,
	.claim_offset		= 0x3100,
	.claim_status_offset	= 0x3300,
};

struct tda54_scm_conf {
	struct device *dev;
	void __iomem *base;
	u32 proxy;
	const struct tda54_scm_conf_data *pdata;
};

/*
 * Claim @reg for this proxy before writing it. Registers claimed by a
 * proxy can no longer be written through any other proxy's address,
 * until that proxy releases the claim.
 */
static void tda54_scm_conf_claim(struct tda54_scm_conf *scm, unsigned int reg)
{
	unsigned int block = reg / TDA54_SCM_CONF_CLAIM_BLOCK_SIZE;
	unsigned int bit = (reg % TDA54_SCM_CONF_CLAIM_BLOCK_SIZE) /
			   TDA54_SCM_CONF_CLAIM_REG_STRIDE;
	unsigned int claim_reg = scm->pdata->claim_offset +
				 block * TDA54_SCM_CONF_CLAIM_REG_STRIDE;
	u32 val;

	val = readl(scm->base + claim_reg);
	val |= BIT(bit);
	writel(val, scm->base + claim_reg);
}

static int tda54_scm_conf_reg_read(void *context, unsigned int reg,
				   unsigned int *val)
{
	struct tda54_scm_conf *scm = context;

	*val = readl(scm->base + reg);

	return 0;
}

static int tda54_scm_conf_reg_write(void *context, unsigned int reg,
				    unsigned int val)
{
	struct tda54_scm_conf *scm = context;

	/* Proxy0 always has write access; every other proxy must claim first. */
	if (scm->proxy)
		tda54_scm_conf_claim(scm, reg);

	writel(val, scm->base + reg);

	return 0;
}

static const struct regmap_config tda54_scm_conf_regmap_cfg = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.reg_read = tda54_scm_conf_reg_read,
	.reg_write = tda54_scm_conf_reg_write,
};

static irqreturn_t tda54_scm_conf_irq_handler(int irq, void *data)
{
	struct tda54_scm_conf *scm = data;
	u32 status, reason, type, readback;

	status = readl(scm->base + scm->pdata->intr_offset +
		       TDA54_SCM_CONF_INTR_STATUS);
	printk("isr: proxy=%u status=0x%x\n", scm->proxy, status);
	if (!status)
		return IRQ_NONE;

	reason = readl(scm->base + scm->pdata->fault_offset);
	type = FIELD_GET(TDA54_SCM_CONF_FAULT_TYPE_MASK,
			 readl(scm->base + scm->pdata->fault_offset + 0x4));

	dev_err_ratelimited(scm->dev,
			    "system controller fault: reason 0x%x type 0x%x\n",
			    reason, type);

	writel(TDA54_SCM_CONF_FAULT_CLEAR_VAL,
	       scm->base + scm->pdata->fault_offset + TDA54_SCM_CONF_INTR_CLEAR);

	readback = readl(scm->base + scm->pdata->fault_offset);
	printk("isr: after FAULT_CLR, fault_offset readback=0x%x\n",
		 readback);

	/*
	 * Offset TDA54_SCM_CONF_INTR_STATUS (0x4) is the combined enabled
	 * status/clear register: reading it returns the enabled status,
	 * writing it W1C-clears the pending event.
	 */
	writel(status, scm->base + scm->pdata->intr_offset +
	       TDA54_SCM_CONF_INTR_STATUS);

	readback = readl(scm->base + scm->pdata->intr_offset +
			  TDA54_SCM_CONF_INTR_STATUS);
	printk("isr: after INTR_STATUS clear, readback=0x%x (%s)\n",
		 readback, readback ? "NOT CLEARED" : "cleared");

	return IRQ_HANDLED;
}

static int tda54_scm_conf_register_socinfo(struct device *dev, struct regmap *regmap)
{
	u32 jtag_id;
	int ret;

	ret = regmap_read(regmap, TDA54_JTAG_ID_REG_OFFSET, &jtag_id);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read JTAG ID\n");

	return k3_socinfo_register(dev, jtag_id);
}

static int tda54_scm_conf_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct tda54_scm_conf *scm;
	struct regmap *regmap;
	void __iomem *base;
	int irq, ret;

	scm = devm_kzalloc(dev, sizeof(*scm), GFP_KERNEL);
	if (!scm)
		return -ENOMEM;

	scm->dev = dev;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	if (of_property_read_u32(dev->of_node, "ti,proxy-id", &scm->proxy))
		return dev_err_probe(dev, -ENODATA, "missing ti,proxy-id property\n");

	if (scm->proxy >= 8)
		return dev_err_probe(dev, -EINVAL,
				     "ti,proxy-id must be between 0 and 7\n");

	scm->base = base + scm->proxy * TDA54_SCM_CONF_PROXY_STRIDE;

	/* Get register offsets from driver data (matched by compatible) */
	scm->pdata = of_device_get_match_data(dev);

	regmap = devm_regmap_init(dev, NULL, scm, &tda54_scm_conf_regmap_cfg);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	writel(TDA54_SCM_CONF_LOCK_KICK0_UNLOCK_VAL, scm->base + scm->pdata->lock_offset);
	writel(TDA54_SCM_CONF_LOCK_KICK1_UNLOCK_VAL, scm->base + scm->pdata->lock_offset + 4);

	if (scm->pdata->intr_offset)
		tda54_scm_conf_claim_test(scm);

	ret = of_syscon_register_regmap(dev->of_node, regmap);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register syscon regmap\n");

	irq = platform_get_irq_optional(pdev, 0);
	if (irq == -EPROBE_DEFER)
		return irq;

	if (irq > 0 && scm->pdata->intr_offset && scm->pdata->fault_offset) {
		ret = devm_request_irq(dev, irq, tda54_scm_conf_irq_handler,
				       IRQF_SHARED, dev_name(dev), scm);
		if (ret)
			return dev_err_probe(dev, ret,
					     "failed to request irq %d\n", irq);

		if (scm->pdata->intr_offset)
			writel(TDA54_SCM_CONF_INTR_ALL,
			       scm->base + scm->pdata->intr_offset + TDA54_SCM_CONF_INTR_ENABLE);
	}

	ret = tda54_scm_conf_register_socinfo(dev, regmap);
	if (ret)
		return ret;

	return 0;
}

static const struct of_device_id tda54_scm_conf_of_match[] = {
	{ .compatible = "ti,tda54-scm-conf", .data = &tda54_scm_conf_data },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, tda54_scm_conf_of_match);

static struct platform_driver tda54_scm_conf_driver = {
	.probe = tda54_scm_conf_probe,
	.driver = {
		.name = "k3-tda54-scm-conf",
		.of_match_table = tda54_scm_conf_of_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(tda54_scm_conf_driver);

MODULE_DESCRIPTION("TDA54 SCM Configuration syscon driver");
MODULE_LICENSE("GPL");
