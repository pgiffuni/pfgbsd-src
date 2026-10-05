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
 * Deadline timers, ported from openPOWERLINK 2.7.2
 * stack/include/kernel/hrestimer.h.
 *
 * Upstream's timer interface is five functions; the FreeBSD backend behind it
 * is one callout plus an absolute deadline.  Two properties of the upstream
 * contract are preserved because the cyclic engine depends on them:
 *
 *   - the handle is a generation counter.  modify_timer() increments it before
 *     arming, so a callback that is already running for an older deadline
 *     sees a stale handle and discards itself instead of acting twice;
 *
 *   - the two timers the cyclic engine uses are separate objects, the cycle
 *     timer being continuous and the slot timer one shot.
 *
 * Context: a timer callback runs in the thread that armed the timer, which
 * for the cycle engine is the real time thread.  It must not sleep, must not
 * allocate, and must not call netgraph.
 */

#ifndef _SYS_NET_POWERLINK_POWERLINK_TIMER_H_
#define	_SYS_NET_POWERLINK_POWERLINK_TIMER_H_

#include <sys/param.h>
#include <sys/types.h>

#include <sys/callout.h>
#include <sys/time.h>

#include <net/powerlink/powerlink_edrv.h>

/*
 * A timer is one callout, the callback it will run and the generation
 * counter that callback checks.  A deadline of 0 means "not armed".
 */
struct powerlink_timer {
	struct callout	 co;
	void		*cb;
	void		*ctx;
	powerlink_time_ns deadline;
	uint32_t	 generation;
	int		 armed;
	const char	*name;
};

typedef	void powerlink_timer_cb(void *ctx, const char *name);

void	powerlink_timer_init(struct powerlink_timer *, const char *name);
void	powerlink_timer_destroy(struct powerlink_timer *);

/*
 * Arm a deadline.  "relative_ns" is relative to now, as upstream's
 * modify_timer() is; negative values are ignored.  Returns 0 or an errno.
 */
int	powerlink_timer_modify(struct powerlink_timer *, sbintime_t relative_ns,
	    powerlink_timer_cb *cb, void *ctx);

/* Disarm and drain: after this the callback cannot still be running. */
void	powerlink_timer_stop(struct powerlink_timer *);

static inline powerlink_time_ns
powerlink_now_ns(void)
{
	struct timespec ts;

	getnanouptime(&ts);

	return ((uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec);
}

#endif /* _SYS_NET_POWERLINK_POWERLINK_TIMER_H_ */
