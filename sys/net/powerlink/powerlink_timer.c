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
 * Deadline timers on FreeBSD callouts.  See powerlink_timer.h for why the
 * generation counter exists.
 */

#include <sys/param.h>
#include <sys/callout.h>
#include <sys/string.h>
#include <sys/time.h>

#include <net/powerlink/powerlink_edrv.h>
#include <net/powerlink/powerlink_timer.h>

void
powerlink_timer_init(struct powerlink_timer *timer, const char *name)
{

	memset(timer, 0, sizeof(*timer));
	timer->name = name;
	callout_init(&timer->co, 0);
}

/*
 * The callout runs outside the network epoch with interrupts disabled.  It
 * does nothing of its own beyond checking that it is still the current
 * generation: the real work belongs to whoever armed it.
 */
static void
powerlink_timer_callout(void *arg)
{
	struct powerlink_timer *timer = arg;
	powerlink_timer_cb *cb;
	void *ctx;

	if (!timer->armed)
		return;

	cb = timer->cb;
	ctx = timer->ctx;
	timer->armed = 0;

	if (cb != NULL)
		(*cb)(ctx, timer->name);
}

/*
 * Arm a deadline relative to now, replacing any earlier one.  Draining
 * first is what makes the generation counter necessary: a callback that is
 * already running for the previous deadline must see a stale generation and
 * drop itself rather than run twice.
 */
int
powerlink_timer_modify(struct powerlink_timer *timer, sbintime_t relative_ns,
    powerlink_timer_cb *cb, void *ctx)
{

	if (cb == NULL || relative_ns < 0)
		return (EINVAL);

	callout_drain(&timer->co);

	timer->cb = cb;
	timer->ctx = ctx;
	timer->generation++;
	timer->armed = 1;
	timer->deadline = powerlink_now_ns() + (uint64_t)relative_ns;

	callout_reset_sbt(&timer->co, (sbintime_t)relative_ns, 0,
	    powerlink_timer_callout, timer, CALLOUT_RETURNUNLOCKED);

	return (0);
}

void
powerlink_timer_stop(struct powerlink_timer *timer)
{

	/*
	 * Drain rather than stop: a callback may already be on its way to
	 * the owner, and the owner must be able to rely on it not running
	 * once this returns.
	 */
	callout_drain(&timer->co);
	timer->armed = 0;
	timer->deadline = 0;
	timer->cb = NULL;
	timer->ctx = NULL;
}

void
powerlink_timer_destroy(struct powerlink_timer *timer)
{

	/*
	 * Draining is the whole teardown: once it returns the callback
	 * cannot be running and will not be armed again, which is what the
	 * netgraph and bridge callouts do as well.
	 */
	powerlink_timer_stop(timer);
}
