/*
 * Copyright (c) 2014-2018, NVIDIA CORPORATION.  All rights reserved.
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
 *
 * Clock-gating data from the NVIDIA GM20B nvgpu tables.
 */

#include "priv.h"

#include <core/option.h>
#include <core/tegra.h>

static const struct {
	u32 addr;
	u32 prod;
	u32 disable;
} gm20b_clkgate[] = {
	/* SLCG BUS */
	{ 0x00001c04, 0x00000000, 0x000003fe },
	/* SLCG CE2 */
	{ 0x00106f28, 0x00000000, 0x000007fe },
	/* SLCG CHIPLET */
	{ 0x0010c07c, 0x00000000, 0x00000007 },
	{ 0x0010e07c, 0x00000000, 0x00000007 },
	{ 0x0010d07c, 0x00000000, 0x00000007 },
	{ 0x0010e17c, 0x00000000, 0x00000007 },
	/* SLCG FB */
	{ 0x00100d14, 0x00000000, 0xfffffffe },
	{ 0x00100c9c, 0x00000000, 0x000001fe },
	/* SLCG FIFO */
	{ 0x000026ac, 0x00000100, 0x0001fffe },
	/* SLCG GR */
	{ 0x004041f4, 0x00000000, 0x03fffffe },
	{ 0x0040917c, 0x00020008, 0x0003fffe },
	{ 0x00409894, 0x00000040, 0x0003fffe },
	{ 0x004078c4, 0x00000000, 0x000001fe },
	{ 0x00406004, 0x00000000, 0x0001fffe },
	{ 0x00405864, 0x00000000, 0x000001fe },
	{ 0x00405910, 0xfffffff0, 0xfffffffe },
	{ 0x00408044, 0x00000000, 0x000007fe },
	{ 0x00407004, 0x00000000, 0x0000007e },
	{ 0x0041a17c, 0x00020008, 0x0003fffe },
	{ 0x0041a894, 0x00000040, 0x0003fffe },
	{ 0x00418504, 0x00000000, 0x0007fffe },
	{ 0x0041860c, 0x00000000, 0x000001fe },
	{ 0x0041868c, 0x00000000, 0x0000001e },
	{ 0x0041871c, 0x00000000, 0x0000003e },
	{ 0x00418388, 0x00000000, 0x00000001 },
	{ 0x0041882c, 0x00000000, 0x0001fffe },
	{ 0x00418bc0, 0x00000000, 0x000001fe },
	{ 0x00418974, 0x00000000, 0x0001fffe },
	{ 0x00418c74, 0xffffffc0, 0xfffffffe },
	{ 0x00418cf4, 0xfffffffc, 0xfffffffe },
	{ 0x00418d74, 0xffffffe0, 0xfffffffe },
	{ 0x00418f10, 0xffffffe0, 0xfffffffe },
	{ 0x00418e10, 0xfffffffe, 0xfffffffe },
	{ 0x00419024, 0x000001fe, 0x000001fe },
	{ 0x0041889c, 0x00000000, 0x000001fe },
	{ 0x00419d64, 0x00000000, 0x000001ff },
	{ 0x00419a44, 0x00000000, 0x0000000e },
	{ 0x00419a4c, 0x00000000, 0x000001fe },
	{ 0x00419a54, 0x00000000, 0x0000003e },
	{ 0x00419a5c, 0x00000000, 0x0000000e },
	{ 0x00419a64, 0x00000000, 0x000001fe },
	{ 0x00419a6c, 0x00000000, 0x0000000e },
	{ 0x00419a74, 0x00000000, 0x0000000e },
	{ 0x00419a7c, 0x00000000, 0x0000003e },
	{ 0x00419a84, 0x00000000, 0x0000000e },
	{ 0x0041986c, 0x00000104, 0x00fffffe },
	{ 0x00419cd8, 0x00000000, 0x001ffffe },
	{ 0x00419ce0, 0x00000000, 0x001ffffe },
	{ 0x00419c74, 0x0000001e, 0x0000001e },
	{ 0x00419fd4, 0x00000000, 0x0003fffe },
	{ 0x00419fdc, 0xffedff00, 0xfffffffe },
	{ 0x00419fe4, 0x00001b00, 0x00001ffe },
	{ 0x00419ff4, 0x00000000, 0x00003ffe },
	{ 0x00419ffc, 0x00000000, 0x0001fffe },
	{ 0x0041be2c, 0x04115fc0, 0xfffffffe },
	{ 0x0041bfec, 0xfffffff0, 0xfffffffe },
	{ 0x0041bed4, 0xfffffff6, 0xfffffffe },
	{ 0x00408814, 0x00000000, 0x0001fffe },
	{ 0x0040881c, 0x00000000, 0x0001fffe },
	{ 0x00408a84, 0x00000000, 0x0001fffe },
	{ 0x00408a8c, 0x00000000, 0x0001fffe },
	{ 0x00408a94, 0x00000000, 0x0001fffe },
	{ 0x00408a9c, 0x00000000, 0x0001fffe },
	{ 0x00408aa4, 0x00000000, 0x0001fffe },
	{ 0x00408aac, 0x00000000, 0x0001fffe },
	{ 0x004089ac, 0x00000000, 0x0001fffe },
	{ 0x00408a24, 0x00000000, 0x000001ff },
	/* SLCG LTC */
	{ 0x0017e050, 0x00000000, 0xfffffffe },
	{ 0x0017e35c, 0x00000000, 0xfffffffe },
	/* SLCG PERF */
	{ 0x001be018, 0x000001ff, 0x00000000 },
	{ 0x001bc018, 0x000001ff, 0x00000000 },
	{ 0x001b8018, 0x000001ff, 0x00000000 },
	{ 0x001b4124, 0x00000001, 0x00000000 },
	/* SLCG PRIRING */
	{ 0x001200a8, 0x00000000, 0x00000001 },
	/* SLCG XBAR */
	{ 0x0013cbe4, 0x00000000, 0x1ffffffe },
	{ 0x0013cc04, 0x00000000, 0x1ffffffe },
	/* BLCG BUS */
	{ 0x00001c00, 0x00000042, 0x00000000 },
	/* BLCG FB */
	{ 0x00100d10, 0x0000c242, 0x00000000 },
	{ 0x00100d30, 0x0000c242, 0x00000000 },
	{ 0x00100d3c, 0x00000242, 0x00000000 },
	{ 0x00100d48, 0x0000c242, 0x00000000 },
	{ 0x00100d1c, 0x00000042, 0x00000000 },
	{ 0x00100c98, 0x00000242, 0x00000000 },
	/* BLCG FIFO */
	{ 0x000026a4, 0x0000c242, 0x00000000 },
	/* BLCG GR */
	{ 0x004041f0, 0x00004046, 0x00000000 },
	{ 0x00409890, 0x0000007f, 0x00000000 },
	{ 0x004098b0, 0x0000007f, 0x00000000 },
	{ 0x004078c0, 0x00000042, 0x00000000 },
	{ 0x00406000, 0x00004044, 0x00000000 },
	{ 0x00405860, 0x00004042, 0x00000000 },
	{ 0x0040590c, 0x00004044, 0x00000000 },
	{ 0x00408040, 0x00004044, 0x00000000 },
	{ 0x00407000, 0x00004041, 0x00000000 },
	{ 0x00405bf0, 0x00004044, 0x00000000 },
	{ 0x0041a890, 0x0000007f, 0x00000000 },
	{ 0x0041a8b0, 0x0000007f, 0x00000000 },
	{ 0x00418500, 0x00004044, 0x00000000 },
	{ 0x00418608, 0x00004042, 0x00000000 },
	{ 0x00418688, 0x00004042, 0x00000000 },
	{ 0x00418718, 0x00000042, 0x00000000 },
	{ 0x00418828, 0x00000044, 0x00000000 },
	{ 0x00418bbc, 0x00004042, 0x00000000 },
	{ 0x00418970, 0x00004042, 0x00000000 },
	{ 0x00418c70, 0x00004044, 0x00000000 },
	{ 0x00418cf0, 0x00004044, 0x00000000 },
	{ 0x00418d70, 0x00004044, 0x00000000 },
	{ 0x00418f0c, 0x00004044, 0x00000000 },
	{ 0x00418e0c, 0x00004044, 0x00000000 },
	{ 0x00419020, 0x00004042, 0x00000000 },
	{ 0x00419038, 0x00000042, 0x00000000 },
	{ 0x00418898, 0x00000042, 0x00000000 },
	{ 0x00419a40, 0x00000042, 0x00000000 },
	{ 0x00419a48, 0x00004042, 0x00000000 },
	{ 0x00419a50, 0x00004042, 0x00000000 },
	{ 0x00419a58, 0x00004042, 0x00000000 },
	{ 0x00419a60, 0x00004042, 0x00000000 },
	{ 0x00419a68, 0x00004042, 0x00000000 },
	{ 0x00419a70, 0x00004042, 0x00000000 },
	{ 0x00419a78, 0x00004042, 0x00000000 },
	{ 0x00419a80, 0x00004042, 0x00000000 },
	{ 0x00419868, 0x00000042, 0x00000000 },
	{ 0x00419cd4, 0x00000002, 0x00000000 },
	{ 0x00419cdc, 0x00000002, 0x00000000 },
	{ 0x00419c70, 0x00004044, 0x00000000 },
	{ 0x00419fd0, 0x00004044, 0x00000000 },
	{ 0x00419fd8, 0x00004046, 0x00000000 },
	{ 0x00419fe0, 0x00004044, 0x00000000 },
	{ 0x00419fe8, 0x00000042, 0x00000000 },
	{ 0x00419ff0, 0x00004045, 0x00000000 },
	{ 0x00419ff8, 0x00000002, 0x00000000 },
	{ 0x00419f90, 0x00000002, 0x00000000 },
	{ 0x0041be28, 0x00000042, 0x00000000 },
	{ 0x0041bfe8, 0x00004044, 0x00000000 },
	{ 0x0041bed0, 0x00004044, 0x00000000 },
	{ 0x00408810, 0x00004042, 0x00000000 },
	{ 0x00408818, 0x00004042, 0x00000000 },
	{ 0x00408a80, 0x00004042, 0x00000000 },
	{ 0x00408a88, 0x00004042, 0x00000000 },
	{ 0x00408a90, 0x00004042, 0x00000000 },
	{ 0x00408a98, 0x00004042, 0x00000000 },
	{ 0x00408aa0, 0x00004042, 0x00000000 },
	{ 0x00408aa8, 0x00004042, 0x00000000 },
	{ 0x004089a8, 0x00004042, 0x00000000 },
	{ 0x004089b0, 0x00000042, 0x00000000 },
	{ 0x004089b8, 0x00004042, 0x00000000 },
	/* BLCG LTC */
	{ 0x0017e030, 0x00000044, 0x00000000 },
	{ 0x0017e040, 0x00000044, 0x00000000 },
	{ 0x0017e3e0, 0x00000044, 0x00000000 },
	{ 0x0017e3c8, 0x00000044, 0x00000000 },
	/* BLCG XBAR */
	{ 0x0013cbe0, 0x00000042, 0x00000000 },
	{ 0x0013cc00, 0x00000042, 0x00000000 },
};

/* NVIDIA hw_therm_gm20b.h THERM_GATE_CTRL(engine) fields. */
#define GM20B_GATE_CTRL(e)	(0x020200 + (e) * 4)
#define GM20B_BLK_CLK		0x0000000c
#define GM20B_BLK_CLK_AUTO	0x00000004
/* ENG_CLK [1:0] and ENG_PWR [5:4]. NVIDIA ELCG_RUN is ENG_CLK=RUN with
 * ENG_PWR=AUTO ("hw expectation"); ELCG_AUTO then changes ENG_CLK only.
 */
#define GM20B_ELCG		0x00000033
#define GM20B_ELCG_RUN		0x00000010
#define GM20B_ELCG_AUTO		0x00000011
/* ENG_IDLE_FILT_EXP [12:8] = 9, MANT [15:13] = 2: 2 << 9 = 1024 clocks. */
#define GM20B_IDLE_FILT		0x0000ff00
#define GM20B_IDLE_FILT_1024	0x00004900
#define GM20B_FECS_IDLE_FILTER	0x00020288
#define GM20B_HUBMMU_IDLE_FILTER 0x0002028c

static void
gm20b_clkgate_tables(struct nvkm_device *device, bool enable)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(gm20b_clkgate); i++)
		nvkm_wr32(device, gm20b_clkgate[i].addr,
			  enable ? gm20b_clkgate[i].prod : gm20b_clkgate[i].disable);
}

/* NVIDIA ELCG covers every active engine: GR and the copy engines. */
static void
gm20b_elcg(struct nvkm_device *device, u32 mask, u32 data)
{
	struct nvkm_top_device *info;

	list_for_each_entry(info, &device->top->device, head) {
		if (info->type != NVKM_ENGINE_GR && info->type != NVKM_ENGINE_CE)
			continue;
		if (info->engine < 0 || info->engine >= 32)
			continue;
		nvkm_mask(device, GM20B_GATE_CTRL(info->engine), mask, data);
	}
}

/* GR lifecycle, called with the GR use mutex held (or GR recovery):
 * running=false before GR init and at GR fini, true once GR init and the
 * ctxsw falcons are up. As NVIDIA gr_prepare/init: ELCG RUN while GR is
 * reset or initialising; afterwards reload the tables that a GR reset
 * cleared and switch ELCG to AUTO. Device-level gating owns boot/resume.
 */
void
gm20b_device_clkgate_gr(struct nvkm_device *device, bool running)
{
	struct nvkm_device_tegra *tdev = device->func->tegra(device);

	if (!tdev->clkgating)
		return;

	if (running)
		gm20b_clkgate_tables(device, true);
	gm20b_elcg(device, GM20B_ELCG, running ? GM20B_ELCG_AUTO : GM20B_ELCG_RUN);
	nvkm_rd32(device, GM20B_FECS_IDLE_FILTER);
}

void
gm20b_device_clkgate(struct nvkm_device *device, bool enable)
{
	struct nvkm_device_tegra *tdev = device->func->tegra(device);
	struct nvkm_subdev *gr;
	struct nvkm_top_device *info;
	int engine = -1;

	if (device->chipset != 0x12b && device->chipset != 0x12e)
		return;
	if (!device->gr || !device->top)
		return;
	if (enable && !nvkm_boolopt(device->cfgopt, "NvPmEnableGating", true))
		return;
	/* The first device fini precedes init. Do not touch uninitialised HW. */
	if (!enable && !tdev->clkgating)
		return;

	list_for_each_entry(info, &device->top->device, head) {
		if (info->type == NVKM_ENGINE_GR && info->inst == 0) {
			engine = info->engine;
			break;
		}
	}
	if (engine < 0 || engine >= 32)
		return;
	gr = &device->gr->engine.subdev;

	if (!enable) {
		/* ELCG and BLK_CLK RUN before removing the tables. */
		mutex_lock(&gr->use.mutex);
		gm20b_elcg(device, GM20B_ELCG, GM20B_ELCG_RUN);
		tdev->clkgating = false;
		mutex_unlock(&gr->use.mutex);
		nvkm_mask(device, GM20B_GATE_CTRL(engine), GM20B_BLK_CLK, 0);
		gm20b_clkgate_tables(device, false);
		/* Flush posted writes before power-off. */
		nvkm_rd32(device, GM20B_GATE_CTRL(engine));
		return;
	}

	/* NVIDIA gm20b_elcg_init_idle_filters() at power-on. */
	gm20b_elcg(device, GM20B_IDLE_FILT, GM20B_IDLE_FILT_1024);
	nvkm_wr32(device, GM20B_FECS_IDLE_FILTER, 0);
	nvkm_wr32(device, GM20B_HUBMMU_IDLE_FILTER, 0);

	gm20b_clkgate_tables(device, true);
	/* BLCG mode (THERM_GATE_CTRL.BLK_CLK) is GR only, as NVIDIA. */
	nvkm_mask(device, GM20B_GATE_CTRL(engine), GM20B_BLK_CLK, GM20B_BLK_CLK_AUTO);

	/* GR initialises lazily on its first user, usually after this point;
	 * on resume it is already running. ELCG AUTO only once GR is up.
	 */
	mutex_lock(&gr->use.mutex);
	tdev->clkgating = true;
	gm20b_elcg(device, GM20B_ELCG, gr->use.enabled ? GM20B_ELCG_AUTO : GM20B_ELCG_RUN);
	mutex_unlock(&gr->use.mutex);
	/* Flush posted writes before handing engines to clients. */
	nvkm_rd32(device, GM20B_GATE_CTRL(engine));
}
