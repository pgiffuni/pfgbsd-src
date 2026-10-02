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

#ifndef _SYS_NET_MODBUS_MODBUS_CLIENT_H_
#define	_SYS_NET_MODBUS_MODBUS_CLIENT_H_

#include <sys/param.h>
#include <sys/types.h>

#include <net/modbus/modbus.h>
#include <net/modbus/modbus_pdu.h>

/*
 * Client side transaction handling.
 *
 * Section 4.5 of the MODBUS Application Protocol Specification V1.1b3
 * describes the transaction: one request, one response, and nothing else may
 * be in flight until it completes.  This module owns exactly that state; it
 * does not own a timer, so it can be driven and tested without one.  The node
 * arms a callout and calls modbus_client_expired() when it wants a timeout.
 */

#define	MODBUS_CLIENT_MAX_PDU	MODBUS_PDU_MAXLEN

enum modbus_client_result {
	MODBUS_CLIENT_IDLE = 0,		/* nothing outstanding */
	MODBUS_CLIENT_PENDING,		/* a request is outstanding */
	MODBUS_CLIENT_DONE,		/* a response was accepted */
	MODBUS_CLIENT_EXPIRED,		/* no response arrived in time */
	MODBUS_CLIENT_FAILED		/* the response was not usable */
};

struct modbus_client {
	uint8_t		 request[MODBUS_CLIENT_MAX_PDU];
	uint16_t	 reqlen;	/* bytes of request to send */
	uint16_t	 unit;		/* unit identifier to address */
	uint16_t	 tid;		/* transaction identifier, Modbus/TCP */
	struct modbus_pdu_buf response;
	uint8_t		 state;		/* enum modbus_client_result */
};

/* Start a transaction.  Rejects a second concurrent request. */
int modbus_client_start(struct modbus_client *, const uint8_t *pdu,
    uint16_t pdulen, uint16_t unit, uint16_t tid);

/* The request to put on the wire, or NULL when nothing is outstanding. */
const uint8_t *modbus_client_request(struct modbus_client *, uint16_t *len,
    uint16_t *unit, uint16_t *tid);

/*
 * Feed a received response.  Returns MODBUS_CLIENT_DONE when the response
 * belongs to the outstanding request and can be read with
 * modbus_client_response(), MODBUS_CLIENT_IDLE when it is not ours, and
 * MODBUS_CLIENT_FAILED when it answers us but cannot be used.
 */
enum modbus_client_result modbus_client_input(struct modbus_client *,
    const uint8_t *pdu, uint16_t pdulen);

const struct modbus_pdu_buf *modbus_client_response(struct modbus_client *);

/* Give up on the outstanding request. */
enum modbus_client_result modbus_client_expired(struct modbus_client *);

void modbus_client_reset(struct modbus_client *);

#endif /* _SYS_NET_MODBUS_MODBUS_CLIENT_H_ */
