/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_SOC_TI_K3_SOCINFO_H__
#define __LINUX_SOC_TI_K3_SOCINFO_H__

#include <linux/types.h>

struct device;

int k3_socinfo_register(struct device *dev, u32 jtag_id);

#endif /* __LINUX_SOC_TI_K3_SOCINFO_H__ */
