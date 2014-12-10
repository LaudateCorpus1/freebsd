/*
 * Copyright (c) 2010 Damien Miller. All rights reserved.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <strings.h>

/**
 * timingsafe_bcmp, unlike bcmp
 * does not stop if the two current bytes differ
 * rather go through the whole n bytes
 */
int
timingsafe_bcmp(const void *b1, const void *b2, size_t n)
{
	char *p1, *p2;
	int r;

	p1 = (char *)b1;
	p2 = (char *)b2;
	r = 0;

	while (n-- != 0)
		r |= *p1++ ^ *p2++;
	
	return (r != 0);
}
