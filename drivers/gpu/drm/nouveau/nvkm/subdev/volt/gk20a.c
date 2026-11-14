/*
 * Copyright (c) 2014-2016, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
#define gk20a_volt(p) container_of((p), struct gk20a_volt, base)
#include "priv.h"

#include <core/tegra.h>
#include <subdev/clk.h>
#include <linux/of.h>
#include <linux/thermal.h>

#include "gk20a.h"

static const struct cvb_coef gk20a_cvb_coef[] = {
	/* MHz,        c0,     c1,   c2,    c3,     c4,   c5 */
	/*  72 */ { 1209886, -36468,  515,   417, -13123,  203},
	/* 108 */ { 1130804, -27659,  296,   298, -10834,  221},
	/* 180 */ { 1162871, -27110,  247,   238, -10681,  268},
	/* 252 */ { 1220458, -28654,  247,   179, -10376,  298},
	/* 324 */ { 1280953, -30204,  247,   119,  -9766,  304},
	/* 396 */ { 1344547, -31777,  247,   119,  -8545,  292},
	/* 468 */ { 1420168, -34227,  269,    60,  -7172,  256},
	/* 540 */ { 1490757, -35955,  274,    60,  -5188,  197},
	/* 612 */ { 1599112, -42583,  398,     0,  -1831,  119},
	/* 648 */ { 1366986, -16459, -274,     0,  -3204,   72},
	/* 684 */ { 1391884, -17078, -274,   -60,  -1526,   30},
	/* 708 */ { 1415522, -17497, -274,   -60,   -458,    0},
	/* 756 */ { 1464061, -18331, -274,  -119,   1831,  -72},
	/* 804 */ { 1524225, -20064, -254,  -119,   4272, -155},
	/* 852 */ { 1608418, -21643, -269,     0,    763,  -48},
};

/*
 * cvb_mv = ((c2 * speedo / s_scale + c1) * speedo / s_scale + c0)
 */
static inline int
gk20a_volt_get_cvb_voltage(int speedo, int s_scale, const struct cvb_coef *coef)
{
	int mv;

	mv = DIV_ROUND_CLOSEST(coef->c2 * speedo, s_scale);
	mv = DIV_ROUND_CLOSEST((mv + coef->c1) * speedo, s_scale) + coef->c0;
	return mv;
}

/*
 * cvb_t_mv =
 * ((c2 * speedo / s_scale + c1) * speedo / s_scale + c0) +
 * ((c3 * speedo / s_scale + c4 + c5 * T / t_scale) * T / t_scale)
 */
static inline int
gk20a_volt_get_cvb_t_voltage(int speedo, int temp, int s_scale, int t_scale,
			     const struct cvb_coef *coef)
{
	int cvb_mv, mv;

	cvb_mv = gk20a_volt_get_cvb_voltage(speedo, s_scale, coef);

	mv = DIV_ROUND_CLOSEST(coef->c3 * speedo, s_scale) + coef->c4 +
		DIV_ROUND_CLOSEST(coef->c5 * temp, t_scale);
	mv = DIV_ROUND_CLOSEST(mv * temp, t_scale) + cvb_mv;
	return mv;
}

static int
gk20a_volt_calc_voltage(const struct cvb_coef *coef, int speedo)
{
	static const int v_scale = 1000;
	int mv;

	mv = gk20a_volt_get_cvb_t_voltage(speedo, -10, 100, 10, coef);
	mv = DIV_ROUND_UP(mv, v_scale);

	return mv * 1000;
}

static int
gk20a_volt_vid_get(struct nvkm_volt *base)
{
	struct gk20a_volt *volt = gk20a_volt(base);
	int i, uv;

	uv = regulator_get_voltage(volt->vdd);

	for (i = 0; i < volt->base.vid_nr; i++)
		if (volt->base.vid[i].uv >= uv)
			return i;

	return -EINVAL;
}

static int
gk20a_volt_vid_set(struct nvkm_volt *base, u8 vid)
{
	struct gk20a_volt *volt = gk20a_volt(base);
	struct nvkm_subdev *subdev = &volt->base.subdev;

	nvkm_debug(subdev, "set voltage as %duv\n", volt->base.vid[vid].uv);
	return regulator_set_voltage(volt->vdd, volt->base.vid[vid].uv, 1200000);
}

static int
gk20a_volt_set_id(struct nvkm_volt *base, u8 id, int condition)
{
	struct gk20a_volt *volt = gk20a_volt(base);
	struct nvkm_subdev *subdev = &volt->base.subdev;
	int prev_uv = regulator_get_voltage(volt->vdd);
	int target_uv = volt->base.vid[id].uv;
	int ret;

	nvkm_debug(subdev, "prev=%d, target=%d, condition=%d\n",
		   prev_uv, target_uv, condition);
	if (!condition ||
		(condition < 0 && target_uv < prev_uv) ||
		(condition > 0 && target_uv > prev_uv)) {
		ret = gk20a_volt_vid_set(&volt->base, volt->base.vid[id].vid);
	} else {
		ret = 0;
	}

	return ret;
}

static const struct nvkm_volt_func
gk20a_volt = {
	.vid_get = gk20a_volt_vid_get,
	.vid_set = gk20a_volt_vid_set,
	.set_id = gk20a_volt_set_id,
};

int
gk20a_volt_ctor(struct nvkm_device *device, enum nvkm_subdev_type type, int inst,
		const struct cvb_coef *coefs, int nb_coefs,
		int vmin, struct gk20a_volt *volt)
{
	struct nvkm_device_tegra *tdev = device->func->tegra(device);
	int i, uv;

	nvkm_volt_ctor(&gk20a_volt, device, type, inst, &volt->base);

	uv = regulator_get_voltage(tdev->vdd);
	nvkm_debug(&volt->base.subdev, "the default voltage is %duV\n", uv);

	volt->vdd = tdev->vdd;

	volt->base.vid_nr = nb_coefs;
	for (i = 0; i < volt->base.vid_nr; i++) {
		volt->base.vid[i].vid = i;
		volt->base.vid[i].uv = max(
			gk20a_volt_calc_voltage(&coefs[i], tdev->gpu_speedo),
			vmin);
		nvkm_debug(&volt->base.subdev, "%2d: vid=%d, uv=%d\n", i,
			   volt->base.vid[i].vid, volt->base.vid[i].uv);
	}

	return 0;
}

/* Inserted into volt/gk20a.c. NVIDIA CVB ranges; only the clock worker
 * changes live VIDs. Thermal callbacks never write a regulator directly.
 */
#define B01_THERM_STATES 6
#define B01_THERM_VIDS 32

static void gk20a_volt_thermal_table(struct gk20a_volt *volt, unsigned int state)
{
	int i;

	for (i = 0; i < volt->base.vid_nr; i++)
		volt->base.vid[i].uv = volt->thermal_uv[state][i];
}

static int gk20a_volt_thermal_prepare(struct nvkm_volt *base)
{
	struct gk20a_volt *volt = gk20a_volt(base);
	unsigned int state = READ_ONCE(volt->thermal_requested);

	if (!volt->thermal_cdev || !READ_ONCE(volt->thermal_live) ||
	    state == READ_ONCE(volt->thermal_applied))
		return 0;

	volt->thermal_next = state;
	gk20a_volt_thermal_table(volt, state);
	return 1;
}

static void gk20a_volt_thermal_complete(struct nvkm_volt *base, int error)
{
	struct gk20a_volt *volt = gk20a_volt(base);

	if (error)
		gk20a_volt_thermal_table(volt, volt->thermal_applied);
	else
		WRITE_ONCE(volt->thermal_applied, volt->thermal_next);
}

static int gk20a_volt_thermal_max(struct thermal_cooling_device *cdev,
				unsigned long *state)
{
	*state = B01_THERM_STATES - 1;
	return 0;
}

static int gk20a_volt_thermal_get(struct thermal_cooling_device *cdev,
				unsigned long *state)
{
	struct gk20a_volt *volt = cdev->devdata;

	*state = READ_ONCE(volt->thermal_applied);
	return 0;
}

static int gk20a_volt_thermal_set(struct thermal_cooling_device *cdev,
				unsigned long state)
{
	struct gk20a_volt *volt = cdev->devdata;
	struct nvkm_clk *clk = volt->base.subdev.device->clk;
	int ret = 0;

	if (state >= B01_THERM_STATES)
		return -EINVAL;

	/* Serialise callbacks with clock fini. The worker does not take this
	 * lock, or the synchronous completion would deadlock thermal updates.
	 */
	mutex_lock(&volt->thermal_lock);
	WRITE_ONCE(volt->thermal_requested, state);
	if (volt->thermal_live && state != volt->thermal_applied) {
		ret = nvkm_clk_update_volt(clk);
		if (READ_ONCE(volt->thermal_applied) != state)
			ret = ret ?: -EAGAIN;
	}
	mutex_unlock(&volt->thermal_lock);
	return ret;
}

static const struct thermal_cooling_device_ops gk20a_volt_thermal_ops = {
	.get_max_state = gk20a_volt_thermal_max,
	.get_cur_state = gk20a_volt_thermal_get,
	.set_cur_state = gk20a_volt_thermal_set,
};

static int gk20a_volt_thermal_oneinit(struct nvkm_volt *base)
{
	struct gk20a_volt *volt = gk20a_volt(base);
	struct device_node *np;

	/* No matching DT node retains the accepted static peak table. */
	np = of_find_compatible_node(NULL, NULL,
				     "nvidia,tegra210-rail-scaling-cdev");
	if (!np)
		return 0;
	if (of_device_is_available(np)) {
		volt->thermal_cdev = thermal_of_cooling_device_register(np, 0,
			"gpu_scaling", volt, &gk20a_volt_thermal_ops);
		if (IS_ERR(volt->thermal_cdev)) {
			int ret = PTR_ERR(volt->thermal_cdev);

			volt->thermal_cdev = NULL;
			of_node_put(np);
			return ret;
		}
	}
	of_node_put(np);
	return 0;
}

static int gk20a_volt_thermal_init(struct nvkm_volt *base)
{
	struct gk20a_volt *volt = gk20a_volt(base);
	int i, uv = 0;

	if (!volt->thermal_cdev)
		return 0;

	/* Boot/resume starts cold. Only a new valid thermal request can leave
	 * this profile. Raise voltage before clock/NA initialisation, when the
	 * boot clock is not yet known to the clock subdevice.
	 */
	mutex_lock(&volt->thermal_lock);
	WRITE_ONCE(volt->thermal_live, false);
	WRITE_ONCE(volt->thermal_applied, 0);
	gk20a_volt_thermal_table(volt, 0);
	for (i = 0; i < base->vid_nr; i++)
		uv = max(uv, (int)base->vid[i].uv);
	mutex_unlock(&volt->thermal_lock);
	return regulator_set_voltage(volt->vdd, uv, 1200000);
}

static int gk20a_volt_thermal_active(struct nvkm_volt *base, bool live)
{
	struct gk20a_volt *volt = gk20a_volt(base);
	int ret = 0;

	mutex_lock(&volt->thermal_lock);
	if (!live) {
		WRITE_ONCE(volt->thermal_requested, 0);
		/* Platform sleep only suspends devfreq; it need not fini NVKM.
		 * Establish the cold profile while MMIO is still available.
		 */
		if (volt->thermal_live && volt->thermal_applied) {
			ret = nvkm_clk_update_volt(base->subdev.device->clk);
			if (volt->thermal_applied)
				ret = ret ?: -EAGAIN;
		}
		if (!ret)
			WRITE_ONCE(volt->thermal_live, false);
	} else {
		WRITE_ONCE(volt->thermal_live, true);
		if (volt->thermal_requested != volt->thermal_applied)
			ret = nvkm_clk_update_volt(base->subdev.device->clk);
	}
	mutex_unlock(&volt->thermal_lock);
	return ret;
}

static void gk20a_volt_thermal_dtor(struct nvkm_volt *base)
{
	struct gk20a_volt *volt = gk20a_volt(base);

	if (volt->thermal_cdev)
		thermal_cooling_device_unregister(volt->thermal_cdev);
	mutex_destroy(&volt->thermal_lock);
}

static int gk20a_volt_thermal_build(struct gk20a_volt *volt,
		const struct cvb_coef *coefs, int count, int vmin, int scale)
{
	/* Original NVIDIA ranges -25/20/30/50/70/90C. Two extra degrees at
	 * the lower edge cover the DT hysteresis while the board cools.
	 * State 5 is the conservative fallback above the characterised range.
	 */
	static const int low[] = { -25, 18, 28, 48, 68, -25 };
	static const int high[] = { 20, 30, 50, 70, 90, 90 };
	struct nvkm_device *device = volt->base.subdev.device;
	int state, i, j, t, uv, candidate, best;
	int selectors = regulator_count_voltages(volt->vdd);

	if (count > B01_THERM_VIDS || selectors <= 0)
		return -EINVAL;

	for (state = 0; state < B01_THERM_STATES; state++) {
		for (i = 0; i < count; i++) {
			uv = state == 0 ? max(vmin, 800000) : vmin;
			for (t = low[state]; t <= high[state]; t++) {
				candidate = gk20a_volt_get_cvb_t_voltage(
					device->func->tegra(device)->gpu_speedo,
					t, 100, scale, &coefs[i]);
				uv = max(uv, DIV_ROUND_UP(candidate, 1000) * 1000);
			}
			best = INT_MAX;
			for (j = 0; j < selectors; j++) {
				candidate = regulator_list_voltage(volt->vdd, j);
				if (candidate >= uv && candidate < best)
					best = candidate;
			}
			if (best == INT_MAX || best > 1200000)
				return -EINVAL;
			if (i)
				best = max(best, (int)volt->thermal_uv[state][i - 1]);
			volt->thermal_uv[state][i] = best;
		}
	}
	mutex_init(&volt->thermal_lock);
	return 0;
}

static const struct nvkm_volt_func gk20a_volt_b01 = {
	.oneinit = gk20a_volt_thermal_oneinit,
	.init = gk20a_volt_thermal_init,
	.dtor = gk20a_volt_thermal_dtor,
	.prepare = gk20a_volt_thermal_prepare,
	.complete = gk20a_volt_thermal_complete,
	.active = gk20a_volt_thermal_active,
	.vid_get = gk20a_volt_vid_get,
	.vid_set = gk20a_volt_vid_set,
	.set_id = gk20a_volt_set_id,
};

int
gk20a_volt_ctor_b01(struct nvkm_device *device, enum nvkm_subdev_type type, int inst,
		   const struct cvb_coef *coefs, int nb_coefs,
		   int vmin, int thermal_scale, struct gk20a_volt *volt)
{
	int i, j, t, uv, candidate, count, best, ret;

	ret = gk20a_volt_ctor(device, type, inst, coefs, nb_coefs, vmin, volt);
	if (ret)
		return ret;

	count = regulator_count_voltages(volt->vdd);
	if (count <= 0)
		return count ?: -EINVAL;

	for (i = 0; i < nb_coefs; i++) {
		/* Use the worst CVB voltage over the board's operating range.
		 * No temperature poller or userspace voltage writer is needed.
		 */
		uv = vmin;
		for (t = -10; t <= 90; t++) {
			candidate = gk20a_volt_get_cvb_t_voltage(
				device->func->tegra(device)->gpu_speedo,
				t, 100, thermal_scale, &coefs[i]);
			uv = max(uv, DIV_ROUND_UP(candidate, 1000) * 1000);
		}

		/* NA calibration must use an actual regulator selector, not
		 * an unrepresentable CVB value (e.g. 677mV on a 5mV rail).
		 */
		best = INT_MAX;
		for (j = 0; j < count; j++) {
			candidate = regulator_list_voltage(volt->vdd, j);
			if (candidate >= uv && candidate < best)
				best = candidate;
		}
		if (best == INT_MAX || best > 1200000)
			return -EINVAL;
		if (i)
			best = max(best, (int)volt->base.vid[i - 1].uv);
		volt->base.vid[i].uv = best;
	}

	ret = gk20a_volt_thermal_build(volt, coefs, nb_coefs, vmin, thermal_scale);
	if (ret)
		return ret;
	for (i = 0; i < nb_coefs; i++)
		volt->thermal_ref[i] = volt->base.vid[i].uv;
	volt->base.vid_ref = volt->thermal_ref;
	volt->base.func = &gk20a_volt_b01;
	return 0;
}

int
gk20a_volt_new(struct nvkm_device *device, enum nvkm_subdev_type type, int inst, struct nvkm_volt **pvolt)
{
	struct gk20a_volt *volt;

	volt = kzalloc_obj(*volt);
	if (!volt)
		return -ENOMEM;
	*pvolt = &volt->base;

	return gk20a_volt_ctor(device, type, inst, gk20a_cvb_coef,
			       ARRAY_SIZE(gk20a_cvb_coef), 0, volt);
}
