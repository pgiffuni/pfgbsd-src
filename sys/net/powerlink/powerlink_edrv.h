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
 * The Ethernet driver interface, ported from openPOWERLINK 2.7.2
 * stack/include/kernel/edrv.h.
 *
 * The protocol engine is written against this interface and nothing else, so
 * the same engine runs over a virtual netgraph transport and over a real
 * interface.  The structure and function names follow upstream so the two can
 * be compared side by side; the types are FreeBSD's, and the platform
 * extensions live in powerlink_edrv_freebsd.h.
 *
 * Ownership, which upstream documents only in comments:
 *
 *   tx buffer	 The descriptor belongs to the engine.  alloc fills pBuffer
 *		 from maxBufferSize, which the engine preset; free releases
 *		 pBuffer and clears it.  send does not take ownership: the
 *		 buffer stays valid until free.
 *   rx buffer	 pBuffer belongs to the driver and is valid only for the
 *		 duration of the receive callback, unless the callback
 *		 returns POWERLINK_RX_RELEASE_LATER, in which case the
 *		 engine returns it with release_rx_buffer.
 *   filters	 The array belongs to the engine and stays valid across the
 *		 call.  change_rx_filter never blocks.
 */

#ifndef _SYS_NET_POWERLINK_POWERLINK_EDRV_H_
#define	_SYS_NET_POWERLINK_POWERLINK_EDRV_H_

#include <sys/param.h>
#include <sys/types.h>

/* Largest frame the engine will hand to or accept from a driver. */
#define	EDRV_MAX_MTU	1500
#define	EDRV_MAX_FRAME	(EDRV_MAX_MTU + 14)	/* header and FCS */

/* Monotonic timestamp, nanoseconds, as the engine keeps it. */
typedef uint64_t powerlink_time_ns;

/*
 * What the receive callback returns about the buffer it was given.
 */
enum powerlink_rx_release {
	POWERLINK_RX_RELEASE_NOW = 0,	/* the driver may reuse it at once */
	POWERLINK_RX_RELEASE_LATER	/* the engine returns it later */
};

/*
 * Transmit buffer.  The launch time fields are kept even though the first
 * FreeBSD driver cannot use them: they are part of the contract, and a driver
 * that can schedule a transmission itself needs them.
 */
struct powerlink_tx_buffer {
	size_t		 txFrameSize;	/* payload length */
	uint32_t	 timeOffsetNs;	/* offset from the cycle start */
	int		 fLaunchTimeValid;
	union {
		uint64_t nanoseconds;
		uint64_t ticks;
	}		 launchTime;	/* absolute launch time */
	void		(*pfnTxHandler)(struct powerlink_tx_buffer *);
	uint32_t	 txBufferNumber;
	void		*txBufferArg;
	void		*pBuffer;	/* owned by the driver */
	size_t		 maxBufferSize;	/* set by the engine */
};

/*
 * Receive buffer.  bufferInFrame is how much of the frame this buffer holds,
 * so that a driver without receive interrupt coalescing can deliver a frame
 * in pieces.
 */
enum powerlink_buffer_in_frame {
	POWERLINK_BUF_FIRST = 0,
	POWERLINK_BUF_MIDDLE,
	POWERLINK_BUF_LAST
};

struct powerlink_rx_buffer {
	enum powerlink_buffer_in_frame bufferInFrame;
	size_t			 rxFrameSize;
	void			*pBuffer;
	powerlink_time_ns	*pRxTimeStamp;	/* optional */
};

/*
 * One receive filter.  The value and mask are compared against the first
 * bytes of the frame, and pBuffer holds an automatic reply frame when the
 * filter is changed with POWERLINK_FILTER_CHANGE_AUTO_RESPONSE.
 */
struct powerlink_filter {
	uint32_t	 handle;
	int		 fEnable;
	uint8_t		 aFilterValue[22];
	uint8_t		 aFilterMask[22];
	struct powerlink_tx_buffer *pTxBuffer;
};

#define	POWERLINK_FILTER_CHANGE_VALUE		0x01
#define	POWERLINK_FILTER_CHANGE_MASK		0x02
#define	POWERLINK_FILTER_CHANGE_STATE		0x04
#define	POWERLINK_FILTER_CHANGE_AUTO_RESPONSE	0x08
#define	POWERLINK_FILTER_CHANGE_ALL	\
    (POWERLINK_FILTER_CHANGE_VALUE | POWERLINK_FILTER_CHANGE_MASK | \
     POWERLINK_FILTER_CHANGE_STATE)

struct powerlink_edrv_init {
	const uint8_t		*pMacAddr;	/* zero: read it from the NIC */
	const char		*pIfName;	/* interface name, required */
	struct powerlink_rx_buffer *
				(*pfnRxHandler)(struct powerlink_rx_buffer *);
};

/*
 * What every driver must provide.  A driver that cannot do something returns
 POWERLINK_EDRV_UNSUPPORTED rather than failing an operation.
 */
struct powerlink_edrv_ops {
	int	(*init)(void *ctx, const struct powerlink_edrv_init *);
	void	(*exit)(void *ctx);
	int	(*get_mac_addr)(void *ctx, uint8_t *);
	int	(*set_rx_multicast)(void *ctx, const uint8_t *);
	int	(*clear_rx_multicast)(void *ctx, const uint8_t *);
	int	(*change_rx_filter)(void *ctx, struct powerlink_filter *,
		    uint32_t count, uint32_t entry_changed, uint32_t flags);
	int	(*alloc_tx_buffer)(void *ctx, struct powerlink_tx_buffer *);
	int	(*free_tx_buffer)(void *ctx, struct powerlink_tx_buffer *);
	int	(*send_tx_buffer)(void *ctx, struct powerlink_tx_buffer *);
	void	(*release_rx_buffer)(void *ctx, struct powerlink_rx_buffer *);
	int	(*update_tx_buffer)(void *ctx, struct powerlink_tx_buffer *);
	powerlink_time_ns (*get_mac_time)(void *ctx);
};

/* Returned by a driver for an operation its hardware cannot perform. */
#define	POWERLINK_EDRV_UNSUPPORTED	ENOTSUP

#endif	/* _SYS_NET_POWERLINK_POWERLINK_EDRV_H_ */
