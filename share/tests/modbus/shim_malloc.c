/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Pedro Giffuni
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Host allocator for the protocol core.
 *
 * The kernel allocator takes a malloc type and flags; a host build maps them
 * onto the libc allocator while remembering the size, so the protocol core
 * can be tested without a kernel.  This file is not part of the module.
 */

#include <stdlib.h>

#include <sys/malloc.h>

/* Inside this file the shim macros must not replace the libc calls. */
#undef malloc
#undef free

struct shim_block {
	size_t	 size;
	void	*base;
};

void *
shim_malloc(size_t size, int flags)
{
	struct shim_block *b;

	b = calloc(1, sizeof(*b) + size);
	if (b == NULL)
		return (NULL);
	b->size = size;
	b->base = (char *)b + sizeof(*b);

	return (b->base);
}

void
shim_free(void *ptr)
{
	struct shim_block *b;

	if (ptr == NULL)
		return;
	b = (struct shim_block *)((char *)ptr - sizeof(*b));
	free(b);
}
