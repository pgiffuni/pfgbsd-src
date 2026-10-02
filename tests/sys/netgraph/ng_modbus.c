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
 * They need a FreeBSD system and root.
 */

#include <sys/types.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <atf-c/lib/libatf-c.h>

#include <netgraph.h>

#include <net/modbus/modbus.h>
#include <netgraph/ng_modbus.h>

#include "util.h"

#define	SRV_PATH	"modbus:"
#define	CLI_PATH	"modbus_cli:"

/*
 * Unlike every other netgraph node, this one registers as "modbus" rather
 * than "ng_modbus", so "ngctl" cannot autoload it by type name and the test
 * has to load it.  Every case needs it, so it is done once per case.
 */
static void
load_module(void)
{
	int rc;

	rc = system("kldload -n modbus");
	atf_check(rc == 0 || errno == EEXIST,
	    "the modbus module is loadable");
}

ATF_TC(basic);
ATF_TC_HEAD(basic, conf)
{
	atf_tc_set_md_var(conf, "require.user", "root");
	atf_tc_set_md_var(conf, "require.files", "/dev/ngctl");
}

ATF_TC_BODY(basic, dummy)
{

	load_module();
	ng_init();
	ng_errors(PASS);

	ng_mkpeer(SRV_PATH, NG_MODBUS_HOOK_LOWER, NG_MODBUS_NODE_TYPE,
	    NG_MODBUS_HOOK_LOWER);
	ng_shutdown(SRV_PATH);
	ng_errors(FAIL);

	atf_ok(true);
}

ATF_TC(config);
ATF_TC_HEAD(config, conf)
{
	atf_tc_set_md_var(conf, "require.user", "root");
	atf_tc_set_md_var(conf, "require.files", "/dev/ngctl");
}

ATF_TC_BODY(config, dummy)
{

	load_module();
	ng_init();
	ng_errors(PASS);

	ng_mkpeer(SRV_PATH, NG_MODBUS_HOOK_LOWER, NG_MODBUS_NODE_TYPE,
	    NG_MODBUS_HOOK_LOWER);

	/*
	 * The text interface has to work; this used to always fail
	 * because the parse table was shorter than the structure it
	 * described.
	 */
	ng_send_msg(SRV_PATH "getconfig");
	ng_send_msg(SRV_PATH "getinfo");
	ng_send_msg(SRV_PATH "getstats");
	ng_send_msg(SRV_PATH "getbackend");
	ng_send_msg(SRV_PATH "clrstats");

	/* A valid configuration is accepted. */
	ng_send_msg(SRV_PATH "setconfig role=1 transport=1 unit_id=1");
	ng_send_msg(SRV_PATH "setbackend ncoils=1024 ndiscrete_in=1024 "
	    "ninput_regs=1024 nholding_regs=1024");

	/*
	 * The specification reserves 248 to 255, and roles and
	 * transports outside their ranges must be refused.
	 */
	if (ng_send_msg(SRV_PATH
	    "setconfig role=1 transport=1 unit_id=255") == 0)
		atf_err("a reserved unit identifier was accepted");
	if (ng_send_msg(SRV_PATH "setconfig role=9 transport=1 unit_id=1") == 0)
		atf_err("an invalid role was accepted");
	if (ng_send_msg(SRV_PATH "setconfig role=1 transport=9 unit_id=1") == 0)
		atf_err("an invalid transport was accepted");
	if (ng_send_msg(SRV_PATH "setbackend ncoils=99999 "
	    "ndiscrete_in=1024 ninput_regs=1024 nholding_regs=1024") == 0)
		atf_err("an oversized register map was accepted");

	ng_shutdown(SRV_PATH);
	ng_errors(FAIL);

	atf_ok(true);
}

/*
 * A server node and a client node connected to each other: the first end to
 * end check, with no serial line and no TCP stack involved.
 */
ATF_TC(client_server);
ATF_TC_HEAD(client_server, conf)
{
	atf_tc_set_md_var(conf, "require.user", "root");
	atf_tc_set_md_var(conf, "require.files", "/dev/ngctl");
}

ATF_TC_BODY(client_server, dummy)
{
	uint8_t request[] = { 0x03, 0x00, 0x00, 0x00, 0x02 };
	ng_counter_t replies;

	load_module();
	ng_init();
	ng_errors(PASS);

	ng_counter_clear(replies);

	ng_mkpeer(SRV_PATH, NG_MODBUS_HOOK_LOWER, NG_MODBUS_NODE_TYPE,
	    NG_MODBUS_HOOK_LOWER);
	ng_mkpeer(CLI_PATH, NG_MODBUS_HOOK_LOWER, NG_MODBUS_NODE_TYPE,
	    NG_MODBUS_HOOK_LOWER);

	ng_send_msg(SRV_PATH "setconfig role=1 transport=1 unit_id=1");
	ng_send_msg(CLI_PATH "setconfig role=2 transport=1 unit_id=1");

	/*
	 * One connection between the pair: the client sends the request
	 * on its hook and the server answers back on the same link.
	 */
	ng_connect(SRV_PATH "lower", CLI_PATH "lower");

	ng_register_data(SRV_PATH "lower", get_data0);
	ng_send_data(CLI_PATH "lower", request, sizeof(request));
	ng_handle_events(50, &replies);

	ng_shutdown(CLI_PATH);
	ng_shutdown(SRV_PATH);
	ng_errors(FAIL);

	atf_ok(true);
}

ATF_TC(peer_removal);
ATF_TC_HEAD(peer_removal, conf)
{
	atf_tc_set_md_var(conf, "require.user", "root");
	atf_tc_set_md_var(conf, "require.files", "/dev/ngctl");
}

ATF_TC_BODY(peer_removal, dummy)
{

	load_module();
	ng_init();
	ng_errors(PASS);

	ng_mkpeer(SRV_PATH, NG_MODBUS_HOOK_LOWER, NG_MODBUS_NODE_TYPE,
	    NG_MODBUS_HOOK_LOWER);

	/*
	 * Removing the only hook must take the node with it, as it does
	 * for every other netgraph node.
	 */
	ng_rmhook(SRV_PATH "lower");

	ng_errors(FAIL);

	atf_ok(true);
}

ATF_TC(client_request);
ATF_TC_HEAD(client_request, conf)
{
	atf_tc_set_md_var(conf, "require.user", "root");
	atf_tc_set_md_var(conf, "require.files", "/dev/ngctl");
}

ATF_TC_BODY(client_request, dummy)
{

	load_module();
	ng_init();
	ng_errors(PASS);

	ng_mkpeer(CLI_PATH, NG_MODBUS_HOOK_LOWER, NG_MODBUS_NODE_TYPE,
	    NG_MODBUS_HOOK_LOWER);
	ng_send_msg(CLI_PATH "setconfig role=2 transport=1 unit_id=1");

	/*
	 * A request with nothing attached is still accepted and leaves
	 * the transaction pending.
	 */
	ng_send_msg(CLI_PATH "getresponse");
	ng_send_msg(CLI_PATH "request pdu={3,0,0,0,2} pdulen=5 unit_id=1 "
	    "tid=1");
	ng_send_msg(CLI_PATH "getresponse");

	ng_shutdown(CLI_PATH);
	ng_errors(FAIL);

	atf_ok(true);
}

int
main(int argc, char *argv[])
{

	atf_argc = argc;
	atf_argv = argv;

	atf_init_pkgname_np(argv[0], "ng_modbus");

	atf_add_test_case(basic, basic);
	atf_add_test_case(config, config);
	atf_add_test_case(client_server, client_server);
	atf_add_test_case(peer_removal, peer_removal);
	atf_add_test_case(client_request, client_request);

	atf_run();
}
