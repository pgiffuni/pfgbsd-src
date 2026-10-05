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
 * The real time execution layer.  See powerlink_rt.h for the design.
 */

#include <sys/param.h>
#include <sys/condvar.h>
#include <sys/cpuset.h>
#include <sys/endian.h>
#include <sys/kthread.h>
#include <sys/lock.h>
#include <sys/mtx.h>
#include <sys/string.h>
#include <sys/proc.h>
#include <sys/sched.h>
#include <sys/smp.h>
#include <sys/time.h>
#include <sys/unistd.h>

/* Needs the machine types, so it comes after the system headers. */
#if defined(__amd64__)
#include <machine/cpufunc.h>
#endif

#include <net/powerlink/powerlink.h>
#include <net/powerlink/powerlink_edrv.h>
#include <net/powerlink/powerlink_rt.h>

/*
 * The internal priority the cycle thread asks for.  The real time range is
 * fixed at compile time in this kernel (sys/sys/priority.h), and
 * PRI_MIN_REALTIME is the lowest real time priority, so the cycle thread
 * disturbs the machine as little as possible.  A higher one can be requested
 * through the interface; none of it is a guarantee.
 */
#define	POWERLINK_RT_PRIO_DEFAULT	PRI_MIN_REALTIME

/*
 * The spin reads the clock over and over, so it needs a hint to the
 * processor that the loop is hot.  Where the architecture provides one, use
 * it; elsewhere a plain read of the clock is a busy loop and that is all the
 * fallback is.
 */
#if defined(__amd64__)
#define	POWERLINK_CPU_PAUSE()	ia32_pause()
#elif defined(__aarch64__) || defined(__arm__)
#define	POWERLINK_CPU_PAUSE()	__asm__ __volatile__("yield")
#else
#define	POWERLINK_CPU_PAUSE()	((void)0)
#endif

/*
 * Sleep until just before the deadline, then read the clock to it.  The sleep
 * itself cannot be shorter than a tick, so the spin always covers at least
 * one tick: that is what makes the wakeup independent of timer granularity.
 */
static void
powerlink_rt_wait_to(uint64_t deadline_ns)
{
	uint64_t now, spin_start;
	int error;

	now = powerlink_now_ns();
	if (now >= deadline_ns)
		return;

	spin_start = (deadline_ns > POWERLINK_RT_SPIN_NS) ?
	    deadline_ns - POWERLINK_RT_SPIN_NS : deadline_ns;

	if (spin_start > now) {
		/*
		 * pause_sbt() rides the sub-tick callout machinery, so a
		 * deadline below one tick is still expressible.  An
		 * interrupted sleep is not an error here: the spin below
		 * finishes the job either way.
		 */
		error = pause_sbt("pl_rt", (sbintime_t)(spin_start - now),
		    0, 0);
		if (error != 0 && error != EINTR)
			return;
	}

	do {
		now = powerlink_now_ns();
		if (now >= deadline_ns)
			break;
		POWERLINK_CPU_PAUSE();
	} while (now < deadline_ns);
}

static void
powerlink_rt_run_cycle(struct powerlink_rt *rt)
{
	powerlink_cycle_fn *cycle;
	void *ctx;
	uint64_t deadline, wakeup, start, latency, duration;
	uint32_t len;
	int overrun;

	/*
	 * Take the deadline forward under the lock, then run the cycle with
	 * no lock held: the cycle may be long, and holding a spin lock
	 * across it would block the control plane behind it.
	 */
	mtx_lock(&rt->lock);
	deadline = rt->next_deadline_ns;
	len = rt->cycle_len_ns;
	cycle = rt->cycle;
	ctx = rt->cycle_ctx;
	if (len != 0)
		rt->next_deadline_ns = deadline + len;
	mtx_unlock(&rt->lock);

	if (cycle == NULL)
		return;

	powerlink_rt_wait_to(deadline);

	wakeup = powerlink_now_ns();
	latency = (wakeup > deadline) ? wakeup - deadline : 0;
	start = wakeup;
	overrun = cycle(ctx, start, deadline);
	duration = powerlink_now_ns() - start;

	mtx_lock(&rt->lock);
	rt->stats.cycles++;
	rt->stats.last_wakeup_ns = wakeup;
	rt->stats.last_start_ns = start;
	if (overrun != 0 || wakeup > deadline)
		rt->stats.deadline_misses++;
	if (latency > rt->stats.wakeup_latency_max_ns)
		rt->stats.wakeup_latency_max_ns = latency;
	if (duration > rt->stats.cycle_duration_max_ns)
		rt->stats.cycle_duration_max_ns = duration;
	mtx_unlock(&rt->lock);
}

/*
 * The cycle thread.  It leaves the loop when the state stops being RUNNING
 * and announces that it has done so, because this kernel has no thread join
 * primitive that a module may use.
 */
static void
powerlink_rt_thread(void *arg)
{
	struct powerlink_rt *rt = arg;

	mtx_lock(&rt->lock);
	while (rt->state == POWERLINK_RT_RUNNING) {
		mtx_unlock(&rt->lock);
		powerlink_rt_run_cycle(rt);
		mtx_lock(&rt->lock);
	}
	rt->state = POWERLINK_RT_EXITED;
	cv_signal(&rt->cv);
	mtx_unlock(&rt->lock);

	kthread_exit();
	/* NOTREACHED */
}

int
powerlink_rt_create(struct powerlink_rt *rt)
{

	memset(rt, 0, sizeof(*rt));
	mtx_init(&rt->lock, "pl_rt", NULL, MTX_SPIN);
	cv_init(&rt->cv, "pl_rt");
	rt->state = POWERLINK_RT_STOPPED;

	return (0);
}

void
powerlink_rt_destroy(struct powerlink_rt *rt)
{

	/*
	 * Stop first: the thread reads this state and the cycle callback, so
	 * both must be quiescent before the memory goes away.
	 */
	powerlink_rt_stop(rt);
	cv_destroy(&rt->cv);
	mtx_destroy(&rt->lock);
}

int
powerlink_rt_start(struct powerlink_rt *rt, powerlink_cycle_fn *cycle,
    void *ctx, uint32_t cycle_len_us, int cpu, int prio)
{
	struct thread *td;
	int error;

	if (cycle == NULL || cycle_len_us == 0)
		return (EINVAL);

	mtx_lock(&rt->lock);
	if (rt->state == POWERLINK_RT_RUNNING) {
		mtx_unlock(&rt->lock);
		return (EBUSY);
	}

	rt->cycle = cycle;
	rt->cycle_ctx = ctx;
	rt->cycle_len_ns = (uint32_t)cycle_len_us * 1000;
	rt->cpu = cpu;
	rt->prio = prio;
	memset(&rt->stats, 0, sizeof(rt->stats));

	/*
	 * The thread is created stopped so its scheduling class, priority and
	 * affinity are in place before it can run a cycle.
	 */
	td = NULL;
	error = kthread_add(powerlink_rt_thread, rt, NULL, &td, RFSTOPPED, 0,
	    "%s rt", POWERLINK_MODULE_NAME);
	if (error != 0) {
		mtx_unlock(&rt->lock);
		return (error);
	}

	if (cpu >= 0 && cpu < mp_ncpus)
		cpuset_setithread(td->td_tid, cpu);

	thread_lock(td);
	sched_class(td, PRI_REALTIME);
	sched_prio(td, (prio > 0) ? prio : POWERLINK_RT_PRIO_DEFAULT);
	thread_unlock(td);

	rt->thread = td;
	rt->state = POWERLINK_RT_RUNNING;
	/* The first deadline is one cycle away, from now. */
	rt->next_deadline_ns = powerlink_now_ns() + rt->cycle_len_ns;
	mtx_unlock(&rt->lock);

	kthread_resume(td);

	return (0);
}

void
powerlink_rt_stop(struct powerlink_rt *rt)
{
	struct thread *td;
	int waited_ms = 0;

	mtx_lock(&rt->lock);
	if (rt->state != POWERLINK_RT_RUNNING) {
		mtx_unlock(&rt->lock);
		return;
	}
	rt->state = POWERLINK_RT_STOPPED;
	rt->cycle_len_ns = 0;
	td = rt->thread;
	rt->thread = NULL;

	/*
	 * The thread notices between cycles.  Wait for it to say so rather
	 * than freeing the state it reads; the wait is bounded so a thread
	 * stuck in a driver cannot hang the control plane forever.
	 */
	while (rt->state != POWERLINK_RT_EXITED) {
		if (waited_ms >= POWERLINK_RT_STOP_WAIT_MS)
			break;
		cv_timedwait_sbt(&rt->cv, &rt->lock,
		    (sbintime_t)((POWERLINK_RT_STOP_WAIT_MS - waited_ms) *
		    1000000LL), 0, 0);
		waited_ms++;
	}

	rt->cycle = NULL;
	rt->cycle_ctx = NULL;
	mtx_unlock(&rt->lock);

	(void)td;
}

void
powerlink_rt_get_stats(struct powerlink_rt *rt, struct powerlink_rt_stats *out)
{

	mtx_lock(&rt->lock);
	*out = rt->stats;
	mtx_unlock(&rt->lock);
}
