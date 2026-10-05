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

#ifndef _SYS_NET_POWERLINK_POWERLINK_EDRV_NG_H_
#define	_SYS_NET_POWERLINK_POWERLINK_EDRV_NG_H_

#include <sys/param.h>
#include <sys/types.h>

#include <sys/mbuf.h>
#include <sys/mutex.h>
#include <sys/mtx.h>

#include <netgraph/ng_message.h>
#include <netgraph/netgraph.h>

#include <net/powerlink/powerlink_edrv.h>

/*
 * The virtual driver: it replaces upstream's veth, so that a POWERLINK
 * endpoint can be built out of netgraph nodes instead of hardware.  It
 * implements the same interface as a hardware driver would, which is the
 * point: the protocol engine above it does not know which one it has.
 */

struct powerlink_instance;

#define	POWERLINK_EDRV_NG_NFILTER	16

struct powerlink_edrv_ng {
	/* Must stay first: the instance casts this to its driver. */
	struct powerlink_edrv_ops ops;
	void			*ctx;		/* struct powerlink_edrv_ng * */
	struct powerlink_instance	*inst;
	hook_p			 lower;		/* the netgraph hook, or NULL */
	struct mtx			 lock;	/* protects lower, attached */
	int				 attached;
	uint8_t			 macaddr[6];
	/* Preallocated transmit descriptors, so the cycle never allocates. */
	struct powerlink_tx_buffer	*tx_buffers;
	uint32_t			 tx_count;
	uint32_t			 tx_next;
	/* The filter table the engine installed, applied on receive. */
	struct powerlink_filter		 filters[POWERLINK_EDRV_NG_NFILTER];
};

/* Initialise a driver bound to a netgraph hook.  Does not attach. */
int	powerlink_edrv_ng_init(struct powerlink_edrv_ng *,
	    struct powerlink_instance *);

/* Attach to, and detach from, the netgraph hook the node owns. */
int	powerlink_edrv_ng_attach(struct powerlink_edrv_ng *, hook_p);
void	powerlink_edrv_ng_detach(struct powerlink_edrv_ng *);

void	powerlink_edrv_ng_destroy(struct powerlink_edrv_ng *);

/*
 * Hand a received mbuf to the engine.  Called by the netgraph node from its
 * receive path, which runs in netgraph context: it must not sleep.
 */
void	powerlink_edrv_ng_input(struct powerlink_edrv_ng *, struct mbuf *);

#endif /* _SYS_NET_POWERLINK_POWERLINK_EDRV_NG_H_ */
