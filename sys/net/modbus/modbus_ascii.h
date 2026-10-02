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

#ifndef _SYS_NET_MODBUS_MODBUS_ASCII_H_
#define	_SYS_NET_MODBUS_MODBUS_ASCII_H_

#include <sys/param.h>
#include <sys/types.h>

#include <net/modbus/modbus.h>

/*
 * ASCII framing, section 2.5.2 of the MODBUS over Serial Line Specification
 * V1.02: a message starts with a colon, every byte is sent as two upper case
 * hexadecimal characters, a longitudinal redundancy check closes the message
 * and the frame ends with CR LF.
 *
 * Like the RTU parser this is a pure state machine: it is fed one character
 * at a time and owns no timer, buffer or hook.
 */
#define	MODBUS_ASCII_START	':'
#define	MODBUS_ASCII_CR		0x0d
#define	MODBUS_ASCII_LF		0x0a

enum modbus_ascii_result {
	MODBUS_ASCII_ACCEPTED = 0,	/* character stored, no frame yet */
	MODBUS_ASCII_FRAME,		/* a decoded frame is readable */
	MODBUS_ASCII_ERROR		/* the frame was malformed */
};

enum modbus_ascii_state {
	MODBUS_ASCII_IDLE = 0,		/* waiting for the colon */
	MODBUS_ASCII_BODY,		/* collecting hexadecimal pairs */
	MODBUS_ASCII_SAW_CR,		/* CR seen, LF must follow */
	MODBUS_ASCII_READY		/* a verified frame is readable */
};

struct modbus_ascii {
	uint8_t		buf[MODBUS_RTU_ADU_MAXLEN];
	uint16_t	len;		/* decoded bytes */
	uint8_t		state;		/* enum modbus_ascii_state */
	uint8_t		high;		/* pending high nibble */
	int		have_high;
};

void modbus_ascii_init(struct modbus_ascii *);
enum modbus_ascii_result modbus_ascii_input(struct modbus_ascii *, uint8_t c);

/*
 * The decoded frame, address first and LRC last, exactly as it would be sent
 * over RTU.  Only available after MODBUS_ASCII_FRAME.
 */
const uint8_t *modbus_ascii_frame(struct modbus_ascii *, uint16_t *len);
void modbus_ascii_release(struct modbus_ascii *);

/*
 * Section 2.5.2.2: add the bytes discarding carries and take the two's
 * complement.
 */
uint8_t modbus_lrc(const uint8_t *buf, size_t len);

/*
 * Encode one frame, including the colon, the LRC and CR LF.  Returns the
 * number of characters written, or 0 if the buffer is too small.
 */
size_t modbus_ascii_encode(const uint8_t *in, size_t inlen, uint8_t *out,
    size_t outcap);

#endif /* _SYS_NET_MODBUS_MODBUS_ASCII_H_ */
