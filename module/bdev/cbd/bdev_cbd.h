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

#ifndef SPDK_BDEV_CBD_H
#define SPDK_BDEV_CBD_H

#include "spdk/stdinc.h"

#include "spdk/bdev.h"
#include "spdk/rpc.h"

typedef void (*spdk_delete_cbd_complete)(void *cb_arg, int bdeverrno);

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Create cbd bdev.
 *
 * \param name Name of cbd bdev.
 * \param cbd_name Curve volume name.
 * \param exclusive Shared or Exclusive open.
 * \param blocksize Size of block.
 * \return 0 on sucess, non-zero on failure
 */

int bdev_cbd_create(struct spdk_bdev **bdev, const char *name,
	const char *cbd_name, int exclusive, unsigned blocksize);
/**
 * Delete cbd bdev.
 *
 * \param name Name of cbd bdev.
 * \param cb_fn Function to call after deletion.
 * \param cb_arg Argument to pass to cb_fn.
 */
void bdev_cbd_delete(const char *name, spdk_delete_cbd_complete cb_fn,
	void *cb_arg);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_BDEV_CBD_H */
