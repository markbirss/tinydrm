#define DEBUG
#define VERBOSE_DEBUG
/*
 * MIPI Display Bus Interface (DBI) LCD controller support
 *
 * Copyright 2016 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_fb_cma_helper.h>
#include <drm/tinydrm/mipi-dbi.h>
#include <drm/tinydrm/tinydrm.h>
#include <drm/tinydrm/tinydrm-helpers.h>
#include <linux/dma-buf.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>
#include <linux/swab.h>
#include <video/mipi_display.h>

#define MIPI_DBI_MAX_SPI_READ_SPEED 2000000 /* 2MHz */

#define DCS_POWER_MODE_DISPLAY			BIT(2)
#define DCS_POWER_MODE_DISPLAY_NORMAL_MODE	BIT(3)
#define DCS_POWER_MODE_SLEEP_MODE		BIT(4)
#define DCS_POWER_MODE_PARTIAL_MODE		BIT(5)
#define DCS_POWER_MODE_IDLE_MODE		BIT(6)
#define DCS_POWER_MODE_RESERVED_MASK		(BIT(0) | BIT(1) | BIT(7))

/**
 * DOC: overview
 *
 * This library provides helpers for MIPI Display Bus Interface (DBI)
 * compatible display controllers.
 *
 * Many controllers are MIPI compliant and can use this library.
 * If a controller uses registers 0x2A and 0x2B to set the area to update
 * and uses register 0x2C to write to frame memory, it is most likely MIPI
 * compliant.
 *
 * Only MIPI Type 1 displays are supported since a full frame memory is needed.
 *
 * There are 3 MIPI DBI implementation types:
 *
 * A. Motorola 6800 type parallel bus
 *
 * B. Intel 8080 type parallel bus
 *
 * C. SPI type with 3 options:
 *
 *    1. 9-bit with the Data/Command signal as the ninth bit
 *    2. Same as above except it's sent as 16 bits
 *    3. 8-bit with the Data/Command signal as a separate D/CX pin
 *
 * Currently mipi_dbi only supports Type C options 1 and 3 with
 * mipi_dbi_spi_init().
 */

#define MIPI_DBI_DEBUG_COMMAND(cmd, data, len) \
({ \
	if (!len) \
		DRM_DEBUG_DRIVER("cmd=%02x\n", cmd); \
	else if (len <= 32) \
		DRM_DEBUG_DRIVER("cmd=%02x, par=%*ph\n", cmd, len, data); \
	else \
		DRM_DEBUG_DRIVER("cmd=%02x, len=%zu\n", cmd, len); \
})

static const u8 mipi_dbi_dcs_read_commands[] = {
	MIPI_DCS_GET_DISPLAY_ID,
	MIPI_DCS_GET_RED_CHANNEL,
	MIPI_DCS_GET_GREEN_CHANNEL,
	MIPI_DCS_GET_BLUE_CHANNEL,
	MIPI_DCS_GET_DISPLAY_STATUS,
	MIPI_DCS_GET_POWER_MODE,
	MIPI_DCS_GET_ADDRESS_MODE,
	MIPI_DCS_GET_PIXEL_FORMAT,
	MIPI_DCS_GET_DISPLAY_MODE,
	MIPI_DCS_GET_SIGNAL_MODE,
	MIPI_DCS_GET_DIAGNOSTIC_RESULT,
	MIPI_DCS_READ_MEMORY_START,
	MIPI_DCS_READ_MEMORY_CONTINUE,
	MIPI_DCS_GET_SCANLINE,
	MIPI_DCS_GET_DISPLAY_BRIGHTNESS,	/* MIPI DCS 1.3 */
	MIPI_DCS_GET_CONTROL_DISPLAY,		/* MIPI DCS 1.3 */
	MIPI_DCS_GET_POWER_SAVE,		/* MIPI DCS 1.3 */
	MIPI_DCS_GET_CABC_MIN_BRIGHTNESS,	/* MIPI DCS 1.3 */
	MIPI_DCS_READ_DDB_START,
	MIPI_DCS_READ_DDB_CONTINUE,
	0, /* sentinel */
};

static bool mipi_dbi_command_is_read(struct mipi_dbi *mipi, u8 cmd)
{
	unsigned int i;

	if (!mipi->read_commands)
		return false;

	for (i = 0; i < 0xff; i++) {
		if (!mipi->read_commands[i])
			return false;
		if (cmd == mipi->read_commands[i])
			return true;
	}

	return false;
}

/*
 * MIPI DBI Type C Option 1
 *
 * If the SPI controller doesn't have 9 bits per word support,
 * use blocks of 9 bytes to send 8x 9-bit words with a 8-bit SPI transfer.
 * Pad partial blocks with MIPI_DCS_NOP (zero).
 */

#define SHIFT_U9_INTO_U64(_dst, _src, _pos) \
{ \
	(_dst) |= 1ULL << (63 - ((_pos) * 9)); \
	(_dst) |= (u64)(_src) << (63 - 8 - ((_pos) * 9)); \
}

static int mipi_dbi_spi1e_transfer(struct mipi_dbi *mipi, int dc,
				   const void *buf, size_t len,
				   size_t max_chunk)
{
	struct spi_device *spi = mipi->spi;
	struct spi_transfer tr = {
		.bits_per_word = 8,
	};
	struct spi_message m;
	size_t max_src_chunk, chunk;
	int i, ret = 0;
	u8 *dst;
	void *buf_dc;
	const u8 *src = buf;

	max_chunk = tinydrm_spi_max_transfer_size(spi, max_chunk);
	if (max_chunk < 9)
		return -EINVAL;

	if (drm_debug & DRM_UT_DRIVER)
		pr_debug("[drm:%s] dc=%d, max_chunk=%zu, transfers:\n",
			 __func__, dc, max_chunk);

	spi_message_init_with_transfers(&m, &tr, 1);

	if (!dc) {
		/* pad at beginning of block */
		if (WARN_ON_ONCE(len != 1))
			return -EINVAL;

		dst = kzalloc(9, GFP_KERNEL);
		if (!dst)
			return -ENOMEM;

		dst[8] = *src;
		tr.tx_buf = dst;
		tr.len = 9;

		tinydrm_dbg_spi_message(spi, &m);
		ret = spi_sync(spi, &m);
		kfree(dst);

		return ret;
	}

	/* 8-byte aligned max_src_chunk that fits max_chunk */
	max_src_chunk = max_chunk / 9 * 8;
	max_src_chunk = min(max_src_chunk, len);
	max_src_chunk = max_t(size_t, 8, max_src_chunk & ~0x7);

	max_chunk = max_src_chunk + (max_src_chunk / 8);
	buf_dc = kmalloc(max_chunk, GFP_KERNEL);
	if (!buf_dc)
		return -ENOMEM;

	tr.tx_buf = buf_dc;

	while (len) {
		size_t added = 0;

		chunk = min(len, max_src_chunk);
		len -= chunk;
		dst = buf_dc;

		if (chunk < 8) {
			/* pad at end of block */
			u64 tmp = 0;
			int j;

			for (j = 0; j < chunk; j++)
				SHIFT_U9_INTO_U64(tmp, *src++, j);

			*(u64 *)dst = cpu_to_be64(tmp);
			dst[8] = 0x00;
			chunk = 8;
			added = 1;
		} else {
			for (i = 0; i < chunk; i += 8) {
				u64 tmp = 0;

				SHIFT_U9_INTO_U64(tmp, *src++, 0);
				SHIFT_U9_INTO_U64(tmp, *src++, 1);
				SHIFT_U9_INTO_U64(tmp, *src++, 2);
				SHIFT_U9_INTO_U64(tmp, *src++, 3);
				SHIFT_U9_INTO_U64(tmp, *src++, 4);
				SHIFT_U9_INTO_U64(tmp, *src++, 5);
				SHIFT_U9_INTO_U64(tmp, *src++, 6);

				tmp |= 0x1;
				/* TODO: unaligned access here? */
				*(u64 *)dst = cpu_to_be64(tmp);
				dst += 8;
				*dst++ = *src++;
				added++;
			}
		}

		tr.len = chunk + added;

		tinydrm_dbg_spi_message(spi, &m);
		ret = spi_sync(spi, &m);
		if (ret)
			goto err_free;
	};

err_free:
	kfree(buf_dc);

	return ret;
}

static int mipi_dbi_spi1_transfer(struct mipi_dbi *mipi, int dc,
				  const void *buf, size_t len,
				  size_t max_chunk)
{
	struct spi_device *spi = mipi->spi;
	struct spi_transfer tr = {
		.bits_per_word = 9,
	};
	const u8 *src8 = buf;
	struct spi_message m;
	size_t max_src_chunk;
	int ret = 0;
	u16 *dst16;

	if (!tinydrm_spi_bpw_supported(spi, 9))
		return mipi_dbi_spi1e_transfer(mipi, dc, buf, len, max_chunk);

	max_chunk = tinydrm_spi_max_transfer_size(spi, max_chunk);

	if (drm_debug & DRM_UT_DRIVER)
		pr_debug("[drm:%s] dc=%d, max_chunk=%zu, transfers:\n",
			 __func__, dc, max_chunk);

	max_src_chunk = min(max_chunk / 2, len);

	dst16 = kmalloc(max_src_chunk * 2, GFP_KERNEL);
	if (!dst16)
		return -ENOMEM;

	spi_message_init_with_transfers(&m, &tr, 1);
	tr.tx_buf = dst16;

	while (len) {
		size_t chunk = min(len, max_src_chunk);
		unsigned int i;

		for (i = 0; i < chunk; i++) {
			dst16[i] = *src8++;
			if (dc)
				dst16[i] |= 0x0100;
		}

		tr.len = chunk;
		len -= chunk;

		tinydrm_dbg_spi_message(spi, &m);
		ret = spi_sync(spi, &m);
		if (ret)
			goto err_free;
	};

err_free:
	kfree(dst16);

	return ret;
}

static int mipi_dbi_typec1_command(struct mipi_dbi *mipi, u8 cmd,
				   u8 *parameters, size_t num)
{
	int ret;

	if (mipi_dbi_command_is_read(mipi, cmd))
		return -ENOTSUPP;

	MIPI_DBI_DEBUG_COMMAND(cmd, parameters, num);

	ret = mipi_dbi_spi1_transfer(mipi, 0, &cmd, 1, 4096);
	if (ret)
		return ret;

	if (num)
		ret = mipi_dbi_spi1_transfer(mipi, 1, parameters, num, 4096);

	return ret;
}

/* MIPI DBI Type C Option 3 */

static int mipi_dbi_typec3_command_read(struct mipi_dbi *mipi, u8 cmd,
					u8 *data, size_t len)
{
	struct spi_device *spi = mipi->spi;
	u32 speed_hz = min_t(u32, MIPI_DBI_MAX_SPI_READ_SPEED,
			     spi->max_speed_hz / 2);
	struct spi_transfer tr[2] = {
		{
			.speed_hz = speed_hz,
			.tx_buf = &cmd,
			.len = 1,
		}, {
			.speed_hz = speed_hz,
			.len = len,
		},
	};
	struct spi_message m;
	u8 *buf;
	int ret;

	if (!len)
		return -EINVAL;

	if (mipi->write_only)
		return -EACCES;

	/*
	 * Support non-standard 24-bit and 32-bit Nokia read commands which
	 * start with a dummy clock, so we need to read an extra byte.
	 */
	if (cmd == MIPI_DCS_GET_DISPLAY_ID ||
	    cmd == MIPI_DCS_GET_DISPLAY_STATUS) {
		if (!(len == 3 || len == 4))
			return -EINVAL;

		tr[1].len = len + 1;
	}

	buf = kmalloc(tr[1].len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	tr[1].rx_buf = buf;
	gpiod_set_value_cansleep(mipi->dc, 0);

	spi_message_init_with_transfers(&m, tr, ARRAY_SIZE(tr));
	ret = spi_sync(spi, &m);
	if (ret)
		goto err_free;

	tinydrm_dbg_spi_message(spi, &m);

	if (tr[1].len == len) {
		memcpy(data, buf, len);
	} else {
		unsigned int i;

		for (i = 0; i < len; i++)
			data[i] = (buf[i] << 1) | !!(buf[i + 1] & BIT(7));
	}

	MIPI_DBI_DEBUG_COMMAND(cmd, data, len);

err_free:
	kfree(buf);

	return ret;
}

static int mipi_dbi_typec3_command(struct mipi_dbi *mipi, u8 cmd,
				   u8 *par, size_t num)
{
	struct spi_device *spi = mipi->spi;
	unsigned int bpw = 8;
	int ret;

	if (mipi_dbi_command_is_read(mipi, cmd))
		return mipi_dbi_typec3_command_read(mipi, cmd, par, num);

	MIPI_DBI_DEBUG_COMMAND(cmd, par, num);

	gpiod_set_value_cansleep(mipi->dc, 0);
	ret = tinydrm_spi_transfer(spi, 0, NULL, 8, &cmd, 1);
	if (ret || !num)
		return ret;

	if (cmd == MIPI_DCS_WRITE_MEMORY_START && !mipi->swap_bytes)
		bpw = 16;

	gpiod_set_value_cansleep(mipi->dc, 1);

	return tinydrm_spi_transfer(spi, 0, NULL, bpw, par, num);
}

/**
 * mipi_dbi_spi_init - Initialize MIPI DBI SPI interfaced controller
 * @spi: SPI device
 * @dc: D/C gpio (optional)
 * @write_only: Controller is write-only
 * @mipi: &mipi_dbi structure to initialize
 * @pipe_funcs: Display pipe functions
 * @driver: DRM driver
 * @mode: Display mode
 * @rotation: Initial rotation in degrees Counter Clock Wise
 *
FIXME XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
 * This function initializes a &mipi_dbi structure using mipi_dbi_init()

 * and intitalizes a &regmap that can be used to send commands to
 * the controller. mipi_dbi_write() can be used to send commands.

 * If @dc is set, a Type C Option 3 interface is assumed, if not
 * Type C Option 1.
 *
 * If the SPI master driver doesn't support the necessary bits per word,
 * the following transformation is used:
 *
 * - 9-bit: reorder buffer as 9x 8-bit words, padded with no-op command.
 * - 16-bit: if big endian send as 8-bit, if little endian swap bytes
 *
 * Returns:
 * Zero on success, negative error code on failure.
 */
int mipi_dbi_spi_init(struct spi_device *spi, struct mipi_dbi *mipi,
		      struct gpio_desc *dc, bool write_only,
		      const struct drm_simple_display_pipe_funcs *pipe_funcs,
		      struct drm_driver *driver,
		      const struct drm_display_mode *mode,
		      unsigned int rotation)
{
	struct device *dev = &spi->dev;

	mipi->spi = spi;
	mipi->write_only = write_only;
	mipi->read_commands = mipi_dbi_dcs_read_commands;

	if (dc) {
		mipi->command = mipi_dbi_typec3_command;
		mipi->dc = dc;
	} else {
		mipi->command = mipi_dbi_typec1_command;
	}

	if (tinydrm_machine_little_endian() &&
	    !tinydrm_spi_bpw_supported(spi, 16))
		mipi->swap_bytes = true;

	return mipi_dbi_init(dev, mipi, pipe_funcs, driver, mode, rotation);
}
EXPORT_SYMBOL(mipi_dbi_spi_init);

static int mipi_dbi_buf_copy(void *dst, struct drm_framebuffer *fb,
				struct drm_clip_rect *clip, bool swap)
{
	struct drm_gem_cma_object *cma_obj = drm_fb_cma_get_gem_obj(fb, 0);
	struct dma_buf_attachment *import_attach = cma_obj->base.import_attach;
	struct drm_format_name_buf format_name;
	void *src = cma_obj->vaddr;
	int ret = 0;

	if (import_attach) {
		ret = dma_buf_begin_cpu_access(import_attach->dmabuf,
					       DMA_FROM_DEVICE);
		if (ret)
			return ret;
	}

	switch (fb->format->format) {
	case DRM_FORMAT_RGB565:
		if (swap)
			tinydrm_swab16(dst, src, fb, clip);
		else
			tinydrm_memcpy(dst, src, fb, clip);
		break;
	case DRM_FORMAT_XRGB8888:
		tinydrm_xrgb8888_to_rgb565(dst, src, fb, clip, swap);
		break;
	default:
		dev_err_once(fb->dev->dev, "Format is not supported: %s\n",
			     drm_get_format_name(fb->format->format,
						 &format_name));
		return -EINVAL;
	}

	if (import_attach)
		ret = dma_buf_end_cpu_access(import_attach->dmabuf,
					     DMA_FROM_DEVICE);
	return ret;
}

static int mipi_dbi_fb_dirty(struct drm_framebuffer *fb,
			     struct drm_file *file_priv,
			     unsigned int flags, unsigned int color,
			     struct drm_clip_rect *clips,
			     unsigned int num_clips)
{
	struct drm_gem_cma_object *cma_obj = drm_fb_cma_get_gem_obj(fb, 0);
	struct tinydrm_device *tdev = drm_to_tinydrm(fb->dev);
	struct mipi_dbi *mipi = mipi_dbi_from_tinydrm(tdev);
	bool swap = mipi->swap_bytes;
	struct drm_clip_rect clip;
	int ret = 0;
	bool full;
	void *tr;

	mutex_lock(&tdev->dev_lock);

	if (!tinydrm_check_dirty(fb, &clips, &num_clips))
		goto out_unlock;

	full = tinydrm_merge_clips(&clip, clips, num_clips, flags,
				   fb->width, fb->height);

	DRM_DEBUG("Flushing [FB:%d] x1=%u, x2=%u, y1=%u, y2=%u\n", fb->base.id,
		  clip.x1, clip.x2, clip.y1, clip.y2);

	if (!full || swap || fb->format->format == DRM_FORMAT_XRGB8888) {
		tr = mipi->tx_buf;
		ret = mipi_dbi_buf_copy(mipi->tx_buf, fb, &clip, swap);
		if (ret)
			goto out_unlock;
	} else {
		tr = cma_obj->vaddr;
	}

	mipi_dbi_command(mipi, MIPI_DCS_SET_COLUMN_ADDRESS,
			 (clip.x1 >> 8) & 0xFF, clip.x1 & 0xFF,
			 (clip.x2 >> 8) & 0xFF, (clip.x2 - 1) & 0xFF);
	mipi_dbi_command(mipi, MIPI_DCS_SET_PAGE_ADDRESS,
			 (clip.y1 >> 8) & 0xFF, clip.y1 & 0xFF,
			 (clip.y2 >> 8) & 0xFF, (clip.y2 - 1) & 0xFF);

	ret = mipi_dbi_command_buf(mipi, MIPI_DCS_WRITE_MEMORY_START, tr,
				(clip.x2 - clip.x1) * (clip.y2 - clip.y1) * 2);
	if (ret)
		goto out_unlock;

	if (!tdev->enabled) {
		if (mipi->enable_delay_ms)
			msleep(mipi->enable_delay_ms);
		ret = tinydrm_enable_backlight(mipi->backlight);
		if (ret) {
			DRM_ERROR("Failed to enable backlight %d\n", ret);
			goto out_unlock;
		}
		tdev->enabled = true;
	}

out_unlock:
	mutex_unlock(&tdev->dev_lock);

	if (ret)
		dev_err_once(fb->dev->dev, "Failed to update display %d\n",
			     ret);

	return ret;
}

static const struct drm_framebuffer_funcs mipi_dbi_fb_funcs = {
	.destroy	= drm_fb_cma_destroy,
	.create_handle	= drm_fb_cma_create_handle,
	.dirty		= mipi_dbi_fb_dirty,
};

static void mipi_dbi_blank(struct mipi_dbi *mipi)
{
	struct drm_device *drm = &mipi->tinydrm.drm;
	u16 height = drm->mode_config.min_height;
	u16 width = drm->mode_config.min_width;
	size_t len = width * height * 2;

	memset(mipi->tx_buf, 0, len);

	mipi_dbi_command(mipi, MIPI_DCS_SET_COLUMN_ADDRESS, 0, 0,
			 (width >> 8) & 0xFF, (width - 1) & 0xFF);
	mipi_dbi_command(mipi, MIPI_DCS_SET_PAGE_ADDRESS, 0, 0,
			 (height >> 8) & 0xFF, (height - 1) & 0xFF);
	mipi_dbi_command_buf(mipi, MIPI_DCS_WRITE_MEMORY_START,
			     (u8 *)mipi->tx_buf, len);
}

/**
 * mipi_dbi_pipe_disable - MIPI DBI pipe disable helper
 * @pipe: Display pipe
 *
 * This function disables the display pipeline by disabling backlight and
 * regulator if present.
 * Drivers can use this as their &drm_simple_display_pipe_funcs->disable
 * callback.
 */
void mipi_dbi_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	struct tinydrm_device *tdev = pipe_to_tinydrm(pipe);
	struct mipi_dbi *mipi = mipi_dbi_from_tinydrm(tdev);

	DRM_DEBUG_KMS("\n");

	mutex_lock(&tdev->dev_lock);

	if (tdev->enabled) {
		if (mipi->backlight)
			tinydrm_disable_backlight(mipi->backlight);
		else if (!mipi->regulator)
			mipi_dbi_blank(mipi);
	}
	tdev->enabled = false;

	if (tdev->prepared && mipi->regulator) {
		regulator_disable(mipi->regulator);
		tdev->prepared = false;
	}

	mutex_unlock(&tdev->dev_lock);
}
EXPORT_SYMBOL(mipi_dbi_pipe_disable);

static const uint32_t mipi_dbi_formats[] = {
	DRM_FORMAT_RGB565,
	DRM_FORMAT_XRGB8888,
};

/**
 * mipi_dbi_init - MIPI DBI initialization
 * @dev: Parent device
 * @mipi: &mipi_dbi structure to initialize
 * @pipe_funcs: Display pipe functions
 * @driver: DRM driver
 * @mode: Display mode
 * @rotation: Initial rotation in degrees Counter Clock Wise
 *
 * This function initializes a &mipi_dbi structure and it's underlying
 * @tinydrm_device and &drm_device. It also sets up the display pipeline.
 * Supported formats: Native RGB565 and emulated XRGB8888.
 * Objects created by this function will be automatically freed on driver
 * detach (devres).
 *
 * Returns:
 * Zero on success, negative error code on failure.
 */
int mipi_dbi_init(struct device *dev, struct mipi_dbi *mipi,
		  const struct drm_simple_display_pipe_funcs *pipe_funcs,
		  struct drm_driver *driver,
		  const struct drm_display_mode *mode, unsigned int rotation)
{
	size_t bufsize = mode->vdisplay * mode->hdisplay * sizeof(u16);
	struct tinydrm_device *tdev = &mipi->tinydrm;
	struct drm_device *drm = &tdev->drm;
	int ret;

	mipi->tx_buf = devm_kmalloc(dev, bufsize, GFP_KERNEL);
	if (!mipi->tx_buf)
		return -ENOMEM;

	ret = devm_tinydrm_init(dev, tdev, &mipi_dbi_fb_funcs, driver);
	if (ret)
		return ret;

	/* TODO: Maybe add DRM_MODE_CONNECTOR_SPI */
	ret = tinydrm_display_pipe_init(tdev, pipe_funcs,
					DRM_MODE_CONNECTOR_VIRTUAL,
					mipi_dbi_formats,
					ARRAY_SIZE(mipi_dbi_formats), mode,
					rotation);
	if (ret)
		return ret;

	drm->mode_config.preferred_depth = 16;
	mipi->rotation = rotation;

	drm_mode_config_reset(drm);

	DRM_DEBUG_KMS("preferred_depth=%u, rotation = %u\n",
		      drm->mode_config.preferred_depth, rotation);

	return 0;
}
EXPORT_SYMBOL(mipi_dbi_init);

/**
 * mipi_dbi_hw_reset - Hardware reset of controller
 * @mipi: MIPI DBI structure
 *
 * Reset controller if the &mipi_dbi->reset gpio is set.
 */
void mipi_dbi_hw_reset(struct mipi_dbi *mipi)
{
	if (!mipi->reset)
		return;

	gpiod_set_value_cansleep(mipi->reset, 0);
	msleep(20);
	gpiod_set_value_cansleep(mipi->reset, 1);
	msleep(120);
}
EXPORT_SYMBOL(mipi_dbi_hw_reset);

/**
 * mipi_dbi_display_is_on - check if display is on
 * @reg: LCD register
 *
 * This function checks the Power Mode register (if readable) to see if
 * display output is turned on. This can be used to see if the bootloader
 * has already turned on the display avoiding flicker when the pipeline is
 * enabled.
 *
 * Returns:
 * true if the display can be verified to be on, false otherwise.
 */
bool mipi_dbi_display_is_on(struct mipi_dbi *mipi)
{
	u8 val;

	if (mipi_dbi_command_buf(mipi, MIPI_DCS_GET_POWER_MODE, &val, 1))
		return false;

	val &= ~DCS_POWER_MODE_RESERVED_MASK;

	if (val != (DCS_POWER_MODE_DISPLAY |
	    DCS_POWER_MODE_DISPLAY_NORMAL_MODE | DCS_POWER_MODE_SLEEP_MODE))
		return false;

	DRM_DEBUG_DRIVER("Display is ON\n");

	return true;
}
EXPORT_SYMBOL(mipi_dbi_display_is_on);

#ifdef CONFIG_DEBUG_FS

static bool mipi_dbi_debugfs_readreg(struct seq_file *m, struct mipi_dbi *mipi,
				     u8 cmd, const char *desc,
				     u8 *buf, size_t len)
{
	int ret;

	ret = mipi_dbi_command_buf(mipi, cmd, buf, len);
	if (ret) {
		seq_printf(m, "\n%s: command %02Xh failed: %d\n", desc, cmd,
			   ret);
		return false;
	}

	seq_printf(m, "\n%s (%02Xh=%*phN):\n", desc, cmd, len, buf);

	return true;
}

static void
seq_bit_val(struct seq_file *m, const char *desc, u32 val, u8 bit)
{
	bool bit_val = !!(val & BIT(bit));

	seq_printf(m, "    D%u=%u: %s\n", bit, bit_val, desc);
}

static void
seq_bit_reserved(struct seq_file *m, u32 val, u8 end, u8 start)
{
	int i;

	for (i = end; i >= start; i--)
		seq_bit_val(m, "Reserved", val, i);
}

static void
seq_bit_array(struct seq_file *m, const char *desc, u32 val, u8 end, u8 start)
{
	u32 bits_val = (val & GENMASK(end, start)) >> start;
	int i;

	seq_printf(m, "    D[%u:%u]=%u: %s ", end, start, bits_val, desc);
	for (i = end; i >= start; i--)
		seq_printf(m, "%u ", !!(val & BIT(i)));

	seq_putc(m, '\n');
}

static void
seq_bit_text(struct seq_file *m, const char *desc, u32 val, u8 bit,
	     const char *on, const char *off)
{
	bool bit_val = val & BIT(bit);

	seq_printf(m, "    D%u=%u: %s %s\n", bit, bit_val, desc,
		   bit_val ? on : off);
}

static inline void
seq_bit_on_off(struct seq_file *m, const char *desc, u32 val, u8 bit)
{
	seq_bit_text(m, desc, val, bit, "On", "Off");
}

static char *mipi_pixel_format_str(u8 val)
{
	switch (val) {
	case 0:
		return "Reserved";
	case 1:
		return "3 bits/pixel";
	case 2:
		return "8 bits/pixel";
	case 3:
		return "12 bits/pixel";
	case 4:
		return "Reserved";
	case 5:
		return "16 bits/pixel";
	case 6:
		return "18 bits/pixel";
	case 7:
		return "24 bits/pixel";
	default:
		return "Illegal format";
	}
}

static int mipi_dbi_debugfs_show(struct seq_file *m, void *arg)
{
	struct drm_info_node *node = (struct drm_info_node *)m->private;
	struct drm_device *drm = node->minor->dev;
	struct tinydrm_device *tdev = drm_to_tinydrm(drm);
	struct mipi_dbi *mipi = mipi_dbi_from_tinydrm(tdev);
	u8 buf[4];
	u8 val8;
	int ret;

	ret = mipi_dbi_command_buf(mipi, MIPI_DCS_GET_POWER_MODE, buf, 1);
	if (ret == -EACCES || ret == -ENOTSUPP) {
		seq_puts(m, "Controller is write-only\n");
		return 0;
	}

	/*
	 * Read Display ID (04h) and Read Display Status (09h) are
	 * non-standard commands that Nokia wanted back in the day,
	 * so most vendors implemented them.
	 */
	if (mipi_dbi_debugfs_readreg(m, mipi, MIPI_DCS_GET_DISPLAY_ID,
				     "Display ID", buf, 3)) {
		seq_printf(m, "    ID1 = 0x%02x\n", buf[0]);
		seq_printf(m, "    ID2 = 0x%02x\n", buf[1]);
		seq_printf(m, "    ID3 = 0x%02x\n", buf[2]);
	}

	if (mipi_dbi_debugfs_readreg(m, mipi, MIPI_DCS_GET_DISPLAY_STATUS,
				     "Display status", buf, 4)) {
		u32 stat;

		stat = (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3];

		seq_bit_on_off(m, "Booster voltage status:", stat, 31);
		seq_bit_val(m, "Row address order", stat, 30);
		seq_bit_val(m, "Column address order", stat, 29);
		seq_bit_val(m, "Row/column exchange", stat, 28);
		seq_bit_text(m, "Vertical refresh:", stat, 27,
			     "Bottom to Top", "Top to Bottom");
		seq_bit_text(m, "RGB/BGR order:", stat, 26, "BGR", "RGB");
		seq_bit_text(m, "Horizontal refresh order:", stat, 25,
			     "Right to Left", "Left to Right");
		seq_bit_reserved(m, stat, 24, 23);
		seq_bit_array(m, "Interface color pixel format:", stat, 22, 20);
		seq_bit_on_off(m, "Idle mode:", stat, 19);
		seq_bit_on_off(m, "Partial mode:", stat, 18);
		seq_bit_text(m, "Sleep:", stat, 17, "Out", "In");
		seq_bit_on_off(m, "Display normal mode:", stat, 16);
		seq_bit_on_off(m, "Vertical scrolling status:", stat, 15);
		seq_bit_reserved(m, stat, 14, 14);
		seq_bit_val(m, "Inversion status", stat, 13);
		seq_bit_val(m, "All pixel ON", stat, 12);
		seq_bit_val(m, "All pixel OFF", stat, 11);
		seq_bit_on_off(m, "Display:", stat, 10);
		seq_bit_on_off(m, "Tearing effect line:", stat, 9);
		seq_bit_array(m, "Gamma curve selection:", stat, 8, 6);
		seq_bit_text(m, "Tearing effect line mode:", stat, 5,
			     "Mode 2, both H-Blanking and V-Blanking",
			     "Mode 1, V-Blanking only");
		seq_bit_reserved(m, stat, 4, 0);
	}

	if (mipi_dbi_debugfs_readreg(m, mipi, MIPI_DCS_GET_POWER_MODE,
				     "Power mode", &val8, 1)) {
		seq_bit_text(m, "Booster", val8, 7, "On", "Off or faulty");
		seq_bit_on_off(m, "Idle Mode", val8, 6);
		seq_bit_on_off(m, "Partial Mode", val8, 5);
		seq_bit_text(m, "Sleep", val8, 4, "Out Mode", "In Mode");
		seq_bit_on_off(m, "Display Normal Mode", val8, 3);
		seq_bit_on_off(m, "Display is", val8, 2);
		seq_bit_reserved(m, val8, 1, 0);
	}

	if (mipi_dbi_debugfs_readreg(m, mipi, MIPI_DCS_GET_ADDRESS_MODE,
				     "Address mode", &val8, 1)) {
		seq_bit_text(m, "Page Address Order:", val8, 7,
			     "Bottom to Top", "Top to Bottom");
		seq_bit_text(m, "Column Address Order:", val8, 6,
			     "Right to Left", "Left to Right");
		seq_bit_text(m, "Page/Column Order:", val8, 5,
			     "Reverse Mode", "Normal Mode");
		seq_bit_text(m, "Line Address Order: LCD Refresh", val8, 4,
			     "Bottom to Top", "Top to Bottom");
		seq_bit_text(m, "RGB/BGR Order:", val8, 3, "BGR", "RGB");
		seq_bit_text(m, "Display Data Latch Data Order: LCD Refresh",
			     val8, 2, "Right to Left", "Left to Right");
		seq_bit_reserved(m, val8, 1, 0);
	}

	if (mipi_dbi_debugfs_readreg(m, mipi, MIPI_DCS_GET_PIXEL_FORMAT,
				     "Pixel format", &val8, 1)) {
		u8 dpi = (val8 >> 4) & 0x7;
		u8 dbi = val8 & 0x7;

		seq_bit_reserved(m, val8, 7, 7);
		seq_printf(m, "    D[6:4]=%u: DPI: %s\n", dpi,
			   mipi_pixel_format_str(dpi));
		seq_bit_reserved(m, val8, 3, 3);
		seq_printf(m, "    D[2:0]=%u: DBI: %s\n", dbi,
			   mipi_pixel_format_str(dbi));
	}

	if (mipi_dbi_debugfs_readreg(m, mipi, MIPI_DCS_GET_DISPLAY_MODE,
				     "Image Mode", &val8, 1)) {
		u8 gc = val8 & 0x7;

		seq_bit_on_off(m, "Vertical Scrolling Status:", val8, 7);
		seq_bit_reserved(m, val8, 6, 6);
		seq_bit_on_off(m, "Inversion:", val8, 5);
		seq_bit_reserved(m, val8, 4, 3);
		seq_printf(m, "    D[2:0]=%u: Gamma Curve Selection: ", gc);
		if (gc < 4)
			seq_printf(m, "GC%u\n", gc);
		else
			seq_puts(m, "Reserved\n");
	}

	if (mipi_dbi_debugfs_readreg(m, mipi, MIPI_DCS_GET_SIGNAL_MODE,
				     "Signal Mode", &val8, 1)) {
		seq_bit_on_off(m, "Tearing Effect Line:", val8, 7);
		seq_bit_text(m, "Tearing Effect Line Output Mode: Mode",
			     val8, 6, "2", "1");
		seq_bit_reserved(m, val8, 5, 0);
	}

	if (mipi_dbi_debugfs_readreg(m, mipi, MIPI_DCS_GET_DIAGNOSTIC_RESULT,
				     "Diagnostic result", &val8, 1)) {
		seq_bit_text(m, "Register Loading Detection:", val8, 7,
			     "OK", "Fault or reset");
		seq_bit_text(m, "Functionality Detection:", val8, 6,
			     "OK", "Fault or reset");
		seq_bit_text(m, "Chip Attachment Detection:", val8, 5,
			     "Fault", "OK or unimplemented");
		seq_bit_text(m, "Display Glass Break Detection:", val8, 4,
			     "Fault", "OK or unimplemented");
		seq_bit_reserved(m, val8, 3, 0);
	}

	return 0;
}

static const struct drm_info_list mipi_dbi_debugfs_list[] = {
	{ "fb",   drm_fb_cma_debugfs_show, 0 },
	{ "mipi",   mipi_dbi_debugfs_show, 0 },
};

/**
 * mipi_dbi_debugfs_init - Create debugfs entries
 * @minor: DRM minor
 *
 * Drivers can use this as their &drm_driver->debugfs_init callback.
 *
 * Returns:
 * Zero on success, negative error code on failure.
 */
int mipi_dbi_debugfs_init(struct drm_minor *minor)
{
	return drm_debugfs_create_files(mipi_dbi_debugfs_list,
					ARRAY_SIZE(mipi_dbi_debugfs_list),
					minor->debugfs_root, minor);
}
EXPORT_SYMBOL(mipi_dbi_debugfs_init);

/**
 * mipi_dbi_debugfs_cleanup - Cleanup debugfs entries
 * @minor: DRM minor
 *
 * Drivers can use this as their &drm_driver->debugfs_cleanup callback.
 */
void mipi_dbi_debugfs_cleanup(struct drm_minor *minor)
{
	drm_debugfs_remove_files(mipi_dbi_debugfs_list,
				 ARRAY_SIZE(mipi_dbi_debugfs_list), minor);
}
EXPORT_SYMBOL(mipi_dbi_debugfs_cleanup);

#endif

MODULE_LICENSE("GPL");
