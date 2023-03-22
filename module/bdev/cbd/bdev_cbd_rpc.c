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

#include "bdev_cbd.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/log.h"

struct rpc_create_cbd {
	char *name;
	char *cbd_name;
	uint32_t exclusive;
	uint32_t blocksize;
};

static void
init_rpc_create_cbd(struct rpc_create_cbd *req)
{
	req->name = NULL;
	req->cbd_name = NULL;
	req->exclusive = 1;
	req->blocksize = 4096;
}

static void
free_rpc_create_cbd(struct rpc_create_cbd *req)
{
	free(req->name);
	free(req->cbd_name);
}

static const struct spdk_json_object_decoder rpc_create_cbd_decoders[] = {
	{"name", offsetof(struct rpc_create_cbd, name), spdk_json_decode_string, true},
	{"cbd", offsetof(struct rpc_create_cbd, cbd_name), spdk_json_decode_string},
	{"exclusive", offsetof(struct rpc_create_cbd, exclusive), spdk_json_decode_uint32},
	{"blocksize", offsetof(struct rpc_create_cbd, blocksize), spdk_json_decode_uint32},
};

static void
rpc_bdev_cbd_create(struct spdk_jsonrpc_request *request,
		    const struct spdk_json_val *params)
{
	struct rpc_create_cbd req;
	struct spdk_json_write_ctx *w = NULL;
	struct spdk_bdev *bdev = NULL;
	int rc = 0;

	init_rpc_create_cbd(&req);
	if (spdk_json_decode_object(params, rpc_create_cbd_decoders,
				    SPDK_COUNTOF(rpc_create_cbd_decoders),
				    &req)) {
		SPDK_DEBUGLOG(bdev_cbd, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = bdev_cbd_create(&bdev, req.name, req.cbd_name, !!req.exclusive,
			     req.blocksize);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_string(w, spdk_bdev_get_name(bdev));
	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_create_cbd(&req);
}
SPDK_RPC_REGISTER("bdev_cbd_create", rpc_bdev_cbd_create, SPDK_RPC_RUNTIME)

struct rpc_bdev_cbd_delete {
	char *name;
};

static void
free_rpc_bdev_cbd_delete(struct rpc_bdev_cbd_delete *req)
{
	free(req->name);
}

static const struct spdk_json_object_decoder rpc_bdev_cbd_delete_decoders[] = {
	{"name", offsetof(struct rpc_bdev_cbd_delete, name), spdk_json_decode_string},
};

static void
_rpc_bdev_cbd_delete_cb(void *cb_arg, int bdeverrno)
{
	struct spdk_jsonrpc_request *request = (struct spdk_jsonrpc_request *)cb_arg;

	if (bdeverrno == 0) {
		spdk_jsonrpc_send_bool_response(request, true);
	} else {
		spdk_jsonrpc_send_error_response(request, bdeverrno,
			 spdk_strerror(-bdeverrno));
	}
}

static void
rpc_bdev_cbd_delete(struct spdk_jsonrpc_request *request,
		    const struct spdk_json_val *params)
{
	struct rpc_bdev_cbd_delete req = {NULL};

	if (spdk_json_decode_object(params, rpc_bdev_cbd_delete_decoders,
				    SPDK_COUNTOF(rpc_bdev_cbd_delete_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, 
		    SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
		    "spdk_json_decode_object failed");
	} else {
		bdev_cbd_delete(req.name, _rpc_bdev_cbd_delete_cb, request);
	}

	free_rpc_bdev_cbd_delete(&req);
}
SPDK_RPC_REGISTER("bdev_cbd_delete", rpc_bdev_cbd_delete, SPDK_RPC_RUNTIME)

struct rpc_bdev_cbd_refresh {
	char *name;
};

static void
free_rpc_bdev_cbd_refresh(struct rpc_bdev_cbd_refresh *req)
{
	free(req->name);
}

static const struct spdk_json_object_decoder rpc_bdev_cbd_refresh_decoders[] = {
	{"name", offsetof(struct rpc_bdev_cbd_refresh, name), spdk_json_decode_string},
};

static void
rpc_bdev_cbd_refresh(struct spdk_jsonrpc_request *request,
		    const struct spdk_json_val *params)
{
	struct rpc_bdev_cbd_refresh req = {NULL};
	int rc = 0;

	if (spdk_json_decode_object(params, rpc_bdev_cbd_refresh_decoders,
				    SPDK_COUNTOF(rpc_bdev_cbd_refresh_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, 
		    SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
		    "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = bdev_cbd_refresh(req.name);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	spdk_jsonrpc_send_bool_response(request, true);

cleanup:
	free_rpc_bdev_cbd_refresh(&req);
}
SPDK_RPC_REGISTER("bdev_cbd_refresh", rpc_bdev_cbd_refresh, SPDK_RPC_RUNTIME)
