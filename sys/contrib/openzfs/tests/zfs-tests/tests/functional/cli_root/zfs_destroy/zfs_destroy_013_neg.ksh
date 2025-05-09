#!/bin/ksh -p
# SPDX-License-Identifier: CDDL-1.0
#
# This file and its contents are supplied under the terms of the
# Common Development and Distribution License ("CDDL"), version 1.0.
# You may only use this file in accordance with the terms of version
# 1.0 of the CDDL.
#
# A full copy of the text of the CDDL should have accompanied this
# source.  A copy of the CDDL is also available via the Internet at
# http://www.illumos.org/license/CDDL.
#

#
# Copyright (c) 2012, 2016 by Delphix. All rights reserved.
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/cli_root/zfs_destroy/zfs_destroy.cfg
. $STF_SUITE/tests/functional/cli_root/zfs_destroy/zfs_destroy_common.kshlib

################################################################################
#
# Destroy of held snapshot should fail.
#
# 1. Create test environment without clones.
# 2. 'zfs hold <tag> <snap>'
# 3. 'zfs destroy <snap>' should fail.
# 4. Verify snapshot still exists.
# 5. 'zfs release <tag> <snap>'
# 6. Verify that we can destroy the snapshot.
#
################################################################################

function test_snap_run
{
    typeset dstype=$1

    snap=$(eval echo \$${dstype}SNAP)
    log_must zfs hold zfstest $snap
    log_mustnot zfs destroy $snap
    log_must datasetexists $snap
    log_must zfs release zfstest $snap
    log_must zfs destroy $snap
    log_mustnot datasetexists $snap
}

log_assert "zfs destroy of held snapshots fails"
log_onexit cleanup_testenv

setup_testenv snap

for dstype in FS VOL; do
    if [[ $dstype == VOL ]]; then
		if is_global_zone; then
			test_snap_run $dstype
		fi
	else
		test_snap_run $dstype
	fi
done

log_pass "zfs destroy of held snapshots fails"
