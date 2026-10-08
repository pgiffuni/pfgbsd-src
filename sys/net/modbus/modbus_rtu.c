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
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
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
 * RTU framing state machine with timer support.
 * t1.5: inter-character timeout (1.5 character times)
 * t3.5: inter-frame gap (3.5 character times)
 */

#include <sys/param.h>
#include <sys/types.h>
#include <sys/callout.h>
#include <sys/time.h>

#include <net/modbus/modbus_sys.h>

#include <net/modbus/modbus.h>
#include <net/modbus/modbus_crc.h>
#include <net/modbus/modbus_rtu.h>

#define	MODBUS_RTU_MINLEN		4

void
modbus_rtu_init(struct modbus_rtu *rtu)
{

	memset(rtu, 0, sizeof(*rtu));
}

/*
 * Drop the current frame and reset to idle state.
 */
void
modbus_rtu_release(struct modbus_rtu *rtu)
{

	rtu->len = 0;
	rtu->state = MODBUS_RTU_IDLE;
}

/*
 * Arm the t3.5 inter-frame gap timer.  Must be called after a frame is
 * fully processed to detect the inter-frame gap.
 */
void
modbus_rtu_arm_t35(struct modbus_rtu *rtu, uint32_t t35_ns,
    void (*gap_cb)(struct modbus_rtu *, uint32_t))
{
	rtu->t35_cb = gap_cb;
	rtu->t35_ns = t35_ns;
}

/*
 * The CRC covers everything except the two CRC bytes themselves, and the low
 * order byte was transmitted first, so the check compares the two bytes in
 * that order.
 */
static int
modbus_rtu_crc_ok(const struct modbus_rtu *rtu)
{
	uint16_t crc;

	if (rtu->len < 4)
		return (0);

	crc = modbus_crc16(rtu->buf, rtu->len - 2);

	return ((crc & 0xff) == rtu->buf[rtu->len - 2] &&
	    (crc >> 8) == rtu->buf[rtu->len - 1]);
}

const uint8_t *
modbus_rtu_frame(struct modbus_rtu *rtu, uint16_t *len)
{
	*len = 0;
	if (rtu->len < 4)
		return (NULL);

	if (!modbus_rtu_crc_ok(rtu))
		return (NULL);

	*len = rtu->len;

	return (rtu->buf);
}

/*
 * Check if the t3.5 timer has expired.  Called from the t3.5 timer callback.
 * Returns MODBUS_RTU_DISCARDED if the frame should be dropped, otherwise
 * MODBUS_RTU_ACCEPTED.
 */
enum modbus_rtu_result
modbus_rtu_t35_expired(struct modbus_rtu *rtu)
{
	if (rtu->t35_cb != NULL) {
		rtu->t35_cb(rtu, rtu->t35_ns);
		return (MODBUS_RTU_DISCARDED);
	}
	return (MODBUS_RTU_ACCEPTED);
}
