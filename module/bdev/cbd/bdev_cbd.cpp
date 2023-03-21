/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Netease Inc.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Author: Xu Yifeng
 * Date  : 2023/03/21
 */

#include <libcurve.h>
#include "bdev_cbd.h"
#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/bdev.h"
#include "spdk/thread.h"
#include "spdk/json.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/likely.h"
#include "spdk/bdev_module.h"
#include "spdk/log.h"


#define CURVE_CONF_PATH "/etc/curve/client.conf"

using namespace curve::client;
static pthread_mutex_t curve_init_lock = PTHREAD_MUTEX_INITIALIZER;
static curve::client::CurveClient *g_curve;

static int bdev_cbd_count = 0;

struct bdev_cbd {
	struct spdk_bdev disk;
	char *cbd_name;
	int exclusive;
	int curve_fd;
	int64_t curve_size;

	pthread_mutex_t mutex;
	struct spdk_thread *main_td;
	struct spdk_thread *destruct_td;
	uint32_t ch_count;
	struct spdk_io_channel *group_ch;

	TAILQ_ENTRY(bdev_cbd) tailq;
	struct spdk_poller *reset_timer;
	struct spdk_bdev_io *reset_bdev_io;
};

struct bdev_cbd_io_channel {
	struct bdev_cbd *cbd;
};

struct bdev_cbd_io {
	struct			spdk_thread *submit_td;
	enum			spdk_bdev_io_status status;
	struct CurveAioContext  curve;
	int			auto_buf;
};

static void
copy_from_iov(void *_buf, const struct iovec *iov, int iovcnt, size_t len)
{
	char *buf = (char *)_buf;
	size_t off = 0;
	int i = 0;

	while (len > 0 && i < iovcnt) {
		size_t to_copy = spdk_min(iov[i].iov_len, len);
		memcpy(buf+off, iov[i].iov_base, to_copy);
		off += to_copy;
		len -= to_copy;
		i++;
	}
	SPDK_STATIC_ASSERT(len == 0, "Invalid iovcnt and len arguments");
}

static void
copy_to_iov(struct iovec *iov, int iovcnt, size_t len, const void *_buf)
{
	const char *buf = (const char *)_buf;
	size_t off = 0;
	int i = 0;

	while (len > 0 && i < iovcnt) {
		size_t to_copy = spdk_min(iov[i].iov_len, len);
		memcpy(iov[i].iov_base, buf, to_copy);
		off += to_copy;
		len -= to_copy;
		i++;
	}
	SPDK_STATIC_ASSERT(len == 0, "Invalid iovcnt and len arguments");
}

static int
init_curve(void)
{
	if (g_curve)
		return 0;

	int ret = 0;
	pthread_mutex_lock(&curve_init_lock);
	if (!g_curve) {
		curve::client::CurveClient *curve_client;
		curve_client = new curve::client::CurveClient;
		const char *env = getenv("SPDK_CURVE_CLIENT_CONF");
		if (env == NULL) {
			env = CURVE_CONF_PATH;
		}
		if (curve_client->Init(env)) {
			SPDK_ERRLOG("Can not init curve client\n");
			delete curve_client;
			ret = -1;
		}
		__sync_synchronize();
		g_curve = curve_client;
	}
	pthread_mutex_unlock(&curve_init_lock);
	return ret;
}

static void
bdev_cbd_free(struct bdev_cbd *cbd)
{
	if (!cbd) {
		return;
	}

	free(cbd->disk.name);
	free(cbd->cbd_name);

	if (cbd->curve_fd != -1)
		g_curve->Close(cbd->curve_fd);

	pthread_mutex_destroy(&cbd->mutex);
	free(cbd);
}

static int
curvedev_get_path(const char *curve_path, char *path) 
{
	char *p = NULL;

	strcpy(path, curve_path);
	p = strstr(path, "//");
	if (p == NULL) {
		errno = EINVAL;
		return -1;
	}
	strcpy(path, p+1);
	return 0;
}

static void *
bdev_cbd_init_context(void *arg)
{
	struct bdev_cbd *cbd = (struct bdev_cbd *)arg;
	char path[1024];
	int curve_fd;
	OpenFlags openflags;

	if (init_curve()) {
		return NULL;
	}
	if (curvedev_get_path(cbd->cbd_name, path)) {
		SPDK_ERRLOG("Invalid cbd name=%p\n", cbd->cbd_name);
		return NULL;
	}

	openflags.exclusive = cbd->exclusive ? true : false;
	curve_fd = g_curve->Open(path, openflags);
	if (curve_fd < 0) {
		SPDK_ERRLOG("Can not open curve volume %s\n", path);
		return NULL;
	}

	/* Save size info */
	cbd->curve_size = g_curve->StatFile(path);
	if (cbd->curve_size < 0) {
		g_curve->Close(curve_fd);
		SPDK_ERRLOG("Failed to StateFile %s\n", path);
		return NULL;
	}
	/* Close it now, we will open it later when first channel is created */
	g_curve->Close(curve_fd);

	return arg;
}

static int
bdev_cbd_init(struct bdev_cbd *cbd)
{
	int ret = 0;

	if (spdk_call_unaffinitized(bdev_cbd_init_context, cbd) == NULL) {
		SPDK_ERRLOG("Cannot init cbd context for cbd=%p\n", cbd);
		return -1;
	}

	return ret;
}

static void
bdev_cbd_exit(struct bdev_cbd *cbd)
{
	if (cbd->curve_fd > -1) {
		g_curve->Close(cbd->curve_fd);
		cbd->curve_fd = -1;
	}
}

static void
_bdev_cbd_io_complete(void *_cbd_io)
{
	struct bdev_cbd_io *cbd_io = (struct bdev_cbd_io *) _cbd_io;

	spdk_bdev_io_complete(spdk_bdev_io_from_ctx(cbd_io), cbd_io->status);
}

static void
bdev_cbd_io_complete(struct spdk_bdev_io *bdev_io, enum spdk_bdev_io_status status)
{
	struct bdev_cbd_io *cbd_io = (struct bdev_cbd_io *)bdev_io->driver_ctx;
	/* possible null if called from curve io engine */
	struct spdk_thread *current_thread = spdk_get_thread();

	assert(cbd_io->submit_td != NULL);
	cbd_io->status = status;

	if (cbd_io->auto_buf) {
		if ((status == SPDK_BDEV_IO_STATUS_SUCCESS) &&
		    (cbd_io->curve.op == LIBCURVE_OP_READ)) {
			copy_to_iov(bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
				cbd_io->curve.length, cbd_io->curve.buf);
		}
		free(cbd_io->curve.buf);
	}

	if (cbd_io->submit_td != current_thread) {
		spdk_thread_send_msg(cbd_io->submit_td, _bdev_cbd_io_complete,
				     cbd_io);
	} else {
		_bdev_cbd_io_complete(cbd_io);
	}
}

static void
bdev_cbd_finish_aiocb(struct CurveAioContext* curve)
{
	struct bdev_cbd_io *cbd_io = SPDK_CONTAINEROF(curve, bdev_cbd_io, curve);
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(cbd_io);
	enum spdk_bdev_io_status bio_status;

	if ((size_t)curve->ret == curve->length) {
		bio_status = SPDK_BDEV_IO_STATUS_SUCCESS;
	} else {
		bio_status = SPDK_BDEV_IO_STATUS_FAILED;
	}

	bdev_cbd_io_complete(bdev_io, bio_status);
}

static int
bdev_cbd_setup_buf(struct bdev_cbd_io *cbd_io, const struct iovec *iov,
	const int iovcnt, const size_t len)
{
	cbd_io->auto_buf = 0;
	if (iovcnt == 1) {
		cbd_io->curve.buf = iov[0].iov_base;
		return 0;
	}
	cbd_io->curve.buf = (char *)malloc(len);
	if (cbd_io->curve.buf == NULL) {
		return -1;
	}
	cbd_io->auto_buf = 1;
	return 0;
}

static void
bdev_cbd_start_aio(struct bdev_cbd *cbd, struct spdk_bdev_io *bdev_io,
		   struct iovec *iov, int iovcnt, uint64_t offset, size_t len)
{
	struct bdev_cbd_io *cbd_io = (struct bdev_cbd_io *)bdev_io->driver_ctx;

	cbd_io->curve.offset = offset;
	cbd_io->curve.length = len;
	if (bdev_cbd_setup_buf(cbd_io, iov, iovcnt, len)) {
		bdev_cbd_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_NOMEM);
		return;
	}
	cbd_io->curve.cb = bdev_cbd_finish_aiocb;
	if (bdev_io->type == SPDK_BDEV_IO_TYPE_READ) {
		cbd_io->curve.op = LIBCURVE_OP_READ;
		if (g_curve->AioRead(cbd->curve_fd, &cbd_io->curve, UserDataType::RawBuffer)) {
			bdev_cbd_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_AIO_ERROR);
		}
	} else if (bdev_io->type == SPDK_BDEV_IO_TYPE_WRITE) {
		cbd_io->curve.op = LIBCURVE_OP_WRITE;
		if (cbd_io->auto_buf)
			copy_from_iov(cbd_io->curve.buf, iov, iovcnt, len);
		if (g_curve->AioWrite(cbd->curve_fd, &cbd_io->curve, UserDataType::RawBuffer)) {
			bdev_cbd_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_AIO_ERROR);
		}
	} else {
		SPDK_STATIC_ASSERT(false, "io type not supported");
	}
}

static int bdev_cbd_library_init(void);
static void bdev_cbd_library_fini(void);

static int
bdev_cbd_get_ctx_size(void)
{
	return sizeof(struct bdev_cbd_io);
}

static struct spdk_bdev_module cbd_if = {
        .module_init = bdev_cbd_library_init,
        .module_fini = bdev_cbd_library_fini,
        .name = "cbd",
        .get_ctx_size = bdev_cbd_get_ctx_size,
};

SPDK_BDEV_MODULE_REGISTER(cbd, &cbd_if)

static int
bdev_cbd_reset_timer(void *arg)
{
	struct bdev_cbd *cbd = (struct bdev_cbd *)arg;

	/*
	 * TODO: This should check if any I/O is still in flight before completing the reset.
	 * For now, just complete after the timer expires.
	 */
	bdev_cbd_io_complete(cbd->reset_bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
	spdk_poller_unregister(&cbd->reset_timer);
	cbd->reset_bdev_io = NULL;

	return SPDK_POLLER_BUSY;
}

static void
bdev_cbd_reset(struct bdev_cbd *cbd, struct spdk_bdev_io *bdev_io)
{
	/*
	 * HACK: Since libcurve doesn't provide any way to cancel outstanding aio, just kick off a
	 * timer to wait for in-flight I/O to complete.
	 */
	assert(cbd->reset_bdev_io == NULL);
	cbd->reset_bdev_io = bdev_io;
	cbd->reset_timer = SPDK_POLLER_REGISTER(bdev_cbd_reset_timer, cbd, 1 * 1000 * 1000);
}

static void
_bdev_cbd_destruct_done(void *io_device)
{
	struct bdev_cbd *cbd = (struct bdev_cbd *)io_device;

	assert(cbd != NULL);
	assert(cbd->ch_count == 0);

	spdk_bdev_destruct_done(&cbd->disk, 0);
	bdev_cbd_free(cbd);
}

static void
bdev_cbd_free_cb(void *io_device)
{
	struct bdev_cbd *cbd = (struct bdev_cbd *) io_device;

	/* The io device has been unregistered.  Send a message back to the
	 * original thread that started the destruct operation, so that the
	 * bdev unregister callback is invoked on the same thread that started
	 * this whole process.
	 */
	spdk_thread_send_msg(cbd->destruct_td, _bdev_cbd_destruct_done, cbd);
}

static void
_bdev_cbd_destruct(void *ctx)
{
	struct bdev_cbd *cbd = (struct bdev_cbd *)ctx;

	spdk_io_device_unregister(cbd, bdev_cbd_free_cb);
}

static int
bdev_cbd_destruct(void *ctx)
{
	struct bdev_cbd *cbd = (struct bdev_cbd*) ctx;
	struct spdk_thread *td;

	if (cbd->main_td == NULL) {
		td = spdk_get_thread();
	} else {
		td = cbd->main_td;
	}

	/* Start the destruct operation on the cbd bdev's
	 * main thread.  This guarantees it will only start
	 * executing after any messages related to channel
	 * deletions have finished completing.  *Always*
	 * send a message, even if this function gets called
	 * from the main thread, in case there are pending
	 * channel delete messages in flight to this thread.
	 */
	assert(cbd->destruct_td == NULL);
	cbd->destruct_td = td;
	spdk_thread_send_msg(td, _bdev_cbd_destruct, cbd);

	/* Return 1 to indicate the destruct path is asynchronous. */
	return 1;
}

static void
bdev_cbd_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io,
		    bool success)
{
	struct bdev_cbd *cbd = (struct bdev_cbd *)bdev_io->bdev->ctxt;

	if (!success) {
		bdev_cbd_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	bdev_cbd_start_aio(cbd,
			   bdev_io,
			   bdev_io->u.bdev.iovs,
			   bdev_io->u.bdev.iovcnt,
			   bdev_io->u.bdev.offset_blocks * bdev_io->bdev->blocklen,
			   bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
}

static void
_bdev_cbd_submit_request(void *io)
{
	struct spdk_bdev_io *bdev_io = (struct spdk_bdev_io *) io;
	struct bdev_cbd_io *cbd_io = (struct bdev_cbd_io *)bdev_io->driver_ctx;
	struct bdev_cbd *cbd = (struct bdev_cbd *)bdev_io->bdev->ctxt;

	cbd_io->auto_buf = 0;
	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		spdk_bdev_io_get_buf(bdev_io, bdev_cbd_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		break;

	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		bdev_cbd_start_aio(cbd,
				   bdev_io,
				   bdev_io->u.bdev.iovs,
				   bdev_io->u.bdev.iovcnt,
				   bdev_io->u.bdev.offset_blocks * bdev_io->bdev->blocklen,
				   bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		break;

	case SPDK_BDEV_IO_TYPE_FLUSH:
		bdev_cbd_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
		break;

	case SPDK_BDEV_IO_TYPE_RESET:
		bdev_cbd_reset((struct bdev_cbd *)bdev_io->bdev->ctxt,
			       bdev_io);
		break;

	default:
		SPDK_ERRLOG("Unsupported IO type =%d\n", bdev_io->type);
		bdev_cbd_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		break;
	}
}

static void
bdev_cbd_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct spdk_thread *submit_td = spdk_io_channel_get_thread(ch);
	struct bdev_cbd *cbd = (struct bdev_cbd *)bdev_io->bdev->ctxt;
	struct bdev_cbd_io *cbd_io = (struct bdev_cbd_io *)bdev_io->driver_ctx;

	cbd_io->submit_td = submit_td;
	if (cbd->main_td != submit_td) {
		spdk_thread_send_msg(cbd->main_td, _bdev_cbd_submit_request, bdev_io);
	} else {
		_bdev_cbd_submit_request(bdev_io);
	}
}

static bool
bdev_cbd_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_RESET:
		return true;

	default:
		return false;
	}
}

static void
bdev_cbd_free_channel_resources(struct bdev_cbd *cbd)
{
	assert(cbd != NULL);
	assert(cbd->main_td == spdk_get_thread());
	assert(cbd->ch_count == 0);

	spdk_put_io_channel(cbd->group_ch);
	if (cbd->curve_fd > -1) {
		bdev_cbd_exit(cbd);
	}

	cbd->main_td = NULL;
	cbd->group_ch = NULL;
}

/* called when first channel is created */
static void *
bdev_cbd_handle(void *arg)
{
	struct bdev_cbd *cbd = (struct bdev_cbd *) arg;
	char path[1024];
	int curve_fd;
	OpenFlags openflags;
	void *ret = arg;

	if (curvedev_get_path(cbd->cbd_name, path)) {
		SPDK_ERRLOG("Invalid cbd name=%p\n", cbd->cbd_name);
		return NULL;
	}

	openflags.exclusive = cbd->exclusive ? true : false;
	curve_fd = g_curve->Open(path, openflags);
	if (curve_fd < 0) {
		SPDK_ERRLOG("Can not open curve volume %s, %s\n", path,
			    strerror(errno));
		return NULL;
	}

	cbd->curve_fd = curve_fd;
	return ret;
}

static int
_bdev_cbd_create_cb(struct bdev_cbd *cbd)
{
	cbd->main_td = spdk_get_thread();
	cbd->group_ch = spdk_get_io_channel(&cbd_if);
	assert(cbd->group_ch != NULL);

	if (spdk_call_unaffinitized(bdev_cbd_handle, cbd) == NULL) {
		bdev_cbd_free_channel_resources(cbd);
		return -1;
	}

	return 0;
}

/* iochannel creation callback */
static int
bdev_cbd_create_cb(void *io_device, void *ctx_buf)
{
	struct bdev_cbd_io_channel *ch = (struct bdev_cbd_io_channel *)ctx_buf;
	struct bdev_cbd *cbd = (struct bdev_cbd *)io_device;
	int rc;

	ch->cbd = cbd;
	pthread_mutex_lock(&cbd->mutex);
	if (cbd->ch_count == 0) {
		assert(cbd->main_td == NULL);
		rc = _bdev_cbd_create_cb(cbd);
		if (rc) {
			SPDK_ERRLOG("Cannot create channel for cbd=%p %s\n", cbd, cbd->cbd_name);
			pthread_mutex_unlock(&cbd->mutex);
			return rc;
		}
	}

	cbd->ch_count++;
	pthread_mutex_unlock(&cbd->mutex);

	return 0;
}

static void
_bdev_cbd_destroy_cb(void *ctx)
{
	struct bdev_cbd *cbd = (struct bdev_cbd *)ctx;

	pthread_mutex_lock(&cbd->mutex);
	assert(cbd->ch_count > 0);
	cbd->ch_count--;

	if (cbd->ch_count > 0) {
		/* A new channel was created between when message was sent and this function executed */
		pthread_mutex_unlock(&cbd->mutex);
		return;
	}

	bdev_cbd_free_channel_resources(cbd);
	pthread_mutex_unlock(&cbd->mutex);
}

static void
bdev_cbd_destroy_cb(void *io_device, void *ctx_buf)
{
	struct bdev_cbd *cbd = (struct bdev_cbd *)io_device;
	struct spdk_thread *thread;

	pthread_mutex_lock(&cbd->mutex);
	assert(cbd->ch_count > 0);
	cbd->ch_count--;
	if (cbd->ch_count == 0) {
		assert(cbd->main_td != NULL);
		if (cbd->main_td != spdk_get_thread()) {
			/* The final channel was destroyed on a different thread
			 * than where the first channel was created. Pass a message
			 * to the main thread to unregister the poller. */
			cbd->ch_count++;
			thread = cbd->main_td;
			pthread_mutex_unlock(&cbd->mutex);
			spdk_thread_send_msg(thread, _bdev_cbd_destroy_cb, cbd);
			return;
		}

		bdev_cbd_free_channel_resources(cbd);
	}
	pthread_mutex_unlock(&cbd->mutex);
}

static struct spdk_io_channel *
bdev_cbd_get_io_channel(void *ctx)
{
	struct bdev_cbd *cbd_bdev = (struct bdev_cbd *) ctx;

	return spdk_get_io_channel(cbd_bdev);
}

static int
bdev_cbd_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct bdev_cbd *cbd_bdev = (struct bdev_cbd *) ctx;

	spdk_json_write_named_object_begin(w, "cbd");

	spdk_json_write_named_string(w, "cbd", cbd_bdev->cbd_name);

	spdk_json_write_object_end(w);

	return 0;
}

static void
bdev_cbd_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	struct bdev_cbd *cbd = (struct bdev_cbd *)bdev->ctxt;

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "bdev_cbd_create");
	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "name", bdev->name);
	spdk_json_write_named_string(w, "cbd", cbd->cbd_name);
	spdk_json_write_named_uint32(w, "blocksize", bdev->blocklen);
	spdk_json_write_named_int32(w, "exclusive", cbd->exclusive);

	spdk_json_write_object_end(w);
}

static const struct spdk_bdev_fn_table cbd_fn_table = {
	.destruct		= bdev_cbd_destruct,
	.submit_request		= bdev_cbd_submit_request,
	.io_type_supported	= bdev_cbd_io_type_supported,
	.get_io_channel		= bdev_cbd_get_io_channel,
	.dump_info_json		= bdev_cbd_dump_info_json,
	.write_config_json	= bdev_cbd_write_config_json,
};

extern "C" int
bdev_cbd_create(struct spdk_bdev **bdev, const char *name,
		const char *cbd_name, int exclusive, unsigned blocksize)
{
	struct bdev_cbd *cbd = NULL;
	int ret = 0;

	if (cbd_name == NULL) {
		return -EINVAL;
	}

	cbd = (struct bdev_cbd *) calloc(1, sizeof(struct bdev_cbd));
	if (cbd == NULL) {
		SPDK_ERRLOG("Failed to allocate bdev_cbd struct\n");
		return -ENOMEM;
	}
	cbd->curve_fd = -1;

	ret = pthread_mutex_init(&cbd->mutex, NULL);
	if (ret) {
		SPDK_ERRLOG("Cannot init mutex on cbd=%p\n", name);
		free(cbd);
		return -ret;
	}

	cbd->cbd_name = strdup(cbd_name);
	if (NULL == cbd->cbd_name) {
		bdev_cbd_free(cbd);
		return -ENOMEM;
	}

	ret = bdev_cbd_init(cbd);
	if (ret < 0) {
		bdev_cbd_free(cbd);
		SPDK_ERRLOG("Failed to init cbd device\n");
		return ret;
	}

	if (name) {
		cbd->disk.name = strdup(name);
	} else {
		cbd->disk.name = spdk_sprintf_alloc("Curve%d", bdev_cbd_count);
	}
	if (!cbd->disk.name) {
		bdev_cbd_free(cbd);
		return -ENOMEM;
	}
	cbd->disk.product_name = (char *) "Curve Cbd Disk";
	bdev_cbd_count++;

	cbd->disk.write_cache = 0;
	cbd->disk.required_alignment = 0;
	cbd->disk.blocklen = blocksize;
	cbd->disk.blockcnt = cbd->curve_size / cbd->disk.blocklen;
	cbd->disk.ctxt = cbd;
	cbd->disk.fn_table = &cbd_fn_table;
	cbd->disk.module = &cbd_if;

	SPDK_NOTICELOG("Add cbd dev %s, volume %s\n", cbd->disk.name, cbd_name);

	spdk_io_device_register(cbd, bdev_cbd_create_cb,
				bdev_cbd_destroy_cb,
				sizeof(struct bdev_cbd_io_channel),
				cbd_name);
	ret = spdk_bdev_register(&cbd->disk);
	if (ret) {
		spdk_io_device_unregister(cbd, NULL);
		bdev_cbd_free(cbd);
		return ret;
	}

	*bdev = &(cbd->disk);

	return ret;
}

extern "C" void
bdev_cbd_delete(const char *name, spdk_delete_cbd_complete cb_fn, void *cb_arg)
{
	int rc = spdk_bdev_unregister_by_name(name, &cbd_if, cb_fn, cb_arg);
	if (rc != 0) {
		cb_fn(cb_arg, rc);
	} else {
		SPDK_NOTICELOG("Deleted %s cbd bdev\n", name);
	}
}

static int
bdev_cbd_group_create_cb(void *io_device, void *ctx_buf)
{
	return 0;
}

static void
bdev_cbd_group_destroy_cb(void *io_device, void *ctx_buf)
{
}

static int
bdev_cbd_library_init(void)
{
	spdk_io_device_register(&cbd_if, bdev_cbd_group_create_cb,
				bdev_cbd_group_destroy_cb,
				0, "bdev_cbd_poll_groups");
	return 0;
}

static void
bdev_cbd_library_fini(void)
{
	spdk_io_device_unregister(&cbd_if, NULL);
}

SPDK_LOG_REGISTER_COMPONENT(bdev_cbd)

