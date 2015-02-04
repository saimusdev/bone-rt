/*
 * Copyright (C) 2012 Texas Instruments
 * Author: Rob Clark <robdclark@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/kfifo.h>

#include "tilcdc_drv.h"
#include "tilcdc_regs.h"

struct tilcdc_crtc {
	struct drm_crtc base;

	const struct tilcdc_panel_info *info;
	uint32_t dirty;
	dma_addr_t start, end;
	struct drm_pending_vblank_event *event;
	int dpms;
	wait_queue_head_t frame_done_wq;
	bool frame_done;

	/* fb currently set to scanout 0/1: */
	struct drm_framebuffer *scanout[2];

	/* for deferred fb unref's: */
	DECLARE_KFIFO_PTR(unref_fifo, struct drm_framebuffer *);
	struct work_struct work;
};
#define to_tilcdc_crtc(x) container_of(x, struct tilcdc_crtc, base)

static void unref_worker(struct work_struct *work)
{
	struct tilcdc_crtc *tilcdc_crtc = container_of(work, struct tilcdc_crtc, work);
	struct drm_device *dev = tilcdc_crtc->base.dev;
	struct drm_framebuffer *fb;

	mutex_lock(&dev->mode_config.mutex);
	while (kfifo_get(&tilcdc_crtc->unref_fifo, &fb))
		drm_framebuffer_unreference(fb);
	mutex_unlock(&dev->mode_config.mutex);
}

static void set_scanout(struct drm_crtc *crtc, int n)
{
	static const uint32_t base_reg[] = {
			LCDC_DMA_FB_BASE_ADDR_0_REG, LCDC_DMA_FB_BASE_ADDR_1_REG,
	};
	static const uint32_t ceil_reg[] = {
			LCDC_DMA_FB_CEILING_ADDR_0_REG, LCDC_DMA_FB_CEILING_ADDR_1_REG,
	};
	static const uint32_t stat[] = {
			LCDC_END_OF_FRAME0, LCDC_END_OF_FRAME1,
	};
	struct tilcdc_crtc *tilcdc_crtc = to_tilcdc_crtc(crtc);
	struct drm_device *dev = crtc->dev;

	tilcdc_write(dev, base_reg[n], tilcdc_crtc->start);
	tilcdc_write(dev, ceil_reg[n], tilcdc_crtc->end);
	if (tilcdc_crtc->scanout[n]) {
		if (kfifo_put(&tilcdc_crtc->unref_fifo,
				(const struct drm_framebuffer **)&tilcdc_crtc->scanout[n])) {
			struct tilcdc_drm_private *priv = dev->dev_private;
			queue_work(priv->wq, &tilcdc_crtc->work);
		} else {
			dev_err(dev->dev, "unref fifo full!\n");
			drm_framebuffer_unreference(tilcdc_crtc->scanout[n]);
		}
	}
	tilcdc_crtc->scanout[n] = crtc->fb;
	drm_framebuffer_reference(tilcdc_crtc->scanout[n]);
	tilcdc_crtc->dirty &= ~stat[n];
}

static void update_scanout(struct drm_crtc *crtc)
{
	struct tilcdc_crtc *tilcdc_crtc = to_tilcdc_crtc(crtc);
	struct drm_device *dev = crtc->dev;
	struct drm_framebuffer *fb = crtc->fb;
	struct drm_gem_cma_object *gem;
	unsigned int depth, bpp;

	drm_fb_get_bpp_depth(fb->pixel_format, &depth, &bpp);
	gem = drm_fb_cma_get_gem_obj(fb, 0);

	tilcdc_crtc->start = gem->paddr + fb->offsets[0] +
			(crtc->y * fb->pitches[0]) + (crtc->x * bpp/8);

	tilcdc_crtc->end = tilcdc_crtc->start +
			(crtc->mode.vdisplay * fb->pitches[0]);

	if (tilcdc_crtc->dpms == DRM_MODE_DPMS_ON) {
		/* already enabled, so just mark the frames that need
		 * updating and they will be updated on vblank:
		 */
		tilcdc_crtc->dirty |= LCDC_END_OF_FRAME0 | LCDC_END_OF_FRAME1;
		drm_vblank_get(dev, 0);
	} else {
		/* not enabled yet, so update registers immediately: */
		set_scanout(crtc, 0);
		set_scanout(crtc, 1);
	}
}

static void start(struct drm_crtc *crtc)
{
	struct drm_device *dev = crtc->dev;
	struct tilcdc_drm_private *priv = dev->dev_private;

	if (priv->rev == 2) {
		tilcdc_set(dev, LCDC_CLK_RESET_REG, LCDC_CLK_MAIN_RESET);
		msleep(1);
		tilcdc_clear(dev, LCDC_CLK_RESET_REG, LCDC_CLK_MAIN_RESET);
		msleep(1);
	}

	tilcdc_set(dev, LCDC_DMA_CTRL_REG, LCDC_DUAL_FRAME_BUFFER_ENABLE);
	tilcdc_set(dev, LCDC_RASTER_CTRL_REG, LCDC_PALETTE_LOAD_MODE(DATA_ONLY));
	tilcdc_set(dev, LCDC_RASTER_CTRL_REG, LCDC_RASTER_ENABLE);
}

static void stop(struct drm_crtc *crtc)
{
	struct drm_device *dev = crtc->dev;

	tilcdc_clear(dev, LCDC_RASTER_CTRL_REG, LCDC_RASTER_ENABLE);
}

static void tilcdc_crtc_destroy(struct drm_crtc *crtc)
{
	struct tilcdc_crtc *tilcdc_crtc = to_tilcdc_crtc(crtc);

	WARN_ON(tilcdc_crtc->dpms == DRM_MODE_DPMS_ON);

	drm_crtc_cleanup(crtc);
	WARN_ON(!kfifo_is_empty(&tilcdc_crtc->unref_fifo));
	kfifo_free(&tilcdc_crtc->unref_fifo);
	kfree(tilcdc_crtc);
}

static int tilcdc_crtc_page_flip(struct drm_crtc *crtc,
		struct drm_framebuffer *fb,
		struct drm_pending_vblank_event *event)
{
	struct tilcdc_crtc *tilcdc_crtc = to_tilcdc_crtc(crtc);
	struct drm_device *dev = crtc->dev;

	if (tilcdc_crtc->event) {
		dev_err(dev->dev, "already pending page flip!\n");
		return -EBUSY;
	}

	crtc->fb = fb;
	tilcdc_crtc->event = event;
	pm_runtime_get_sync(dev->dev);
	update_scanout(crtc);
	pm_runtime_put_sync(dev->dev);

	return 0;
}

static void tilcdc_crtc_dpms(struct drm_crtc *crtc, int mode)
{
	struct tilcdc_crtc *tilcdc_crtc = to_tilcdc_crtc(crtc);
	struct drm_device *dev = crtc->dev;
	struct tilcdc_drm_private *priv = dev->dev_private;

	/* we really only care about on or off: */
	if (mode != DRM_MODE_DPMS_ON)
		mode = DRM_MODE_DPMS_OFF;

	if (tilcdc_crtc->dpms == mode)
		return;

	tilcdc_crtc->dpms = mode;

	pm_runtime_get_sync(dev->dev);

	if (mode == DRM_MODE_DPMS_ON) {
		pm_runtime_forbid(dev->dev);
		start(crtc);
	} else {
		tilcdc_crtc->frame_done = false;
		stop(crtc);

		/* if necessary wait for framedone irq which will still come
		 * before putting things to sleep..
		 */
		if (priv->rev == 2) {
			int ret = wait_event_timeout(
					tilcdc_crtc->frame_done_wq,
					tilcdc_crtc->frame_done,
					msecs_to_jiffies(50));
			if (ret == 0)
				dev_err(dev->dev, "timeout waiting for framedone\n");
		}
		pm_runtime_allow(dev->dev);
	}

	pm_runtime_put_sync(dev->dev);
}

static bool tilcdc_crtc_mode_fixup(struct drm_crtc *crtc,
		const struct drm_display_mode *mode,
		struct drm_display_mode *adjusted_mode)
{
	return true;
}

static void tilcdc_crtc_prepare(struct drm_crtc *crtc)
{
	tilcdc_crtc_dpms(crtc, DRM_MODE_DPMS_OFF);
}

static void tilcdc_crtc_commit(struct drm_crtc *crtc)
{
	tilcdc_crtc_dpms(crtc, DRM_MODE_DPMS_ON);
}

static int tilcdc_crtc_mode_set(struct drm_crtc *crtc,
		struct drm_display_mode *mode,
		struct drm_display_mode *adjusted_mode,
		int x, int y,
		struct drm_framebuffer *old_fb)
{
	struct tilcdc_crtc *tilcdc_crtc = to_tilcdc_crtc(crtc);
	struct drm_device *dev = crtc->dev;
	struct tilcdc_drm_private *priv = dev->dev_private;
	const struct tilcdc_panel_info *info = tilcdc_crtc->info;
	uint32_t reg, hbp, hfp, hsw, vbp, vfp, vsw;
	int ret;

	ret = tilcdc_crtc_mode_valid(crtc, mode, 0, 0, NULL);
	if (WARN_ON(ret))
		return ret;

	if (WARN_ON(!info))
		return -EINVAL;

	pm_runtime_get_sync(dev->dev);

	/* Configure the Burst Size and fifo threshold of DMA: */
	reg = tilcdc_read(dev, LCDC_DMA_CTRL_REG) & ~0x00000770;
	switch (info->dma_burst_sz) {
	case 1:
		reg |= LCDC_DMA_BURST_SIZE(LCDC_DMA_BURST_1);
		break;
	case 2:
		reg |= LCDC_DMA_BURST_SIZE(LCDC_DMA_BURST_2);
		break;
	case 4:
		reg |= LCDC_DMA_BURST_SIZE(LCDC_DMA_BURST_4);
		break;
	case 8:
		reg |= LCDC_DMA_BURST_SIZE(LCDC_DMA_BURST_8);
		break;
	case 16:
		reg |= LCDC_DMA_BURST_SIZE(LCDC_DMA_BURST_16);
		break;
	default:
		return -EINVAL;
	}
	reg |= (info->fifo_th << 8);
	tilcdc_write(dev, LCDC_DMA_CTRL_REG, reg);

	/* Configure timings: */
	hbp = mode->htotal - mode->hsync_end;
	hfp = mode->hsync_start - mode->hdisplay;
	hsw = mode->hsync_end - mode->hsync_start;
	vbp = mode->vtotal - mode->vsync_end;
	vfp = mode->vsync_start - mode->vdisplay;
	vsw = mode->vsync_end - mode->vsync_start;

	DBG("%dx%d, hbp=%u, hfp=%u, hsw=%u, vbp=%u, vfp=%u, vsw=%u",
			mode->hdisplay, mode->vdisplay, hbp, hfp, hsw, vbp, vfp, vsw);

	/* Configure the AC Bias Period and Number of Transitions per Interrupt: */
	reg = tilcdc_read(dev, LCDC_RASTER_TIMING_2_REG);
	reg &= ~0x000fff00;
	reg |= LCDC_AC_BIAS_FREQUENCY(info->ac_bias) |
		LCDC_AC_BIAS_TRANSITIONS_PER_INT(info->ac_bias_intrpt);

	/* subtract one from hfp, hbp, hsw because the hardware uses a value of 0 as 1 */
	if (priv->rev == 2) {
		/* clear bits we're going to set */
		reg &= ~0x78000033;
		reg |= ((hfp-1) & 0x300) >> 8;
		reg |= ((hbp-1) & 0x300) >> 4;
		reg |= ((hsw-1) & 0x3c0) << 21;
	}
	tilcdc_write(dev, LCDC_RASTER_TIMING_2_REG, reg);

	/* subtract one from hfp, hbp, hsw because the hardware uses a value of 0 as 1 */
	/* weirdly on BeagleBone Black) the hbp is still wrong when analyzed on hdmi timing tool */
	/* all other values come out correct - need to look into why this is happening */
	/* might be something that is happening after the LCD output in the hdmi encoder */
	reg = (((mode->hdisplay >> 4) - 1) << 4) |
	    (((hbp-1) & 0xff) << 24) |
	    (((hfp-1) & 0xff) << 16) |
	    (((hsw-1) & 0x3f) << 10);
	if (priv->rev == 2)
		reg |= (((mode->hdisplay >> 4) - 1) & 0x40) >> 3;

	tilcdc_write(dev, LCDC_RASTER_TIMING_0_REG, reg);

	/* only the vertical sync width maps 0 as 1 so only subtract 1 from vsw */
	reg = ((mode->vdisplay - 1) & 0x3ff) |
	    ((vbp & 0xff) << 24) |
	    ((vfp & 0xff) << 16) |
	    (((vsw-1) & 0x3f) << 10);
	tilcdc_write(dev, LCDC_RASTER_TIMING_1_REG, reg);

        if (priv->rev == 2) {
		if ((mode->vdisplay - 1) & 0x400)
			tilcdc_set(dev, LCDC_RASTER_TIMING_2_REG, LCDC_LPP_B10);
		else
			tilcdc_clear(dev, LCDC_RASTER_TIMING_2_REG, LCDC_LPP_B10);
        }

	/* Configure display type: */
	reg = tilcdc_read(dev, LCDC_RASTER_CTRL_REG) &
	    ~(LCDC_TFT_MODE | LCDC_MONO_8BIT_MODE | LCDC_MONOCHROME_MODE |
	      LCDC_V2_TFT_24BPP_MODE | LCDC_V2_TFT_24BPP_UNPACK | 0x000ff000);
	reg |= LCDC_TFT_MODE; /* no monochrome/passive support */
	if (info->tft_alt_mode)
		reg |= LCDC_TFT_ALT_ENABLE;
	if (priv->rev == 2) {
		unsigned int depth, bpp;

		drm_fb_get_bpp_depth(crtc->fb->pixel_format, &depth, &bpp);
		switch (bpp) {
		case 16:
			break;
		case 32:
		        reg |= LCDC_V2_TFT_24BPP_UNPACK;
			/* fallthrough */
		case 24:
			reg |= LCDC_V2_TFT_24BPP_MODE;
			break;
		default:
			dev_err(dev->dev, "invalid pixel format\n");
			return -EINVAL;
		}
	}
	reg |= info->fdd < 12;
	tilcdc_write(dev, LCDC_RASTER_CTRL_REG, reg);

	if (info->invert_pxl_clk)
		tilcdc_set(dev, LCDC_RASTER_TIMING_2_REG, LCDC_INVERT_PIXEL_CLOCK);
	else
		tilcdc_clear(dev, LCDC_RASTER_TIMING_2_REG, LCDC_INVERT_PIXEL_CLOCK);

	if (info->sync_ctrl)
		tilcdc_set(dev, LCDC_RASTER_TIMING_2_REG, LCDC_SYNC_CTRL);
	else
		tilcdc_clear(dev, LCDC_RASTER_TIMING_2_REG, LCDC_SYNC_CTRL);

	if (info->sync_edge)
		tilcdc_set(dev, LCDC_RASTER_TIMING_2_REG, LCDC_SYNC_EDGE);
	else
		tilcdc_clear(dev, LCDC_RASTER_TIMING_2_REG, LCDC_SYNC_EDGE);

	if (mode->flags & DRM_MODE_FLAG_NHSYNC)
		tilcdc_set(dev, LCDC_RASTER_TIMING_2_REG, LCDC_INVERT_HSYNC);
	else
		tilcdc_clear(dev, LCDC_RASTER_TIMING_2_REG, LCDC_INVERT_HSYNC);

	if (mode->flags & DRM_MODE_FLAG_NVSYNC)
		tilcdc_set(dev, LCDC_RASTER_TIMING_2_REG, LCDC_INVERT_VSYNC);
	else
		tilcdc_clear(dev, LCDC_RASTER_TIMING_2_REG, LCDC_INVERT_VSYNC);

	if (info->raster_order)
		tilcdc_set(dev, LCDC_RASTER_CTRL_REG, LCDC_RASTER_ORDER);
	else
		tilcdc_clear(dev, LCDC_RASTER_CTRL_REG, LCDC_RASTER_ORDER);


	update_scanout(crtc);
	tilcdc_crtc_update_clk(crtc);

	pm_runtime_put_sync(dev->dev);

	return 0;
}

static int tilcdc_crtc_mode_set_base(struct drm_crtc *crtc, int x, int y,
		struct drm_framebuffer *old_fb)
{
	struct drm_device *dev = crtc->dev;

	pm_runtime_get_sync(dev->dev);
	update_scanout(crtc);
	pm_runtime_put_sync(dev->dev);
	return 0;
}

static void tilcdc_crtc_load_lut(struct drm_crtc *crtc)
{
}

static const struct drm_crtc_funcs tilcdc_crtc_funcs = {
		.destroy        = tilcdc_crtc_destroy,
		.set_config     = drm_crtc_helper_set_config,
		.page_flip      = tilcdc_crtc_page_flip,
};

static const struct drm_crtc_helper_funcs tilcdc_crtc_helper_funcs = {
		.dpms           = tilcdc_crtc_dpms,
		.mode_fixup     = tilcdc_crtc_mode_fixup,
		.prepare        = tilcdc_crtc_prepare,
		.commit         = tilcdc_crtc_commit,
		.mode_set       = tilcdc_crtc_mode_set,
		.mode_set_base  = tilcdc_crtc_mode_set_base,
		.load_lut       = tilcdc_crtc_load_lut,
};

int tilcdc_crtc_max_width(struct drm_crtc *crtc)
{
	struct drm_device *dev = crtc->dev;
	struct tilcdc_drm_private *priv = dev->dev_private;
	int max_width = 0;

	if (priv->rev == 1)
		max_width = 1024;
	else if (priv->rev == 2)
		max_width = 2048;

	return max_width;
}

/* this should find it's way to DT */
static int audio_mode_check(int hdisplay, int vdisplay, int refresh,
		int cea_mode)
{
	/* only cea modes can handle audio */
	if (cea_mode <= 0)
		return 0;

	/* hardcoded mode that supports audio (at 24Hz) */
	if (hdisplay == 1920 && vdisplay == 1080 && refresh == 24)
		return 1;

	/* from the rest, only those modes that refresh at 50/60 */
	if (refresh != 50 && refresh != 60)
		return 0;

	return 1;
}

int tilcdc_crtc_mode_valid(struct drm_crtc *crtc, struct drm_display_mode *mode,
		int rb_check, int audio, struct edid *edid)
{
  	struct tilcdc_drm_private *priv = crtc->dev->dev_private;
	unsigned int bandwidth;
	uint32_t hbp, hfp, hsw, vbp, vfp, vsw;
	int has_audio, is_cea_mode, can_output_audio, refresh;
	uint8_t cea_mode;

	int rb;

	/* check to see if the width is within the range that the LCD Controller physically supports */
	if (mode->hdisplay > tilcdc_crtc_max_width(crtc)) {
		return MODE_VIRTUAL_X;
	}

	/* width must be multiple of 16 */
	if (mode->hdisplay & 0xf)
		return MODE_VIRTUAL_X;

	if (mode->vdisplay > 2048)
		return MODE_VIRTUAL_Y;

	/* set if there's audio capability */
	has_audio = edid && drm_detect_monitor_audio(edid);

	/* only 50 & 60Hz modes reliably support audio */
	refresh = drm_mode_vrefresh(mode);

	/* set if it's a cea mode */
	cea_mode = drm_match_cea_mode(mode);
	is_cea_mode = cea_mode > 0;

	/* set if we can output audio */
	can_output_audio = edid && has_audio &&
			audio_mode_check(mode->hdisplay, mode->vdisplay,
					refresh, cea_mode);

	DBG("mode %dx%d@%d pixel-clock %d audio %s cea %s can_output %s",
		mode->hdisplay, mode->vdisplay, refresh,
		mode->clock,
		has_audio ? "true" : "false",
		is_cea_mode ? "true" : "false",
		can_output_audio ? "true" : "false" );

	/* we only prune the mode if we ask for it */
	if (audio && edid && has_audio && !can_output_audio) {
		DBG("Pruning mode : Does not support audio");
		return MODE_BAD;
	}

	hbp = mode->htotal - mode->hsync_end;
	hfp = mode->hsync_start - mode->hdisplay;
	hsw = mode->hsync_end - mode->hsync_start;
	vbp = mode->vtotal - mode->vsync_end;
	vfp = mode->vsync_start - mode->vdisplay;
	vsw = mode->vsync_end - mode->vsync_start;

	if (hbp & ~0x3ff) {
		DBG("Pruning mode : Horizontal Back Porch out of range");
		return MODE_BAD;
	}

	if (hfp & ~0x3ff) {
		DBG("Pruning mode : Horizontal Front Porch out of range");
		return MODE_BAD;
	}

	if (hsw & ~0x3ff) {
		DBG("Pruning mode : Horizontal Sync Width out of range");
		return MODE_BAD;
	}

	if (vbp & ~0xff) {
		DBG("Pruning mode : Vertical Back Porch out of range");
		return MODE_BAD;
	}

	if (vfp & ~0xff) {
		DBG("Pruning mode : Vertical Front Porch out of range");
		return MODE_BAD;
	}

	if (vsw & ~0x3f) {
		DBG("Pruning mode : Vertical Sync Width out of range");
		return MODE_BAD;
	}

	/* some devices have a maximum allowed pixel clock */
	/* configured from the DT */
	if (mode->clock > priv->max_pixelclock) {
		DBG("Pruning mode, pixel clock too high");
		return MODE_BAD;
	}

	/* some devices further limit the max horizontal resolution */
	/* configured from the DT */
	if (mode->hdisplay > priv->max_width) {
		DBG("Pruning mode, above max width of %d supported by device", priv->max_width);
		return MODE_BAD;
	}

	/* filter out modes that would require too much memory bandwidth: */
	/* configured from the DT */
	bandwidth = mode->hdisplay * mode->vdisplay * drm_mode_vrefresh(mode);
	if (bandwidth > priv->max_bandwidth) {
		DBG("Pruning mode, exceeds defined bandwidth limit");
		return MODE_BAD;
	}

	if (rb_check) {
		/* we only support reduced blanking modes */
		rb = (mode->htotal - mode->hdisplay == 160) &&
			(mode->hsync_end - mode->hdisplay == 80) &&
			(mode->hsync_end - mode->hsync_start == 32) &&
			(mode->vsync_start - mode->vdisplay == 3);
		if (!rb) {
			DBG("Pruning mode, only support reduced blanking modes");
			return MODE_BAD;
		}
	}

	return MODE_OK;
}

void tilcdc_crtc_set_panel_info(struct drm_crtc *crtc,
		const struct tilcdc_panel_info *info)
{
	struct tilcdc_crtc *tilcdc_crtc = to_tilcdc_crtc(crtc);
	tilcdc_crtc->info = info;
}

void tilcdc_crtc_update_clk(struct drm_crtc *crtc)
{
	struct tilcdc_crtc *tilcdc_crtc = to_tilcdc_crtc(crtc);
	struct drm_device *dev = crtc->dev;
	struct tilcdc_drm_private *priv = dev->dev_private;
	int dpms = tilcdc_crtc->dpms;
	unsigned int lcd_clk, div;
	int ret;

	pm_runtime_get_sync(dev->dev);

	if (dpms == DRM_MODE_DPMS_ON)
		tilcdc_crtc_dpms(crtc, DRM_MODE_DPMS_OFF);

	/* in raster mode, minimum divisor is 2: */
	ret = clk_set_rate(priv->disp_clk, crtc->mode.clock * 1000 * 2);
	if (ret) {
		dev_err(dev->dev, "failed to set display clock rate to: %d\n",
				crtc->mode.clock);
		goto out;
	}

	lcd_clk = clk_get_rate(priv->clk);
	div = lcd_clk / (crtc->mode.clock * 1000);

	DBG("lcd_clk=%u, mode clock=%d, div=%u", lcd_clk, crtc->mode.clock, div);
	DBG("fck=%lu, dpll_disp_ck=%lu", clk_get_rate(priv->clk), clk_get_rate(priv->disp_clk));

	/* Configure the LCD clock divisor. */
	tilcdc_write(dev, LCDC_CTRL_REG, LCDC_CLK_DIVISOR(div) |
			LCDC_RASTER_MODE);

	if (priv->rev == 2)
		tilcdc_set(dev, LCDC_CLK_ENABLE_REG,
				LCDC_V2_DMA_CLK_EN | LCDC_V2_LIDD_CLK_EN |
				LCDC_V2_CORE_CLK_EN);

	if (dpms == DRM_MODE_DPMS_ON)
		tilcdc_crtc_dpms(crtc, DRM_MODE_DPMS_ON);

out:
	pm_runtime_put_sync(dev->dev);
}

irqreturn_t tilcdc_crtc_irq(struct drm_crtc *crtc)
{
	struct tilcdc_crtc *tilcdc_crtc = to_tilcdc_crtc(crtc);
	struct drm_device *dev = crtc->dev;
	struct tilcdc_drm_private *priv = dev->dev_private;
	uint32_t stat = tilcdc_read_irqstatus(dev);

	if ((stat & LCDC_SYNC_LOST) && (stat & LCDC_FIFO_UNDERFLOW)) {
		stop(crtc);
		dev_err(dev->dev, "error: %08x\n", stat);
		tilcdc_clear_irqstatus(dev, stat);
		start(crtc);
	} else if (stat & LCDC_PL_LOAD_DONE) {
		tilcdc_clear_irqstatus(dev, stat);
	} else {
		struct drm_pending_vblank_event *event;
		unsigned long flags;
		uint32_t dirty = tilcdc_crtc->dirty & stat;

		tilcdc_clear_irqstatus(dev, stat);

		if (dirty & LCDC_END_OF_FRAME0)
			set_scanout(crtc, 0);

		if (dirty & LCDC_END_OF_FRAME1)
			set_scanout(crtc, 1);

		drm_handle_vblank(dev, 0);

		spin_lock_irqsave(&dev->event_lock, flags);
		event = tilcdc_crtc->event;
		tilcdc_crtc->event = NULL;
		if (event)
			drm_send_vblank_event(dev, 0, event);
		spin_unlock_irqrestore(&dev->event_lock, flags);

		if (dirty && !tilcdc_crtc->dirty)
			drm_vblank_put(dev, 0);
	}

	if (priv->rev == 2) {
		if (stat & LCDC_FRAME_DONE) {
			tilcdc_crtc->frame_done = true;
			wake_up(&tilcdc_crtc->frame_done_wq);
		}
		tilcdc_write(dev, LCDC_END_OF_INT_IND_REG, 0);
	}

	return IRQ_HANDLED;
}

void tilcdc_crtc_cancel_page_flip(struct drm_crtc *crtc, struct drm_file *file)
{
	struct tilcdc_crtc *tilcdc_crtc = to_tilcdc_crtc(crtc);
	struct drm_pending_vblank_event *event;
	struct drm_device *dev = crtc->dev;
	unsigned long flags;

	/* Destroy the pending vertical blanking event associated with the
	 * pending page flip, if any, and disable vertical blanking interrupts.
	 */
	spin_lock_irqsave(&dev->event_lock, flags);
	event = tilcdc_crtc->event;
	if (event && event->base.file_priv == file) {
		tilcdc_crtc->event = NULL;
		event->base.destroy(&event->base);
		drm_vblank_put(dev, 0);
	}
	spin_unlock_irqrestore(&dev->event_lock, flags);
}

struct drm_crtc *tilcdc_crtc_create(struct drm_device *dev)
{
	struct tilcdc_crtc *tilcdc_crtc;
	struct drm_crtc *crtc;
	int ret;

	tilcdc_crtc = kzalloc(sizeof(*tilcdc_crtc), GFP_KERNEL);
	if (!tilcdc_crtc) {
		dev_err(dev->dev, "allocation failed\n");
		return NULL;
	}

	crtc = &tilcdc_crtc->base;

	tilcdc_crtc->dpms = DRM_MODE_DPMS_OFF;
	init_waitqueue_head(&tilcdc_crtc->frame_done_wq);

	ret = kfifo_alloc(&tilcdc_crtc->unref_fifo, 16, GFP_KERNEL);
	if (ret) {
		dev_err(dev->dev, "could not allocate unref FIFO\n");
		goto fail;
	}

	INIT_WORK(&tilcdc_crtc->work, unref_worker);

	ret = drm_crtc_init(dev, crtc, &tilcdc_crtc_funcs);
	if (ret < 0)
		goto fail;

	drm_crtc_helper_add(crtc, &tilcdc_crtc_helper_funcs);

	return crtc;

fail:
	tilcdc_crtc_destroy(crtc);
	return NULL;
}

struct tilcdc_panel_info *tilcdc_of_get_panel_info(struct device_node *np)
{
	struct device_node *info_np;
	struct tilcdc_panel_info *info;
	int ret = 0;

	if (!np)
		return NULL;

	info_np = of_get_child_by_name(np, "panel-info");
	if (!info_np) {
		pr_err("%s: could not find panel-info node\n",
				of_node_full_name(np));
		return NULL;
	}

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info) {
		pr_err("%s: allocation failed\n",
				of_node_full_name(np));
		goto err_no_mem;
	}

	ret |= of_property_read_u32(info_np, "ac-bias", &info->ac_bias);
	ret |= of_property_read_u32(info_np, "ac-bias-intrpt", &info->ac_bias_intrpt);
	ret |= of_property_read_u32(info_np, "dma-burst-sz", &info->dma_burst_sz);
	ret |= of_property_read_u32(info_np, "bpp", &info->bpp);
	ret |= of_property_read_u32(info_np, "fdd", &info->fdd);
	ret |= of_property_read_u32(info_np, "sync-edge", &info->sync_edge);
	ret |= of_property_read_u32(info_np, "sync-ctrl", &info->sync_ctrl);
	ret |= of_property_read_u32(info_np, "raster-order", &info->raster_order);
	ret |= of_property_read_u32(info_np, "fifo-th", &info->fifo_th);

	/* optional: */
	info->tft_alt_mode      = of_property_read_bool(info_np, "tft-alt-mode");
	info->invert_pxl_clk    = of_property_read_bool(info_np, "invert-pxl-clk");

	if (ret) {
		pr_err("%s: error reading panel-info properties\n",
				of_node_full_name(info_np));
		goto err_bad_prop;
	}

	/* release ref */
	of_node_put(info_np);

	return info;

err_bad_prop:
	kfree(info);
err_no_mem:
	of_node_put(info_np);
	return NULL;
}

