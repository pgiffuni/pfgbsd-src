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

#ifndef _SYS_NET_MODBUS_MODBUS_SYS_H_
#define	_SYS_NET_MODBUS_MODBUS_SYS_H_

/*
 * The system headers the core needs, which are not the same in a kernel
 * build and in a host build.
 *
 * Two of them exist only in the kernel: <sys/systm.h>, which declares
 * memmove() and memset(), and the be16dec() and be16enc() of
 * <sys/endian.h>.  A host build gets the first pair from the C library and
 * defines the second pair here, identically.  Everything else this directory
 * includes, <sys/param.h>, <sys/types.h>, <sys/errno.h> and
 * <sys/malloc.h>, is present in both, so this is the only place that has
 * to choose.
 *
 * This header exists so the protocol core can be compiled and tested
 * outside the kernel; it declares nothing of its own.
 */

#ifdef _KERNEL
#include <sys/systm.h>		/* memmove(), memset() */
#include <sys/endian.h>		/* be16dec(), be16enc() */
#else
#include <string.h>

/*
 * The kernel's byte order helpers are not in a host <sys/endian.h>, and the
 * engine only needs sixteen bit big endian, so give it exactly that.  The
 * definitions are the same as the kernel's: most significant byte first.
 */
static __inline uint16_t
be16dec(const void *pp)
{
	const uint8_t *p = pp;

	return ((uint16_t)((p[0] << 8) | p[1]));
}

static __inline void
be16enc(void *pp, uint16_t u)
{
	uint8_t *p = pp;

	p[0] = (uint8_t)(u >> 8);
	p[1] = (uint8_t)(u & 0xff);
}
#endif

#endif /* _SYS_NET_MODBUS_MODBUS_SYS_H_ */
