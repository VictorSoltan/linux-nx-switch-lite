/*
 * Copyright 2015 Red Hat Inc.
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
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: Ben Skeggs <bskeggs@redhat.com>
 */
#include "priv.h"

#include <subdev/fb.h>
#include <subdev/timer.h>
#include <core/memory.h>

/* Tegra has no VRAM: compbit backing store in system memory, sized and based
 * as nvgpu gm20b_ltc_init_comptags/gm20b_ltc_init_cbc, lines for 1 GiB.
 */
static int
gm20b_ltc_oneinit_tag_ram(struct nvkm_ltc *ltc)
{
	struct nvkm_device *device = ltc->subdev.device;
	struct nvkm_fb *fb = device->fb;
	const u32 param = nvkm_rd32(device, 0x17e280);
	const u32 cpc = param & 0xffff;
	const u32 line = 512 << ((param >> 24) & 0xf);
	const u32 slices = param >> 28;
	u32 lines = 8192, size;
	u64 addr, base;
	int ret;

	ltc->num_tags = 0;
	if (!cpc || !slices || !ltc->ltc_nr)
		goto mm_init;

	size = DIV_ROUND_UP(lines, cpc) * line * slices * ltc->ltc_nr;
	size = ALIGN(size + (ltc->ltc_nr << 11), SZ_64K);
	lines = min_t(u64, (u64)size * cpc / (line * slices * ltc->ltc_nr), 0x1ffff);

	ret = nvkm_memory_new(device, NVKM_MEM_TARGET_INST, size, SZ_64K, true,
			      &ltc->tag_ram);
	if (ret)
		goto mm_init;

	addr = nvkm_memory_addr(ltc->tag_ram);
	base = DIV_ROUND_UP_ULL(addr >> 11, ltc->ltc_nr);
	/* nvgpu gm20b_ltc_cbc_fix_config (Bug 1477079): doubled with two LTCs */
	if (ltc->ltc_nr == 2)
		base *= 2;
	if (base > U32_MAX) {
		nvkm_memory_unref(&ltc->tag_ram);
		goto mm_init;
	}

	ltc->tag_base = base;
	ltc->num_tags = lines;
	dev_info(device->dev, "ltc: %u comptag lines, CBC %u KiB at %010llx\n",
		  lines, size >> 10, addr);
mm_init:
	nvkm_mm_fini(&fb->tags.mm);
	return nvkm_mm_init(&fb->tags.mm, 0, 0, ltc->num_tags, 1);
}

static int
gm200_ltc_oneinit(struct nvkm_ltc *ltc)
{
	struct nvkm_device *device = ltc->subdev.device;

	ltc->ltc_nr = nvkm_rd32(device, 0x12006c);
	ltc->lts_nr = nvkm_rd32(device, 0x17e280) >> 28;

	if (device->type == NVKM_DEVICE_TEGRA)
		return gm20b_ltc_oneinit_tag_ram(ltc);
	return gf100_ltc_oneinit_tag_ram(ltc);
}
static void
gm200_ltc_init(struct nvkm_ltc *ltc)
{
	struct nvkm_device *device = ltc->subdev.device;

	/* No VBIOS on Tegra to do this: GM20B stayed on one of its two LTCs. Same writes as nvgpu gm20b_ltc_init_fs_state and gm20b_fb_init_fs. */
	nvkm_wr32(device, 0x17e27c, ltc->ltc_nr);
	nvkm_wr32(device, 0x17e000, ltc->ltc_nr);
	nvkm_wr32(device, 0x100800, ltc->ltc_nr);
	nvkm_mask(device, 0x17e318, 0x00008000, 0x00008000); /* DSTG_CFG0 VDC_4TO2_DISABLE */
	nvkm_wr32(device, 0x17e278, ltc->tag_base);

	/* As nvgpu gm20b_ltc_init_cbc: drop stale cached compbits. */
	if (device->type == NVKM_DEVICE_TEGRA && ltc->num_tags) {
		nvkm_mask(device, 0x17e26c, 0x00000002, 0x00000002);
		gm107_ltc_cbc_wait(ltc);
	}
}

static const struct nvkm_ltc_func
gm200_ltc = {
	.oneinit = gm200_ltc_oneinit,
	.init = gm200_ltc_init,
	.intr = gm107_ltc_intr,
	.cbc_clear = gm107_ltc_cbc_clear,
	.cbc_wait = gm107_ltc_cbc_wait,
	.zbc_color = 16,
	.zbc_depth = 16,
	.zbc_clear_color = gm107_ltc_zbc_clear_color,
	.zbc_clear_depth = gm107_ltc_zbc_clear_depth,
	.invalidate = gf100_ltc_invalidate,
	.flush = gf100_ltc_flush,
};

int
gm200_ltc_new(struct nvkm_device *device, enum nvkm_subdev_type type, int inst,
	      struct nvkm_ltc **pltc)
{
	return nvkm_ltc_new_(&gm200_ltc, device, type, inst, pltc);
}
