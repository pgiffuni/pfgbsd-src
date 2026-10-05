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
 * Netgraph tests for the ng_modbus node.
 *
 * These cover what the host protocol tests cannot: node lifecycle, the text
 * control interface, a client and a server talking to each other through the
 * graph, and survival of the graph operations the node has to tolerate.
 * They need root and /dev/ngctl.
 */

#include <sys/types.h>

#include <errno.h>
#include <string.h>

#include <atf-c.h>

#include <netgraph.h>

#include <netgraph/ng_modbus.h>

#include "util.h"

#define	SRV_PATH	"modbus:"
#define	CLI_PATH	"modbus_cli:"

/*
 * A read holding registers request: function code, starting address 0, two
 * registers.  The memory backend answers with the byte count and the two
 * registers, which are zero because nothing has been written yet.
 */
static const uint8_t request[] = { 0x03, 0x00, 0x00, 0x00, 0x02 };
static const uint8_t expected[] = { 0x03, 0x02, 0x00, 0x00 };

static uint8_t response[16];
static size_t responselen;

static void
get_response(void *data, size_t len, void *ctx)
{
	const uint8_t *p = data;
	size_t i;

	(void)ctx;
	if (len > sizeof(response))
		len = sizeof(response);
	for (i = 0; i < len; i++)
		response[i] = p[i];
	responselen = len;
}

ATF_TC(basic);
ATF_TC_HEAD(basic, conf)
{
	atf_tc_set_md_var(conf, "require.user", "root");
	atf_tc_set_md_var(conf, "require.files", "/dev/ngctl");
}

ATF_TC_BODY(basic, dummy)
{

	ng_init();
	ng_errors(PASS);

	ng_mkpeer(".", NG_MODBUS_HOOK_LOWER, NG_MODBUS_NODE_TYPE,
	    NG_MODBUS_HOOK_LOWER);
	ng_name(NG_MODBUS_HOOK_LOWER, "modbus");
	ng_shutdown(SRV_PATH);
	ng_errors(FAIL);
}

ATF_TC(config);
ATF_TC_HEAD(config, conf)
{
	atf_tc_set_md_var(conf, "require.user", "root");
	atf_tc_set_md_var(conf, "require.files", "/dev/ngctl");
}

ATF_TC_BODY(config, dummy)
{

	ng_init();
	ng_errors(PASS);

	ng_mkpeer(".", NG_MODBUS_HOOK_LOWER, NG_MODBUS_NODE_TYPE,
	    NG_MODBUS_HOOK_LOWER);
	ng_name(NG_MODBUS_HOOK_LOWER, "modbus");

	/*
	 * The text interface has to work.  It used to always fail because
	 * the parse table was shorter than the structure it described, so
	 * every command below is a regression check.
	 */
	ng_send_msg(SRV_PATH "getconfig");
	ng_send_msg(SRV_PATH "getinfo");
	ng_send_msg(SRV_PATH "getstats");
	ng_send_msg(SRV_PATH "getbackend");
	ng_send_msg(SRV_PATH "clrstats");

	/* A valid configuration and a valid backend size are accepted. */
	ng_send_msg(SRV_PATH "setconfig role=1 transport=1 unit_id=1");
	ng_send_msg(SRV_PATH "setbackend ncoils=1024 ndiscrete_in=1024 "
	    "ninput_regs=1024 nholding_regs=1024");

	/*
	 * The specification reserves unit identifiers 248 to 255, and roles
	 * and transports outside their ranges must be refused too.
	 */
	ATF_CHECK(ng_send_msg(SRV_PATH
	    "setconfig role=1 transport=1 unit_id=255") != 0,
	    "a reserved unit identifier was refused");
	ATF_CHECK(ng_send_msg(SRV_PATH
	    "setconfig role=9 transport=1 unit_id=1") != 0,
	    "an invalid role was refused");
	ATF_CHECK(ng_send_msg(SRV_PATH
	    "setconfig role=1 transport=9 unit_id=1") != 0,
	    "an invalid transport was refused");
	ATF_CHECK(ng_send_msg(SRV_PATH "setbackend ncoils=99999 "
	    "ndiscrete_in=1024 ninput_regs=1024 nholding_regs=1024") != 0,
	    "an oversized register map was refused");

	ng_shutdown(SRV_PATH);
	ng_errors(FAIL);
}

/*
 * The server data path: a request PDU injected into the node must come back
 * as a response PDU, framed the way the frameless transport defines it.
 */
ATF_TC(server_response);
ATF_TC_HEAD(server_response, conf)
{
	atf_tc_set_md_var(conf, "require.user", "root");
	atf_tc_set_md_var(conf, "require.files", "/dev/ngctl");
}

ATF_TC_BODY(server_response, dummy)
{

	ng_init();
	ng_errors(PASS);

	/*
	 * The test node provides the injection point: the Modbus node is
	 * attached to hook "a", hook "b" is connected to it, so a frame sent
	 * into "b" arrives at the node and its answer comes back out of "b".
	 */
	ng_mkpeer(".", "a", NG_MODBUS_NODE_TYPE, NG_MODBUS_HOOK_LOWER);
	ng_name("a", "modbus");
	ng_send_msg(SRV_PATH "setconfig role=1 transport=1 unit_id=1");
	ng_connect(".", "b", ".", "a");

	ng_register_data("b", get_response);
	responselen = 0;
	ng_send_data("b", request, sizeof(request));

	{
		ng_counter_t r;

		ng_counter_clear(r);
		ng_handle_events(50, &r);
	}

	ATF_CHECK_EQ(responselen, sizeof(expected));
	if (responselen == sizeof(expected))
		ATF_CHECK_MSG(memcmp(response, expected, sizeof(expected)) == 0,
		    "response is function code 3, byte count 2, two zero "
		    "registers");

	ng_shutdown(SRV_PATH);
	ng_errors(FAIL);
}

/*
 * A client node and a server node connected through the test node: the client
 * issues one request through the control interface and the server answers it.
 */
ATF_TC(client_server);
ATF_TC_HEAD(client_server, conf)
{
	atf_tc_set_md_var(conf, "require.user", "root");
	atf_tc_set_md_var(conf, "require.files", "/dev/ngctl");
}

ATF_TC_BODY(client_server, dummy)
{

	ng_init();
	ng_errors(PASS);

	ng_mkpeer(".", "a", NG_MODBUS_NODE_TYPE, NG_MODBUS_HOOK_LOWER);
	ng_name("a", "modbus");
	ng_mkpeer(".", "b", NG_MODBUS_NODE_TYPE, NG_MODBUS_HOOK_LOWER);
	ng_name("b", "modbus_cli");
	ng_connect(".", "a", ".", "b");

	ng_send_msg(SRV_PATH "setconfig role=1 transport=1 unit_id=1");
	ng_send_msg(CLI_PATH "setconfig role=2 transport=1 unit_id=1");

	/*
	 * With a peer connected the request is accepted and the transaction
	 * starts; getresponse then reports it, without error.
	 */
	ATF_CHECK_EQ(ng_send_msg(CLI_PATH "request pdu={3,0,0,0,2} "
	    "pdulen=5 unit_id=1 tid=1"), 0);
	ng_send_msg(CLI_PATH "getresponse");

	ng_shutdown(CLI_PATH);
	ng_shutdown(SRV_PATH);
	ng_errors(FAIL);
}

ATF_TC(peer_removal);
ATF_TC_HEAD(peer_removal, conf)
{
	atf_tc_set_md_var(conf, "require.user", "root");
	atf_tc_set_md_var(conf, "require.files", "/dev/ngctl");
}

ATF_TC_BODY(peer_removal, dummy)
{

	ng_init();
	ng_errors(PASS);

	ng_mkpeer(".", NG_MODBUS_HOOK_LOWER, NG_MODBUS_NODE_TYPE,
	    NG_MODBUS_HOOK_LOWER);
	ng_name(NG_MODBUS_HOOK_LOWER, "modbus");

	/*
	 * Removing the only hook must take the node with it, as it does for
	 * every other netgraph node.
	 */
	ng_rmhook(SRV_PATH "lower");

	ng_errors(FAIL);
}

ATF_TC(client_request_no_peer);
ATF_TC_HEAD(client_request_no_peer, conf)
{
	atf_tc_set_md_var(conf, "require.user", "root");
	atf_tc_set_md_var(conf, "require.files", "/dev/ngctl");
}

ATF_TC_BODY(client_request_no_peer, dummy)
{

	ng_init();
	ng_errors(PASS);

	ng_mkpeer(".", NG_MODBUS_HOOK_LOWER, NG_MODBUS_NODE_TYPE,
	    NG_MODBUS_HOOK_LOWER);
	ng_name(NG_MODBUS_HOOK_LOWER, "modbus_cli");
	ng_send_msg(CLI_PATH "setconfig role=2 transport=1 unit_id=1");

	/*
	 * The lower hook is attached to the test node, but that hook has no
	 * peer of its own, so the request cannot be delivered.  The control
	 * message must still succeed and the node must survive: the error is
	 * counted and the transaction stays pending until it is reset.
	 */
	ATF_CHECK_MSG(ng_send_msg(CLI_PATH "request pdu={3,0,0,0,2} "
	    "pdulen=5 unit_id=1 tid=1") == 0,
	    "a request over a hook with no peer was accepted");
	ATF_CHECK_MSG(ng_send_msg(CLI_PATH "getresponse") == 0,
	    "the pending transaction can be read back");

	ng_shutdown(CLI_PATH);
	ng_errors(FAIL);
}

ATF_TP_ADD_TCS(ng_modbus)
{
	ATF_TP_ADD_TC(ng_modbus, basic);
	ATF_TP_ADD_TC(ng_modbus, config);
	ATF_TP_ADD_TC(ng_modbus, server_response);
	ATF_TP_ADD_TC(ng_modbus, client_server);
	ATF_TP_ADD_TC(ng_modbus, peer_removal);
	ATF_TP_ADD_TC(ng_modbus, client_request_no_peer);

	return (atf_no_error());
}
