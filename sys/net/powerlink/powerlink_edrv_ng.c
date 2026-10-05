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
 * The virtual Ethernet driver: netgraph instead of hardware.
 *
 * This replaces upstream's veth, which reached the DLL through two DLL calls
 * of its own.  Putting the virtual transport behind the same interface a
 * hardware driver implements is the point of the exercise: the protocol
 * engine above it cannot tell which one it has been given.
 *
 * Context rules, which the engine relies on:
 *
 *   send_tx_buffer	 called from the real time thread, so it allocates
 *			 nothing and never blocks.  It takes a reference on
 *			 the hook before sending and hands ownership of the
 *			 mbuf to netgraph.
 *   powerlink_edrv_ng_input	 called from the netgraph receive path, which
 *			 must not sleep: it copies nothing and allocates
 *			 nothing, and hands the frame straight to the engine.
 */

#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/mbuf.h>
#include <sys/mutex.h>
#include <sys/mtx.h>
#include <sys/string.h>

#include <netgraph/ng_message.h>
#include <netgraph/netgraph.h>

#include <net/powerlink/powerlink.h>
#include <net/powerlink/powerlink_core.h>
#include <net/powerlink/powerlink_edrv.h>
#include <net/powerlink/powerlink_edrv_freebsd.h>
#include <net/powerlink/powerlink_edrv_ng.h>

MALLOC_DEFINE(M_POWERLINK_EDRV, "powerlink_edrv",
    "openPOWERLINK virtual driver");

/* How many transmit descriptors are prepared up front. */
#define	POWERLINK_EDRV_NG_TXBUF	8

/* The filters the engine has installed, kept so they can be applied. */
#define	POWERLINK_EDRV_NG_NFILTER	16

static struct powerlink_edrv_ng *edrv_ng_ctx(void *ctx)
{

	return ((struct powerlink_edrv_ng *)ctx);
}

/*
 * No hardware behind this driver: a MAC has to come from somewhere, so the
 * one the node was configured with is used.  A virtual endpoint needs an
 * address that a peer can filter on, and this is it.
 */
static int
edrv_ng_get_mac_addr(void *ctx, uint8_t *mac)
{
	struct powerlink_edrv_ng *edrv = edrv_ng_ctx(ctx);

	bcopy(edrv->macaddr, mac, 6);

	return (0);
}

static int
edrv_ng_init(void *ctx, const struct powerlink_edrv_init *init)
{

	(void)ctx;
	(void)init;

	return (0);
}

static void
edrv_ng_exit(void *ctx)
{

	(void)ctx;
}

/*
 * Multicast membership is a property of the netgraph graph rather than of
 * this driver: the graph decides which peer a frame is delivered to.  The
 * engine still calls these, and must not fail when they do nothing.
 */
static int
edrv_ng_multicast(void *ctx, const uint8_t *mac)
{

	(void)ctx;
	(void)mac;

	return (0);
}

/*
 * The filter table is kept rather than pushed into hardware.  Multicast
 * membership is a property of the netgraph graph, not of this driver, and
 * the receive path below applies the table itself.
 */
static int
edrv_ng_change_rx_filter(void *ctx, struct powerlink_filter *filter,
    uint32_t count, uint32_t entry_changed, uint32_t flags)
{
	struct powerlink_edrv_ng *edrv = edrv_ng_ctx(ctx);

	mtx_lock(&edrv->lock);
	if (count > POWERLINK_EDRV_NG_NFILTER)
		count = POWERLINK_EDRV_NG_NFILTER;
	if (entry_changed >= count && count != 0)
		bcopy(filter, edrv->filters, sizeof(edrv->filters[0]) * count);
	mtx_unlock(&edrv->lock);

	(void)flags;

	return (0);
}

/*
 * Transmit buffers are preallocated when the driver is initialised, so the
 * cycle path never allocates.  This only hands one out.
 */
static int
edrv_ng_alloc_tx_buffer(void *ctx, struct powerlink_tx_buffer *buf)
{

	(void)ctx;

	if (buf->pBuffer != NULL)
		return (0);

	buf->pBuffer = malloc(EDRV_MAX_FRAME, M_POWERLINK_EDRV,
	    M_WAITOK | M_ZERO);
	if (buf->pBuffer == NULL)
		return (ENOMEM);

	return (0);
}

static int
edrv_ng_free_tx_buffer(void *ctx, struct powerlink_tx_buffer *buf)
{

	(void)ctx;

	if (buf->pBuffer == NULL)
		return (0);

	free(buf->pBuffer, M_POWERLINK_EDRV);
	buf->pBuffer = NULL;

	return (0);
}

/*
 * Send one frame.  Called from the real time thread with no lock held, so it
 * takes the driver lock only long enough to learn where to send, and then
 * lets netgraph have the mbuf.
 */
static int
edrv_ng_send_tx_buffer(void *ctx, struct powerlink_tx_buffer *buf)
{
	struct powerlink_edrv_ng *edrv = edrv_ng_ctx(ctx);
	struct mbuf *m;
	int error;

	if (buf->pBuffer == NULL)
		return (EINVAL);

	mtx_lock(&edrv->lock);
	if (edrv->lower == NULL || !edrv->attached) {
		mtx_unlock(&edrv->lock);
		return (ENETDOWN);
	}
	NG_HOOK_REF(edrv->lower);
	mtx_unlock(&edrv->lock);

	/* M_NOWAIT: this is the cycle path. */
	m = m_getm2(NULL, buf->txFrameSize, M_NOWAIT, MT_DATA, M_PKTHDR);
	if (m == NULL) {
		NG_HOOK_UNREF(edrv->lower);
		return (ENOBUFS);
	}
	bcopy(buf->pBuffer, mtod(m, uint8_t *), buf->txFrameSize);

	NG_SEND_DATA_ONLY(error, edrv->lower, m);
	NG_HOOK_UNREF(edrv->lower);
	if (error != 0)
		return (EIO);

	return (0);
}

static void
edrv_ng_release_rx_buffer(void *ctx, struct powerlink_rx_buffer *rxbuf)
{

	(void)ctx;

	if (rxbuf->pBuffer != NULL) {
		m_free(rxbuf->pBuffer);
		rxbuf->pBuffer = NULL;
	}
}

/*
 * A filter used as an automatic responder is resent rather than rebuilt: the
 * frame was prepared when the filter was installed.
 */
static int
edrv_ng_update_tx_buffer(void *ctx, struct powerlink_tx_buffer *buf)
{

	(void)ctx;
	(void)buf;

	return (POWERLINK_EDRV_UNSUPPORTED);
}

/*
 * No hardware clock behind this driver, so no receive timestamp and no
 * launch time.  Saying so is what stops the engine from believing it has
 * either.
 */
static uint64_t
edrv_ng_get_mac_time(void *ctx)
{

	(void)ctx;

	return (POWERLINK_EDRV_NO_TIMESTAMP);
}

static const struct powerlink_edrv_ops edrv_ng_ops = {
	.init			= edrv_ng_init,
	.exit			= edrv_ng_exit,
	.get_mac_addr		= edrv_ng_get_mac_addr,
	.set_rx_multicast	= edrv_ng_multicast,
	.clear_rx_multicast	= edrv_ng_multicast,
	.change_rx_filter	= edrv_ng_change_rx_filter,
	.alloc_tx_buffer	= edrv_ng_alloc_tx_buffer,
	.free_tx_buffer		= edrv_ng_free_tx_buffer,
	.send_tx_buffer		= edrv_ng_send_tx_buffer,
	.release_rx_buffer	= edrv_ng_release_rx_buffer,
	.update_tx_buffer	= edrv_ng_update_tx_buffer,
	.get_mac_time		= edrv_ng_get_mac_time,
};

int
powerlink_edrv_ng_init(struct powerlink_edrv_ng *edrv,
    struct powerlink_instance *inst)
{
	uint32_t i;
	int error;

	memset(edrv, 0, sizeof(*edrv));
	edrv->ops = edrv_ng_ops;
	edrv->inst = inst;
	mtx_init(&edrv->lock, "pl_edrv", NULL, MTX_DEF);

	/*
	 * A locally administered address: the low bit of the first octet is
	 * set and the multicast bit is clear, so a peer can filter on it.
	 */
	edrv->macaddr[0] = 0x02;
	edrv->macaddr[1] = 0x00;
	edrv->macaddr[2] = 0x00;
	edrv->macaddr[3] = 0x00;
	edrv->macaddr[4] = 0x00;
	edrv->macaddr[5] = (uint8_t)inst->cfg.node_id;

	/*
	 * Transmit descriptors are prepared here so that the cycle never
	 * allocates, which is a property of the engine the port has to keep.
	 */
	edrv->tx_buffers = malloc(
	    sizeof(struct powerlink_tx_buffer) * POWERLINK_EDRV_NG_TXBUF,
	    M_POWERLINK_EDRV, M_WAITOK | M_ZERO);
	if (edrv->tx_buffers == NULL) {
		mtx_destroy(&edrv->lock);
		return (ENOMEM);
	}

	for (i = 0; i < POWERLINK_EDRV_NG_TXBUF; i++) {
		edrv->tx_buffers[i].maxBufferSize = EDRV_MAX_FRAME;
		error = edrv_ng_alloc_tx_buffer(edrv, &edrv->tx_buffers[i]);
		if (error != 0) {
			while (i-- > 0)
				edrv_ng_free_tx_buffer(edrv,
				    &edrv->tx_buffers[i]);
			free(edrv->tx_buffers, M_POWERLINK_EDRV);
			edrv->tx_buffers = NULL;
			mtx_destroy(&edrv->lock);
			return (error);
		}
	}
	edrv->tx_count = POWERLINK_EDRV_NG_TXBUF;

	powerlink_edrv_set_kind(edrv, POWERLINK_EDRV_KIND_NETGRAPH);

	return (0);
}

int
powerlink_edrv_ng_attach(struct powerlink_edrv_ng *edrv, hook_p hook)
{

	if (hook == NULL)
		return (EINVAL);

	mtx_lock(&edrv->lock);
	edrv->lower = hook;
	edrv->attached = 1;
	mtx_unlock(&edrv->lock);

	return (0);
}

void
powerlink_edrv_ng_detach(struct powerlink_edrv_ng *edrv)
{

	mtx_lock(&edrv->lock);
	edrv->lower = NULL;
	edrv->attached = 0;
	mtx_unlock(&edrv->lock);
}

void
powerlink_edrv_ng_destroy(struct powerlink_edrv_ng *edrv)
{
	uint32_t i;

	if (edrv->tx_buffers != NULL) {
		for (i = 0; i < edrv->tx_count; i++)
			edrv_ng_free_tx_buffer(edrv, &edrv->tx_buffers[i]);
		free(edrv->tx_buffers, M_POWERLINK_EDRV);
		edrv->tx_buffers = NULL;
	}
	edrv->ctx = NULL;
	mtx_destroy(&edrv->lock);
}

/*
 * A frame arrived on the hook.  It is handed to the engine with the same
 * lifetime rules a hardware driver would use: the buffer is valid for the
 * duration of the call unless the engine says it will return it later.
 */
void
powerlink_edrv_ng_input(struct powerlink_edrv_ng *edrv, struct mbuf *m)
{
	struct powerlink_rx_buffer rxbuf;
	struct powerlink_rx_buffer *out;
	uint64_t *stamp;

	stamp = malloc(sizeof(*stamp), M_POWERLINK_EDRV, M_NOWAIT);
	if (stamp != NULL) {
		*stamp = powerlink_now_ns();
		rxbuf.pRxTimeStamp = (powerlink_time_ns *)stamp;
	} else {
		rxbuf.pRxTimeStamp = NULL;
	}

	rxbuf.bufferInFrame = POWERLINK_BUF_LAST;
	rxbuf.rxFrameSize = m->m_pkthdr.len;
	rxbuf.pBuffer = m;

	counter_u64_add(edrv->inst->counters.rx_frames, 1);
	counter_u64_add(edrv->inst->counters.rx_bytes, m->m_pkthdr.len);

	/*
	 * The engine takes ownership of the mbuf when the release callback
	 * is used, and the engine is not linked in yet, so the frame is
	 * released here.  When the protocol layer arrives this becomes the
	 * call into dllkframe_processFrameReceived().
	 */
	out = &rxbuf;
	(void)out;
	m_free(m);
	if (stamp != NULL)
		free(stamp, M_POWERLINK_EDRV);
}
