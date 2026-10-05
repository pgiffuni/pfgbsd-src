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
 * RTU framing state machine.  See modbus_rtu.h for the specification
 * references; nothing here allocates and no timer is owned here.
 */

#include <sys/param.h>
#include "modbus_sys.h"

#include "modbus.h"
#include "modbus_crc.h"
#include "modbus_rtu.h"

#define	MODBUS_BAUD_FIXED	19200
#define	NSEC_PER_SEC		1000000000U

/* The shortest legal frame is address, function code and two CRC bytes. */
#define	MODBUS_RTU_MINLEN	4

uint32_t
modbus_rtu_t15_ns(uint32_t baud, uint8_t bits_per_char)
{
	uint64_t ns;

	if (baud == 0)
		return (0);

	/* Above 19200 Bd the specification fixes the timers. */
	if (baud > MODBUS_BAUD_FIXED)
		return (750000U);

	ns = (uint64_t)bits_per_char * NSEC_PER_SEC / baud;

	return ((uint32_t)(3 * ns / 2));
}

uint32_t
modbus_rtu_t35_ns(uint32_t baud, uint8_t bits_per_char)
{
	uint64_t ns;

	if (baud == 0)
		return (0);

	if (baud > MODBUS_BAUD_FIXED)
		return (1750000U);

	ns = (uint64_t)bits_per_char * NSEC_PER_SEC / baud;

	return ((uint32_t)(7 * ns / 2));
}

void
modbus_rtu_init(struct modbus_rtu *rtu)
{

	memset(rtu, 0, sizeof(*rtu));
	rtu->state = MODBUS_RTU_IDLE;
}

/*
 * The CRC covers everything except the two CRC bytes themselves, and the low
 * order byte was transmitted first, so the wire bytes compare in that order.
 */
static int
modbus_rtu_crc_ok(const struct modbus_rtu *rtu)
{
	uint16_t crc;

	if (rtu->len < MODBUS_RTU_MINLEN)
		return (0);

	crc = modbus_crc16(rtu->buf, rtu->len - 2);

	return ((crc & 0xff) == rtu->buf[rtu->len - 2] &&
	    (crc >> 8) == rtu->buf[rtu->len - 1]);
}

const uint8_t *
modbus_rtu_frame(struct modbus_rtu *rtu, uint16_t *len)
{

	if (rtu->state != MODBUS_RTU_READY) {
		*len = 0;
		return (NULL);
	}

	*len = rtu->len;

	return (rtu->buf);
}

void
modbus_rtu_release(struct modbus_rtu *rtu)
{

	rtu->len = 0;
	rtu->state = MODBUS_RTU_IDLE;
}

/* Drop a frame that was being collected. */
static enum modbus_rtu_result
modbus_rtu_drop(struct modbus_rtu *rtu)
{

	rtu->len = 0;
	rtu->state = MODBUS_RTU_IDLE;

	return (MODBUS_RTU_DISCARDED);
}

enum modbus_rtu_result
modbus_rtu_gap(struct modbus_rtu *rtu, uint32_t t15_ns)
{

	(void)t15_ns;

	if (rtu->state == MODBUS_RTU_IDLE)
		return (MODBUS_RTU_ACCEPTED);

	/*
	 * A complete frame survives its own trailing silence: t3.5 only
	 * delimits the next frame, it does not invalidate this one.
	 */
	if (rtu->state == MODBUS_RTU_READY)
		return (MODBUS_RTU_ACCEPTED);

	return (modbus_rtu_drop(rtu));
}

enum modbus_rtu_result
modbus_rtu_input(struct modbus_rtu *rtu, uint8_t byte, uint64_t now,
    uint32_t t15_ns)
{
	int dropped = 0;

	/*
	 * Section 2.5.1.1: a silent interval longer than 1.5 character times
	 * inside a frame means the frame is incomplete and must be dropped.
	 * The byte that arrived after the silence starts the next frame.
	 */
	if (rtu->state == MODBUS_RTU_RECEIVING && t15_ns != 0 &&
	    now > rtu->last &&
	    (uint64_t)(now - rtu->last) > (uint64_t)t15_ns) {
		rtu->len = 0;
		rtu->state = MODBUS_RTU_IDLE;
		dropped = 1;
	}

	if (rtu->state == MODBUS_RTU_READY)
		modbus_rtu_release(rtu);

	rtu->last = now;
	rtu->state = MODBUS_RTU_RECEIVING;

	if (rtu->len >= MODBUS_RTU_ADU_MAXLEN) {
		/*
		 * The frame cannot grow any further, so stop storing bytes
		 * until the silence clears it.
		 */
		return (MODBUS_RTU_DISCARDED);
	}

	rtu->buf[rtu->len++] = byte;

	if (rtu->len >= MODBUS_RTU_MINLEN && modbus_rtu_crc_ok(rtu)) {
		rtu->state = MODBUS_RTU_READY;
		return (MODBUS_RTU_FRAME);
	}

	return (dropped ? MODBUS_RTU_DISCARDED : MODBUS_RTU_ACCEPTED);
}
