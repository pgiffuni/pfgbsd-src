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
 * Memory backed register storage.  See modbus_backend.h for the data model
 * reference; nothing here is aware of the function codes that use it.
 */

#include <sys/param.h>
#include "modbus_sys.h"
#include <sys/malloc.h>
#include <sys/errno.h>

#include "modbus.h"
#include "modbus_backend.h"

#ifdef _KERNEL
#include <sys/kernel.h>
static MALLOC_DEFINE(M_MODBUS, "modbus_backend",
    "modbus register storage");

/*
 * The kernel and the C library take the malloc type and the flags in a
 * different order, so keep the two calls in one place and let a host build
 * of this file use the C library allocator directly.
 */
static void *
modbus_alloc(size_t size)
{

	return (malloc(size, M_MODBUS, M_WAITOK | M_ZERO));
}

static void
modbus_free(void *p)
{

	free(p, M_MODBUS);
}
#else
static void *
modbus_alloc(size_t size)
{

	return (malloc(size, M_WAITOK | M_ZERO, NULL));
}

static void
modbus_free(void *p)
{

	free(p, NULL);
}
#endif

/*
 * Section 4.4: every area holds at most 65536 items, but a node is
 * configured with bounded sizes so that a control message can never ask for
 * an unbounded allocation.
 */
static int
modbus_backend_ok_size(uint16_t n)
{

	return (n >= MODBUS_BACKEND_MIN && n <= MODBUS_BACKEND_MAX);
}

static void
modbus_backend_zero(struct modbus_backend *be)
{

	memset(be, 0, sizeof(*be));
}

int
modbus_backend_init(struct modbus_backend *be,
    const struct modbus_backend_config *cfg)
{

	modbus_backend_zero(be);

	if (!modbus_backend_ok_size(cfg->ncoils) ||
	    !modbus_backend_ok_size(cfg->ndiscrete_in) ||
	    !modbus_backend_ok_size(cfg->ninput_regs) ||
	    !modbus_backend_ok_size(cfg->nholding_regs))
		return (EINVAL);

	if (cfg->ncoils != 0) {
		be->coils = modbus_alloc(cfg->ncoils);
		if (be->coils == NULL)
			goto nomem;
	}
	if (cfg->ndiscrete_in != 0) {
		be->discrete_in = modbus_alloc(cfg->ndiscrete_in);
		if (be->discrete_in == NULL)
			goto nomem;
	}
	if (cfg->ninput_regs != 0) {
		be->input_regs = modbus_alloc(2 * cfg->ninput_regs);
		if (be->input_regs == NULL)
			goto nomem;
	}
	if (cfg->nholding_regs != 0) {
		be->holding_regs = modbus_alloc(2 * cfg->nholding_regs);
		if (be->holding_regs == NULL)
			goto nomem;
	}

	be->ncoils = cfg->ncoils;
	be->ndiscrete_in = cfg->ndiscrete_in;
	be->ninput_regs = cfg->ninput_regs;
	be->nholding_regs = cfg->nholding_regs;

	return (0);

nomem:
	modbus_backend_destroy(be);
	return (ENOMEM);
}

void
modbus_backend_destroy(struct modbus_backend *be)
{

	modbus_free(be->coils);
	modbus_free(be->discrete_in);
	modbus_free(be->input_regs);
	modbus_free(be->holding_regs);
	modbus_backend_zero(be);
}

/*
 * Resolve an area to its storage and size.  An area that is not configured,
 * or that is read only when written, is reported as an illegal address so
 * that the function handler can turn it into exception 02 without knowing
 * how the backend is built.
 */
static int
modbus_backend_area(const struct modbus_backend *be, modbus_area_t area,
    int writing, uint16_t *sizep)
{
	const void *base;
	uint16_t n;

	switch (area) {
	case MODBUS_AREA_COIL:
		base = be->coils;
		n = be->ncoils;
		break;
	case MODBUS_AREA_DISCRETE_IN:
		base = be->discrete_in;
		n = be->ndiscrete_in;
		break;
	case MODBUS_AREA_INPUT_REG:
		base = be->input_regs;
		n = be->ninput_regs;
		break;
	case MODBUS_AREA_HOLDING_REG:
		base = be->holding_regs;
		n = be->nholding_regs;
		break;
	default:
		return (MODBUS_EX_ILLEGAL_ADDRESS);
	}

	if (base == NULL || n == 0)
		return (MODBUS_EX_ILLEGAL_ADDRESS);
	if (writing && (area == MODBUS_AREA_DISCRETE_IN ||
	    area == MODBUS_AREA_INPUT_REG))
		return (MODBUS_EX_ILLEGAL_ADDRESS);

	*sizep = n;
	return (0);
}

/*
 * A range is legal only when it fits entirely inside the area.  Addresses do
 * not wrap: section 4.4 defines the PDU address space as 0..65535 and leaves
 * the binding to device memory to the application, so a request that runs off
 * the end of the map is rejected rather than silently folded back.
 */
static int
modbus_backend_range(uint16_t addr, uint16_t n, uint16_t size)
{

	if (n == 0 || addr > size || n > size - addr)
		return (MODBUS_EX_ILLEGAL_ADDRESS);

	return (0);
}

int
modbus_backend_read_bits(const struct modbus_backend *be, modbus_area_t area,
    uint16_t addr, uint16_t nbits, uint8_t *out)
{
	const uint8_t *bits;
	uint16_t size;
	uint16_t i;
	int error;

	if ((error = modbus_backend_area(be, area, 0, &size)) != 0)
		return (error);
	if ((error = modbus_backend_range(addr, nbits, size)) != 0)
		return (error);

	switch (area) {
	case MODBUS_AREA_COIL:
		bits = be->coils;
		break;
	default:
		bits = be->discrete_in;
		break;
	}

	for (i = 0; i < nbits; i++) {
		/*
		 * The wire format packs the lowest addressed bit into the
		 * least significant bit of the first data byte.
		 */
		if (bits[addr + i] != 0)
			out[i / 8] |= (uint8_t)(1U << (i % 8));
	}

	return (MODBUS_EX_OK);
}

int
modbus_backend_write_bits(const struct modbus_backend *be, modbus_area_t area,
    uint16_t addr, uint16_t nbits, const uint8_t *in)
{
	uint8_t *bits;
	uint16_t size;
	uint16_t i;
	int error;

	if ((error = modbus_backend_area(be, area, 1, &size)) != 0)
		return (error);
	if ((error = modbus_backend_range(addr, nbits, size)) != 0)
		return (error);

	bits = (area == MODBUS_AREA_COIL) ? be->coils : be->discrete_in;

	for (i = 0; i < nbits; i++) {
		bits[addr + i] =
		    (in[i / 8] & (uint8_t)(1U << (i % 8))) ? 1 : 0;
	}

	return (MODBUS_EX_OK);
}

int
modbus_backend_read_registers(const struct modbus_backend *be,
    modbus_area_t area, uint16_t addr, uint16_t nregs, uint16_t *out)
{
	const uint16_t *regs;
	uint16_t size;
	uint16_t i;
	int error;

	if ((error = modbus_backend_area(be, area, 0, &size)) != 0)
		return (error);
	if ((error = modbus_backend_range(addr, nregs, size)) != 0)
		return (error);

	regs = (area == MODBUS_AREA_INPUT_REG) ?
	    be->input_regs : be->holding_regs;

	for (i = 0; i < nregs; i++)
		out[i] = regs[addr + i];

	return (MODBUS_EX_OK);
}

int
modbus_backend_write_registers(const struct modbus_backend *be,
    modbus_area_t area, uint16_t addr, uint16_t nregs, const uint16_t *in)
{
	uint16_t *regs;
	uint16_t size;
	uint16_t i;
	int error;

	if ((error = modbus_backend_area(be, area, 1, &size)) != 0)
		return (error);
	if ((error = modbus_backend_range(addr, nregs, size)) != 0)
		return (error);

	if (area != MODBUS_AREA_HOLDING_REG)
		return (MODBUS_EX_ILLEGAL_ADDRESS);

	regs = be->holding_regs;

	for (i = 0; i < nregs; i++)
		regs[addr + i] = in[i];

	return (MODBUS_EX_OK);
}

/*
 * Section 6.16: the new value is
 *	(current AND and_mask) OR (or_mask AND NOT and_mask).
 */
int
modbus_backend_mask_register(struct modbus_backend *be, uint16_t addr,
    uint16_t and_mask, uint16_t or_mask)
{
	uint16_t size;
	int error;

	if ((error = modbus_backend_area(be, MODBUS_AREA_HOLDING_REG, 1,
	    &size)) != 0)
		return (error);
	if ((error = modbus_backend_range(addr, 1, size)) != 0)
		return (error);

	be->holding_regs[addr] =
	    (uint16_t)((be->holding_regs[addr] & and_mask) |
	    (or_mask & ~and_mask));

	return (MODBUS_EX_OK);
}
