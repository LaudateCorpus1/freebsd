/*
 * Copyright (c) 2011 Theo de Raadt.
 * Copyright (c) 2008 Damien Miller.
 * Copyright (c) 1996, 1997, 2000-2002 Michael Shalayeff.
 * Copyright (c) 2013 Markus Friedl.
 * Copyright Theodore Ts'o, 1994, 1995, 1996, 1997, 1998, 1999.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, and the entire permission notice in its entirety,
 *    including the disclaimer of warranties.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote
 *    products derived from this software without specific prior
 *    written permission.
 *
 * ALTERNATIVELY, this product may be distributed under the terms of
 * the GNU Public License, in which case the provisions of the GPL are
 * required INSTEAD OF the above restrictions.  (This clause is
 * necessary due to a potential bad interaction between the GPL and
 * the restrictions contained in a BSD-style copyright.)
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/types.h>
#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/random.h>
#include <sys/libkern.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/time.h>
#include <sys/systm.h>

#define KEYSTREAM_ONLY
#include <crypto/chacha_private.h>

#define	ARC4_RESEED_BYTES 65536
#define	ARC4_RESEED_SECONDS 300
#define	ARC4_KEYBYTES (256 / 8)

int arc4rand_iniseed_state = ARC4_ENTR_NONE;

static int arc4_numruns = 0;
static time_t arc4_t_reseed;
static struct mtx arc4_mtx;

#define	KEYSZ		32
#define	IVSZ		8
#define	BLOCKSZ		64
#define	RSBUFSZ		(16*BLOCKSZ)

static int rs_initialized;
static chacha_ctx rs;		/* chacha context for random keystream */
/* keystream blocks */
static u_char rs_buf[RSBUFSZ];
static size_t rs_have;		/* valid bytes at end of rs_buf */
static size_t rs_count;		/* bytes till reseed */

static __inline void _rs_rekey(u_char *dat, size_t datlen);
static __inline void _rs_stir(int);

static __inline void
_rs_init(u_char *buf, size_t n)
{
	KASSERT(n >= (KEYSZ + IVSZ), ("_rs_init size too small"));

	chacha_keysetup(&rs, buf, (KEYSZ * 8), 0);
	chacha_ivsetup(&rs, (buf + KEYSZ));
}

static void
_rs_seed(u_char *buf, size_t n)
{
	_rs_rekey(buf, n);

	/* reset rs_buf */
	rs_have = 0;
	memset(rs_buf, 0, sizeof(rs_buf));

	rs_count = 1600000;
}

static __inline void
_rs_stir_if_needed(size_t len)
{
	if (!rs_initialized) {
		_rs_init(rs_buf, (KEYSZ + IVSZ));
		rs_count = 1024 * 1024 * 1024;
		rs_initialized = 1;
	} else if (rs_count <= len) {
		_rs_stir(0);
	} else {
		rs_count -= len;
	}
}

static __inline void
_rs_rekey(u_char *dat, size_t datlen)
{
	size_t n, r;
#ifndef	KEYSTREAM_ONLY
	memset(rs_buf, 0, RSBUFSZ);
#endif

	chacha_encrypt_bytes(&rs, rs_buf, rs_buf, RSBUFSZ);
	/* with user provided data, we fill a bit more */
	if (dat) {
		r = MIN(datlen, (KEYSZ + IVSZ));
		for (n = 0; n < r; n++)
			rs_buf[n] ^= dat[n];
	}
	
	/* backtracking resistance, we force the reinitialization */
	_rs_init(rs_buf, (KEYSZ + IVSZ));
	memset(rs_buf, 0, (KEYSZ + IVSZ));
	rs_have = (RSBUFSZ - KEYSZ - IVSZ);
}

static __inline void
_rs_random_buf(void *_buf, size_t n)
{
	u_char *buf = (u_char *)_buf;
	u_char *keystream;
	size_t m;

	_rs_stir_if_needed(n);
	while (n > 0) {
		if (rs_have > 0) {
			m = MIN(n, rs_have);
			keystream = (rs_buf + RSBUFSZ - rs_have);
			memcpy(buf, keystream, m);
			memset(keystream, 0, m);
			buf += m;
			n -= m;
			rs_have -= m;
		}

		if (rs_have == 0)
			_rs_rekey(NULL, 0);
	}
}

static __inline void
_rs_random_u32(u_int32_t *val)
{
	u_char *keystream;

	_rs_stir_if_needed(sizeof(*val));
	if (rs_have < sizeof(*val))
		_rs_rekey(NULL, 0);
	keystream = (rs_buf + RSBUFSZ - rs_have);
	memcpy(val, keystream, sizeof(*val));
	memset(keystream, 0, sizeof(*val));
	rs_have -= sizeof(*val);
	return;
}

/*
 * Stir our S-box.
 */
static void
_rs_stir(int lock)
{
	u_int8_t key[KEYSZ + IVSZ], *p;
	int r, n;
	struct timespec ts_now;

	/*
	 * XXX read_random() returns unsafe numbers if the entropy
	 * device is not loaded -- MarkM.
	 */
	r = read_random(key, ARC4_KEYBYTES);
	nanotime(&ts_now);

	if (lock)
		mtx_lock(&arc4_mtx);

	/* If r == 0 || -1, just use what was on the stack. */
	if (r > 0) {
		for (n = r; n < sizeof(key); n++)
			key[n] = key[n % r];
	}

	/* 
	 * Even if read_random does not provide some bytes
	 * we have at least the possibility to fill with some time value
	 */
	for (p = (u_int8_t *)&ts_now, n = 0; n < sizeof(ts_now); n++)
		key[n] ^= p[n];

	_rs_seed(key, sizeof(key));

	arc4_t_reseed = ts_now.tv_sec + ARC4_RESEED_SECONDS;
	arc4_numruns = 0;

	if (lock)
		mtx_unlock(&arc4_mtx);

	explicit_bzero(key, sizeof(key));
}

/*
 * Initialize our S-box to its beginning defaults.
 */
static void
arc4_init(void)
{
	mtx_init(&arc4_mtx, "arc4_mtx", NULL, MTX_DEF);
	_rs_stir(1);

	arc4_t_reseed = 0;
}

SYSINIT(arc4_init, SI_SUB_LOCK, SI_ORDER_ANY, arc4_init, NULL);

/*
 * MPSAFE
 */
void
arc4rand(void *ptr, u_int len, int reseed)
{
	struct timespec ts;

	nanotime(&ts);
	if (atomic_cmpset_int(&arc4rand_iniseed_state, ARC4_ENTR_HAVE,
		ARC4_ENTR_SEED) || reseed ||
		(arc4_numruns > ARC4_RESEED_BYTES) ||
		(ts.tv_sec > arc4_t_reseed))
		_rs_stir(0);

	mtx_lock(&arc4_mtx);
	arc4_numruns += len;

	_rs_random_buf(ptr, len);
	
	mtx_unlock(&arc4_mtx);
}

uint32_t
arc4random(void)
{
	uint32_t ret;
	
	mtx_lock(&arc4_mtx);
	_rs_random_u32(&ret);
	mtx_unlock(&arc4_mtx);

	return (ret);
}
