/*
 * FastRPC reverse tunnel
 *
 * Copyright (C) 2023 Richard Acayan
 *
 * This file is part of sensh.
 *
 * Sensh is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <stddef.h>
#include <stdio.h>

#include "fastrpc.h"
#include "fastrpc_adsp_listener.h"
#include "iobuffer.h"
#include "listener.h"

const struct fastrpc_interface *fastrpc_listener_interfaces[] = {
};

size_t fastrpc_listener_n_interfaces = sizeof(fastrpc_listener_interfaces)
				     / sizeof(*fastrpc_listener_interfaces);

static int adsp_listener_init2(int fd)
{
	return fastrpc2(&adsp_listener_init2_def, fd, ADSP_LISTENER_HANDLE);
}

static int adsp_listener_next2(int fd,
			       uint32_t ret_rctx,
			       uint32_t ret_res,
			       uint32_t ret_outbuf_len, void *ret_outbuf,
			       uint32_t *rctx,
			       uint32_t *handle,
			       uint32_t *sc,
			       uint32_t *inbufs_len,
			       uint32_t inbufs_size, void *inbufs)
{
	return fastrpc2(&adsp_listener_next2_def, fd, ADSP_LISTENER_HANDLE,
			ret_rctx,
			ret_res,
			ret_outbuf_len, ret_outbuf,
			rctx,
			handle,
			sc,
			inbufs_len,
			inbufs_size, inbufs);
}

static struct fastrpc_io_buffer *allocate_outbufs(const struct fastrpc_function_def_interp2 *def,
						  uint32_t *first_inbuf)
{
	struct fastrpc_io_buffer *out;
	size_t out_count;
	size_t i, j;
	off_t off;
	uint32_t *sizes;

	out_count = def->out_bufs + (def->out_nums && 1);
	out = malloc(sizeof(struct fastrpc_io_buffer) * out_count);
	if (out == NULL)
		return NULL;

	out[0].s = def->out_nums * 4;
	if (out[0].s) {
		out[0].p = malloc(def->out_nums * 4);
		if (out[0].p == NULL)
			goto err_free_out;
	}

	off = def->out_nums && 1;
	sizes = &first_inbuf[def->in_nums + def->in_bufs];

	for (i = 0; i < def->out_bufs; i++) {
		out[off + i].s = sizes[i];
		out[off + i].p = malloc(sizes[i]);
		if (out[off + i].p == NULL)
			goto err_free_prev;
	}

	return out;

err_free_prev:
	for (j = 0; j < i; j++)
		free(out[off + j].p);

err_free_out:
	free(out);
	return NULL;
}

static int check_inbuf_sizes(const struct fastrpc_function_def_interp2 *def,
			     const struct fastrpc_io_buffer *inbufs)
{
	uint8_t i;
	const uint32_t *sizes = &((const uint32_t *) inbufs[0].p)[def->in_nums];

	if (inbufs[0].s != 4 * (def->in_nums
			      + def->in_bufs
			      + def->out_bufs))
		return -1;

	for (i = 0; i < def->in_bufs; i++) {
		if (inbufs[i + 1].s != sizes[i])
			return -1;
	}

	return 0;
}

static int return_for_next_invoke(int fd,
				  uint32_t result,
				  uint32_t *rctx,
				  uint32_t *handle,
				  uint32_t *sc,
				  const struct fastrpc_io_buffer *returned,
				  struct fastrpc_io_buffer **decoded)
{
	struct fastrpc_decoder_context *ctx;
	char inbufs[256];
	char *outbufs = NULL;
	uint32_t inbufs_len;
	uint32_t outbufs_len;
	int ret;

	outbufs_len = outbufs_calculate_size(REMOTE_SCALARS_OUTBUFS(*sc), returned);

	if (outbufs_len) {
		outbufs = malloc(outbufs_len);
		if (outbufs == NULL) {
			perror("Could not allocate encoded output buffer");
			return -1;
		}

		outbufs_encode(REMOTE_SCALARS_OUTBUFS(*sc), returned, outbufs);
	}

	ret = adsp_listener_next2(fd,
				  *rctx, result,
				  outbufs_len, outbufs,
				  rctx, handle, sc,
				  &inbufs_len, 256, inbufs);
	if (ret) {
		fprintf(stderr, "Could not fetch next FastRPC message: %d\n", ret);
		goto err_free_outbufs;
	}

	if (inbufs_len > 256) {
		fprintf(stderr, "Large (>256B) input buffers aren't implemented\n");
		ret = -1;
		goto err_free_outbufs;
	}

	ctx = inbuf_decode_start(*sc);
	if (!ctx) {
		perror("Could not start decoding\n");
		ret = -1;
		goto err_free_outbufs;
	}

	inbuf_decode(ctx, inbufs_len, inbufs);

	if (!inbuf_decode_is_complete(ctx)) {
		fprintf(stderr, "Expected more input buffers\n");
		ret = -1;
		goto err_free_outbufs;
	}

	*decoded = inbuf_decode_finish(ctx);

err_free_outbufs:
	free(outbufs);
	return ret;
}

static int invoke_requested_procedure(uint32_t handle,
				      uint32_t sc,
			              uint32_t *result,
				      const struct fastrpc_io_buffer *decoded,
				      struct fastrpc_io_buffer **returned)
{
	const struct fastrpc_function_impl *impl;
	uint8_t in_count;
	uint8_t out_count;
	int ret;

	if (handle >= fastrpc_listener_n_interfaces
	 || REMOTE_SCALARS_METHOD(sc) >= fastrpc_listener_interfaces[handle]->n_procs)
		return -1;

	impl = &fastrpc_listener_interfaces[handle]->procs[REMOTE_SCALARS_METHOD(sc)];

	if (impl->def == NULL
	 || impl->impl == NULL)
		return -1;

	in_count = impl->def->in_bufs + ((impl->def->in_nums
				       || impl->def->in_bufs
				       || impl->def->out_bufs) && 1);
	out_count = impl->def->out_bufs + (impl->def->out_nums && 1);

	if (REMOTE_SCALARS_INBUFS(sc) != in_count
	 || REMOTE_SCALARS_OUTBUFS(sc) != out_count)
		return -1;

	ret = check_inbuf_sizes(impl->def, decoded);
	if (ret)
		return ret;

	*returned = allocate_outbufs(impl->def, decoded[0].p);
	if (*returned == NULL && out_count > 0)
		return -1;

	*result = impl->impl(decoded, *returned);

	return 0;
}

int run_fastrpc_listener(int fd)
{
	struct fastrpc_io_buffer *decoded = NULL,
				 *returned = NULL;
	uint32_t result = 0xffffffff;
	uint32_t handle;
	uint32_t rctx = 0;
	uint32_t sc = REMOTE_SCALARS_MAKE(0, 0, 0);
	uint32_t n_outbufs = 0;
	int ret;

	ret = adsp_listener_init2(fd);
	if (ret) {
		fprintf(stderr, "Could not initialize the listener: %u\n", ret);
		return ret;
	}

	while (!ret) {
		ret = return_for_next_invoke(fd,
					     result, &rctx, &handle, &sc,
					     returned, &decoded);
		if (ret)
			break;

		if (returned != NULL)
			iobuf_free(n_outbufs, returned);

		ret = invoke_requested_procedure(handle, sc, &result,
						 decoded, &returned);
		if (ret)
			break;

		if (decoded != NULL)
			iobuf_free(REMOTE_SCALARS_INBUFS(sc), decoded);

		n_outbufs = REMOTE_SCALARS_OUTBUFS(sc);
	}

	return ret;
}
