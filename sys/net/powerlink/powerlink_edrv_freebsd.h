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
 * FreeBSD extensions to the Ethernet driver interface.  The protocol visible
 * contract stays in powerlink_edrv.h; anything a FreeBSD driver needs that
 * upstream does not have belongs here.
 */

#ifndef _SYS_NET_POWERLINK_POWERLINK_EDRV_FREEBSD_H_
#define	_SYS_NET_POWERLINK_POWERLINK_EDRV_FREEBSD_H_

#include <sys/param.h>
#include <sys/types.h>

#include <net/powerlink/powerlink_edrv.h>

/*
 * A driver that can schedule a transmission for an absolute time, or report
 * the time a frame was received, returns one of these from get_mac_time().
 * A driver with no such hardware returns POWERLINK_EDRV_UNSUPPORTED from
 * get_mac_time() instead, and the engine falls back to the software clock.
 */
#define	POWERLINK_EDRV_NO_TIMESTAMP	((powerlink_time_ns)0)

/*
 * Which backend a driver is.  The engine behaves the same either way; this
 * exists so that the status interface can report it, and so that a hardware
 * driver cannot be mistaken for the virtual one.
 */
enum powerlink_edrv_kind {
	POWERLINK_EDRV_KIND_NONE = 0,
	POWERLINK_EDRV_KIND_NETGRAPH,	/* carried over netgraph */
	POWERLINK_EDRV_KIND_IFNET	/* carried over an Ethernet interface */
};

/* Attach a backend to a driver context.  Called before init(). */
void	powerlink_edrv_set_kind(void *ctx, enum powerlink_edrv_kind);
enum powerlink_edrv_kind powerlink_edrv_get_kind(void *ctx);

/*
 * Report whether the driver can take a launch time.  The engine sets
 * fLaunchTimeValid only when the driver says it can act on it, so a driver
 * that cannot must answer zero rather than silently ignore the field.
 */
int	powerlink_edrv_has_launch_time(void *ctx);

#endif	/* _SYS_NET_POWERLINK_POWERLINK_EDRV_FREEBSD_H_ */
