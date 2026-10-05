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
 * Function code handlers.
 *
 * Each handler validates the request it is given before touching a backend,
 * so no request length, quantity or byte count from the wire can reach the
 * backend unchecked.  Section references are to the MODBUS Application
 * Protocol Specification V1.1b3.
 */

#include <sys/param.h>
#include "modbus_sys.h"
#include <sys/errno.h>

#include "modbus.h"
#include "modbus_backend.h"
#include "modbus_pdu.h"
#include "modbus_fc.h"

/* Section 6.5: a coil is written with one of exactly two values. */
#define	MODBUS_COIL_OFF		0x0000
#define	MODBUS_COIL_ON		0xff00

static int
modbus_fc_fail(struct modbus_pdu_buf *rsp, uint8_t fc, modbus_exc_t exc)
{

	modbus_pdu_exception(rsp, fc, exc);

	return (exc);
}

/*
 * Sections 6.1 and 6.2: read a run of bits.  The response carries one byte
 * per eight bits with the lowest addressed bit in the least significant
 * position, padded with zero bits.
 */
static int
modbus_fc_read_bits(struct modbus_backend *be,
    const struct modbus_pdu *req, struct modbus_pdu_buf *rsp,
    modbus_area_t area)
{
	uint8_t data[MODBUS_MAX_READ_BITS / 8];
	uint16_t addr, qty;
	uint8_t nbytes;
	int error;

	if (req->len != 4)
		return (modbus_fc_fail(rsp, req->fc, MODBUS_EX_ILLEGAL_VALUE));

	addr = modbus_get_u16(req->data);
	qty = modbus_get_u16(req->data + 2);
	if (qty == 0 || qty > MODBUS_MAX_READ_BITS)
		return (modbus_fc_fail(rsp, req->fc, MODBUS_EX_ILLEGAL_VALUE));

	memset(data, 0, sizeof(data));
	if ((error = modbus_backend_read_bits(be, area, addr, qty, data)) != 0)
		return (modbus_fc_fail(rsp, req->fc,
		    (modbus_exc_t)error));

	nbytes = (uint8_t)((qty + 7) / 8);

	modbus_pdu_buf_set_fc(rsp, req->fc);
	modbus_pdu_buf_append(rsp, &nbytes, 1);
	modbus_pdu_buf_append(rsp, data, nbytes);

	return (MODBUS_EX_OK);
}

/*
 * Sections 6.3 and 6.4: read a run of registers.  Byte count is two per
 * register and must match the quantity.
 */
static int
modbus_fc_read_registers(struct modbus_backend *be,
    const struct modbus_pdu *req, struct modbus_pdu_buf *rsp,
    modbus_area_t area)
{
	uint16_t regs[MODBUS_MAX_READ_REGS];
	uint8_t wire[2 * MODBUS_MAX_READ_REGS];
	uint16_t addr, qty, i;
	uint8_t nbytes;
	int error;

	if (req->len != 4)
		return (modbus_fc_fail(rsp, req->fc, MODBUS_EX_ILLEGAL_VALUE));

	addr = modbus_get_u16(req->data);
	qty = modbus_get_u16(req->data + 2);
	if (qty == 0 || qty > MODBUS_MAX_READ_REGS)
		return (modbus_fc_fail(rsp, req->fc, MODBUS_EX_ILLEGAL_VALUE));

	if ((error = modbus_backend_read_registers(be, area, addr, qty,
	    regs)) != 0)
		return (modbus_fc_fail(rsp, req->fc, (modbus_exc_t)error));

	for (i = 0; i < qty; i++)
		modbus_put_u16(wire + 2 * i, regs[i]);

	nbytes = (uint8_t)(2 * qty);

	modbus_pdu_buf_set_fc(rsp, req->fc);
	modbus_pdu_buf_append(rsp, &nbytes, 1);
	modbus_pdu_buf_append(rsp, wire, nbytes);

	return (MODBUS_EX_OK);
}

/* Section 6.5: write one coil; the request is echoed unchanged. */
static int
modbus_fc_write_single_coil(struct modbus_backend *be,
    const struct modbus_pdu *req, struct modbus_pdu_buf *rsp)
{
	uint8_t value[2];
	uint16_t addr, on;
	int error;

	if (req->len != 4)
		return (modbus_fc_fail(rsp, req->fc, MODBUS_EX_ILLEGAL_VALUE));

	addr = modbus_get_u16(req->data);
	on = modbus_get_u16(req->data + 2);
	if (on != MODBUS_COIL_OFF && on != MODBUS_COIL_ON)
		return (modbus_fc_fail(rsp, req->fc, MODBUS_EX_ILLEGAL_VALUE));

	value[0] = (on == MODBUS_COIL_ON) ? 1 : 0;
	if ((error = modbus_backend_write_bits(be, MODBUS_AREA_COIL, addr, 1,
	    value)) != 0)
		return (modbus_fc_fail(rsp, req->fc, (modbus_exc_t)error));

	modbus_pdu_buf_set_fc(rsp, req->fc);
	modbus_pdu_buf_append(rsp, req->data, 4);

	return (MODBUS_EX_OK);
}

/* Section 6.6: write one register; the request is echoed unchanged. */
static int
modbus_fc_write_single_register(struct modbus_backend *be,
    const struct modbus_pdu *req, struct modbus_pdu_buf *rsp)
{
	uint16_t addr, value;
	int error;

	if (req->len != 4)
		return (modbus_fc_fail(rsp, req->fc, MODBUS_EX_ILLEGAL_VALUE));

	addr = modbus_get_u16(req->data);
	value = modbus_get_u16(req->data + 2);

	if ((error = modbus_backend_write_registers(be,
	    MODBUS_AREA_HOLDING_REG, addr, 1, &value)) != 0)
		return (modbus_fc_fail(rsp, req->fc, (modbus_exc_t)error));

	modbus_pdu_buf_set_fc(rsp, req->fc);
	modbus_pdu_buf_append(rsp, req->data, 4);

	return (MODBUS_EX_OK);
}

/*
 * Section 6.11: write a run of coils.  The byte count must match the
 * quantity exactly; anything else is an invalid value rather than a short
 * read, because the padding bits are defined to be zero.
 */
static int
modbus_fc_write_multiple_coils(struct modbus_backend *be,
    const struct modbus_pdu *req, struct modbus_pdu_buf *rsp)
{
	uint8_t echo[4];
	uint16_t addr, qty, nbytes;
	int error;

	if (req->len < 6)
		return (modbus_fc_fail(rsp, req->fc, MODBUS_EX_ILLEGAL_VALUE));

	addr = modbus_get_u16(req->data);
	qty = modbus_get_u16(req->data + 2);
	nbytes = req->data[4];
	if (qty == 0 || qty > MODBUS_MAX_WRITE_BITS)
		return (modbus_fc_fail(rsp, req->fc, MODBUS_EX_ILLEGAL_VALUE));
	if (nbytes != (uint16_t)((qty + 7) / 8) ||
	    req->len != 5u + nbytes)
		return (modbus_fc_fail(rsp, req->fc, MODBUS_EX_ILLEGAL_VALUE));

	if ((error = modbus_backend_write_bits(be, MODBUS_AREA_COIL, addr, qty,
	    req->data + 5)) != 0)
		return (modbus_fc_fail(rsp, req->fc, (modbus_exc_t)error));

	modbus_put_u16(echo, addr);
	modbus_put_u16(echo + 2, qty);

	modbus_pdu_buf_set_fc(rsp, req->fc);
	modbus_pdu_buf_append(rsp, echo, sizeof(echo));

	return (MODBUS_EX_OK);
}

/* Section 6.12: write a run of registers; the address and quantity echo. */
static int
modbus_fc_write_multiple_registers(struct modbus_backend *be,
    const struct modbus_pdu *req, struct modbus_pdu_buf *rsp)
{
	uint8_t echo[4];
	uint16_t regs[MODBUS_MAX_WRITE_REGS];
	uint16_t addr, qty, nbytes, i;
	int error;

	if (req->len < 7)
		return (modbus_fc_fail(rsp, req->fc, MODBUS_EX_ILLEGAL_VALUE));

	addr = modbus_get_u16(req->data);
	qty = modbus_get_u16(req->data + 2);
	nbytes = req->data[4];
	if (qty == 0 || qty > MODBUS_MAX_WRITE_REGS)
		return (modbus_fc_fail(rsp, req->fc, MODBUS_EX_ILLEGAL_VALUE));
	if (nbytes != (uint16_t)(2 * qty) || req->len != 5u + nbytes)
		return (modbus_fc_fail(rsp, req->fc, MODBUS_EX_ILLEGAL_VALUE));

	for (i = 0; i < qty; i++)
		regs[i] = modbus_get_u16(req->data + 5 + 2 * i);

	if ((error = modbus_backend_write_registers(be,
	    MODBUS_AREA_HOLDING_REG, addr, qty, regs)) != 0)
		return (modbus_fc_fail(rsp, req->fc, (modbus_exc_t)error));

	modbus_put_u16(echo, addr);
	modbus_put_u16(echo + 2, qty);

	modbus_pdu_buf_set_fc(rsp, req->fc);
	modbus_pdu_buf_append(rsp, echo, sizeof(echo));

	return (MODBUS_EX_OK);
}

/*
 * Section 6.16: mask write.  The response echoes the request, so the three
 * fields are validated by length alone.
 */
static int
modbus_fc_mask_write_register(struct modbus_backend *be,
    const struct modbus_pdu *req, struct modbus_pdu_buf *rsp)
{
	uint8_t data[6];
	uint16_t addr;
	int error;

	if (req->len != 6)
		return (modbus_fc_fail(rsp, req->fc, MODBUS_EX_ILLEGAL_VALUE));

	addr = modbus_get_u16(req->data);
	modbus_put_u16(data, addr);
	modbus_put_u16(data + 2, modbus_get_u16(req->data + 2));
	modbus_put_u16(data + 4, modbus_get_u16(req->data + 4));

	if ((error = modbus_backend_mask_register(be, addr,
	    modbus_get_u16(req->data + 2), modbus_get_u16(req->data + 4))) != 0)
		return (modbus_fc_fail(rsp, req->fc, (modbus_exc_t)error));

	modbus_pdu_buf_set_fc(rsp, req->fc);
	modbus_pdu_buf_append(rsp, data, sizeof(data));

	return (MODBUS_EX_OK);
}

/*
 * Section 6.17: one transaction that writes and then reads.  The write is
 * performed first, and the response carries only the read data.
 */
static int
modbus_fc_read_write_multiple_registers(struct modbus_backend *be,
    const struct modbus_pdu *req, struct modbus_pdu_buf *rsp)
{
	uint16_t regs[MODBUS_MAX_READ_REGS];
	uint8_t wire[2 * MODBUS_MAX_READ_REGS];
	uint16_t raddr, rqty, waddr, wqty, nbytes, i;
	uint8_t bcnt;
	int error;

	if (req->len < 11)
		return (modbus_fc_fail(rsp, req->fc, MODBUS_EX_ILLEGAL_VALUE));

	raddr = modbus_get_u16(req->data);
	rqty = modbus_get_u16(req->data + 2);
	waddr = modbus_get_u16(req->data + 4);
	wqty = modbus_get_u16(req->data + 6);
	nbytes = req->data[8];

	if (rqty == 0 || rqty > MODBUS_MAX_READ_REGS)
		return (modbus_fc_fail(rsp, req->fc, MODBUS_EX_ILLEGAL_VALUE));
	if (wqty == 0 || wqty > MODBUS_MAX_RW_REGS)
		return (modbus_fc_fail(rsp, req->fc, MODBUS_EX_ILLEGAL_VALUE));
	if (nbytes != (uint16_t)(2 * wqty) || req->len != 9u + nbytes)
		return (modbus_fc_fail(rsp, req->fc, MODBUS_EX_ILLEGAL_VALUE));

	for (i = 0; i < wqty; i++)
		regs[i] = modbus_get_u16(req->data + 9 + 2 * i);

	if ((error = modbus_backend_write_registers(be,
	    MODBUS_AREA_HOLDING_REG, waddr, wqty, regs)) != 0)
		return (modbus_fc_fail(rsp, req->fc, (modbus_exc_t)error));

	if ((error = modbus_backend_read_registers(be,
	    MODBUS_AREA_HOLDING_REG, raddr, rqty, regs)) != 0)
		return (modbus_fc_fail(rsp, req->fc, (modbus_exc_t)error));

	for (i = 0; i < rqty; i++)
		modbus_put_u16(wire + 2 * i, regs[i]);

	bcnt = (uint8_t)(2 * rqty);

	modbus_pdu_buf_set_fc(rsp, req->fc);
	modbus_pdu_buf_append(rsp, &bcnt, 1);
	modbus_pdu_buf_append(rsp, wire, bcnt);

	return (MODBUS_EX_OK);
}

/*
 * Dispatch one request.  A short switch keeps each function's validation
 * next to its handler, which is easier to check against the specification
 * than a table of pointers would be.
 */
int
modbus_fc_dispatch(struct modbus_backend *be,
    const struct modbus_pdu *req, struct modbus_pdu_buf *rsp)
{

	switch (req->fc) {
	case MODBUS_FC_READ_COILS:
		return (modbus_fc_read_bits(be, req, rsp, MODBUS_AREA_COIL));
	case MODBUS_FC_READ_DISCRETE_IN:
		return (modbus_fc_read_bits(be, req, rsp,
		    MODBUS_AREA_DISCRETE_IN));
	case MODBUS_FC_READ_HOLDING_REG:
		return (modbus_fc_read_registers(be, req, rsp,
		    MODBUS_AREA_HOLDING_REG));
	case MODBUS_FC_READ_INPUT_REG:
		return (modbus_fc_read_registers(be, req, rsp,
		    MODBUS_AREA_INPUT_REG));
	case MODBUS_FC_WRITE_SINGLE_COIL:
		return (modbus_fc_write_single_coil(be, req, rsp));
	case MODBUS_FC_WRITE_SINGLE_REG:
		return (modbus_fc_write_single_register(be, req, rsp));
	case MODBUS_FC_WRITE_MULTI_COIL:
		return (modbus_fc_write_multiple_coils(be, req, rsp));
	case MODBUS_FC_WRITE_MULTI_REG:
		return (modbus_fc_write_multiple_registers(be, req, rsp));
	case MODBUS_FC_MASK_WRITE_REG:
		return (modbus_fc_mask_write_register(be, req, rsp));
	case MODBUS_FC_READ_WRITE_MULTI_REG:
		return (modbus_fc_read_write_multiple_registers(be, req, rsp));
	default:
		/* Section 7: an unsupported function code. */
		return (modbus_fc_fail(rsp, req->fc,
		    MODBUS_EX_ILLEGAL_FUNCTION));
	}
}
