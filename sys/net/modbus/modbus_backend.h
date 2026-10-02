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

#ifndef _SYS_NET_MODBUS_MODBUS_BACKEND_H_
#define	_SYS_NET_MODBUS_MODBUS_BACKEND_H_

#include <sys/param.h>
#include <sys/types.h>

#include <net/modbus/modbus.h>

/*
 * Register and coil storage.
 *
 * The specification does not define what the registers contain, section 4.3
 * only defines the four areas and their access rights, so the contents live
 * in a backend.  The first backend is plain memory: one allocation per area,
 * sized when the node is configured, with no allocation in the data path.
 */
struct modbus_backend {
	uint8_t		*coils;		/* one byte per bit, one area */
	uint8_t		*discrete_in;	/* one byte per bit */
	uint16_t	*input_regs;
	uint16_t	*holding_regs;
	uint16_t	 ncoils;
	uint16_t	 ndiscrete_in;
	uint16_t	 ninput_regs;
	uint16_t	 nholding_regs;
};

/*
 * Sizes are bounded so that a configuration mistake cannot turn into a large
 * allocation, and so a node always has a fixed memory footprint.
 */
#define	MODBUS_BACKEND_MIN	1
#define	MODBUS_BACKEND_MAX	4096
#define	MODBUS_BACKEND_DEFAULT	1024

struct modbus_backend_config {
	uint16_t	ncoils;
	uint16_t	ndiscrete_in;
	uint16_t	ninput_regs;
	uint16_t	nholding_regs;
};

int modbus_backend_init(struct modbus_backend *,
    const struct modbus_backend_config *);
void modbus_backend_destroy(struct modbus_backend *);

/*
 * All of these return MODBUS_EX_OK, or a Modbus exception code: never a
 * kernel errno.  MODBUS_AREA_DISCRETE_IN and MODBUS_AREA_INPUT_REG are read
 * only and answer MODBUS_EX_ILLEGAL_ADDRESS when written, matching the data
 * model of section 4.3.
 */
int modbus_backend_read_bits(const struct modbus_backend *, modbus_area_t,
    uint16_t addr, uint16_t nbits, uint8_t *out);
int modbus_backend_write_bits(const struct modbus_backend *, modbus_area_t,
    uint16_t addr, uint16_t nbits, const uint8_t *in);
int modbus_backend_read_registers(const struct modbus_backend *,
    modbus_area_t, uint16_t addr, uint16_t nregs, uint16_t *out);
int modbus_backend_write_registers(const struct modbus_backend *,
    modbus_area_t, uint16_t addr, uint16_t nregs, const uint16_t *in);
int modbus_backend_mask_register(struct modbus_backend *, uint16_t addr,
    uint16_t and_mask, uint16_t or_mask);

#endif /* _SYS_NET_MODBUS_MODBUS_BACKEND_H_ */
