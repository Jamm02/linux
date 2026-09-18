// SPDX-License-Identifier: GPL-2.0
/*
 * TI K3 SoC info driver
 *
 * Copyright (C) 2020 Texas Instruments Incorporated - http://www.ti.com
 */

#include <linux/init.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/soc/ti/k3-socinfo.h>

#define CTRLMMR_WKUP_JTAGID_REG	0

static const struct regmap_config k3_chipinfo_regmap_cfg = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
};

static int k3_chipinfo_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct regmap *regmap;
	void __iomem *base;
	u32 jtag_id;
	int ret;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	regmap = devm_regmap_init_mmio(dev, base, &k3_chipinfo_regmap_cfg);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	ret = regmap_read(regmap, CTRLMMR_WKUP_JTAGID_REG, &jtag_id);
	if (ret)
		return ret;

	return k3_socinfo_register(dev, jtag_id);
}

static const struct of_device_id k3_chipinfo_of_match[] = {
	{ .compatible = "ti,am654-chipid", },
	{ /* sentinel */ },
};

static struct platform_driver k3_chipinfo_driver = {
	.driver = {
		.name = "k3-chipinfo",
		.of_match_table = k3_chipinfo_of_match,
	},
	.probe = k3_chipinfo_probe,
};

static int __init k3_chipinfo_init(void)
{
	return platform_driver_register(&k3_chipinfo_driver);
}
subsys_initcall(k3_chipinfo_init);
