// SPDX-License-Identifier: GPL-2.0+
/*
 * DRM driver for Ilitek ILI9486 panels
 *
 * Copyright 2020 Kamlesh Gurudasani <kamlesh.gurudasani@gmail.com>
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/spi/spi.h>

#include <video/mipi_display.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fbdev_dma.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_mipi_dbi.h>
#include <drm/drm_modeset_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_gem.h>
#include <drm/drm_prime.h>
#include <linux/dma-buf.h>
#include <linux/iosys-map.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>
#include <linux/ktime.h>
#include <linux/sysfs.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_vma_manager.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_print.h>
#include <drm/drm_rect.h>

#define ILI9486_ITFCTR1         0xb0
#define ILI9486_PWCTRL1         0xc2
#define ILI9486_VMCTRL1         0xc5
#define ILI9486_PGAMCTRL        0xe0
#define ILI9486_NGAMCTRL        0xe1
#define ILI9486_DGAMCTRL        0xe2
#define ILI9486_MADCTL_BGR      BIT(3)
#define ILI9486_MADCTL_MV       BIT(5)
#define ILI9486_MADCTL_MX       BIT(6)
#define ILI9486_MADCTL_MY       BIT(7)

/*
 * Non-blocking dirty path. The core's mipi_dbi_pipe_update copies
 * the damage into tx_buf and then sends it over SPI synchronously inside the atomic commit, so
 * X sits in DIRTYFB for one full frame (25.8 ms at 100 MHz) on every panel update and the cursor
 * stutters on BOTH screens. Here the commit only converts the damage rect into one of two
 * panel-sized RGB565 buffers (the "pending" one) and queues a worker; the worker swaps the
 * buffers under the lock and does the window + WRITE_MEMORY_START transfer with no lock held.
 * A commit that lands while a transfer is in flight overwrites the pending buffer with the
 * union of the pending rect and the new damage (the framebuffer always holds the whole current
 * screen, so the union read from the current fb is correct), i.e. frames coalesce instead of
 * queueing. Stats: /sys/bus/spi/devices/spi0.0/mhs35_stats.
 */
struct mhs35_dev {
	struct mipi_dbi_dev dbidev;
	struct workqueue_struct *wq;
	struct work_struct tx_work;
	struct mutex lock;		/* pend_*, bufs[] ownership */
	void *bufs[2];
	int pend;			/* index the commit writes; the worker transmits 1 - pend */
	bool pend_valid;
	struct drm_rect pend_rect;
	u64 queued, sent, coalesced, errors;
	s64 copy_last_us, copy_max_us, tx_last_us, tx_max_us;
};

static inline struct mhs35_dev *to_mhs35(struct mipi_dbi_dev *dbidev)
{
	return container_of(dbidev, struct mhs35_dev, dbidev);
}

/*
 * The PiScreen/waveshare rpi-lcd-35 has a SPI to 16-bit parallel bus converter
 * in front of the  display controller. This means that 8-bit values have to be
 * transferred as 16-bit.
 */
static int waveshare_command(struct mipi_dbi *mipi, u8 *cmd, u8 *par,
			     size_t num)
{
	struct spi_device *spi = mipi->spi;
	unsigned int bpw = 8;
	void *data = par;
	u32 speed_hz;
	int i, ret;
	__be16 *buf;

	buf = kmalloc(32 * sizeof(u16), GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	/*
	 * The displays are Raspberry Pi HATs and connected to the 8-bit only
	 * SPI controller, so 16-bit command and parameters need byte swapping
	 * before being transferred as 8-bit on the big endian SPI bus.
	 */
	buf[0] = cpu_to_be16(*cmd);
	spi_bus_lock(spi->controller);
	gpiod_set_value_cansleep(mipi->dc, 0);
	speed_hz = mipi_dbi_spi_cmd_max_speed(spi, 2);
	ret = mipi_dbi_spi_transfer(spi, speed_hz, 8, buf, 2);
	spi_bus_unlock(spi->controller);
	if (ret || !num)
		goto free;

	/* 8-bit configuration data, not 16-bit pixel data */
	if (num <= 32) {
		for (i = 0; i < num; i++)
			buf[i] = cpu_to_be16(par[i]);
		num *= 2;
		data = buf;
	}

	/*
	 * Check whether pixel data bytes needs to be swapped or not
	 */
	if (*cmd == MIPI_DCS_WRITE_MEMORY_START && !mipi->swap_bytes)
		bpw = 16;

	spi_bus_lock(spi->controller);
	gpiod_set_value_cansleep(mipi->dc, 1);
	speed_hz = mipi_dbi_spi_cmd_max_speed(spi, num);
	ret = mipi_dbi_spi_transfer(spi, speed_hz, bpw, data, num);
	spi_bus_unlock(spi->controller);
 free:
	kfree(buf);

	return ret;
}


/*
 * Out-of-tree override of the in-tree
 * ili9486 driver. X (modesetting, reverse PRIME) treats this panel as a GPU screen whose
 * own front buffer is 1024x768, and the in-tree driver only accepts framebuffers of exactly
 * hdisplay x vdisplay, so the sink CRTC could never be set. The pipe only ever scans out a
 * hdisplay x vdisplay window at (0,0) of the framebuffer, so larger framebuffers are safe,
 * provided the enable-time flush is clipped to the visible source rectangle (the core's
 * mipi_dbi_enable_flush flushes fb->width x fb->height into a tx_buf sized for the mode).
 */
static void mhs35_enable_flush(struct mipi_dbi_dev *dbidev,
			       struct drm_plane_state *plane_state)
{
	struct drm_shadow_plane_state *shadow_plane_state = to_drm_shadow_plane_state(plane_state);
	struct drm_framebuffer *fb = plane_state->fb;
	struct mipi_dbi *dbi = &dbidev->dbi;
	const struct drm_format_info *dst_format;
	unsigned int width, height, xs, xe, ys, ye;
	struct drm_rect rect;
	size_t len;
	int idx, ret;

	if (!fb)
		return;

	drm_rect_fp_to_int(&rect, &plane_state->src);
	width = drm_rect_width(&rect);
	height = drm_rect_height(&rect);
	if (!width || !height || width > dbidev->mode.hdisplay || height > dbidev->mode.vdisplay) {
		drm_err_once(&dbidev->drm, "refusing enable flush of %ux%u src rect\n", width, height);
		return;
	}

	if (!drm_dev_enter(&dbidev->drm, &idx))
		return;

	ret = mipi_dbi_buf_copy(dbidev->tx_buf, &shadow_plane_state->data[0], fb, &rect,
				dbi->swap_bytes, &shadow_plane_state->fmtcnv_state);
	if (ret)
		goto err_msg;

	xs = rect.x1 + dbidev->left_offset;
	xe = rect.x2 - 1 + dbidev->left_offset;
	ys = rect.y1 + dbidev->top_offset;
	ye = rect.y2 - 1 + dbidev->top_offset;
	mipi_dbi_command(dbi, MIPI_DCS_SET_COLUMN_ADDRESS, (xs >> 8) & 0xff,
			 xs & 0xff, (xe >> 8) & 0xff, xe & 0xff);
	mipi_dbi_command(dbi, MIPI_DCS_SET_PAGE_ADDRESS, (ys >> 8) & 0xff,
			 ys & 0xff, (ye >> 8) & 0xff, ye & 0xff);

	if (fb->format->format == DRM_FORMAT_XRGB8888)
		dst_format = drm_format_info(dbidev->pixel_format);
	else
		dst_format = fb->format;
	len = drm_format_info_min_pitch(dst_format, 0, width) * height;

	ret = mipi_dbi_command_buf(dbi, MIPI_DCS_WRITE_MEMORY_START, (u8 *)dbidev->tx_buf, len);
err_msg:
	if (ret)
		drm_err_once(&dbidev->drm, "Failed to update display %d\n", ret);
	backlight_enable(dbidev->backlight);
	drm_dev_exit(idx);
}

static void waveshare_enable(struct drm_simple_display_pipe *pipe,
			     struct drm_crtc_state *crtc_state,
			     struct drm_plane_state *plane_state)
{
	struct mipi_dbi_dev *dbidev = drm_to_mipi_dbi_dev(pipe->crtc.dev);
	struct mipi_dbi *dbi = &dbidev->dbi;
	u8 addr_mode;
	int ret, idx;

	if (!drm_dev_enter(pipe->crtc.dev, &idx))
		return;

	DRM_DEBUG_KMS("\n");

	ret = mipi_dbi_poweron_conditional_reset(dbidev);
	if (ret < 0)
		goto out_exit;
	if (ret == 1)
		goto out_enable;

	mipi_dbi_command(dbi, ILI9486_ITFCTR1);
	mipi_dbi_command(dbi, MIPI_DCS_EXIT_SLEEP_MODE);
	msleep(250);

	mipi_dbi_command(dbi, MIPI_DCS_SET_PIXEL_FORMAT, 0x55);

	mipi_dbi_command(dbi, ILI9486_PWCTRL1, 0x44);

	mipi_dbi_command(dbi, ILI9486_VMCTRL1, 0x00, 0x00, 0x00, 0x00);

	mipi_dbi_command(dbi, ILI9486_PGAMCTRL,
			 0x0F, 0x1F, 0x1C, 0x0C, 0x0F, 0x08, 0x48, 0x98,
			 0x37, 0x0A, 0x13, 0x04, 0x11, 0x0D, 0x0);
	mipi_dbi_command(dbi, ILI9486_NGAMCTRL,
			 0x0F, 0x32, 0x2E, 0x0B, 0x0D, 0x05, 0x47, 0x75,
			 0x37, 0x06, 0x10, 0x03, 0x24, 0x20, 0x00);
	mipi_dbi_command(dbi, ILI9486_DGAMCTRL,
			 0x0F, 0x32, 0x2E, 0x0B, 0x0D, 0x05, 0x47, 0x75,
			 0x37, 0x06, 0x10, 0x03, 0x24, 0x20, 0x00);

	mipi_dbi_command(dbi, MIPI_DCS_SET_DISPLAY_ON);
	msleep(100);

 out_enable:
	switch (dbidev->rotation) {
	case 90:
		addr_mode = ILI9486_MADCTL_MY;
		break;
	case 180:
		addr_mode = ILI9486_MADCTL_MV;
		break;
	case 270:
		addr_mode = ILI9486_MADCTL_MX;
		break;
	default:
		addr_mode = ILI9486_MADCTL_MV | ILI9486_MADCTL_MY |
			ILI9486_MADCTL_MX;
		break;
	}
	addr_mode |= ILI9486_MADCTL_BGR;
	mipi_dbi_command(dbi, MIPI_DCS_SET_ADDRESS_MODE, addr_mode);
	mhs35_enable_flush(dbidev, plane_state);
 out_exit:
	drm_dev_exit(idx);
}

static void mhs35_tx_work(struct work_struct *work)
{
	struct mhs35_dev *mdev = container_of(work, struct mhs35_dev, tx_work);
	struct mipi_dbi_dev *dbidev = &mdev->dbidev;
	struct mipi_dbi *dbi = &dbidev->dbi;
	int idx;

	if (!drm_dev_enter(&dbidev->drm, &idx))
		return;

	for (;;) {
		unsigned int xs, xe, ys, ye;
		struct drm_rect rect;
		ktime_t t0;
		void *buf;
		size_t len;
		s64 us;
		int ret;

		mutex_lock(&mdev->lock);
		if (!mdev->pend_valid) {
			mutex_unlock(&mdev->lock);
			break;
		}
		rect = mdev->pend_rect;
		buf = mdev->bufs[mdev->pend];
		mdev->pend ^= 1;
		mdev->pend_valid = false;
		mutex_unlock(&mdev->lock);

		/* the commit side now owns the other buffer; this one is ours until the transfer ends */
		len = (size_t)drm_rect_width(&rect) * drm_rect_height(&rect) * sizeof(u16);
		xs = rect.x1 + dbidev->left_offset;
		xe = rect.x2 - 1 + dbidev->left_offset;
		ys = rect.y1 + dbidev->top_offset;
		ye = rect.y2 - 1 + dbidev->top_offset;

		t0 = ktime_get();
		mipi_dbi_command(dbi, MIPI_DCS_SET_COLUMN_ADDRESS, (xs >> 8) & 0xff,
				 xs & 0xff, (xe >> 8) & 0xff, xe & 0xff);
		mipi_dbi_command(dbi, MIPI_DCS_SET_PAGE_ADDRESS, (ys >> 8) & 0xff,
				 ys & 0xff, (ye >> 8) & 0xff, ye & 0xff);
		ret = mipi_dbi_command_buf(dbi, MIPI_DCS_WRITE_MEMORY_START, buf, len);
		us = ktime_us_delta(ktime_get(), t0);

		mdev->tx_last_us = us;
		if (us > mdev->tx_max_us)
			mdev->tx_max_us = us;
		mdev->sent++;
		if (ret) {
			mdev->errors++;
			drm_err_once(&dbidev->drm, "async panel update failed %d\n", ret);
		}
	}

	drm_dev_exit(idx);
}

static void mhs35_pipe_update(struct drm_simple_display_pipe *pipe,
			      struct drm_plane_state *old_state)
{
	struct drm_plane_state *state = pipe->plane.state;
	struct drm_shadow_plane_state *shadow_plane_state = to_drm_shadow_plane_state(state);
	struct mipi_dbi_dev *dbidev = drm_to_mipi_dbi_dev(pipe->crtc.dev);
	struct mhs35_dev *mdev = to_mhs35(dbidev);
	struct drm_framebuffer *fb = state->fb;
	struct drm_rect rect;
	ktime_t t0;
	s64 us;
	int idx, ret;

	if (!pipe->crtc.state->active)
		return;
	if (WARN_ON(!fb))
		return;
	if (!drm_atomic_helper_damage_merged(old_state, state, &rect))
		return;
	if (!drm_dev_enter(fb->dev, &idx))
		return;

	t0 = ktime_get();
	mutex_lock(&mdev->lock);
	if (mdev->pend_valid) {
		/* worker busy with the other buffer: fold the untransmitted rect into this one */
		rect.x1 = min(rect.x1, mdev->pend_rect.x1);
		rect.y1 = min(rect.y1, mdev->pend_rect.y1);
		rect.x2 = max(rect.x2, mdev->pend_rect.x2);
		rect.y2 = max(rect.y2, mdev->pend_rect.y2);
		mdev->coalesced++;
	}
	if (drm_rect_width(&rect) > dbidev->mode.hdisplay ||
	    drm_rect_height(&rect) > dbidev->mode.vdisplay) {
		mutex_unlock(&mdev->lock);
		drm_err_once(&dbidev->drm, "refusing %dx%d damage larger than the panel\n",
			     drm_rect_width(&rect), drm_rect_height(&rect));
		goto out;
	}
	ret = mipi_dbi_buf_copy(mdev->bufs[mdev->pend], &shadow_plane_state->data[0], fb, &rect,
				dbidev->dbi.swap_bytes, &shadow_plane_state->fmtcnv_state);
	if (!ret) {
		mdev->pend_rect = rect;
		mdev->pend_valid = true;
		mdev->queued++;
	} else {
		mdev->errors++;
	}
	mutex_unlock(&mdev->lock);
	us = ktime_us_delta(ktime_get(), t0);
	mdev->copy_last_us = us;
	if (us > mdev->copy_max_us)
		mdev->copy_max_us = us;

	if (ret)
		drm_err_once(&dbidev->drm, "damage copy failed %d\n", ret);
	else
		queue_work(mdev->wq, &mdev->tx_work);
out:
	drm_dev_exit(idx);
}

static void mhs35_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	struct mhs35_dev *mdev = to_mhs35(drm_to_mipi_dbi_dev(pipe->crtc.dev));

	mutex_lock(&mdev->lock);
	mdev->pend_valid = false;
	mutex_unlock(&mdev->lock);
	cancel_work_sync(&mdev->tx_work);
	mipi_dbi_pipe_disable(pipe);
}

static ssize_t mhs35_stats_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct drm_device *drm = dev_get_drvdata(dev);
	struct mhs35_dev *mdev = to_mhs35(drm_to_mipi_dbi_dev(drm));

	return sysfs_emit(buf,
			  "queued %llu\nsent %llu\ncoalesced %llu\nerrors %llu\n"
			  "copy_last_us %lld\ncopy_max_us %lld\ntx_last_us %lld\ntx_max_us %lld\npending %d\n",
			  mdev->queued, mdev->sent, mdev->coalesced, mdev->errors,
			  mdev->copy_last_us, mdev->copy_max_us, mdev->tx_last_us, mdev->tx_max_us,
			  mdev->pend_valid);
}
static DEVICE_ATTR_RO(mhs35_stats);

static const struct drm_simple_display_pipe_funcs waveshare_pipe_funcs = {
	.mode_valid = mipi_dbi_pipe_mode_valid,
	.enable = waveshare_enable,
	.disable = mhs35_pipe_disable,
	.update = mhs35_pipe_update,
	.begin_fb_access = mipi_dbi_pipe_begin_fb_access,
	.end_fb_access = mipi_dbi_pipe_end_fb_access,
	.reset_plane = mipi_dbi_pipe_reset_plane,
	.duplicate_plane_state = mipi_dbi_pipe_duplicate_plane_state,
	.destroy_plane_state = mipi_dbi_pipe_destroy_plane_state,
};

static const struct drm_display_mode waveshare_mode = {
	DRM_SIMPLE_MODE(480, 320, 73, 49),
};

DEFINE_DRM_GEM_DMA_FOPS(ili9486_fops);


/*
 * Note: X's non-glamor reverse-PRIME sink path (AccelMethod none) imports the
 * master's pixmap and then mmaps it through THIS device with DRM_IOCTL_MODE_MAP_DUMB.
 * drm_gem_dumb_map_offset() refuses imported objects, so X logs
 * "randr: failed to create shared pixmap" and never copies a frame. The DMA GEM mmap path
 * maps imported buffers fine (dma_mmap_wc on the imported sg-table), so hand out the offset.
 */
static int mhs35_dumb_map_offset(struct drm_file *file, struct drm_device *dev,
				 u32 handle, u64 *offset)
{
	struct drm_gem_object *obj;
	int ret;

	obj = drm_gem_object_lookup(file, handle);
	if (!obj)
		return -ENOENT;

	ret = drm_gem_create_mmap_offset(obj);
	if (!ret)
		*offset = drm_vma_node_offset_addr(&obj->vma_node);

	drm_gem_object_put(obj);
	return ret;
}


/*
 * Note: vc4 buffers on the Pi 5 are scattered (shmem behind the HVS IOMMU), so the
 * DMA GEM import (contiguous CMA only) refuses them with -EINVAL. In X's non-glamor sink path
 * the imported buffer is only ever CPU-read (X copies it into this device's own dumb buffer,
 * then DIRTYFB), so an import that vmaps the scattered pages and an mmap that defers to the
 * exporter are enough. Native dumb buffers keep the stock DMA GEM behaviour.
 */
static int mhs35_gem_object_mmap(struct drm_gem_object *obj, struct vm_area_struct *vma)
{
	int ret;

	if (!obj->import_attach)
		return drm_gem_dma_object_mmap(obj, vma);

	/* same dance as drm_gem_shmem_mmap() for imported buffers */
	vma->vm_private_data = NULL;
	vma->vm_ops = NULL;
	ret = dma_buf_mmap(obj->dma_buf, vma, 0);
	if (!ret)
		drm_gem_object_put(obj);	/* drop the ref drm_gem_mmap_obj() took */
	return ret;
}

static const struct drm_gem_object_funcs mhs35_gem_funcs = {
	.free = drm_gem_dma_object_free,
	.print_info = drm_gem_dma_object_print_info,
	.get_sg_table = drm_gem_dma_object_get_sg_table,
	.vmap = drm_gem_dma_object_vmap,
	.mmap = mhs35_gem_object_mmap,
	.vm_ops = &drm_gem_dma_vm_ops,
};

static struct drm_gem_object *mhs35_gem_create_object(struct drm_device *dev, size_t size)
{
	struct drm_gem_dma_object *dma_obj;

	dma_obj = kzalloc(sizeof(*dma_obj), GFP_KERNEL);
	if (!dma_obj)
		return ERR_PTR(-ENOMEM);
	dma_obj->base.funcs = &mhs35_gem_funcs;
	return &dma_obj->base;
}

static struct drm_gem_object *
mhs35_gem_prime_import_sg_table(struct drm_device *dev, struct dma_buf_attachment *attach,
				struct sg_table *sgt)
{
	struct drm_gem_dma_object *dma_obj;
	struct drm_gem_object *obj;
	struct iosys_map map;
	int ret;

	ret = dma_buf_vmap_unlocked(attach->dmabuf, &map);
	if (ret) {
		dev_err(dev->dev, "prime: vmap of imported buffer failed %d\n", ret);
		return ERR_PTR(ret);
	}

	obj = mhs35_gem_create_object(dev, attach->dmabuf->size);
	if (IS_ERR(obj)) {
		dma_buf_vunmap_unlocked(attach->dmabuf, &map);
		return obj;
	}
	drm_gem_private_object_init(dev, obj, attach->dmabuf->size);

	dma_obj = to_drm_gem_dma_obj(obj);
	dma_obj->vaddr = map.vaddr;
	dma_obj->dma_addr = sgt->nents ? sg_dma_address(sgt->sgl) : 0;
	dma_obj->sgt = sgt;
	return obj;
}

/*
 * Note: instrumented PRIME import. X's reverse-PRIME sink path imports the vc4
 * master's dma-buf into this device; the stock import returns a silent -EINVAL. This mirrors
 * drm_gem_prime_import_dev() with a message at each step, and retries the attach through the
 * SPI controller device (which has DMA ops) when the SPI slave device fails.
 */
static struct drm_gem_object *mhs35_prime_import_via(struct drm_device *dev,
						     struct dma_buf *dma_buf,
						     struct device *attach_dev,
						     const char *tag)
{
	struct dma_buf_attachment *attach;
	struct sg_table *sgt;
	struct drm_gem_object *obj;
	int ret;

	attach = dma_buf_attach(dma_buf, attach_dev);
	if (IS_ERR(attach)) {
		dev_err(dev->dev, "prime[%s]: attach failed %ld\n", tag, PTR_ERR(attach));
		return ERR_CAST(attach);
	}

	get_dma_buf(dma_buf);

	sgt = dma_buf_map_attachment_unlocked(attach, DMA_BIDIRECTIONAL);
	if (IS_ERR(sgt)) {
		ret = PTR_ERR(sgt);
		dev_err(dev->dev, "prime[%s]: map_attachment failed %d (dma_mask %llx)\n", tag, ret,
			attach_dev->dma_mask ? *attach_dev->dma_mask : 0ULL);
		goto fail_detach;
	}

	dev_info(dev->dev, "prime[%s]: sgt nents=%u orig_nents=%u contiguous=%zu dmabuf size=%zu\n",
		 tag, sgt->nents, sgt->orig_nents, drm_prime_get_contiguous_size(sgt), dma_buf->size);

	obj = dev->driver->gem_prime_import_sg_table(dev, attach, sgt);
	if (IS_ERR(obj)) {
		ret = PTR_ERR(obj);
		dev_err(dev->dev, "prime[%s]: import_sg_table failed %d\n", tag, ret);
		goto fail_unmap;
	}

	obj->import_attach = attach;
	obj->resv = dma_buf->resv;
	dev_info(dev->dev, "prime[%s]: imported OK\n", tag);
	return obj;

fail_unmap:
	dma_buf_unmap_attachment_unlocked(attach, sgt, DMA_BIDIRECTIONAL);
fail_detach:
	dma_buf_detach(dma_buf, attach);
	dma_buf_put(dma_buf);
	return ERR_PTR(ret);
}

static struct drm_gem_object *mhs35_gem_prime_import(struct drm_device *dev,
						     struct dma_buf *dma_buf)
{
	struct drm_gem_object *obj;

	/* no self-import shortcut: this device never re-imports its own buffers in the sink role */
	return mhs35_prime_import_via(dev, dma_buf, dev->dev, "spi-slave");
}

static const struct drm_driver ili9486_driver = {
	.driver_features	= DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops			= &ili9486_fops,
	DRM_GEM_DMA_DRIVER_OPS_VMAP,
	.dumb_map_offset	= mhs35_dumb_map_offset,
	.gem_create_object	= mhs35_gem_create_object,
	.gem_prime_import_sg_table = mhs35_gem_prime_import_sg_table,
	.gem_prime_import	= mhs35_gem_prime_import,
	.debugfs_init		= mipi_dbi_debugfs_init,
	.name			= "ili9486",
	.desc			= "Ilitek ILI9486",
	.date			= "20200118",
	.major			= 1,
	.minor			= 0,
};

static const struct of_device_id ili9486_of_match[] = {
	{ .compatible = "waveshare,rpi-lcd-35" },
	{ .compatible = "ozzmaker,piscreen" },
	{},
};
MODULE_DEVICE_TABLE(of, ili9486_of_match);

static const struct spi_device_id ili9486_id[] = {
	{ "rpi-lcd-35", 0 },
	{ "piscreen", 0 },
	{ }
};
MODULE_DEVICE_TABLE(spi, ili9486_id);

static int ili9486_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct mipi_dbi_dev *dbidev;
	struct mhs35_dev *mdev;
	struct drm_device *drm;
	struct mipi_dbi *dbi;
	struct gpio_desc *dc;
	u32 rotation = 0;
	size_t bufsize;
	int ret, i;

	mdev = devm_drm_dev_alloc(dev, &ili9486_driver,
				  struct mhs35_dev, dbidev.drm);
	if (IS_ERR(mdev))
		return PTR_ERR(mdev);
	dbidev = &mdev->dbidev;
	mutex_init(&mdev->lock);
	INIT_WORK(&mdev->tx_work, mhs35_tx_work);

	dbi = &dbidev->dbi;
	drm = &dbidev->drm;

	dbi->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(dbi->reset))
		return dev_err_probe(dev, PTR_ERR(dbi->reset), "Failed to get GPIO 'reset'\n");

	dc = devm_gpiod_get(dev, "dc", GPIOD_OUT_LOW);
	if (IS_ERR(dc))
		return dev_err_probe(dev, PTR_ERR(dc), "Failed to get GPIO 'dc'\n");

	dbidev->backlight = devm_of_find_backlight(dev);
	if (IS_ERR(dbidev->backlight))
		return PTR_ERR(dbidev->backlight);

	device_property_read_u32(dev, "rotation", &rotation);

	ret = mipi_dbi_spi_init(spi, dbi, dc);
	if (ret)
		return ret;

	/*
	 * Note: X reverse PRIME imports the vc4 master's dma-buf into this device.
	 * dma_buf_map_attachment() maps the exporter's sg-table for the IMPORTING device, and an
	 * SPI slave has no dma_mask, so the DMA core refuses (PRIME_FD_TO_HANDLE = EINVAL). The
	 * mapping is only used for vmap/mmap bookkeeping here (pixels go out through the SPI
	 * controller's own DMA from tx_buf), so a plain direct mask is enough.
	 */
	ret = dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(64));
	if (ret)
		dev_warn(dev, "could not set DMA mask (%d); PRIME import will fail\n", ret);

	dbi->command = waveshare_command;
	dbi->read_commands = NULL;

	ret = mipi_dbi_dev_init(dbidev, &waveshare_pipe_funcs,
				&waveshare_mode, rotation);
	if (ret)
		return ret;

	/* Note: accept framebuffers larger than the panel (see mhs35_enable_flush). */
	drm->mode_config.max_width = 4096;
	drm->mode_config.max_height = 4096;

	drm_mode_config_reset(drm);

	/* Note: two panel-sized RGB565 buffers + ordered worker for the async dirty path. */
	bufsize = (size_t)dbidev->mode.hdisplay * dbidev->mode.vdisplay * sizeof(u16);
	for (i = 0; i < 2; i++) {
		mdev->bufs[i] = devm_kmalloc(dev, bufsize, GFP_KERNEL);
		if (!mdev->bufs[i])
			return -ENOMEM;
	}
	mdev->wq = alloc_ordered_workqueue("mhs35-tx", WQ_HIGHPRI);
	if (!mdev->wq)
		return -ENOMEM;

	ret = drm_dev_register(drm, 0);
	if (ret) {
		destroy_workqueue(mdev->wq);
		return ret;
	}

	spi_set_drvdata(spi, drm);
	if (device_create_file(dev, &dev_attr_mhs35_stats))
		dev_warn(dev, "no mhs35_stats sysfs file\n");

	drm_fbdev_dma_setup(drm, 0);

	return 0;
}

static void ili9486_remove(struct spi_device *spi)
{
	struct drm_device *drm = spi_get_drvdata(spi);
	struct mhs35_dev *mdev = to_mhs35(drm_to_mipi_dbi_dev(drm));

	device_remove_file(&spi->dev, &dev_attr_mhs35_stats);
	drm_dev_unplug(drm);
	drm_atomic_helper_shutdown(drm);
	cancel_work_sync(&mdev->tx_work);
	destroy_workqueue(mdev->wq);
}

static void ili9486_shutdown(struct spi_device *spi)
{
	drm_atomic_helper_shutdown(spi_get_drvdata(spi));
}

static struct spi_driver ili9486_spi_driver = {
	.driver = {
		.name = "ili9486",
		.of_match_table = ili9486_of_match,
	},
	.id_table = ili9486_id,
	.probe = ili9486_probe,
	.remove = ili9486_remove,
	.shutdown = ili9486_shutdown,
};
module_spi_driver(ili9486_spi_driver);

MODULE_DESCRIPTION("Ilitek ILI9486 DRM driver (override: large fb, clipped enable flush, async dirty path)");
MODULE_AUTHOR("Kamlesh Gurudasani <kamlesh.gurudasani@gmail.com>");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(DMA_BUF);
