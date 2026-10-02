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
 * MBAP framing state machine.  See modbus_tcp.h for the specification
 * references.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/endian.h>

#include <net/modbus/modbus.h>
#include <net/modbus/modbus_pdu.h>
#include <net/modbus/modbus_tcp.h>

void
modbus_tcp_init(struct modbus_tcp *tcp)
{

	memset(tcp, 0, sizeof(*tcp));
	tcp->state = MODBUS_TCP_IDLE;
}

void
modbus_tcp_release(struct modbus_tcp *tcp)
{

	tcp->len = 0;
	tcp->expected = 0;
	tcp->state = MODBUS_TCP_IDLE;
}

const uint8_t *
modbus_tcp_frame(struct modbus_tcp *tcp, uint16_t *len, uint16_t *tid,
    uint8_t *unit)
{

	if (tcp->state != MODBUS_TCP_BODY || tcp->len < MODBUS_TCP_MBAPLEN) {
		*len = 0;
		return (NULL);
	}

	*len = (uint16_t)(tcp->len - MODBUS_TCP_MBAPLEN);
	*tid = tcp->tid;
	*unit = tcp->unit;

	return (tcp->buf + MODBUS_TCP_MBAPLEN);
}

/*
 * Validate the header once it has arrived.  The length field must be at least
 * the unit identifier plus a one byte PDU, and at most a unit identifier plus
 * the largest PDU; the protocol identifier must be zero.  Anything else means
 * the stream cannot be trusted, so the parser is reset and the caller is told
 * to resynchronise.
 */
static enum modbus_tcp_result
modbus_tcp_check_header(struct modbus_tcp *tcp)
{
	uint16_t pid, len;

	tcp->tid = modbus_get_u16(tcp->buf);
	pid = modbus_get_u16(tcp->buf + 2);
	len = modbus_get_u16(tcp->buf + 4);

	if (pid != MODBUS_TCP_MBAP_PROTOCOL)
		return (MODBUS_TCP_ERROR);
	if (len < 2 || len > MODBUS_TCP_ADU_MAXLEN - MODBUS_TCP_MBAP_FIXED)
		return (MODBUS_TCP_ERROR);

	tcp->expected = (uint16_t)(MODBUS_TCP_MBAP_FIXED + len);

	return (MODBUS_TCP_ACCEPTED);
}

enum modbus_tcp_result
modbus_tcp_input(struct modbus_tcp *tcp, uint8_t byte)
{
	enum modbus_tcp_result error;

	/* A frame stays readable until the caller releases it. */
	if (tcp->state == MODBUS_TCP_BODY && tcp->expected != 0 &&
	    tcp->len >= tcp->expected)
		modbus_tcp_release(tcp);

	if (tcp->len >= MODBUS_TCP_ADU_MAXLEN) {
		modbus_tcp_release(tcp);
		return (MODBUS_TCP_ERROR);
	}

	tcp->buf[tcp->len++] = byte;

	if (tcp->state == MODBUS_TCP_IDLE) {
		/* Any byte can start a header; it is validated later. */
		tcp->state = MODBUS_TCP_HEADER;
		tcp->expected = 0;
	}

	/*
	 * The length field is the last thing the fixed header needs, so the
	 * stream is checked after six bytes and not one later.
	 */
	if (tcp->len < MODBUS_TCP_MBAP_FIXED)
		return (MODBUS_TCP_ACCEPTED);

	if (tcp->expected == 0) {
		error = modbus_tcp_check_header(tcp);
		if (error != MODBUS_TCP_ACCEPTED) {
			modbus_tcp_release(tcp);
			return (error);
		}
	}

	if (tcp->len < tcp->expected)
		return (MODBUS_TCP_ACCEPTED);

	/* The last byte of the fixed header is the unit identifier. */
	tcp->unit = tcp->buf[MODBUS_TCP_MBAP_FIXED];

	/*
	 * The whole ADU has arrived.  The body state is what
	 * modbus_tcp_frame() looks for, and it also marks the frame as
	 * complete, so the expected size is kept for the release above.
	 */
	tcp->state = MODBUS_TCP_BODY;

	return (MODBUS_TCP_FRAME);
}

size_t
modbus_tcp_encode(uint8_t *out, size_t outcap, uint16_t tid, uint8_t unit,
    const uint8_t *pdu, size_t pdulen)
{
	size_t adulen;

	if (pdulen == 0 || pdulen > MODBUS_PDU_MAXLEN)
		return (0);

	adulen = MODBUS_TCP_MBAPLEN + pdulen;
	if (outcap < adulen)
		return (0);

	modbus_put_u16(out, tid);
	modbus_put_u16(out + 2, MODBUS_TCP_MBAP_PROTOCOL);
	modbus_put_u16(out + 4, (uint16_t)(1 + pdulen));
	out[6] = unit;
	bcopy(pdu, out + MODBUS_TCP_MBAPLEN, pdulen);

	return (adulen);
}
