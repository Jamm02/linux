// SPDX-License-Identifier: GPL-2.0
/*
 * TI K3 SoC information core
 *
 * Copyright (C) 2020 Texas Instruments Incorporated - http://www.ti.com
 */

#include <linux/bitfield.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/soc/ti/k3-socinfo.h>
#include <linux/sys_soc.h>

#define CTRLMMR_WKUP_JTAGID_VARIANT_MASK	GENMASK(31, 28)
#define CTRLMMR_WKUP_JTAGID_PARTNO_MASK	GENMASK(27, 12)
#define CTRLMMR_WKUP_JTAGID_MFG_MASK		GENMASK(11, 1)
#define CTRLMMR_WKUP_JTAGID_MFG_TI		0x17

#define GP_SW1_ADR_MASK			GENMASK(3, 0)

#define JTAG_ID_PARTNO_AM65X		0xBB5A
#define JTAG_ID_PARTNO_J721E		0xBB64
#define JTAG_ID_PARTNO_J7200		0xBB6D
#define JTAG_ID_PARTNO_AM64X		0xBB38
#define JTAG_ID_PARTNO_J721S2		0xBB75
#define JTAG_ID_PARTNO_AM62X		0xBB7E
#define JTAG_ID_PARTNO_J784S4		0xBB80
#define JTAG_ID_PARTNO_AM62AX		0xBB8D
#define JTAG_ID_PARTNO_AM62PX		0xBB9D
#define JTAG_ID_PARTNO_J722S		0xBBA0
#define JTAG_ID_PARTNO_AM62LX		0xBBA7
#define JTAG_ID_PARTNO_TDA54		0xBBC1

static const struct k3_soc_id {
	unsigned int id;
	const char *family_name;
} k3_soc_ids[] = {
	{ JTAG_ID_PARTNO_AM65X, "AM65X" },
	{ JTAG_ID_PARTNO_J721E, "J721E" },
	{ JTAG_ID_PARTNO_J7200, "J7200" },
	{ JTAG_ID_PARTNO_AM64X, "AM64X" },
	{ JTAG_ID_PARTNO_J721S2, "J721S2" },
	{ JTAG_ID_PARTNO_AM62X, "AM62X" },
	{ JTAG_ID_PARTNO_J784S4, "J784S4" },
	{ JTAG_ID_PARTNO_AM62AX, "AM62AX" },
	{ JTAG_ID_PARTNO_AM62PX, "AM62PX" },
	{ JTAG_ID_PARTNO_J722S, "J722S" },
	{ JTAG_ID_PARTNO_AM62LX, "AM62LX" },
	{ JTAG_ID_PARTNO_TDA54, "TDA54" },
};

static const char * const j721e_rev_string_map[] = {
	"1.0", "1.1", "2.0",
};

static const char * const am62lx_rev_string_map[] = {
	"1.0", "1.1",
};

static const char * const am62p_gpsw_rev_string_map[] = {
	"1.0", "1.1", "1.2",
};

static int k3_chipinfo_get_gpsw_variant(struct device *dev)
{
	u32 gpsw_val = 0;
	int ret;

	ret = nvmem_cell_read_u32(dev, "gpsw1", &gpsw_val);
	if (ret)
		return ret;

	return gpsw_val & GP_SW1_ADR_MASK;
}

static int
k3_chipinfo_partno_to_names(unsigned int partno,
			    struct soc_device_attribute *soc_dev_attr)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(k3_soc_ids); i++)
		if (partno == k3_soc_ids[i].id) {
			soc_dev_attr->family = k3_soc_ids[i].family_name;
			return 0;
		}

	return -ENODEV;
}

static int
k3_chipinfo_variant_to_sr(struct device *dev, unsigned int partno,
			  unsigned int variant,
			  struct soc_device_attribute *soc_dev_attr)
{
	int gpsw_variant = 0;

	switch (partno) {
	case JTAG_ID_PARTNO_J721E:
		if (variant >= ARRAY_SIZE(j721e_rev_string_map))
			goto err_unknown_variant;

		soc_dev_attr->revision = kasprintf(GFP_KERNEL, "SR%s",
						  j721e_rev_string_map[variant]);
		break;
	case JTAG_ID_PARTNO_AM62LX:
		if (variant >= ARRAY_SIZE(am62lx_rev_string_map))
			goto err_unknown_variant;

		soc_dev_attr->revision = kasprintf(GFP_KERNEL, "SR%s",
						  am62lx_rev_string_map[variant]);
		break;
	case JTAG_ID_PARTNO_AM62PX:
		/* Check GP_SW1 for silicon revision */
		gpsw_variant = k3_chipinfo_get_gpsw_variant(dev);
		if (gpsw_variant == -EPROBE_DEFER)
			return gpsw_variant;

		if (gpsw_variant < 0 ||
		    gpsw_variant >= ARRAY_SIZE(am62p_gpsw_rev_string_map)) {
			dev_warn(dev,
				 "Failed to get silicon variant (%d), set SR1.0\n",
				 gpsw_variant);
			gpsw_variant = 0;
		}

		soc_dev_attr->revision = kasprintf(
			GFP_KERNEL, "SR%s",
			am62p_gpsw_rev_string_map[gpsw_variant]);
		break;
	default:
		variant++;
		soc_dev_attr->revision = kasprintf(GFP_KERNEL, "SR%x.0",
						  variant);
	}

	if (!soc_dev_attr->revision)
		return -ENOMEM;

	return 0;

err_unknown_variant:
	return -ENODEV;
}

int k3_socinfo_register(struct device *dev, u32 jtag_id)
{
	struct soc_device_attribute *soc_dev_attr;
	struct soc_device *soc_dev;
	struct device_node *node;
	u32 partno_id;
	u32 variant;
	u32 mfg;
	int ret;

	/*
	 * JTAG ID:
	 * Bits 31:28 - Version/variant
	 * Bits 27:12 - Part number
	 * Bits 11:1  - Manufacturer identity
	 * Bit 0      - Always 1
	 */
	mfg = FIELD_GET(CTRLMMR_WKUP_JTAGID_MFG_MASK, jtag_id);
	if (mfg != CTRLMMR_WKUP_JTAGID_MFG_TI) {
		dev_err(dev, "Invalid MFG SoC\n");
		return -ENODEV;
	}

	variant = FIELD_GET(CTRLMMR_WKUP_JTAGID_VARIANT_MASK, jtag_id);
	partno_id = FIELD_GET(CTRLMMR_WKUP_JTAGID_PARTNO_MASK, jtag_id);

	soc_dev_attr = kzalloc_obj(*soc_dev_attr);
	if (!soc_dev_attr)
		return -ENOMEM;

	ret = k3_chipinfo_partno_to_names(partno_id, soc_dev_attr);
	if (ret) {
		dev_err(dev, "Unknown SoC JTAGID[0x%08X]: %d\n",
			jtag_id, ret);
		goto err_free_attr;
	}

	ret = k3_chipinfo_variant_to_sr(dev, partno_id, variant,
					soc_dev_attr);
	if (ret) {
		dev_err(dev, "Unknown SoC SR[0x%08X]: %d\n", jtag_id, ret);
		goto err_free_attr;
	}

	node = of_find_node_by_path("/");
	if (node) {
		of_property_read_string(node, "model", &soc_dev_attr->machine);
		of_node_put(node);
	}

	soc_dev = soc_device_register(soc_dev_attr);
	if (IS_ERR(soc_dev)) {
		ret = PTR_ERR(soc_dev);
		goto err_free_revision;
	}

	dev_info(dev, "Family:%s rev:%s JTAGID[0x%08x] Detected\n",
		 soc_dev_attr->family, soc_dev_attr->revision, jtag_id);

	return 0;

err_free_revision:
	kfree(soc_dev_attr->revision);
err_free_attr:
	kfree(soc_dev_attr);

	return ret;
}
EXPORT_SYMBOL_GPL(k3_socinfo_register);

MODULE_DESCRIPTION("TI K3 SoC information core");
MODULE_LICENSE("GPL");