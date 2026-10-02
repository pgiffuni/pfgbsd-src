/* Development shim for <sys/endian.h>: same big endian semantics as
 * sys/sys/endian.h, so the byte order tests are meaningful on a host. */
#ifndef _SHIM_SYS_ENDIAN_H_
#define	_SHIM_SYS_ENDIAN_H_
#include <stdint.h>

static __inline uint16_t
be16dec(const void *pp)
{
	const uint8_t *p = pp;

	return ((uint16_t)((p[0] << 8) | p[1]));
}

static __inline void
be16enc(void *pp, uint16_t u)
{
	uint8_t *p = pp;

	p[0] = (uint8_t)(u >> 8);
	p[1] = (uint8_t)(u & 0xff);
}
#endif
