// SPDX-License-Identifier: GPL-2.0-only
/* One hardware engine, one owner. The camera buffer stays pinned through the
 * final AXI read. Only compressed bytes cross into the sending application. */
#include <linux/compat.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/sizes.h>
#include <linux/uaccess.h>
#include "ironh264_ioctl.h"
#include "rtlnet_h264_enc.h"

#define RING_BYTES SZ_8M
#define INDEX_ENTRIES 32
#define Y_BYTES (120 * 68 * 256)
#define UV_BYTES (Y_BYTES / 2)
#define AUD_BYTES 6
#define IRQ_MASK (RTLNET_H264_IRQ_PIC_DONE | RTLNET_H264_IRQ_ERR | RTLNET_H264_IRQ_RING_STALL)

struct encoder {
	struct device *dev;
	void __iomem *regs;
	struct miscdevice misc;
	struct mutex lock;
	struct completion done;
	wait_queue_head_t closed;
	atomic_t opened;
	void *ring, *index;
	dma_addr_t ring_dma, index_dma, refs_dma;
	struct dma_buf_attachment *input;
	struct sg_table *mapping;
	rtlnet_h264_enc_buffers_t refs;
	rtlnet_h264_enc_sps_cfg_t sps;
	unsigned int width, sequence, frame_num, irq_events;
	bool high;
};

static irqreturn_t encoder_irq(int irq, void *data)
{
	struct encoder *e = data;
	uintptr_t io = (uintptr_t)e->regs;
	u32 events = rtlnet_h264_enc_irq_status(io) & IRQ_MASK;

	if (!events)
		return IRQ_NONE;
	rtlnet_h264_enc_irq_ack(io, events);
	WRITE_ONCE(e->irq_events, events);
	complete(&e->done);
	return IRQ_HANDLED;
}

static void release_input(struct encoder *e)
{
	struct dma_buf *buf;

	if (!e->input)
		return;
	buf = e->input->dmabuf;
	if (e->mapping)
		dma_buf_unmap_attachment_unlocked(e->input, e->mapping, DMA_TO_DEVICE);
	dma_buf_detach(buf, e->input);
	dma_buf_put(buf);
	e->input = NULL;
	e->mapping = NULL;
}

static int map_input(struct encoder *e, const struct ironh264_frame *f, dma_addr_t *addr)
{
	size_t required = f->stride * ALIGN(f->height, 16);
	struct dma_buf *buf = dma_buf_get(f->dmabuf_fd);
	struct dma_buf_attachment *input;
	struct sg_table *mapping;

	if (IS_ERR(buf))
		return PTR_ERR(buf);
	if (buf->size < required) {
		dma_buf_put(buf);
		return -EINVAL;
	}
	input = dma_buf_attach(buf, e->dev);
	if (IS_ERR(input)) {
		dma_buf_put(buf);
		return PTR_ERR(input);
	}
	e->input = input;
	mapping = dma_buf_map_attachment_unlocked(input, DMA_TO_DEVICE);
	if (IS_ERR(mapping))
		return PTR_ERR(mapping);
	e->mapping = mapping;
	*addr = sg_dma_address(mapping->sgl);
	if (mapping->nents != 1 || sg_dma_len(mapping->sgl) < required ||
	    !IS_ALIGNED(*addr, RTLNET_H264_IN_ALIGN))
		return -EINVAL;
	return 0;
}

static int encode(struct encoder *e, struct ironh264_frame *f)
{
	uintptr_t io = (uintptr_t)e->regs;
	bool hd = f->width == 1920 && f->height == 1080;
	unsigned int mode = hd ? RTLNET_H264_MODE_1080P30 : RTLNET_H264_MODE_720P30;
	rtlnet_h264_enc_cfg_t cfg = {
		.mb_w = f->width / 16, .mb_h = DIV_ROUND_UP(f->height, 16),
		.slices = 4, .qp = 28, .log2_max_frame_num = 8,
		.in_stride = f->stride, .ry_base = e->refs_dma, .ru_base = e->refs_dma + Y_BYTES,
		.ring_base = e->ring_dma, .ring_size = RING_BYTES,
		.idx_base = e->index_dma, .idx_entries = INDEX_ENTRIES,
	};
	rtlnet_h264_enc_index_entry_t entry;
	dma_addr_t dma;
	u8 header[RTLNET_H264_SPS_PPS_MAX_BYTES];
	u8 __user *dest = u64_to_user_ptr(f->output);
	size_t header_bytes = 0;
	u32 next, type, status;
	int ret;

	if ((!hd && (f->width != 1280 || f->height != 720)) ||
	    f->stride != f->width * 2 || !f->capacity || f->reserved)
		return -EINVAL;
	/* A timed out job may still own its source. Never unmap an active DMA. */
	if (rtlnet_h264_enc_status(io) & RTLNET_H264_STATUS_BUSY)
		return -EBUSY;
	release_input(e);
	ret = map_input(e, f, &dma);
	if (ret)
		goto out;
	cfg.in_base = dma;
	if (e->width != f->width) {
		e->sequence = 0;
		e->width = f->width;
	}
	f->keyframe = (e->sequence % 30 == 0);
	type = f->keyframe ? RTLNET_H264_PIC_IDR : RTLNET_H264_PIC_P;
	if (!e->sequence) {
		rtlnet_h264_enc_buffers_init(&e->refs, cfg.ry_base, cfg.ru_base,
			cfg.ry_base + Y_BYTES + UV_BYTES, cfg.ru_base + Y_BYTES + UV_BYTES);
		if (rtlnet_h264_enc_set_profile(io, e->high)) {
			ret = -ENOTSUPP;
			goto out;
		}
	}
	if (rtlnet_h264_enc_configure(io, &cfg) || rtlnet_h264_enc_set_mode(io, mode, &e->sps) ||
	    rtlnet_h264_enc_buffers_begin(io, &e->refs, type)) {
		ret = -EINVAL;
		goto out;
	}
	e->sps.log2_max_frame_num = cfg.log2_max_frame_num;
	/* Capture timestamps carry presentation timing in the transport stream. */
	if (f->keyframe && rtlnet_h264_enc_write_sps_pps_profile(header, sizeof(header),
							&e->sps, e->high, &header_bytes)) {
		ret = -EINVAL;
		goto out;
	}
	rtlnet_h264_enc_ring_reset(io);
	rtlnet_h264_enc_clear_status(io);
	rtlnet_h264_enc_irq_ack(io, IRQ_MASK);
	reinit_completion(&e->done);
	WRITE_ONCE(e->irq_events, 0);
	e->frame_num = f->keyframe ? 0 : rtlnet_h264_enc_next_frame_num(e->frame_num, cfg.log2_max_frame_num);
	dma_wmb();
	if (rtlnet_h264_enc_start_picture_ex(io, e->frame_num, (e->sequence / 30) & 65535, type)) {
		ret = -EIO;
		goto out;
	}
	wait_for_completion_timeout(&e->done, msecs_to_jiffies(2000));
	status = rtlnet_h264_enc_status(io);
	if ((status & (RTLNET_H264_STATUS_DONE | RTLNET_H264_STATUS_BUSY | RTLNET_H264_STATUS_ERR))
	    != RTLNET_H264_STATUS_DONE || READ_ONCE(e->irq_events) & RTLNET_H264_IRQ_RING_STALL) {
		ret = status & RTLNET_H264_STATUS_BUSY ? -ETIMEDOUT : -EIO;
		goto out;
	}
	dma_rmb();
	next = rtlnet_h264_enc_index_wptr(io);
	if (!next || next >= INDEX_ENTRIES) {
		ret = -EIO;
		goto out;
	}
	rtlnet_h264_enc_decode_index_entry(e->index + (next - 1) * RTLNET_H264_IDX_ENTRY_BYTES, &entry);
	if (!entry.is_picture || entry.idr != f->keyframe || entry.offset ||
	    entry.length < AUD_BYTES || entry.length > RING_BYTES) {
		ret = -EIO;
		goto out;
	}
	f->bytes = entry.length + header_bytes;
	if (f->bytes > f->capacity) {
		ret = -ENOSPC;
		goto out;
	}
	if (copy_to_user(dest, e->ring, AUD_BYTES) ||
	    copy_to_user(dest + AUD_BYTES, header, header_bytes) ||
	    copy_to_user(dest + AUD_BYTES + header_bytes, e->ring + AUD_BYTES, entry.length - AUD_BYTES)) {
		ret = -EFAULT;
		goto out;
	}
	rtlnet_h264_enc_set_read_pointers(io, readl(e->regs + RTLNET_H264_REG_RING_WPTR), next);
	rtlnet_h264_enc_buffers_end(&e->refs);
	e->sequence++;
	ret = 0;
out:
	if (ret)
		e->sequence = 0;
	if (!(rtlnet_h264_enc_status(io) & RTLNET_H264_STATUS_BUSY))
		release_input(e);
	return ret;
}

static long encoder_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct encoder *e = file->private_data;
	struct ironh264_frame frame;
	int ret;

	if (cmd != IRONH264_ENCODE)
		return -ENOTTY;
	if (copy_from_user(&frame, (void __user *)arg, sizeof(frame)))
		return -EFAULT;
	if (mutex_lock_interruptible(&e->lock))
		return -ERESTARTSYS;
	ret = encode(e, &frame);
	if (!ret && copy_to_user((void __user *)arg, &frame, sizeof(frame))) {
		e->sequence = 0;
		ret = -EFAULT;
	}
	mutex_unlock(&e->lock);
	return ret;
}

static int encoder_open(struct inode *inode, struct file *file)
{
	struct encoder *e = container_of(file->private_data, struct encoder, misc);

	if (atomic_cmpxchg(&e->opened, 0, 1))
		return -EBUSY;
	file->private_data = e;
	e->sequence = 0;
	return 0;
}

static int encoder_release(struct inode *inode, struct file *file)
{
	struct encoder *e = file->private_data;

	atomic_set(&e->opened, 0);
	wake_up(&e->closed);
	return 0;
}

static const struct file_operations encoder_fops = {
	.owner = THIS_MODULE, .open = encoder_open, .release = encoder_release,
	.unlocked_ioctl = encoder_ioctl, .compat_ioctl = compat_ptr_ioctl,
};

static int encoder_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct encoder *e = devm_kzalloc(dev, sizeof(*e), GFP_KERNEL);
	rtlnet_h264_enc_caps_t caps;
	int irq, ret;

	if (!e)
		return -ENOMEM;
	e->dev = dev;
	e->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(e->regs))
		return PTR_ERR(e->regs);
	if (rtlnet_h264_enc_probe((uintptr_t)e->regs, &caps) || caps.num_engines != 1 || !caps.irq)
		return -ENODEV;
	e->high = device_property_read_bool(dev, "ironkernel,high-profile");
	if (e->high != !!caps.hp_seam)
		return -ENODEV;
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	e->ring = dmam_alloc_coherent(dev, RING_BYTES, &e->ring_dma, GFP_KERNEL);
	e->index = dmam_alloc_coherent(dev, INDEX_ENTRIES * RTLNET_H264_IDX_ENTRY_BYTES, &e->index_dma, GFP_KERNEL);
	if (!e->ring || !e->index || !dmam_alloc_coherent(dev, 2 * (Y_BYTES + UV_BYTES), &e->refs_dma, GFP_KERNEL))
		return -ENOMEM;
	init_completion(&e->done);
	init_waitqueue_head(&e->closed);
	mutex_init(&e->lock);
	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;
	ret = devm_request_irq(dev, irq, encoder_irq, 0, dev_name(dev), e);
	if (ret)
		return ret;
	rtlnet_h264_enc_irq_ack((uintptr_t)e->regs, IRQ_MASK);
	if (rtlnet_h264_enc_irq_set_enable((uintptr_t)e->regs, IRQ_MASK) ||
	    rtlnet_h264_enc_irq_global((uintptr_t)e->regs, 1))
		return -ENODEV;
	e->misc = (struct miscdevice){ .minor = MISC_DYNAMIC_MINOR, .name = "ironh264",
		.fops = &encoder_fops, .parent = dev, .mode = 0600 };
	platform_set_drvdata(pdev, e);
	return misc_register(&e->misc);
}

static void encoder_remove(struct platform_device *pdev)
{
	struct encoder *e = platform_get_drvdata(pdev);
	u32 status;

	misc_deregister(&e->misc);
	wait_event(e->closed, !atomic_read(&e->opened));
	rtlnet_h264_enc_irq_global((uintptr_t)e->regs, 0);
	/* The static platform owns these buffers until the master retires all DMA. */
	readl_poll_timeout(e->regs + RTLNET_H264_REG_STATUS, status,
			   !(status & RTLNET_H264_STATUS_BUSY), 1000, 0);
	release_input(e);
}

static const struct of_device_id encoder_ids[] = {
	{ .compatible = "ironkernel,ironh264-1.0" }, {}
};
MODULE_DEVICE_TABLE(of, encoder_ids);
static struct platform_driver encoder_driver = {
	.probe = encoder_probe, .remove = encoder_remove,
	.driver = { .name = "ironh264", .of_match_table = encoder_ids, .suppress_bind_attrs = true },
};
module_platform_driver(encoder_driver);
MODULE_IMPORT_NS("DMA_BUF");
MODULE_LICENSE("GPL");
