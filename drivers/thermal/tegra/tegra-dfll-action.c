// SPDX-License-Identifier: GPL-2.0-only
/*
 * Connect the Tegra DFLL temperature indices to the thermal framework.
 * Adapted from NVIDIA's tegra-dfll-action.c (2012-2014), by
 * Aleksandr Frid and Paul Walmsley.
 */
#include <linux/err.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/thermal.h>

#include <soc/tegra/tegra-dfll.h>

struct tegra_dfll_action {
	struct tegra_dfll *dfll;
	enum tegra_dfll_thermal_type type;
};

static int tegra_dfll_action_max(struct thermal_cooling_device *cdev,
			       unsigned long *state)
{
	struct tegra_dfll_action *action = cdev->devdata;
	int count = tegra_dfll_count_thermal_states(action->dfll, action->type);

	if (count <= 0)
		return -EINVAL;
	*state = count - 1;
	return 0;
}

static int tegra_dfll_action_get(struct thermal_cooling_device *cdev,
			       unsigned long *state)
{
	struct tegra_dfll_action *action = cdev->devdata;
	int index = tegra_dfll_get_thermal_index(action->dfll, action->type);

	if (index < 0)
		return index;
	*state = index;
	return 0;
}

static int tegra_dfll_action_set(struct thermal_cooling_device *cdev,
			       unsigned long state)
{
	struct tegra_dfll_action *action = cdev->devdata;

	return tegra_dfll_update_thermal_index(action->dfll, action->type, state);
}

static const struct thermal_cooling_device_ops tegra_dfll_action_ops = {
	.get_max_state = tegra_dfll_action_max,
	.get_cur_state = tegra_dfll_action_get,
	.set_cur_state = tegra_dfll_action_set,
};

static int tegra_dfll_action_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct thermal_cooling_device *cdev;
	struct tegra_dfll_action *action;
	const char *type;
	int ret;

	action = devm_kzalloc(dev, sizeof(*action), GFP_KERNEL);
	if (!action)
		return -ENOMEM;
	action->dfll = tegra_dfll_get_by_phandle(dev->of_node, "act-dev");
	if (IS_ERR(action->dfll))
		return dev_err_probe(dev, PTR_ERR(action->dfll), "DFLL unavailable\n");
	ret = of_property_read_string(dev->of_node, "cdev-type", &type);
	if (ret)
		return ret;
	if (!strcmp(type, "DFLL-floor"))
		action->type = TEGRA_DFLL_THERMAL_FLOOR;
	else if (!strcmp(type, "DFLL-cap"))
		action->type = TEGRA_DFLL_THERMAL_CAP;
	else
		return -EINVAL;

	/*
	 * DFLL starts/resumes with its cold floor. The thermal core initializes
	 * all instances anew and reads the sensor before applying their votes,
	 * including after resume. No copied 4.9 cdev->updated PM workaround.
	 */
	cdev = devm_thermal_of_cooling_device_register(dev, 0, type,
						     action, &tegra_dfll_action_ops);
	if (IS_ERR(cdev))
		return dev_err_probe(dev, PTR_ERR(cdev), "thermal registration failed\n");
	return 0;
}

static const struct of_device_id tegra_dfll_action_match[] = {
	{ .compatible = "nvidia,tegra-dfll-cdev-action" },
	{ }
};
MODULE_DEVICE_TABLE(of, tegra_dfll_action_match);

static struct platform_driver tegra_dfll_action_driver = {
	.probe = tegra_dfll_action_probe,
	.driver = {
		.name = "tegra-dfll-action",
		.of_match_table = tegra_dfll_action_match,
	},
};
module_platform_driver(tegra_dfll_action_driver);
MODULE_DESCRIPTION("Tegra DFLL native thermal floor/cap integration");
MODULE_LICENSE("GPL");
