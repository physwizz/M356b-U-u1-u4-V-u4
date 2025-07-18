/*
 * Samsung Exynos SoC series VPU driver
 *
 * Copyright (c) 2015 Samsung Electronics Co., Ltd
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/version.h>
#include <linux/types.h>
#include <linux/time.h>
#include <linux/slab.h>
#include <media/videobuf2-dma-sg.h>
#include <linux/dma-buf.h>

#include "vision-config.h"
#include "vision-buffer.h"

#include "npu-vertex.h"
#include "npu-memory.h"

#include <asm/cacheflush.h>

#define DEBUG_SENTENCE_MAX	300
#define TERM_SIZE		50
#define VISION_QBUF		0xFA36
#define VISION_DQBUF		0x36FA

static dma_addr_t vision_dma_buf_dva_map(struct vb_buffer *buffer,
				__attribute__((unused))u32 size)
{
	return sg_dma_address(buffer->sgt->sgl);
}

static void vision_dma_buf_dva_unmap(
		__attribute__((unused))struct vb_buffer *buffer)
{
	return;
}

static void vision_dma_buf_add_attr(struct dma_buf_attachment *attachment)
{
	attachment->dma_map_attrs |= DMA_ATTR_SKIP_CPU_SYNC;
}

static void vision_dma_buf_sync(struct vb_buffer *buffers,
				__attribute__((unused))u32 size,
				u32 direction, u32 action)
{
	unsigned long flags;

	// With sync fence (temperary solution or POC)
	// In KPI mode, we adapted 'direct path'
	// which is message-getting process of execution in 'interrupt context'
	// If we allow IRQ in dma_buf_sync, it can cause
	// abnormal scheduling in interrupt context
	// (FW response of 'sync' ioctl  vs  qbuf of next execution)
	if (is_kpi_mode_enabled(true))
		local_irq_save(flags);

	if (action == VISION_QBUF)
		dma_buf_end_cpu_access(buffers->dma_buf, direction);
	else // action == VISION_DQBUF
		dma_buf_begin_cpu_access(buffers->dma_buf, direction);

	if (is_kpi_mode_enabled(true))
		local_irq_restore(flags);
}

struct vision_debug_log {
	size_t			dsentence_pos;
	char			*dsentence/*[DEBUG_SENTENCE_MAX]*/;
};

#ifndef CONFIG_NPU_KUNIT_TEST
void vision_dmsg_concate(struct vision_debug_log *log, const char *fmt, ...)
{
	va_list ap;
	char *term/*[50]*/;
	size_t copy_len;

	term = kmalloc(TERM_SIZE, GFP_KERNEL);
	va_start(ap, fmt);
	vsnprintf(term, TERM_SIZE/*sizeof(term)*/, fmt, ap);
	va_end(ap);

	if (log->dsentence_pos >= DEBUG_SENTENCE_MAX) {
		vision_err("debug message(%zd) over max\n", log->dsentence_pos);
		kfree(term);
		return;
	}

	copy_len = min((DEBUG_SENTENCE_MAX-log->dsentence_pos-1), strlen(term));
	strncpy(log->dsentence + log->dsentence_pos, term, copy_len);
	log->dsentence_pos += copy_len;
	log->dsentence[log->dsentence_pos] = 0;
	kfree(term);
}
#endif

char *vision_dmsg_print(struct vision_debug_log *log)
{
	log->dsentence_pos = 0;
	return log->dsentence;
}

static struct vb_fmt vb_fmts[] = {
	{
		.name		= "RGB",
		.colorspace	= VS4L_DF_IMAGE_RGB,
		.planes	= 1,
		.bitsperpixel	= { 24 }
	}, {
		.name		= "ARGB",
		.colorspace	= VS4L_DF_IMAGE_RGBX,
		.planes	= 1,
		.bitsperpixel	= { 32 }
	}, {
		.name		= "YUV 4:2:0 planar, Y/CbCr",
		.colorspace	= VS4L_DF_IMAGE_NV12,
		.planes	= 2,
		.bitsperpixel	= { 8, 8 }
	}, {
		.name		= "YUV 4:2:0 planar, Y/CrCb",
		.colorspace	= VS4L_DF_IMAGE_NV21,
		.planes	= 2,
		.bitsperpixel	= { 8, 8 }
	}, {
		.name		= "YUV 4:2:2 packed, YCbYCr",
		.colorspace	= VS4L_DF_IMAGE_YUYV,
		.planes	= 1,
		.bitsperpixel	= { 16 }
	}, {
		.name		= "YUV 4:4:4 packed, YCbCr",
		.colorspace	= VS4L_DF_IMAGE_YUV4,
		.planes	= 1,
		.bitsperpixel	= { 24 }
	}, {
		.name		= "VX unsigned 8 bit",
		.colorspace	= VS4L_DF_IMAGE_U8,
		.planes	= 1,
		.bitsperpixel	= { 8 }
	}, {
		.name		= "VX unsigned 16 bit",
		.colorspace	= VS4L_DF_IMAGE_U16,
		.planes	= 1,
		.bitsperpixel	= { 16 }
	}, {
		.name		= "VX unsigned 32 bit",
		.colorspace	= VS4L_DF_IMAGE_U32,
		.planes	= 1,
		.bitsperpixel	= { 32 }
	}, {
		.name		= "VX signed 16 bit",
		.colorspace	= VS4L_DF_IMAGE_S16,
		.planes	= 1,
		.bitsperpixel	= { 16 }
	}, {
		.name		= "VX signed 32 bit",
		.colorspace	= VS4L_DF_IMAGE_S32,
		.planes	= 1,
		.bitsperpixel	= { 32 }
	}, {
		.name		= "NPU FORMAT",
		.colorspace	= VS4L_DF_IMAGE_NPU,
		.planes	= 1,
		.bitsperpixel	= { 8 }
	}, {
		.name		= "DSP FORMAT",
		.colorspace	= VS4L_DF_IMAGE_DSP,
		.planes	= 1,
		.bitsperpixel	= { 8 }
	}
};

void __vb_buffer_print(struct vb_container_list *c)
{
	if (!c || !c->containers)
		return;

	vision_dump("bundle clist id = %d\n", c->id);
	vision_dump("bundle clist index = %d\n", c->index);
	vision_dump("bundle clist direction = %d\n", c->direction);
	vision_dump("bundle clist buffer fd = %d\n",
		    c->containers->buffers->m.fd);
	vision_dump("bundle clist buffer fd mapping size = %d\n",
		    c->containers->buffers->dma_buf->size);
	vision_dump("bundle clist buffer fd expectation size = %d\n",
		    c->containers->buffers->size);
	vision_dump("bundle clist buffer daddr = 0x%llx\n",
		    c->containers->buffers->daddr);
}

#ifndef CONFIG_NPU_KUNIT_TEST
void __vb_queue_print(struct vb_queue *q)
{
	struct vb_bundle *bundle, *temp;

	if (!q)
		return;

	vision_dump("----------queued(%d)----------\n", atomic_read(&q->queued_count));
	list_for_each_entry_safe(bundle, temp, &q->queued_list, queued_entry) {
		__vb_buffer_print(&bundle->clist);
	}
	vision_dump("------------------------------\n");
}
#endif

static struct vb_fmt *__vb_find_format(u32 colorspace)
{
	size_t i;
	struct vb_fmt *fmt = NULL;

	for (i = 0; i < ARRAY_SIZE(vb_fmts); ++i) {
		if (vb_fmts[i].colorspace == colorspace) {
			fmt = &vb_fmts[i];
			break;
		}
	}
	return fmt;
}

static int __vb_plane_size(struct vb_format *format)
{
	int ret = 0;
	u32 plane;
	struct vb_fmt *fmt;

	BUG_ON(!format);
	fmt = format->fmt;
	if (fmt->planes > VB_MAX_PLANES) {
		vision_err("planes(%d) is invalid\n", fmt->planes);
		ret = -EINVAL;
		goto p_err;
	}

	for (plane = 0; plane < fmt->planes; ++plane) {
		if (fmt->colorspace == VS4L_DF_IMAGE_NPU ||
			fmt->colorspace == VS4L_DF_IMAGE_DSP) {
			format->size[plane] = (format->pixel_format / 8) *
			format->channels * format->width * format->height;

			vision_info("plane[%u] bpp : %u, ch : %u, w : %u, h : %u, size : %u\n",
				plane, format->pixel_format, format->channels,
				format->width, format->height, format->size[plane]);
		} else {
			format->size[plane] = (fmt->bitsperpixel[plane] / 8) *
			format->width * format->height;

			vision_info("plane[%u] bpp : %u, ch : %u, w : %u, h : %u, size : %u\n",
				plane, fmt->bitsperpixel[plane], format->channels,
				format->width, format->height, format->size[plane]);
		}
	}
p_err:
	return ret;
}

static int __vb_unmap_dmabuf(struct vb_queue *q, struct vb_buffer *buffer)
{
	int ret = 0;

	if (buffer == NULL) {
		vision_err("vb_buffer(buffer) is NULL\n");
		ret = -EFAULT;
		goto p_err;
	}

	if (buffer->vaddr)
		npu_dma_buf_vunmap(buffer->dma_buf, buffer->vaddr);
	if (buffer->daddr)
		vision_dma_buf_dva_unmap(buffer);
	if (buffer->sgt)
		dma_buf_unmap_attachment(
			buffer->attachment, buffer->sgt, DMA_BIDIRECTIONAL);
	if (buffer->attachment)
		dma_buf_detach(buffer->dma_buf, buffer->attachment);
	if (buffer->dma_buf)
		dma_buf_put(buffer->dma_buf);

	buffer->attachment = NULL;
	buffer->dma_buf = NULL;
	buffer->sgt = NULL;
	buffer->daddr = 0;
	buffer->vaddr = NULL;
	buffer->size = 0;

p_err:
	return ret;
}

static int __vb_map_dmabuf(
	struct vb_queue *q, struct vb_buffer *buffer, u32 size)
{
	int ret = 0;
	bool complete_suc = false;

	struct dma_buf *dma_buf;
	struct dma_buf_attachment *attachment;
	struct sg_table *sgt;
	dma_addr_t daddr;
	void *vaddr;
	u32 direction;

	buffer->dma_buf = NULL;
	buffer->attachment = NULL;
	buffer->sgt = NULL;
	buffer->daddr = 0;
	buffer->vaddr = NULL;

	dma_buf = dma_buf_get(buffer->m.fd);
	if (IS_ERR_OR_NULL(dma_buf)) {
		vision_err("dma_buf_get is fail(%p)\n", dma_buf);
		ret = -EINVAL;
		goto p_err;
	}
	buffer->dma_buf = dma_buf;

	if (buffer->dma_buf->size < size) {
		vision_err("Allocate buffer size(%zu) is smaller than expectation(%u)\n",
			buffer->dma_buf->size, size);
		ret = -EINVAL;
		goto p_err;
	}
	buffer->size = size;

	attachment = dma_buf_attach(buffer->dma_buf, q->alloc_ctx);
	if (IS_ERR(attachment)) {
		ret = PTR_ERR(attachment);
		goto p_err;
	}
	buffer->attachment = attachment;

	vision_dma_buf_add_attr(attachment);

	if (q->direction == VS4L_DIRECTION_OT)
		direction = DMA_FROM_DEVICE;
	else
		direction = DMA_TO_DEVICE;

	sgt = dma_buf_map_attachment(attachment, direction);
	if (IS_ERR(sgt)) {
		ret = PTR_ERR(sgt);
		goto p_err;
	}
	buffer->sgt = sgt;

	daddr =  vision_dma_buf_dva_map(buffer, size);
	if (IS_ERR_VALUE(daddr)) {
		vision_err("Failed to allocate iova (err 0x%pK)\n", &daddr);
		ret = -ENOMEM;
		goto p_err;
	}
	buffer->daddr = daddr;

	vaddr = npu_dma_buf_vmap(buffer->dma_buf);
	if (IS_ERR(vaddr)) {
		vision_err("Failed to get vaddr (err 0x%pK)\n", &vaddr);
		ret = -EFAULT;
		goto p_err;
	}
	buffer->vaddr = vaddr;

	complete_suc = true;

	//vision_info("__vb_map_dmabuf, size(%d), daddr(0x%x), vaddr(0x%pK)\n",
		//size, daddr, vaddr);
p_err:
	if (complete_suc != true)
		__vb_unmap_dmabuf(q, buffer);
	return ret;
}

static int __vb_queue_alloc(struct vb_queue *q,
	struct vs4l_container_list *c)
{
	int ret = 0;
	u32 i, j;
	size_t alloc_size;
	u8 *mapped_ptr;
	struct vb_format_list *flist;
	struct vb_bundle *bundle;
	struct vb_container_list *clist;
	struct vb_container *container;
	struct vb_buffer *buffer;

	BUG_ON(!q);
	BUG_ON(!c);
	BUG_ON(c->index >= VB_MAX_BUFFER);

	flist = &q->format;

	/* allocation */
	if (c->count > VB_MAX_CONTAINERLIST) {
		vision_err("c->count(%d) cannot be greater to VB_MAX_CONTAINERLIST(%d)\n", c->count, VB_MAX_CONTAINERLIST);
		ret = -EINVAL;
		goto p_err;
	}

	alloc_size = sizeof(struct vb_bundle);
	alloc_size += sizeof(struct vb_container) * c->count;
	for (i = 0; i < c->count; ++i)
		alloc_size += sizeof(struct vb_buffer) * c->containers[i].count;

	bundle = kzalloc(alloc_size, GFP_KERNEL);
	if (!bundle) {
		vision_err("bundle is NULL\n");
		ret = -ENOMEM;
		goto p_err;
	}

	/* mapping */
	mapped_ptr = (u8 *)bundle + sizeof(struct vb_bundle);
	bundle->clist.containers = (struct vb_container *)mapped_ptr;
	mapped_ptr += sizeof(struct vb_container) * c->count;
	for (i = 0; i < c->count; ++i) {
		bundle->clist.containers[i].buffers =
			(struct vb_buffer *)mapped_ptr;
		mapped_ptr += sizeof(struct vb_buffer) * c->containers[i].count;
	}

	/* fill */
	bundle->state = VB_BUF_STATE_DEQUEUED;
	clear_bit(VS4L_CL_FLAG_ENABLE_FENCE, &bundle->flags);
	clear_bit(VS4L_CL_FLAG_PREPARE, &bundle->flags);
	clear_bit(VS4L_CL_FLAG_INVALID, &bundle->flags);
	clear_bit(VS4L_CL_FLAG_DONE, &bundle->flags);
	clear_bit(VS4L_CL_FLAG_FW_TIMEOUT, &bundle->flags);
	clear_bit(VS4L_CL_FLAG_HW_TIMEOUT_RECOVERED, &bundle->flags);
	clear_bit(VS4L_CL_FLAG_HW_TIMEOUT_NOTRECOVERABLE, &bundle->flags);

	INIT_LIST_HEAD(&bundle->queued_entry);

	clist = &bundle->clist;
	clist->index = c->index;
	clist->id = c->id;
	clist->direction = c->direction;
	clist->count = c->count;
	clist->flags = c->flags;
	if (c->timestamp[5].tv_usec)
		clist->timestamp[5].tv_usec = c->timestamp[5].tv_usec;

	for (i = 0; i < clist->count; ++i) {
		container = &clist->containers[i];
		container->count = c->containers[i].count;
		container->memory = c->containers[i].memory;
		container->reserved[0] = c->containers[i].reserved[0];
		container->reserved[1] = c->containers[i].reserved[1];
		container->reserved[2] = c->containers[i].reserved[2];
		container->reserved[3] = c->containers[i].reserved[3];
		container->target = c->containers[i].target;
		container->type = c->containers[i].type;

		for (j = 0; j < flist->count; ++j) {
			if (container->target == flist->formats[j].target) {
				container->format = &flist->formats[j];
				break;
			}
		}

		if (!container->format) {
			vision_err("format is not found\n");
			kfree(bundle);
			ret = -EINVAL;
			goto p_err;
		}

		for (j = 0; j < container->count; ++j) {
			buffer = &container->buffers[j];
			buffer->roi = c->containers[i].buffers[j].roi;
			buffer->m.userptr =
				c->containers[i].buffers[j].m.userptr;
			buffer->dma_buf = NULL;
			buffer->attachment = NULL;
			buffer->sgt = NULL;
			buffer->vaddr = NULL;
			buffer->daddr = 0;
		}
	}

	q->bufs[c->index] = bundle;
	q->num_buffers++;

p_err:
	return ret;
}

static int __vb_queue_free(struct vb_queue *q,
	struct vb_bundle *bundle)
{
	int ret = 0;

	BUG_ON(!bundle);
	BUG_ON(bundle->clist.index >= VB_MAX_BUFFER);

	if (q == NULL) {
		vision_err("vb_queue(q) is NULL\n");
		ret = -EFAULT;
		goto p_err;
	}

	q->bufs[bundle->clist.index] = NULL;
	kfree(bundle);
	q->num_buffers--;

p_err:
	return ret;
}

static int __vb_queue_check(struct vb_bundle *bundle,
	struct vs4l_container_list *c)
{
	int ret = 0;
	u32 i, j;
	struct vb_container_list *clist;
	struct vb_container *container;
	struct vb_buffer *buffer;

	BUG_ON(!bundle);
	BUG_ON(!c);

	clist = &bundle->clist;

	if (clist->index != c->index) {
		vision_err("index is conflict(%d != %d)\n",
			clist->index, c->index);
		ret = -EINVAL;
		goto p_err;
	}

	if (clist->direction != c->direction) {
		vision_err("direction is conflict(%d != %d)\n",
			clist->direction, c->direction);
		ret = -EINVAL;
		goto p_err;
	}

	if (clist->count != c->count) {
		vision_err("count is conflict(%d != %d)\n",
			clist->count, c->count);
		ret = -EINVAL;
		goto p_err;
	}

	clist->flags = c->flags;
	clist->id = c->id;
	if (c->timestamp[5].tv_usec)
		clist->timestamp[5].tv_usec = c->timestamp[5].tv_usec;

	for (i = 0; i < clist->count; ++i) {
		container = &clist->containers[i];

		if (container->target != c->containers[i].target) {
			vision_err("target is conflict(%d != %d)\n",
				container->target, c->containers[i].target);
			ret = -EINVAL;
			goto p_err;
		}

		if (container->count != c->containers[i].count) {
			vision_err("count is conflict(%d != %d)\n",
				container->count, c->containers[i].count);
			ret = -EINVAL;
			goto p_err;
		}

		for (j = 0; j < container->count; ++j) {
			buffer = &container->buffers[j];
			buffer->roi = c->containers[i].buffers[j].roi;
			if (buffer->m.fd != c->containers[i].buffers[j].m.fd) {
				vision_err("buffer is conflict(%d != %d)\n",
				buffer->m.fd, c->containers[i].buffers[j].m.fd);
				ret = -EINVAL;
				goto p_err;
			}
		}
	}

	return ret;

p_err:
	__vb_buffer_print(clist);
	return ret;
}

static int __vb_buf_prepare(struct vb_queue *q, struct vb_bundle *bundle)
{
	int ret = 0;
	u32 i = 0, j = 0, k = 0;
	struct vb_format *format;
	struct vb_container *container;
	struct vb_buffer *buffer;

	BUG_ON(!q);
	BUG_ON(!bundle);

	if (test_bit(VS4L_CL_FLAG_PREPARE, &bundle->flags))
		return ret;

	for (i = 0; i < bundle->clist.count; ++i) {
		container = &bundle->clist.containers[i];
		format = container->format;

		switch (container->type) {
		case VS4L_BUFFER_LIST:
			k = container->count;
			break;
		case VS4L_BUFFER_ROI:
			k = container->count;
			break;
		case VS4L_BUFFER_PYRAMID:
			k = container->count;
			break;
		default:
			vision_err("unsupported container type\n");
			ret = -EINVAL;
			goto p_err;
		}

		switch (container->memory) {
		case VS4L_MEMORY_DMABUF:
			for (j = 0; j < k; ++j) {
				buffer = &container->buffers[j];
				if (format->colorspace == VS4L_DF_IMAGE_NPU ||
					format->colorspace == VS4L_DF_IMAGE_DSP) {
					ret = __vb_map_dmabuf(
					q, buffer, format->size[0]);
					vision_info("size : %u\n", format->size[0]);
				} else {
					ret = __vb_map_dmabuf(
					q, buffer, format->size[format->plane]);
					vision_info("size : %u\n", format->size[format->plane]);
				}
				if (ret) {
					vision_err("__vb_qbuf_dmabuf is fail(%d)\n",
						ret);
					goto p_err;
				}
				vision_info("prepare dmabuf (%d) - clist : %d container : %d, fd : %d\n", __LINE__, i, j, buffer->m.fd);
			}
			break;
		default:
			buffer = &container->buffers[j];
			vision_err("unsupported container memory type i %d, memory %d, fd %d\n",
					i, container->memory, buffer->m.fd);
			ret = -EINVAL;
			goto p_err;
		}
	}

	ret = call_op(q, buf_prepare, q, &bundle->clist);
	if (ret) {
		vision_err("call_op(buf_prepare) is fail(%d)\n", ret);
		goto p_err;
	}

	set_bit(VS4L_CL_FLAG_PREPARE, &bundle->flags);

p_err:
	return ret;
}

static int __vb_buf_unprepare(struct vb_queue *q, struct vb_bundle *bundle)
{
	int ret = 0;
	u32 i, j, k;
	struct vb_format *format;
	struct vb_container *container;
	struct vb_buffer *buffer;

	BUG_ON(!q);
	BUG_ON(!bundle);

	if (!test_bit(VS4L_CL_FLAG_PREPARE, &bundle->flags))
		return ret;

	for (i = 0; i < bundle->clist.count; ++i) {
		container = &bundle->clist.containers[i];
		format = container->format;

		switch (container->type) {
		case VS4L_BUFFER_LIST:
			k = container->count;
			break;
		case VS4L_BUFFER_ROI:
			k = container->count;
			break;
		case VS4L_BUFFER_PYRAMID:
			k = container->count;
			break;
		default:
			vision_err("unsupported container type\n");
			ret = -EINVAL;
			goto p_err;
		}

		switch (container->memory) {
		case VS4L_MEMORY_DMABUF:
			for (j = 0; j < k; ++j) {
				buffer = &container->buffers[j];
				ret = __vb_unmap_dmabuf(q, buffer);
				if (ret) {
					vision_err("__vb_qbuf_dmabuf is fail(%d)\n",
						ret);
					goto p_err;
				}
				vision_info("unprepare dmabuf (%d) - clist : %d container : %d, fd : %d\n", __LINE__, i, j, buffer->m.fd);
			}
			break;
		default:
			vision_err("unsupported container memory type\n");
			ret = -EINVAL;
			goto p_err;
		}
	}

	ret = call_op(q, buf_unprepare, q, &bundle->clist);
	if (ret) {
		vision_err("call_op(buf_unprepare) is fail(%d)\n", ret);
		goto p_err;
	}

	clear_bit(VS4L_CL_FLAG_PREPARE, &bundle->flags);

	return ret;

p_err:
	return ret;
}
#if IS_ENABLED(CONFIG_NPU_USE_ASYNC)
static bool __is_done_for_me(struct vb_queue *q,
	struct vb_bundle **bundle,
	struct vs4l_container_list *c)
{
	if (q->bufs[c->index]->state != VB_BUF_STATE_DONE)
		return false;

	*bundle = q->bufs[c->index];

	return true;

}
#endif
static int __vb_wait_for_done_vb(struct vb_queue *q,
	struct vb_bundle **bundle,
	struct vs4l_container_list *c,
	int nonblocking)
{
	int ret = 0;

	if (!q->streaming) {
		vision_err("Streaming off, will not wait for buffers\n");
		ret = -EINVAL;
		goto p_err;
	}

	if (nonblocking) {
		if (!list_empty(&q->done_list))
			return ret;

		vision_info("Nonblocking and no buffers to dequeue, will not wait\n");
		ret = -EWOULDBLOCK;
		goto p_err;
	}

	mutex_unlock(q->lock);
#if IS_ENABLED(CONFIG_NPU_USE_ASYNC)
	wait_event(q->done_wq, __is_done_for_me(q, bundle, c));
#else
	wait_event(q->done_wq, !list_empty(&q->done_list));
#endif
	mutex_lock(q->lock);
p_err:
	return ret;
}

static int __vb_get_done_vb(struct vb_queue *q,
	struct vb_bundle **bundle,
	struct vs4l_container_list *c,
	int nonblocking)
{
	int ret = 0;

	/*
	 * Wait for at least one buffer to become available on the done_list.
	 */
	ret = __vb_wait_for_done_vb(q, bundle, c, nonblocking);
	if (ret) {
		vision_err("__vb_wait_for_done_vb is fail\n");
		return ret;
	}

	/*
	 * Driver's lock has been held since we last verified that done_list
	 * is not empty, so no need for another list_empty(done_list) check.
	 */

#if !IS_ENABLED(CONFIG_NPU_USE_ASYNC)
	*bundle = list_first_entry(&q->done_list, struct vb_bundle, done_entry);
#endif

	return ret;
}

void __fill_vs4l_buffer(struct vb_bundle *bundle,
	struct vs4l_container_list *c, bool fill)
{
	struct vb_container_list *clist;
	struct vb_container *container;
	u32 i, j, k;

	clist = &bundle->clist;
	c->flags &= ~(1 << VS4L_CL_FLAG_TIMESTAMP);
	c->flags &= ~(1 << VS4L_CL_FLAG_PREPARE);
	c->flags &= ~(1 << VS4L_CL_FLAG_INVALID);
	c->flags &= ~(1 << VS4L_CL_FLAG_DONE);
	c->flags &= ~(1 << VS4L_CL_FLAG_FW_TIMEOUT);
	c->flags &= ~(1 << VS4L_CL_FLAG_HW_TIMEOUT_RECOVERED);
	c->flags &= ~(1 << VS4L_CL_FLAG_HW_TIMEOUT_NOTRECOVERABLE);

	if (test_bit(VS4L_CL_FLAG_TIMESTAMP, &clist->flags)) {
		c->flags |= (1 << VS4L_CL_FLAG_TIMESTAMP);
		memcpy(c->timestamp, clist->timestamp,
			sizeof(clist->timestamp));
	}

	if (test_bit(VS4L_CL_FLAG_PREPARE, &bundle->flags))
		c->flags |= (1 << VS4L_CL_FLAG_PREPARE);

	if (test_bit(VS4L_CL_FLAG_INVALID, &bundle->flags))
		c->flags |= (1 << VS4L_CL_FLAG_INVALID);

	if (test_bit(VS4L_CL_FLAG_DONE, &bundle->flags))
		c->flags |= (1 << VS4L_CL_FLAG_DONE);

	if (test_bit(VS4L_CL_FLAG_FW_TIMEOUT, &bundle->flags))
		c->flags |= (1 << VS4L_CL_FLAG_FW_TIMEOUT);

	if (test_bit(VS4L_CL_FLAG_HW_TIMEOUT_RECOVERED, &bundle->flags))
		c->flags |= (1 << VS4L_CL_FLAG_HW_TIMEOUT_RECOVERED);

	if (test_bit(VS4L_CL_FLAG_HW_TIMEOUT_NOTRECOVERABLE, &bundle->flags))
		c->flags |= (1 << VS4L_CL_FLAG_HW_TIMEOUT_NOTRECOVERABLE);

	c->index = clist->index;
	c->id = clist->id;

	if (!fill)
		return;

	/* sync buffers */
	for (i = 0; i < bundle->clist.count; ++i) {
		container = &bundle->clist.containers[i];
		k = container->count;
		for (j = 0; j < k; ++j)
			c->containers[i].buffers[j].reserved = container->buffers[j].reserved;
	}
}

static void __vb_dqbuf(struct vb_bundle *bundle)
{
	if (bundle->state == VB_BUF_STATE_DEQUEUED)
		return;

	bundle->state = VB_BUF_STATE_DEQUEUED;
}

int vb_queue_init(struct vb_queue *q,
	void *alloc_ctx,
	const struct vb2_mem_ops *mem_ops,
	const struct vb_ops *ops,
	struct mutex *lock,
	u32 direction)
{
	int ret = 0;

	if (q == NULL) {
		vision_err("vb_queue(q) is NULL\n");
		ret = -EFAULT;
		goto p_err;
	}

	INIT_LIST_HEAD(&q->queued_list);
	atomic_set(&q->queued_count, 0);

	INIT_LIST_HEAD(&q->process_list);
	atomic_set(&q->process_count, 0);

	INIT_LIST_HEAD(&q->done_list);
	atomic_set(&q->done_count, 0);

	spin_lock_init(&q->done_lock);
	init_waitqueue_head(&q->done_wq);

	mutex_init(lock);

	q->num_buffers = 0;
	q->direction = direction;
	q->lock	= lock;
	q->streaming = 0;
	q->alloc_ctx = alloc_ctx;
	q->mem_ops = mem_ops;
	q->ops = ops;

	clear_bit(VB_QUEUE_STATE_FORMAT, &q->state);
	q->format.count = 0;
	q->format.formats = NULL;

p_err:
	return ret;
}

int vb_queue_s_format(struct vb_queue *q, struct vs4l_format_list *flist)
{
	int ret = 0;
	u32 i;
	struct vs4l_format *f;
	struct vb_fmt *fmt;

	q->format.count = flist->count;
	q->format.formats = kcalloc(flist->count,
		sizeof(struct vb_format), GFP_KERNEL);

	if (!q->format.formats) {
		vision_err("q->format.formats is NULL\n");
		ret = -ENOMEM;
		goto p_err;
	}

	if (q->format.count > VB_MAX_BUFFER) {
		vision_err("flist->count(%d) cannot be greater to VB_MAX_BUFFER(%d)\n", q->format.count, VB_MAX_BUFFER);
		ret = -EINVAL;
		if (q->format.formats)
			kfree(q->format.formats);
		q->format.formats = NULL;
		goto p_err;
	}

	for (i = 0; i < flist->count; ++i) {
		f = &flist->formats[i];

		fmt = __vb_find_format(f->format);
		if (!fmt) {
			vision_err("__vb_find_format is fail\n");
			if (q->format.formats)
				kfree(q->format.formats);
			q->format.formats = NULL;
			ret = -EINVAL;
			goto p_err;
		}

		q->format.formats[i].fmt = fmt;
		q->format.formats[i].colorspace = f->format;
		q->format.formats[i].target = f->target;
		q->format.formats[i].plane = f->plane;
		q->format.formats[i].width = f->width;
		q->format.formats[i].height = f->height;
		q->format.formats[i].stride = f->stride;
		q->format.formats[i].cstride = f->cstride;
		q->format.formats[i].channels = f->channels;
		q->format.formats[i].pixel_format = f->pixel_format;

		vision_info("[%u]f : %u, t : %u, p : %u, w : %u, h : %u, s : %u, cs : %u, ch : %u, pf : %u\n",
			i, f->format, f->target, f->plane, f->width, f->height,
			f->stride, f->cstride, f->channels, f->pixel_format);

		vision_info("[queue] f : %u, t : %u, p : %u, w : %u, h : %u, s : %u, cs : %u, ch : %u, pf : %u\n",
			q->format.formats[i].colorspace, q->format.formats[i].target,
			q->format.formats[i].plane, q->format.formats[i].width,
			q->format.formats[i].height, q->format.formats[i].stride,
			q->format.formats[i].cstride, q->format.formats[i].channels,
			q->format.formats[i].pixel_format);

		if (q->format.formats[i].plane >= VB_MAX_PLANES) {
			vision_err("f->plane(%d) cannot be greater or equal to VB_MAX_PLANES(%d)\n", q->format.formats[i].plane, VB_MAX_PLANES);
			if (q->format.formats)
				kfree(q->format.formats);
			q->format.formats = NULL;
			ret = -EINVAL;
			goto p_err;
		}

		ret = __vb_plane_size(&q->format.formats[i]);
		if (ret) {
			vision_err("__vb_plane_size is fail(%d)\n", ret);
			if (q->format.formats)
				kfree(q->format.formats);
			q->format.formats = NULL;
			ret = -EINVAL;
			goto p_err;
		}
	}

	set_bit(VB_QUEUE_STATE_FORMAT, &q->state);

	return ret;

p_err:
	return ret;
}

int vb_queue_start(struct vb_queue *q)
{
	int ret = 0;

	if (!test_bit(VB_QUEUE_STATE_FORMAT, &q->state)) {
		vision_err("format is not configured\n");
		ret = -EINVAL;
		goto p_err;
	}
	q->streaming = 1;
	set_bit(VB_QUEUE_STATE_START, &q->state);

p_err:
	return ret;
}

void __vb_queue_clear(struct vb_queue *q)
{
	struct vb_bundle *pos_vb;
	struct vb_bundle *n_vb;

	BUG_ON(!q);

	list_for_each_entry_safe(pos_vb, n_vb, &q->queued_list, queued_entry) {
		if (pos_vb->state == VB_BUF_STATE_QUEUED) {
			list_del(&pos_vb->queued_entry);
			atomic_dec(&q->queued_count);
		}
	}
}

static int __vb_queue_stop(struct vb_queue *q, int is_forced)
{
	int ret = 0;
	u32 i;
	struct vb_bundle *bundle;

	__vb_queue_clear(q);

	q->streaming = 0;
	wake_up_all(&q->done_wq);

	if (atomic_read(&q->queued_count) > 0) {
		vision_err("queued list is not empty\n");
		if (!is_forced) {
			ret = -EINVAL;
			goto p_err;
		}
	}

	if (atomic_read(&q->process_count) > 0) {
		vision_err("process list is not empty\n");
		if (!is_forced) {
			ret = -EINVAL;
			goto p_err;
		}
	}

	if (atomic_read(&q->done_count) > 0) {
		vision_err("done list is not empty\n");
		if (!is_forced) {
			ret = -EINVAL;
			goto p_err;
		}
	}

	INIT_LIST_HEAD(&q->queued_list);
	INIT_LIST_HEAD(&q->process_list);
	INIT_LIST_HEAD(&q->done_list);

	for (i = 0; i < VB_MAX_BUFFER; ++i) {
		bundle = q->bufs[i];
		if (!bundle)
			continue;

		ret = __vb_buf_unprepare(q, bundle);
		if (ret) {
			vision_err("__vb_buf_unprepare is fail(%d)\n", ret);
			if (!is_forced) {
				ret = -EINVAL;
				goto p_err;
			}
		}

		ret = __vb_queue_free(q, bundle);
		if (ret) {
			vision_err("__vb_queue_free is fail(%d)\n", ret);
			if (!is_forced) {
				ret = -EINVAL;
				goto p_err;
			}
		}
	}

	if (q->format.formats) {
		kfree(q->format.formats);
		q->format.formats = NULL;
	}

	if (q->num_buffers != 0) {
		vision_err("memroy leakage is issued(%d)\n", q->num_buffers);
		BUG();
	}
	clear_bit(VB_QUEUE_STATE_START, &q->state);

p_err:
	if (!is_forced)
		return ret;
	else
		return 0;	/* Always successful on forced stop */
}

int vb_queue_stop(struct vb_queue *q)
{
	return __vb_queue_stop(q, 0);
}

int vb_queue_stop_forced(struct vb_queue *q)
{
	return __vb_queue_stop(q, 1);
}

int vb_queue_qbuf(struct vb_queue *q, struct vs4l_container_list *c)
{
	int ret = 0;
	struct vb_bundle *bundle;
	struct vb_container *container;
	u32 direction;
	u32 i;
	u32 j, k, size;

	if (q->direction != c->direction) {
		vision_err("qbuf: invalid buffer direction\n");
		ret = -EINVAL;
		goto p_err;
	}

	if (c->index >= VB_MAX_CONTAINERLIST) {
		vision_err("qbuf: invalid container list index\n");
		ret = -EINVAL;
		goto p_err;
	}

	for (i = 0; i < c->count; i++) {
		if (c->containers[i].count > VB_MAX_BUFFER) {
			vision_err("qbuf: Max buffers are %d; passed %d buffers\n",
				VB_MAX_BUFFER, c->containers[i].count);
			ret = -EINVAL;
			goto p_err;
		}
	}

	if (c->index >= VB_MAX_BUFFER) {
		vision_err("qbuf: buffer index out of range\n");
		ret = -EINVAL;
		goto p_err;
	}

	bundle = q->bufs[c->index];
	if (bundle) {
		ret = __vb_queue_check(bundle, c);
		if (ret) {
			vision_err("__vb_queue_check is fail(%d)\n", ret);
			goto p_err;
		}
	} else {
		ret = __vb_queue_alloc(q, c);
		if (ret) {
			vision_err("__vb_queue_alloc is fail(%d)\n", ret);
			goto p_err;
		}
		bundle = q->bufs[c->index];
	}

	if (bundle->state != VB_BUF_STATE_DEQUEUED) {
		vision_err("qbuf: buffer already in use\n");
		ret = -EINVAL;
		goto p_err;
	}

	ret = __vb_buf_prepare(q, bundle);
	if (ret) {
		vision_err("__vb_buf_prepare is fail(%d)\n", ret);
		goto p_err;
	}

	if (q->direction == VS4L_DIRECTION_OT)
		direction = DMA_FROM_DEVICE;
	else
		direction = DMA_TO_DEVICE;

	/* sync buffers */
	for (i = 0; i < bundle->clist.count; ++i) {
		container = &bundle->clist.containers[i];
		BUG_ON(!container->format);
		if (container->memory != VS4L_MEMORY_VIRTPTR) {
			k = container->count;
			if (container->format->colorspace == VS4L_DF_IMAGE_NPU ||
				container->format->colorspace == VS4L_DF_IMAGE_DSP)
				size = container->format->size[0];
			else
				size = container->format->size[
					container->format->plane];
			for (j = 0; j < k; ++j) {
				vision_dma_buf_sync(&(container->buffers[j]),
					size, direction, VISION_QBUF);
				container->buffers[j].reserved = c->containers[i].buffers[j].reserved;
			}
		}
	}

	/*
	 * Add to the queued buffers list, a buffer will stay on it until
	 * dequeued in dqbuf.
	 */

	vision_dbg("qbuf bundle %p, id %d, index %d, count %d\n",
			bundle, c->id, c->index, c->count);
	list_add_tail(&bundle->queued_entry, &q->queued_list);
	bundle->state = VB_BUF_STATE_QUEUED;
	atomic_inc(&q->queued_count);

p_err:
	return ret;
}

int vb_queue_prepare(struct vb_queue *q, struct vs4l_container_list *c)
{
	int ret = 0;
	struct vb_bundle *bundle;
	u32 i;

	if (q->direction != c->direction) {
		vision_err("qbuf: invalid buffer direction\n");
		ret = -EINVAL;
		goto p_err;
	}

	if (c->index >= VB_MAX_CONTAINERLIST) {
		vision_err("qbuf: invalid container list index\n");
		ret = -EINVAL;
		goto p_err;
	}

	for (i = 0; i < c->count; i++) {
		if (c->containers[i].count > VB_MAX_BUFFER) {
			vision_err("qbuf: Max buffers are %d; passed %d buffers\n",
				VB_MAX_BUFFER, c->containers[i].count);
			ret = -EINVAL;
			goto p_err;
		}
	}

	if (c->index >= VB_MAX_BUFFER) {
		vision_err("qbuf: buffer index out of range\n");
		ret = -EINVAL;
		goto p_err;
	}

	bundle = q->bufs[c->index];
	if (bundle) {
		ret = __vb_queue_check(bundle, c);
		if (ret) {
			vision_err("__vb_queue_check is fail(%d)\n", ret);
			goto p_err;
		}
	} else {
		ret = __vb_queue_alloc(q, c);
		if (ret) {
			vision_err("__vb_queue_alloc is fail(%d)\n", ret);
			goto p_err;
		}
		bundle = q->bufs[c->index];
	}

	if (bundle->state != VB_BUF_STATE_DEQUEUED) {
		vision_err("qbuf: buffer already in use\n");
		ret = -EINVAL;
		goto p_err;
	}

	ret = __vb_buf_prepare(q, bundle);
	if (ret) {
		vision_err("__vb_buf_prepare is fail(%d)\n", ret);
		goto p_err;
	}

p_err:
	return ret;
}

int vb_queue_unprepare(struct vb_queue *q, struct vs4l_container_list *c)
{
	int ret = 0;
	struct vb_bundle *bundle;

	if (q->direction != c->direction) {
		vision_err("qbuf: invalid buffer direction\n");
		ret = -EINVAL;
		goto p_err;
	}

	if (c->index >= VB_MAX_BUFFER) {
		vision_err("qbuf: buffer index out of range\n");
		ret = -EINVAL;
		goto p_err;
	}

	bundle = q->bufs[c->index];
	if (bundle) {
		ret = __vb_queue_check(bundle, c);
		if (ret) {
			vision_err("__vb_queue_check is fail(%d)\n", ret);
			goto p_err;
		}

	} else {
		vision_err("__vb_bundle doesn't exist(%d)\n", ret);
		ret = -ENOMEM;
		goto p_err;
	}

	ret = __vb_buf_unprepare(q, bundle);
	if (ret) {
		vision_err("__vb_buf_prepare is fail(%d)\n", ret);
		goto p_err;
	}

p_err:
	return ret;
}

int vb_queue_dqbuf(struct vb_queue *q,
	struct vs4l_container_list *c,
	bool nonblocking)
{
	int ret = 0;
	struct vb_bundle *bundle = NULL;

	if (q->direction != c->direction) {
		vision_err("qbuf: invalid buffer direction\n");
		ret = -EINVAL;
		goto p_err;
	}

	ret = __vb_get_done_vb(q, &bundle, c, nonblocking);
	if (ret < 0 || bundle == NULL) {
		if (ret != -EWOULDBLOCK)
			vision_err("__vb_get_done_vb is fail(b %p, r %d)\n", bundle, ret);
		return ret;
	}

	if (bundle->state != VB_BUF_STATE_DONE) {
		vision_err("dqbuf: Invalid buffer state(%X)\n", bundle->state);
		ret = -EINVAL;
		goto p_err;
	}

	/* Fill buffer information for the userspace */
	__fill_vs4l_buffer(bundle, c, false);
	/* Remove from videobuf queue */
	/* go back to dequeued state */
	__vb_dqbuf(bundle);

p_err:
	return ret;
}

void vb_queue_process(struct vb_queue *q, struct vb_bundle *bundle)
{
	BUG_ON(!q);
	BUG_ON(!bundle);
	BUG_ON(q->direction != bundle->clist.direction);

	bundle->state = VB_BUF_STATE_PROCESS;

	list_del(&bundle->queued_entry);
	atomic_dec(&q->queued_count);
}

void vb_queue_done(struct vb_queue *q, struct vb_bundle *bundle)
{
	struct vb_container *container;
	u32 direction;
	u32 i;
	u32 j, k, size;

	BUG_ON(!q);
	BUG_ON(!bundle);
	BUG_ON(q->direction != bundle->clist.direction);

	if (q->direction == VS4L_DIRECTION_OT)
		direction = DMA_FROM_DEVICE;
	else
		direction = DMA_TO_DEVICE;

	/* sync buffers */
	for (i = 0; i < bundle->clist.count; ++i) {
		container = &bundle->clist.containers[i];
		BUG_ON(!container->format);
		if (container->memory != VS4L_MEMORY_VIRTPTR) {
			k = container->count;
			if (container->format->colorspace == VS4L_DF_IMAGE_NPU ||
				container->format->colorspace == VS4L_DF_IMAGE_DSP)
				size = container->format->size[0];
			else {
				size = container->format->size[
				container->format->plane];
			}
			for (j = 0; j < k; ++j) {
				/* only for output */
				if (direction == DMA_FROM_DEVICE)
					vision_dma_buf_sync(&(container->buffers[j]),
						size, direction, VISION_DQBUF);
			}
		}
	}

	if (test_bit(VS4L_CL_FLAG_ENABLE_FENCE, &bundle->clist.flags)) {
		bundle->state = VB_BUF_STATE_DEQUEUED;
	} else {
		bundle->state = VB_BUF_STATE_DONE;
		wake_up(&q->done_wq);
	}

	vision_dbg("done bundle %p, id %d, index %d, count %d\n",
			bundle, bundle->clist.id, bundle->clist.index, bundle->clist.count);
}

void vb_queue_sync(u32 direction, struct vb_container_list *c)
{
	struct vb_container *container;
	u32 i, j, k, size;

	/* sync buffers */
	for (i = 0; i < c->count; ++i) {
		container = &c->containers[i];
		BUG_ON(!container->format);
		if (container->memory != VS4L_MEMORY_VIRTPTR) {
			k = container->count;
			if (container->format->colorspace == VS4L_DF_IMAGE_NPU ||
				container->format->colorspace == VS4L_DF_IMAGE_DSP)
				size = container->format->size[0];
			else
				size = container->format->size[
					container->format->plane];
			for (j = 0; j < k; ++j) {
				vision_dma_buf_sync(&(container->buffers[j]),
					size, direction, VISION_QBUF);
			}
		}
	}
}
