// SPDX-License-Identifier: CDDL-1.0
/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright 2020 Toomas Soome <tsoome@me.com>
 */

#ifndef _LIBZFSBOOTENV_H
#define	_LIBZFSBOOTENV_H extern __attribute__((visibility("default")))

#ifdef __cplusplus
extern "C" {
#endif

typedef enum lzbe_flags {
	lzbe_add,	/* add data to existing nvlist */
	lzbe_replace	/* replace current nvlist */
} lzbe_flags_t;

_LIBZFSBOOTENV_H int lzbe_nvlist_get(const char *, const char *, void **);
_LIBZFSBOOTENV_H int lzbe_nvlist_set(const char *, const char *, void *);
_LIBZFSBOOTENV_H void lzbe_nvlist_free(void *);
_LIBZFSBOOTENV_H int lzbe_add_pair(void *, const char *, const char *, void *,
    size_t);
_LIBZFSBOOTENV_H int lzbe_remove_pair(void *, const char *);
_LIBZFSBOOTENV_H int lzbe_set_boot_device(const char *, lzbe_flags_t,
    const char *);
_LIBZFSBOOTENV_H int lzbe_get_boot_device(const char *, char **);
_LIBZFSBOOTENV_H int lzbe_bootenv_print(const char *, const char *, FILE *);

#ifdef __cplusplus
}
#endif

#endif /* _LIBZFSBOOTENV_H */
