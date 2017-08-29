/*
 * Copyright (C) 2010-2011 Chia-I Wu <olvaffe@gmail.com>
 * Copyright (C) 2010-2011 LunarG Inc.
 * Copyright (C) 2016 Linaro, Ltd., Rob Herring <robh@kernel.org>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#define LOG_TAG "GRALLOC-GBM"

#include <cutils/log.h>
#include <stdlib.h>
#include <stdarg.h>
#include <pthread.h>
#include <errno.h>

#include <hardware/gralloc.h>
#include <system/graphics.h>

#include <gbm.h>

#include "gralloc_drm.h"
#include "gralloc_gbm_priv.h"

struct gbm_module_t {
	gralloc_module_t base;

	pthread_mutex_t mutex;
	struct gbm_device *gbm;
};

static inline int gralloc_gbm_get_bpp(int format)
{
	int bpp;

	switch (format) {
	case HAL_PIXEL_FORMAT_RGBA_8888:
	case HAL_PIXEL_FORMAT_RGBX_8888:
	case HAL_PIXEL_FORMAT_BGRA_8888:
		bpp = 4;
		break;
	case HAL_PIXEL_FORMAT_RGB_888:
		bpp = 3;
		break;
	case HAL_PIXEL_FORMAT_RGB_565:
	case HAL_PIXEL_FORMAT_YCbCr_422_I:
		bpp = 2;
		break;
	/* planar; only Y is considered */
	case HAL_PIXEL_FORMAT_YV12:
	case HAL_PIXEL_FORMAT_YCbCr_422_SP:
	case HAL_PIXEL_FORMAT_YCrCb_420_SP:
		bpp = 1;
		break;
	default:
		bpp = 0;
		break;
	}

	return bpp;
}

/*
 * Initialize the DRM device object
 */
static int gbm_init(struct gbm_module_t *dmod)
{
	int err = 0;

	pthread_mutex_lock(&dmod->mutex);
	if (!dmod->gbm) {
		dmod->gbm = gbm_dev_create();
		if (!dmod->gbm)
			err = -EINVAL;
	}
	pthread_mutex_unlock(&dmod->mutex);

	return err;
}

static int gbm_mod_perform(const struct gralloc_module_t *mod, int op, ...)
{
	struct gbm_module_t *dmod = (struct gbm_module_t *) mod;
	va_list args;
	int err;
	uint32_t uop = static_cast<uint32_t>(op);

	err = gbm_init(dmod);
	if (err)
		return err;

	va_start(args, op);
	switch (uop) {
	case GRALLOC_MODULE_PERFORM_GET_DRM_FD:
		{
			int *fd = va_arg(args, int *);
			*fd = gbm_device_get_fd(dmod->gbm);
			err = 0;
		}
		break;
	/* TODO: This is a stub and should be implemented fully */
	case GRALLOC_MODULE_PERFORM_GET_USAGE:
		{
			err = 0;
		}
		break;
	default:
		err = -EINVAL;
		break;
	}
	va_end(args);

	return err;
}

static int gbm_mod_register_buffer(const gralloc_module_t *mod,
		buffer_handle_t handle)
{
	struct gbm_module_t *dmod = (struct gbm_module_t *) mod;
	int err;

	err = gbm_init(dmod);
	if (err)
		return err;

	pthread_mutex_lock(&dmod->mutex);
	err = gralloc_gbm_handle_register(handle, dmod->gbm);
	pthread_mutex_unlock(&dmod->mutex);

	return err;
}

static int gbm_mod_unregister_buffer(const gralloc_module_t *mod,
		buffer_handle_t handle)
{
	struct gbm_module_t *dmod = (struct gbm_module_t *) mod;
	int err;

	pthread_mutex_lock(&dmod->mutex);
	err = gralloc_gbm_handle_unregister(handle);
	pthread_mutex_unlock(&dmod->mutex);

	return err;
}

static int gbm_mod_lock(const gralloc_module_t *mod, buffer_handle_t handle,
		int usage, int x, int y, int w, int h, void **ptr)
{
	struct gbm_module_t *dmod = (struct gbm_module_t *) mod;
	struct gralloc_gbm_bo_t *bo;
	int err;

	pthread_mutex_lock(&dmod->mutex);

	bo = gralloc_gbm_bo_from_handle(handle);
	if (!bo) {
		err = -EINVAL;
		goto unlock;
	}

	err = gralloc_gbm_bo_lock(bo, usage, x, y, w, h, ptr);
	ALOGV("buffer %p lock usage = %08x", handle, usage);

unlock:
	pthread_mutex_unlock(&dmod->mutex);
	return err;
}

static int gbm_mod_unlock(const gralloc_module_t *mod, buffer_handle_t handle)
{
	struct gbm_module_t *dmod = (struct gbm_module_t *) mod;
	struct gralloc_gbm_bo_t *bo;
	int err = 0;

	pthread_mutex_lock(&dmod->mutex);

	bo = gralloc_gbm_bo_from_handle(handle);
	if (!bo) {
		err = -EINVAL;
		goto unlock;
	}

	gralloc_gbm_bo_unlock(bo);

unlock:
	pthread_mutex_unlock(&dmod->mutex);
	return err;
}

static int gbm_mod_close_gpu0(struct hw_device_t *dev)
{
	struct gbm_module_t *dmod = (struct gbm_module_t *)dev->module;
	struct alloc_device_t *alloc = (struct alloc_device_t *) dev;

	gbm_dev_destroy(dmod->gbm);
	delete alloc;

	return 0;
}

static int gbm_mod_free_gpu0(alloc_device_t *dev, buffer_handle_t handle)
{
	struct gbm_module_t *dmod = (struct gbm_module_t *) dev->common.module;
	struct gralloc_gbm_bo_t *bo;
	int err = 0;

	pthread_mutex_lock(&dmod->mutex);

	bo = gralloc_gbm_bo_from_handle(handle);
	if (!bo) {
		err = -EINVAL;
		goto unlock;
	}

	gbm_free(bo);
	native_handle_close(handle);
	delete handle;

unlock:
	pthread_mutex_unlock(&dmod->mutex);
	return err;
}

static int gbm_mod_alloc_gpu0(alloc_device_t *dev,
		int w, int h, int format, int usage,
		buffer_handle_t *handle, int *stride)
{
	struct gbm_module_t *dmod = (struct gbm_module_t *) dev->common.module;
	struct gralloc_gbm_bo_t *bo;
	int err = 0;

	pthread_mutex_lock(&dmod->mutex);

	bo = gralloc_gbm_bo_create(dmod->gbm, w, h, format, usage);
	if (!bo) {
		err = -errno;
		goto unlock;
	}

	*handle = gralloc_gbm_bo_get_handle(bo);
	/* in pixels */
	*stride = gbm_bo_get_stride(gralloc_gbm_bo_to_gbm_bo(bo)) /
		gralloc_gbm_get_bpp(format);

	ALOGV("buffer %p usage = %08x", *handle, usage);
unlock:
	pthread_mutex_unlock(&dmod->mutex);
	return err;
}

static int gbm_mod_open_gpu0(struct gbm_module_t *dmod, hw_device_t **dev)
{
	struct alloc_device_t *alloc;
	int err;

	err = gbm_init(dmod);
	if (err)
		return err;

	alloc = new alloc_device_t();
	if (!alloc)
		return -EINVAL;

	alloc->common.tag = HARDWARE_DEVICE_TAG;
	alloc->common.version = 0;
	alloc->common.module = &dmod->base.common;
	alloc->common.close = gbm_mod_close_gpu0;

	alloc->alloc = gbm_mod_alloc_gpu0;
	alloc->free = gbm_mod_free_gpu0;

	*dev = &alloc->common;

	return 0;
}

static int gbm_mod_open(const struct hw_module_t *mod,
		const char *name, struct hw_device_t **dev)
{
	struct gbm_module_t *dmod = (struct gbm_module_t *) mod;
	int err;

	if (strcmp(name, GRALLOC_HARDWARE_GPU0) == 0)
		err = gbm_mod_open_gpu0(dmod, dev);
	else
		err = -EINVAL;

	return err;
}

static struct hw_module_methods_t gbm_mod_methods = {
	.open = gbm_mod_open
};

struct gbm_module_t HAL_MODULE_INFO_SYM = {
	.base = {
		.common = {
			.tag = HARDWARE_MODULE_TAG,
			.version_major = 1,
			.version_minor = 0,
			.id = GRALLOC_HARDWARE_MODULE_ID,
			.name = "GBM Memory Allocator",
			.author = "Rob Herring - Linaro",
			.methods = &gbm_mod_methods
		},
		.registerBuffer = gbm_mod_register_buffer,
		.unregisterBuffer = gbm_mod_unregister_buffer,
		.lock = gbm_mod_lock,
		.unlock = gbm_mod_unlock,
		.perform = gbm_mod_perform
	},

	.mutex = PTHREAD_MUTEX_INITIALIZER,
	.gbm = NULL,
};
