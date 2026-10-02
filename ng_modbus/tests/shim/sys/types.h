/* Development shim for <sys/types.h>: pulls in the host's types and adds the
 * few kernel helpers the protocol core uses. */
#ifndef _SHIM_SYS_TYPES_H_
#define	_SHIM_SYS_TYPES_H_
#include_next <sys/types.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#ifndef bcopy
#define	bcopy(s, d, n)	memcpy(d, s, n)
#endif
#endif
