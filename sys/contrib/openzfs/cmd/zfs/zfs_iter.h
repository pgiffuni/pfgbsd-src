// SPDX-License-Identifier: CDDL-1.0
/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or https://opensource.org/licenses/CDDL-1.0.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright 2009 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 * Copyright 2013 Nexenta Systems, Inc. All rights reserved.
 */

#ifndef	ZFS_ITER_H
#define	ZFS_ITER_H

#ifdef	__cplusplus
extern "C" {
#endif

typedef struct zfs_sort_column {
	struct zfs_sort_column	*sc_next;
	struct zfs_sort_column	*sc_last;
	zfs_prop_t		sc_prop;
	char			*sc_user_prop;
	boolean_t		sc_reverse;
} zfs_sort_column_t;

int zfs_for_each(int, char **, int options, zfs_type_t,
    zfs_sort_column_t *, zprop_list_t **, int, zfs_iter_f, void *);
int zfs_add_sort_column(zfs_sort_column_t **, const char *, boolean_t);
void zfs_free_sort_columns(zfs_sort_column_t *);
boolean_t zfs_sort_only_by_fast(const zfs_sort_column_t *);
boolean_t zfs_list_only_by_fast(const zprop_list_t *);

#ifdef	__cplusplus
}
#endif

#endif	/* ZFS_ITER_H */
