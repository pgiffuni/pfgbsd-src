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
 * Canonical PDU representation.
 *
 * MODBUS Application Protocol Specification V1.1b3, section 4.1: a PDU is a
 * function code followed by function specific data; section 4.2: every
 * quantity is transferred most significant byte first.
 *
 * Nothing in this file knows about MBAP, CRC, LRC, ASCII encoding or mbufs.
 */

#include <sys/param.h>
#include "modbus_sys.h"
#include <sys/errno.h>

#include "modbus.h"
#include "modbus_pdu.h"

/*
 * Turn a byte range into a validated PDU view.  The caller keeps ownership
 * of the buffer; the view is only valid while it is.
 */
int
modbus_pdu_parse(const uint8_t *p, size_t len, struct modbus_pdu *pdu)
{

	/* Section 4.1: a PDU is one function code plus at most 252 bytes. */
	if (p == NULL || len < 1 || len > MODBUS_PDU_MAXLEN)
		return (EINVAL);

	/* Function code 0 is not valid; 0x80 and above are exception codes. */
	if (p[0] < MODBUS_FC_MIN || p[0] > MODBUS_FC_MAX)
		return (EINVAL);

	pdu->fc = p[0];
	pdu->data = p + 1;
	pdu->len = (uint16_t)(len - 1);

	return (0);
}

/* Prepare an empty response; its function code is filled in later. */
void
modbus_pdu_buf_init(struct modbus_pdu_buf *pb)
{

	pb->len = 0;
	pb->buf[0] = MODBUS_FC_MIN;
}

/*
 * Append data to a response.  Every response builder goes through here so
 * that no function code can overrun the 253 byte PDU limit by accident.
 */
int
modbus_pdu_buf_append(struct modbus_pdu_buf *pb, const void *data, size_t len)
{
	/*
	 * The first half of the test also guards the invariant, so a bug
	 * elsewhere cannot turn the subtraction into a huge value.
	 */
	if (pb->len > sizeof(pb->buf) || len > sizeof(pb->buf) - pb->len)
		return (EINVAL);

	if (len != 0)
		memmove(pb->buf + pb->len, data, len);
	pb->len += (uint16_t)len;

	return (0);
}

int
modbus_pdu_buf_set_fc(struct modbus_pdu_buf *pb, uint8_t fc)
{

	if (fc < MODBUS_FC_MIN || fc > MODBUS_FC_MAX)
		return (EINVAL);

	pb->buf[0] = fc;
	pb->len = 1;

	return (0);
}

/*
 * Build an exception response, section 7: the function code with bit 7 set
 * followed by the exception code.
 */
void
modbus_pdu_exception(struct modbus_pdu_buf *pb, uint8_t fc, modbus_exc_t exc)
{

	pb->buf[0] = (uint8_t)(fc | MODBUS_FC_MASK);
	pb->buf[1] = (uint8_t)exc;
	pb->len = 2;
}
