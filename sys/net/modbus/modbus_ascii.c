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
 * ASCII framing state machine and LRC.  See modbus_ascii.h for the
 * specification references.
 */

#include <sys/param.h>
#include <sys/systm.h>

#include <net/modbus/modbus.h>
#include <net/modbus/modbus_ascii.h>

/* A frame is a colon, the body in hex pairs, the LRC and CR LF. */
static const char hexdigits[] = "0123456789ABCDEF";

static int
modbus_ascii_nibble(uint8_t c)
{

	if (c >= '0' && c <= '9')
		return (c - '0');
	if (c >= 'A' && c <= 'F')
		return (c - 'A' + 10);

	return (-1);
}

void
modbus_ascii_init(struct modbus_ascii *ascii)
{

	memset(ascii, 0, sizeof(*ascii));
	ascii->state = MODBUS_ASCII_IDLE;
}

uint8_t
modbus_lrc(const uint8_t *buf, size_t len)
{
	uint8_t sum = 0;
	size_t i;

	for (i = 0; i < len; i++)
		sum = (uint8_t)(sum + buf[i]);

	/* Two's complement of an eight bit sum. */
	return ((uint8_t)(0u - sum));
}

void
modbus_ascii_release(struct modbus_ascii *ascii)
{

	ascii->len = 0;
	ascii->have_high = 0;
	ascii->state = MODBUS_ASCII_IDLE;
}

const uint8_t *
modbus_ascii_frame(struct modbus_ascii *ascii, uint16_t *len)
{

	if (ascii->state != MODBUS_ASCII_READY) {
		*len = 0;
		return (NULL);
	}

	*len = ascii->len;

	return (ascii->buf);
}

enum modbus_ascii_result
modbus_ascii_input(struct modbus_ascii *ascii, uint8_t c)
{
	int nibble;

	switch (ascii->state) {
	case MODBUS_ASCII_READY:
		/*
		 * The verified frame stays readable until it is released.
		 * Any character after it means the next frame is starting,
		 * and only a colon may start one.
		 */
		modbus_ascii_release(ascii);
		if (c != MODBUS_ASCII_START)
			return (MODBUS_ASCII_ACCEPTED);
		ascii->state = MODBUS_ASCII_BODY;
		return (MODBUS_ASCII_ACCEPTED);

	case MODBUS_ASCII_IDLE:
		/*
		 * Section 2.5.2.1: the bus is monitored for the colon, which
		 * starts a frame and abandons any frame already in progress.
		 */
		if (c != MODBUS_ASCII_START)
			return (MODBUS_ASCII_ACCEPTED);
		modbus_ascii_release(ascii);
		ascii->state = MODBUS_ASCII_BODY;
		return (MODBUS_ASCII_ACCEPTED);

	case MODBUS_ASCII_SAW_CR:
		if (c != MODBUS_ASCII_LF) {
			modbus_ascii_release(ascii);
			return (MODBUS_ASCII_ERROR);
		}
		/*
		 * The body holds the address, the PDU and the LRC; the LRC
		 * covers everything but itself.
		 */
		if (ascii->len < 2 ||
		    modbus_lrc(ascii->buf, ascii->len - 1) !=
		    ascii->buf[ascii->len - 1]) {
			modbus_ascii_release(ascii);
			return (MODBUS_ASCII_ERROR);
		}
		ascii->state = MODBUS_ASCII_READY;
		return (MODBUS_ASCII_FRAME);

	case MODBUS_ASCII_BODY:
	default:
		break;
	}

	if (c == MODBUS_ASCII_CR) {
		if (ascii->have_high) {
			/* An odd number of hex digits is malformed. */
			modbus_ascii_release(ascii);
			return (MODBUS_ASCII_ERROR);
		}
		ascii->state = MODBUS_ASCII_SAW_CR;
		return (MODBUS_ASCII_ACCEPTED);
	}

	/*
	 * Section 2.5.2.1: a colon means the frame in progress is incomplete
	 * and is discarded, and the colon itself starts the next frame.
	 */
	if (c == MODBUS_ASCII_START) {
		modbus_ascii_release(ascii);
		ascii->state = MODBUS_ASCII_BODY;
		return (MODBUS_ASCII_ACCEPTED);
	}

	/* Only the hexadecimal digits are allowed inside a frame. */
	if ((nibble = modbus_ascii_nibble(c)) < 0) {
		modbus_ascii_release(ascii);
		return (MODBUS_ASCII_ERROR);
	}

	if (!ascii->have_high) {
		ascii->high = (uint8_t)(nibble << 4);
		ascii->have_high = 1;
		return (MODBUS_ASCII_ACCEPTED);
	}

	if (ascii->len >= MODBUS_RTU_ADU_MAXLEN) {
		modbus_ascii_release(ascii);
		return (MODBUS_ASCII_ERROR);
	}

	ascii->buf[ascii->len++] = (uint8_t)(ascii->high | nibble);
	ascii->have_high = 0;

	return (MODBUS_ASCII_ACCEPTED);
}

size_t
modbus_ascii_encode(const uint8_t *in, size_t inlen, uint8_t *out,
    size_t outcap)
{
	uint8_t frame[MODBUS_RTU_ADU_MAXLEN];
	uint8_t lrc;
	size_t need, i, o;

	if (inlen + 1 > MODBUS_RTU_ADU_MAXLEN)
		return (0);

	bcopy(in, frame, inlen);
	lrc = modbus_lrc(frame, inlen);
	frame[inlen] = lrc;

	/* colon + two characters per byte + CR LF */
	need = 1 + 2 * (inlen + 1) + 2;
	if (outcap < need)
		return (0);

	o = 0;
	out[o++] = MODBUS_ASCII_START;
	for (i = 0; i < inlen + 1; i++) {
		out[o++] = (uint8_t)hexdigits[frame[i] >> 4];
		out[o++] = (uint8_t)hexdigits[frame[i] & 0x0f];
	}
	out[o++] = MODBUS_ASCII_CR;
	out[o++] = MODBUS_ASCII_LF;

	return (o);
}
