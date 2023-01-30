/*
 * Application processor control interface
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
#include <stdlib.h>
#include <string.h>

#include "aee_error.h"
#include "fastrpc_remotectl.h"
#include "iobuffer.h"
#include "listener.h"

struct remotectl_open_invoke {
	uint32_t inlen;
	uint32_t outlen;
};

struct remotectl_open_return {
	uint32_t handle;
	uint32_t error;
};

/*
 * This is a function that "opens" (searches an array for) an interface for the
 * remote endpoint to use. If it cannot find the requested interface, it
 * returns -5 with no error string.
 *
 * Having a constant compile-time list of interfaces lets the reverse tunnel
 * easily sanitize inputs.
 *
 * The -5 error was taken from Android code.
 */
static uint32_t localctl_open(const struct fastrpc_io_buffer *inbufs,
			      struct fastrpc_io_buffer *outbufs)
{
	const struct remotectl_open_invoke *first_in = inbufs[0].p;
	struct remotectl_open_return *first_out = outbufs[0].p;
	size_t i;

	if (((const char *) inbufs[1].p)[first_in->inlen - 1] != 0)
		return AEE_EBADPARM;

	memset(outbufs[1].p, 0, first_in->outlen);

	for (i = 0; i < fastrpc_listener_n_interfaces; i++) {
		if (!strcmp(fastrpc_listener_interfaces[i]->name,
			    inbufs[1].p)) {
			first_out->handle = i;
			first_out->error = 0;
			return 0;
		}
	}

	fprintf(stderr, "Could not find local interface %s\n",
			(const char *) inbufs[1].p);

	first_out->handle = 0;
	first_out->error = -5;

	return -5;
}

/*
 * This is a dummy function that is called when the remote endpoint is done
 * using an interface. It does not need to do anything because interfaces are
 * static.
 */
static uint32_t localctl_close(const struct fastrpc_io_buffer *inbufs,
			       struct fastrpc_io_buffer *outbufs)
{
	uint32_t *dlerr_size = inbufs[0].p;
	uint32_t *dlerr_len = outbufs[0].p;

	memset(outbufs[1].p, 0, *dlerr_size);

	*dlerr_len = 0;

	return 0;
}

const struct fastrpc_interface localctl_interface = {
	.name = "remotectl",
	.n_procs = 2,
	.procs = {
		{ .def = &remotectl_open_def, .impl = localctl_open, },
		{ .def = &remotectl_close_def, .impl = localctl_close, },
	},
};
