/* Development shim for <sys/malloc.h>.
 *
 * The kernel passes a malloc type and flags; on a host the allocator is the
 * libc one, so the size is kept and the flags are mapped onto libc options.
 * This lets the protocol core be tested without a kernel.
 */
#ifndef _SHIM_SYS_MALLOC_H_
#define	_SHIM_SYS_MALLOC_H_
#include <stddef.h>

#define	M_WAITOK	0x000000
#define	M_NOWAIT	0x000001
#define	M_ZERO		0x010000

void *shim_malloc(size_t size, int flags);
void shim_free(void *ptr);

/* The kernel passes a malloc type that this build has no use for. */
#define	malloc(size, type, flags)	(shim_malloc((size), (flags)))
#define	free(ptr, type)		(shim_free(ptr))

/* The kernel defines a malloc type per allocation site. */
#define	MALLOC_DEFINE(type, shortdesc, longdesc)	\
	int type##_shim_unused __attribute__((unused))

#endif
