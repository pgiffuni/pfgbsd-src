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

#ifndef _SYS_NET_MODBUS_MODBUS_PDU_H_
#define	_SYS_NET_MODBUS_MODBUS_PDU_H_

#include <sys/param.h>
#include <sys/types.h>
#include <sys/endian.h>

#include <net/modbus/modbus.h>

/* Borrowed, validated view of a received PDU. */
struct modbus_pdu {
	uint8_t		 fc;		/* function code */
	uint16_t		 len;		/* data length, 0..252 */
	const uint8_t	*data;		/* pdu + 1 */
};

/* Owned buffer used to build a response; never exceeds the PDU limit. */
struct modbus_pdu_buf {
	uint8_t		 buf[MODBUS_PDU_MAXLEN];
	uint16_t		 len;
};

/*
 * Section 4.2: every quantity travels most significant byte first.  These
 * helpers keep the byte order explicit and work on any addressable buffer,
 * so no packet data is ever cast to a C structure.
 */
static __inline uint16_t
modbus_get_u16(const uint8_t *p)
{

	return (be16dec(p));
}

static __inline void
modbus_put_u16(uint8_t *p, uint16_t v)
{

	be16enc(p, v);
}

int modbus_pdu_parse(const uint8_t *p, size_t len, struct modbus_pdu *pdu);
void modbus_pdu_buf_init(struct modbus_pdu_buf *pb);
int modbus_pdu_buf_set_fc(struct modbus_pdu_buf *pb, uint8_t fc);
int modbus_pdu_buf_append(struct modbus_pdu_buf *pb, const void *data,
    size_t len);
void modbus_pdu_exception(struct modbus_pdu_buf *pb, uint8_t fc,
    modbus_exc_t exc);

#endif /* _SYS_NET_MODBUS_MODBUS_PDU_H_ */
