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

#ifndef _SYS_NET_MODBUS_MODBUS_RTU_H_
#define	_SYS_NET_MODBUS_MODBUS_RTU_H_

#include <sys/param.h>
#include <sys/types.h>

#include "modbus.h"

/*
 * RTU framing, section 2.5.1.1 of the MODBUS over Serial Line Specification
 * V1.02: a frame is "address, function code, data, CRC", frames are separated
 * by at least 3.5 character times of silence, and a silence longer than 1.5
 * character times inside a frame means the frame is incomplete and must be
 * discarded.
 *
 * The parser below is pure state: it is fed one byte at a time with a
 * timestamp, and it never touches a timer, an mbuf or a netgraph hook.  The
 * caller owns the t1.5 and t3.5 deadlines, which is what makes the framing
 * testable without a serial line and without waiting in real time.
 */
enum modbus_rtu_result {
	MODBUS_RTU_ACCEPTED = 0,	/* byte stored, no frame yet */
	MODBUS_RTU_FRAME,		/* modbus_rtu_frame() returns a frame */
	MODBUS_RTU_DISCARDED		/* a partial frame was thrown away */
};

/* Parser states; a frame stays readable until the next byte or release. */
enum modbus_rtu_state {
	MODBUS_RTU_IDLE = 0,
	MODBUS_RTU_RECEIVING,
	MODBUS_RTU_READY
};

struct modbus_rtu {
	uint8_t		buf[MODBUS_RTU_ADU_MAXLEN];
	uint16_t	len;		/* bytes stored so far */
	uint64_t	last;		/* timestamp of the last byte */
	uint8_t		state;		/* enum modbus_rtu_state */
};

/*
 * Character times.  Below 19200 Bd they follow the configured line; above it
 * the specification fixes t1.5 at 750 us and t3.5 at 1750 us, because the
 * computed values would be shorter than the jitter of a software timer.
 * Both return nanoseconds.
 */
uint32_t modbus_rtu_t15_ns(uint32_t baud, uint8_t bits_per_char);
uint32_t modbus_rtu_t35_ns(uint32_t baud, uint8_t bits_per_char);

void modbus_rtu_init(struct modbus_rtu *);

/*
 * Feed one byte.  "now" and "t15_ns" are in the same time base; a gap longer
 * than t1.5 before this byte discards whatever was being collected.
 */
enum modbus_rtu_result modbus_rtu_input(struct modbus_rtu *, uint8_t byte,
    uint64_t now, uint32_t t15_ns);

/* Report an expired inter-character timer, if a frame was in progress. */
enum modbus_rtu_result modbus_rtu_gap(struct modbus_rtu *, uint32_t t15_ns);

/*
 * The complete frame, including the address and the two CRC bytes.  Only the
 * low order CRC byte is transmitted first, so the check compares the two
 * bytes in that order.  Returns NULL when no verified frame is ready.
 */
const uint8_t *modbus_rtu_frame(struct modbus_rtu *, uint16_t *len);
void modbus_rtu_release(struct modbus_rtu *);

#endif /* _SYS_NET_MODBUS_MODBUS_RTU_H_ */
