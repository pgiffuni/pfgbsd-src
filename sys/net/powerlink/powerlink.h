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
 * Common definitions for the openPOWERLINK port.
 *
 * Ported from openPOWERLINK 2.7.2.  That work is BSD-3-Clause, copyright
 * SYSTEC electronic GmbH and B&R Industrial Automation GmbH; files derived
 * from it keep that notice.  Everything in this directory that is written for
 * FreeBSD carries the SPDX identifier above instead.
 */

#ifndef _SYS_NET_POWERLINK_POWERLINK_H_
#define	_SYS_NET_POWERLINK_POWERLINK_H_

#include <sys/param.h>
#include <sys/types.h>

/* Node type and module, for netgraph and for kldstat. */
#define	POWERLINK_MODULE_NAME	"powerlink"
#define	NG_POWERLINK_NODE_TYPE	"powerlink"

/* Version of this implementation, reported by the status interface. */
#define	POWERLINK_VERSION	1

/*
 * openPOWERLINK is intrinsically single instance: the DLL keeps one global
 * instance that every other module refers to without a context parameter.
 * This port keeps that behaviour and enforces it, rather than pretending
 * otherwise.  See RESEARCH.md, section 3.
 */

/*
 * Runtime configuration.  The protocol is configured in POWERLINK units; the
 * conversion to FreeBSD time units happens once, in the cycle engine.
 */
#define	POWERLINK_MAX_CYCLEN_US	10000		/* 10 ms, protocol maximum */
/* Below this the timing is not useful. */
#define	POWERLINK_MIN_CYCLEN_US	200

#define	POWERLINK_MAX_IFNAMELEN	16

enum powerlink_mode {
	POWERLINK_MODE_MN = 0,		/* manager: issues the cycle */
	POWERLINK_MODE_CN		/* controller: answers it */
};

struct powerlink_config {
	enum powerlink_mode mode;
	uint32_t	    cycle_len_us;	/* cycle time, microseconds */
	uint32_t	    loss_tol_us;	/* loss of frame tolerance */
	uint32_t	    node_id;		/* our node id */
	int		    cpu;	/* CPU to pin the cycle thread to */
	char		    ifname[POWERLINK_MAX_IFNAMELEN];
};

/* Fill a configuration with the defaults: manager, 1 ms, no interface. */
void powerlink_config_defaults(struct powerlink_config *);

/*
 * Validate a configuration supplied from userland.  Returns 0 or an errno;
 * nothing is applied until every field has been accepted.
 */
int powerlink_config_check(const struct powerlink_config *);

#endif	/* _SYS_NET_POWERLINK_POWERLINK_H_ */
