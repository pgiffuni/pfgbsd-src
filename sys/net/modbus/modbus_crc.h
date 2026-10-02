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

#ifndef _SYS_NET_MODBUS_MODBUS_CRC_H_
#define	_SYS_NET_MODBUS_MODBUS_CRC_H_

#include <sys/param.h>
#include <sys/types.h>

/*
 * MODBUS RTU error checking, section 2.5.1.2 of the Serial Line guide and
 * appendix B: a 16 bit CRC seeded with all ones and built with the reversed
 * polynomial 0xa001, least significant bit first.  The low order byte of the
 * result is the one transmitted first.
 */
#define	MODBUS_CRC_INIT		0xffff
#define	MODBUS_CRC_POLY		0xa001

uint16_t modbus_crc16(const uint8_t *buf, size_t len);
void modbus_crc16_update(uint16_t *crc, const uint8_t *buf, size_t len);

#endif /* _SYS_NET_MODBUS_MODBUS_CRC_H_ */
