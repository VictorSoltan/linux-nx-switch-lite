// SPDX-License-Identifier: MIT
#include <linux/clk.h>
#include <linux/interconnect.h>
#include <linux/math64.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>

#include <drm/drm_managed.h>

#include <subdev/clk.h>
#include <subdev/volt.h>

#include "nouveau_drv.h"
#include "nouveau_chan.h"
#include "priv.h"
#include "gk20a_devfreq.h"
#include "gk20a.h"
#include "gp10b.h"

#define PMU_BUSY_CYCLES_NORM_MAX		1000U

#define PWR_PMU_IDLE_COUNTER_TOTAL		0U
#define PWR_PMU_IDLE_COUNTER_BUSY		4U

#define PWR_PMU_IDLE_COUNT_REG_OFFSET		0x0010A508U
#define PWR_PMU_IDLE_COUNT_REG_SIZE		16U
#define PWR_PMU_IDLE_COUNT_MASK			0x7FFFFFFFU
#define PWR_PMU_IDLE_COUNT_RESET_VALUE		(0x1U << 31U)

#define PWR_PMU_IDLE_INTR_REG_OFFSET		0x0010A9E8U
#define PWR_PMU_IDLE_INTR_ENABLE_VALUE		0U

#define PWR_PMU_IDLE_INTR_STATUS_REG_OFFSET	0x0010A9ECU
#define PWR_PMU_IDLE_INTR_STATUS_MASK		0x00000001U
#define PWR_PMU_IDLE_INTR_STATUS_RESET_VALUE	0x1U

#define PWR_PMU_IDLE_THRESHOLD_REG_OFFSET	0x0010A8A0U
#define PWR_PMU_IDLE_THRESHOLD_REG_SIZE		4U
#define PWR_PMU_IDLE_THRESHOLD_MAX_VALUE	0x7FFFFFFFU

#define PWR_PMU_IDLE_CTRL_REG_OFFSET		0x0010A50CU
#define PWR_PMU_IDLE_CTRL_REG_SIZE		16U
#define PWR_PMU_IDLE_CTRL_VALUE_MASK		0x3U
#define PWR_PMU_IDLE_CTRL_VALUE_BUSY		0x2U
#define PWR_PMU_IDLE_CTRL_VALUE_ALWAYS		0x3U
#define PWR_PMU_IDLE_CTRL_FILTER_MASK		(0x1U << 2)
#define PWR_PMU_IDLE_CTRL_FILTER_DISABLED	0x0U

#define PWR_PMU_IDLE_MASK_REG_OFFSET		0x0010A504U
#define PWR_PMU_IDLE_MASK_REG_SIZE		16U
#define PWM_PMU_IDLE_MASK_GR_ENABLED		0x1U
#define PWM_PMU_IDLE_MASK_CE_2_ENABLED		0x200000U

/**
 * struct gk20a_devfreq - Device frequency management
 */
struct gk20a_devfreq {
	/** @devfreq: devfreq device. */
	struct devfreq *devfreq;
	struct icc_bulk_data *emc_paths;
	u32 emc_peak_bw;
	unsigned long emc_gpu_freq;
	unsigned long emc_idle_freq;
	unsigned long emc_load_max_freq;
	u32 emc_load_avg;
	bool emc_load_valid;

	/** @regs: Device registers. */
	void __iomem *regs;

	/** @gov_data: Governor data. */
	struct devfreq_simple_ondemand_data gov_data;

	/** @busy_time: Busy time. */
	ktime_t busy_time;

	/** @total_time: Total time. */
	ktime_t total_time;

	/** @time_last_update: Last update time. */
	ktime_t time_last_update;
};

static struct gk20a_devfreq *dev_to_gk20a_devfreq(struct device *dev)
{
	struct nouveau_drm *drm = dev_get_drvdata(dev);
	struct nvkm_subdev *subdev = nvkm_device_subdev(drm->nvkm, NVKM_SUBDEV_CLK, 0);
	struct nvkm_clk *base = nvkm_clk(subdev);

	switch (drm->nvkm->chipset) {
	case 0x13b: return gp10b_clk(base)->devfreq; break;
	default: return gk20a_clk(base)->devfreq; break;
	}
}

static void gk20a_pmu_init_perfmon_counter(struct gk20a_devfreq *gdevfreq)
{
	u32 data;

	// Set pmu idle intr status bit on total counter overflow
	writel(PWR_PMU_IDLE_INTR_ENABLE_VALUE,
	       gdevfreq->regs + PWR_PMU_IDLE_INTR_REG_OFFSET);

	writel(PWR_PMU_IDLE_THRESHOLD_MAX_VALUE,
	       gdevfreq->regs + PWR_PMU_IDLE_THRESHOLD_REG_OFFSET +
	       (PWR_PMU_IDLE_COUNTER_TOTAL * PWR_PMU_IDLE_THRESHOLD_REG_SIZE));

	// Setup counter for total cycles
	data = readl(gdevfreq->regs + PWR_PMU_IDLE_CTRL_REG_OFFSET +
		     (PWR_PMU_IDLE_COUNTER_TOTAL * PWR_PMU_IDLE_CTRL_REG_SIZE));
	data &= ~(PWR_PMU_IDLE_CTRL_VALUE_MASK | PWR_PMU_IDLE_CTRL_FILTER_MASK);
	data |= PWR_PMU_IDLE_CTRL_VALUE_ALWAYS | PWR_PMU_IDLE_CTRL_FILTER_DISABLED;
	writel(data, gdevfreq->regs + PWR_PMU_IDLE_CTRL_REG_OFFSET +
		     (PWR_PMU_IDLE_COUNTER_TOTAL * PWR_PMU_IDLE_CTRL_REG_SIZE));

	// Setup counter for busy cycles
	writel(PWM_PMU_IDLE_MASK_GR_ENABLED | PWM_PMU_IDLE_MASK_CE_2_ENABLED,
	       gdevfreq->regs + PWR_PMU_IDLE_MASK_REG_OFFSET +
	       (PWR_PMU_IDLE_COUNTER_BUSY * PWR_PMU_IDLE_MASK_REG_SIZE));

	data = readl(gdevfreq->regs + PWR_PMU_IDLE_CTRL_REG_OFFSET +
		     (PWR_PMU_IDLE_COUNTER_BUSY * PWR_PMU_IDLE_CTRL_REG_SIZE));
	data &= ~(PWR_PMU_IDLE_CTRL_VALUE_MASK | PWR_PMU_IDLE_CTRL_FILTER_MASK);
	data |= PWR_PMU_IDLE_CTRL_VALUE_BUSY | PWR_PMU_IDLE_CTRL_FILTER_DISABLED;
	writel(data, gdevfreq->regs + PWR_PMU_IDLE_CTRL_REG_OFFSET +
		     (PWR_PMU_IDLE_COUNTER_BUSY * PWR_PMU_IDLE_CTRL_REG_SIZE));
}

static u32 gk20a_pmu_read_idle_counter(struct gk20a_devfreq *gdevfreq, u32 counter_id)
{
	u32 ret;

	ret = readl(gdevfreq->regs + PWR_PMU_IDLE_COUNT_REG_OFFSET +
		    (counter_id * PWR_PMU_IDLE_COUNT_REG_SIZE));

	return ret & PWR_PMU_IDLE_COUNT_MASK;
}

static void gk20a_pmu_reset_idle_counter(struct gk20a_devfreq *gdevfreq, u32 counter_id)
{
	writel(PWR_PMU_IDLE_COUNT_RESET_VALUE, gdevfreq->regs + PWR_PMU_IDLE_COUNT_REG_OFFSET +
					       (counter_id * PWR_PMU_IDLE_COUNT_REG_SIZE));
}

static u32 gk20a_pmu_read_idle_intr_status(struct gk20a_devfreq *gdevfreq)
{
	u32 ret;

	ret = readl(gdevfreq->regs + PWR_PMU_IDLE_INTR_STATUS_REG_OFFSET);

	return ret & PWR_PMU_IDLE_INTR_STATUS_MASK;
}

static void gk20a_pmu_clear_idle_intr_status(struct gk20a_devfreq *gdevfreq)
{
	writel(PWR_PMU_IDLE_INTR_STATUS_RESET_VALUE,
	       gdevfreq->regs + PWR_PMU_IDLE_INTR_STATUS_REG_OFFSET);
}

static void gk20a_devfreq_update_utilization(struct gk20a_devfreq *gdevfreq)
{
	ktime_t now, last;
	u64 busy_cycles, total_cycles;
	u32 norm, intr_status;

	now = ktime_get();
	last = gdevfreq->time_last_update;
	gdevfreq->total_time = ktime_us_delta(now, last);

	busy_cycles = gk20a_pmu_read_idle_counter(gdevfreq, PWR_PMU_IDLE_COUNTER_BUSY);
	total_cycles = gk20a_pmu_read_idle_counter(gdevfreq, PWR_PMU_IDLE_COUNTER_TOTAL);
	intr_status = gk20a_pmu_read_idle_intr_status(gdevfreq);

	gk20a_pmu_reset_idle_counter(gdevfreq, PWR_PMU_IDLE_COUNTER_BUSY);
	gk20a_pmu_reset_idle_counter(gdevfreq, PWR_PMU_IDLE_COUNTER_TOTAL);

	if (intr_status != 0UL) {
		norm = PMU_BUSY_CYCLES_NORM_MAX;
		gk20a_pmu_clear_idle_intr_status(gdevfreq);
	} else if (total_cycles == 0ULL || busy_cycles > total_cycles) {
		norm = PMU_BUSY_CYCLES_NORM_MAX;
	} else {
		norm = (u32)div64_u64(busy_cycles * PMU_BUSY_CYCLES_NORM_MAX,
				total_cycles);
	}

	gdevfreq->busy_time = div_u64(gdevfreq->total_time * norm, PMU_BUSY_CYCLES_NORM_MAX);
	gdevfreq->time_last_update = now;

	/* NVIDIA's 9/10 load average, with immediate attack for new bursts. */
	if (gdevfreq->emc_paths) {
		gdevfreq->emc_load_avg = max(norm,
			(9 * gdevfreq->emc_load_avg + norm) / 10);
		gdevfreq->emc_load_valid = true;
	}
}

#define GM20B_EMC_PATHS 4

/* Only scale demand within the accepted voltage table's vmin plateau. */
static unsigned long gk20a_devfreq_emc_load_max(struct nvkm_clk *base)
{
	struct nvkm_volt *volt = base->subdev.device->volt;
	struct nvkm_pstate *pstates = base->func->pstates;
	unsigned long freq = 0;
	u32 vmin = U32_MAX;
	int i;

	if (!volt || !volt->vid_nr)
		return 0;

	for (i = 0; i < volt->vid_nr; i++)
		vmin = min(vmin, volt->vid_ref ? volt->vid_ref[i] : volt->vid[i].uv);
	for (i = 0; i < base->func->nr_pstates; i++)
		if (pstates[i].base.voltage < volt->vid_nr &&
		    (volt->vid_ref ? volt->vid_ref[pstates[i].base.voltage] :
		     volt->vid[pstates[i].base.voltage].uv) == vmin)
			freq = max(freq, (unsigned long)
				pstates[i].base.domain[nv_clk_src_gpc] * GK20A_CLK_GPC_MDIV);

	return freq;
}

static u32 gk20a_devfreq_emc_bw(struct gk20a_devfreq *gdevfreq,
			       unsigned long target)
{
	u64 bw;

	if (!gdevfreq->emc_paths || target == gdevfreq->emc_idle_freq)
		return 0;

	bw = div_u64((u64)target * 64, 1000);
	/*
	 * Unknown load, upclocks and rates above fmax@vmin retain the full
	 * reservation. A fresh sample can reduce it on the next existing
	 * devfreq poll; ACTMON and the other ICC consumers remain independent.
	 */
	if (gdevfreq->emc_load_valid && target <= gdevfreq->emc_gpu_freq &&
	    target <= gdevfreq->emc_load_max_freq)
		bw = DIV_ROUND_UP_ULL(bw * gdevfreq->emc_load_avg,
				      PMU_BUSY_CYCLES_NORM_MAX);

	return bw;
}

static int gk20a_devfreq_set_emc(struct gk20a_devfreq *gdevfreq, u32 peak_bw)
{
	unsigned int i;
	int ret;

	if (!gdevfreq->emc_paths || gdevfreq->emc_peak_bw == peak_bw)
		return 0;

	for (i = 0; i < GM20B_EMC_PATHS; i++) {
		gdevfreq->emc_paths[i].avg_bw = 0;
		gdevfreq->emc_paths[i].peak_bw = peak_bw;
	}

	ret = icc_bulk_set_bw(GM20B_EMC_PATHS, gdevfreq->emc_paths);
	if (!ret)
		gdevfreq->emc_peak_bw = peak_bw;

	return ret;
}

static int gk20a_devfreq_target(struct device *dev, unsigned long *freq,
				u32 flags)
{
	struct nouveau_drm *drm = dev_get_drvdata(dev);
	struct nvkm_subdev *subdev = nvkm_device_subdev(drm->nvkm, NVKM_SUBDEV_CLK, 0);
	struct nvkm_clk *base = nvkm_clk(subdev);
	struct gk20a_devfreq *gdevfreq = dev_to_gk20a_devfreq(dev);
	struct nvkm_pstate *pstates = base->func->pstates;
	int nr_pstates = base->func->nr_pstates;
	unsigned long target;
	u32 peak_bw;
	int i, ret;

	for (i = 0; i < nr_pstates - 1; i++)
		if (pstates[i].base.domain[nv_clk_src_gpc] * GK20A_CLK_GPC_MDIV >= *freq)
			break;

	target = pstates[i].base.domain[nv_clk_src_gpc] * GK20A_CLK_GPC_MDIV;
	/*
	 * NVIDIA's GM20B/DDR bandwidth ratio is 64/16 bytes per clock.
	 * Reserve ahead of GPU upclock, without reducing CPU/display votes.
	 * The idle OPP releases our reservation so ACTMON can reach 204MHz.
	 */
	peak_bw = gk20a_devfreq_emc_bw(gdevfreq, target);
	if (peak_bw > gdevfreq->emc_peak_bw) {
		ret = gk20a_devfreq_set_emc(gdevfreq, peak_bw);
		if (ret)
			return ret;
	}

	ret = nvkm_clk_ustate(base, pstates[i].pstate, 0);
	ret |= nvkm_clk_ustate(base, pstates[i].pstate, 1);
	if (ret) {
		nvkm_error(subdev, "cannot update clock\n");
		return ret;
	}

	*freq = target;
	gdevfreq->emc_gpu_freq = target;

	/* nvkm_clk_ustate waits for the clock worker before lowering EMC. */
	return gk20a_devfreq_set_emc(gdevfreq, peak_bw);
}

static int gk20a_devfreq_get_cur_freq(struct device *dev, unsigned long *freq)
{
	struct nouveau_drm *drm = dev_get_drvdata(dev);
	struct nvkm_subdev *subdev = nvkm_device_subdev(drm->nvkm, NVKM_SUBDEV_CLK, 0);
	struct nvkm_clk *base = nvkm_clk(subdev);

	*freq = nvkm_clk_read(base, nv_clk_src_gpc) * GK20A_CLK_GPC_MDIV;

	return 0;
}

static void gk20a_devfreq_reset(struct gk20a_devfreq *gdevfreq)
{
	gk20a_pmu_reset_idle_counter(gdevfreq, PWR_PMU_IDLE_COUNTER_BUSY);
	gk20a_pmu_reset_idle_counter(gdevfreq, PWR_PMU_IDLE_COUNTER_TOTAL);
	gk20a_pmu_clear_idle_intr_status(gdevfreq);

	gdevfreq->busy_time = 0;
	gdevfreq->total_time = 0;
	gdevfreq->time_last_update = ktime_get();
}

static int gk20a_devfreq_get_dev_status(struct device *dev,
					struct devfreq_dev_status *status)
{
	struct nouveau_drm *drm = dev_get_drvdata(dev);
	struct gk20a_devfreq *gdevfreq = dev_to_gk20a_devfreq(dev);

	gk20a_devfreq_get_cur_freq(dev, &status->current_frequency);

	gk20a_devfreq_update_utilization(gdevfreq);

	status->busy_time = ktime_to_ns(gdevfreq->busy_time);
	status->total_time = ktime_to_ns(gdevfreq->total_time);

	gk20a_devfreq_reset(gdevfreq);

	NV_DEBUG(drm, "busy %lu total %lu %lu %% freq %lu MHz\n",
		 status->busy_time, status->total_time,
		 status->busy_time / (status->total_time / 100),
		 status->current_frequency / 1000 / 1000);

	return 0;
}

static struct devfreq_dev_profile gk20a_devfreq_profile = {
	.timer = DEVFREQ_TIMER_DELAYED,
	.polling_ms = 50,
	.target = gk20a_devfreq_target,
	.get_cur_freq = gk20a_devfreq_get_cur_freq,
	.get_dev_status = gk20a_devfreq_get_dev_status,
};

int gk20a_devfreq_init(struct nvkm_clk *base, struct gk20a_devfreq **gdevfreq)
{
	struct nvkm_device *device = base->subdev.device;
	struct nouveau_drm *drm = dev_get_drvdata(device->dev);
	struct nvkm_device_tegra *tdev = device->func->tegra(device);
	struct nvkm_pstate *pstates = base->func->pstates;
	int nr_pstates = base->func->nr_pstates;
	struct gk20a_devfreq *new_gdevfreq;
	int i;

	new_gdevfreq = drmm_kzalloc(drm->dev, sizeof(struct gk20a_devfreq), GFP_KERNEL);
	if (!new_gdevfreq)
		return -ENOMEM;

	new_gdevfreq->regs = tdev->regs;

	if ((device->chipset == 0x12b || device->chipset == 0x12e) &&
	    of_machine_is_compatible("nvidia,tegra210b01") &&
	    of_property_present(device->dev->of_node, "interconnects")) {
		static const char * const names[] = {
			"dma-mem", "write-0", "read-1", "write-1",
		};
		int ret;

		/* devres owns the bulk array through ICC path release. */
		new_gdevfreq->emc_paths = devm_kcalloc(device->dev,
			GM20B_EMC_PATHS, sizeof(*new_gdevfreq->emc_paths), GFP_KERNEL);
		if (!new_gdevfreq->emc_paths)
			return -ENOMEM;
		for (i = 0; i < GM20B_EMC_PATHS; i++)
			new_gdevfreq->emc_paths[i].name = names[i];
		ret = devm_of_icc_bulk_get(device->dev, GM20B_EMC_PATHS,
					 new_gdevfreq->emc_paths);
		if (ret)
			return ret;
		new_gdevfreq->emc_load_max_freq = gk20a_devfreq_emc_load_max(base);
		new_gdevfreq->emc_load_avg = PMU_BUSY_CYCLES_NORM_MAX;
	}

	for (i = 0; i < nr_pstates; i++)
		dev_pm_opp_add(base->subdev.device->dev,
			       pstates[i].base.domain[nv_clk_src_gpc] * GK20A_CLK_GPC_MDIV, 0);

	gk20a_pmu_init_perfmon_counter(new_gdevfreq);
	gk20a_devfreq_reset(new_gdevfreq);

	gk20a_devfreq_profile.initial_freq =
		nvkm_clk_read(base, nv_clk_src_gpc) * GK20A_CLK_GPC_MDIV;
	new_gdevfreq->emc_gpu_freq = gk20a_devfreq_profile.initial_freq;
	new_gdevfreq->emc_idle_freq =
		pstates[0].base.domain[nv_clk_src_gpc] * GK20A_CLK_GPC_MDIV;

	new_gdevfreq->gov_data.upthreshold = 45;
	new_gdevfreq->gov_data.downdifferential = 5;

	*gdevfreq = new_gdevfreq;
	new_gdevfreq->devfreq = devm_devfreq_add_device(device->dev,
							&gk20a_devfreq_profile,
							DEVFREQ_GOV_SIMPLE_ONDEMAND,
							&new_gdevfreq->gov_data);
	if (IS_ERR(new_gdevfreq->devfreq)) {
		int ret = PTR_ERR(new_gdevfreq->devfreq);

		*gdevfreq = NULL;
		return ret;
	}

	return 0;
}

int gk20a_devfreq_resume(struct device *dev)
{
	struct gk20a_devfreq *gdevfreq = dev_to_gk20a_devfreq(dev);
	int ret;

	if (!gdevfreq || !gdevfreq->devfreq)
		return 0;

	/* Resume with a full vote until new PMU counters establish the load. */
	gdevfreq->emc_load_valid = false;
	gdevfreq->emc_load_avg = PMU_BUSY_CYCLES_NORM_MAX;
	ret = gk20a_devfreq_set_emc(gdevfreq,
		gk20a_devfreq_emc_bw(gdevfreq, gdevfreq->emc_gpu_freq));
	if (ret)
		return ret;

	ret = nvkm_volt_active(dev_get_drvdata(dev) ?
		((struct nouveau_drm *)dev_get_drvdata(dev))->nvkm->volt : NULL, true);
	if (ret)
		return ret;
	return devfreq_resume_device(gdevfreq->devfreq);
}

int gk20a_devfreq_suspend(struct device *dev)
{
	struct gk20a_devfreq *gdevfreq = dev_to_gk20a_devfreq(dev);
	int ret;

	if (!gdevfreq || !gdevfreq->devfreq)
		return 0;

	ret = devfreq_suspend_device(gdevfreq->devfreq);
	if (ret)
		return ret;

	ret = nvkm_volt_active(((struct nouveau_drm *)dev_get_drvdata(dev))->nvkm->volt,
			      false);
	if (ret) {
		devfreq_resume_device(gdevfreq->devfreq);
		return ret;
	}
	return gk20a_devfreq_set_emc(gdevfreq, 0);
}
