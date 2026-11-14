/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef __LINUX_OF_GPIO_H
#define __LINUX_OF_GPIO_H

#include <linux/gpio/legacy.h>
#include <linux/of.h>

int of_get_named_gpio(const struct device_node *np, const char *propname, int index);

#endif /* __LINUX_OF_GPIO_H */
