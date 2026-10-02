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
 * Client transaction state.  See modbus_client.h for the specification
 * references.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>

#include <net/modbus/modbus.h>
#include <net/modbus/modbus_client.h>

void
modbus_client_reset(struct modbus_client *cl)
{

	cl->reqlen = 0;
	cl->state = MODBUS_CLIENT_IDLE;
}

int
modbus_client_start(struct modbus_client *cl, const uint8_t *pdu,
    uint16_t pdulen, uint16_t unit, uint16_t tid)
{

	/*
	 * Section 4.5: a transaction is one request and its response, so a
	 * second request while one is outstanding is a caller error rather
	 * than something to queue.
	 */
	if (cl->state == MODBUS_CLIENT_PENDING)
		return (EBUSY);
	if (pdu == NULL || pdulen == 0 || pdulen > MODBUS_CLIENT_MAX_PDU)
		return (EINVAL);

	bcopy(pdu, cl->request, pdulen);
	cl->reqlen = pdulen;
	cl->unit = unit;
	cl->tid = tid;
	cl->response.len = 0;
	cl->state = MODBUS_CLIENT_PENDING;

	return (0);
}

const uint8_t *
modbus_client_request(struct modbus_client *cl, uint16_t *len, uint16_t *unit,
    uint16_t *tid)
{

	if (cl->state != MODBUS_CLIENT_PENDING) {
		*len = 0;
		return (NULL);
	}

	*len = cl->reqlen;
	*unit = cl->unit;
	*tid = cl->tid;

	return (cl->request);
}

const struct modbus_pdu_buf *
modbus_client_response(struct modbus_client *cl)
{

	if (cl->state != MODBUS_CLIENT_DONE)
		return (NULL);

	return (&cl->response);
}

enum modbus_client_result
modbus_client_expired(struct modbus_client *cl)
{

	if (cl->state != MODBUS_CLIENT_PENDING)
		return (MODBUS_CLIENT_IDLE);

	cl->state = MODBUS_CLIENT_EXPIRED;

	return (MODBUS_CLIENT_EXPIRED);
}

/*
 * Accept a response.  The caller has already matched the transport level
 * identifiers, so what is left is whether the response is a PDU this
 * transaction could use.
 */
enum modbus_client_result
modbus_client_input(struct modbus_client *cl, const uint8_t *pdu,
    uint16_t pdulen)
{
	uint8_t fc;

	if (cl->state != MODBUS_CLIENT_PENDING)
		return (MODBUS_CLIENT_IDLE);

	if (pdu == NULL || pdulen == 0 || pdulen > MODBUS_CLIENT_MAX_PDU ||
	    pdulen > sizeof(cl->response.buf)) {
		cl->state = MODBUS_CLIENT_FAILED;
		return (MODBUS_CLIENT_FAILED);
	}

	fc = pdu[0];

	/*
	 * Section 7: a response carries the function code of the request,
	 * with bit 7 set when it is an exception.  Both shapes have to be
	 * accepted here, which is why the request-only parser, which refuses
	 * codes above 0x7f, is not used.
	 */
	if ((fc & MODBUS_FC_MAX) < MODBUS_FC_MIN) {
		cl->state = MODBUS_CLIENT_FAILED;
		return (MODBUS_CLIENT_FAILED);
	}

	if (fc != cl->request[0] && fc != (cl->request[0] | MODBUS_FC_MASK))
		return (MODBUS_CLIENT_IDLE);

	bcopy(pdu, cl->response.buf, pdulen);
	cl->response.len = pdulen;
	cl->state = MODBUS_CLIENT_DONE;

	return (MODBUS_CLIENT_DONE);
}
