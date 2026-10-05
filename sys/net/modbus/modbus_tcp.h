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

#ifndef _SYS_NET_MODBUS_MODBUS_TCP_H_
#define	_SYS_NET_MODBUS_MODBUS_TCP_H_

#include <sys/param.h>
#include <sys/types.h>

#include "modbus.h"

/*
 * Modbus/TCP framing: an MBAP header followed by the PDU.
 *
 * Section 3.1 of the MODBUS Messaging on TCP/IP Implementation Guide V1.0b:
 * the header is transaction identifier (2), protocol identifier (2), length
 * (2) and unit identifier (1).  The length field counts the bytes that
 * follow it, that is the unit identifier and the PDU, and the protocol
 * identifier is zero for Modbus.
 *
 * TCP is a byte stream: one read may carry part of a header, several whole
 * requests, or any mixture.  The parser below is therefore fed one byte at a
 * time and knows nothing about how the bytes were split; the node decides
 * when to start parsing and when to read more.
 */
#define	MODBUS_TCP_MBAP_PROTOCOL	0

/*
 * Bytes of the header that precede the length field.  The length field
 * itself counts the bytes that follow it, the unit identifier included, so
 * a frame is this many bytes plus the length value.
 */
#define	MODBUS_TCP_MBAP_FIXED	6

enum modbus_tcp_result {
	MODBUS_TCP_ACCEPTED = 0,	/* byte stored, no frame yet */
	MODBUS_TCP_FRAME,		/* modbus_tcp_frame() returns a PDU */
	MODBUS_TCP_ERROR		/* unusable stream, resynchronise */
};

enum modbus_tcp_state {
	MODBUS_TCP_IDLE = 0,
	MODBUS_TCP_HEADER,
	MODBUS_TCP_BODY
};

struct modbus_tcp {
	uint8_t		buf[MODBUS_TCP_ADU_MAXLEN];
	uint16_t	len;		/* bytes stored */
	uint16_t	expected;	/* total frame size once known */
	uint16_t	tid;		/* transaction identifier */
	uint8_t		unit;		/* unit identifier */
	uint8_t		state;		/* enum modbus_tcp_state */
};

void modbus_tcp_init(struct modbus_tcp *);
enum modbus_tcp_result modbus_tcp_input(struct modbus_tcp *, uint8_t byte);

/*
 * The request PDU of a complete frame, without the header.  Only available
 * after MODBUS_TCP_FRAME; *tid and *unit report what has to be echoed back.
 */
const uint8_t *modbus_tcp_frame(struct modbus_tcp *, uint16_t *len,
    uint16_t *tid, uint8_t *unit);
void modbus_tcp_release(struct modbus_tcp *);

/*
 * Build a complete ADU: transaction identifier, protocol identifier, length,
 * unit identifier and the PDU.  Returns the number of bytes written, or 0 if
 * the buffer is too small.
 */
size_t modbus_tcp_encode(uint8_t *out, size_t outcap, uint16_t tid,
    uint8_t unit, const uint8_t *pdu, size_t pdulen);

#endif /* _SYS_NET_MODBUS_MODBUS_TCP_H_ */
