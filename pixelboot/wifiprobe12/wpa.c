/*
 * HackiOS's WPA2 arithmetic, hackios/wifi/lx/src/lx_wpa.c, built into wifiprobe12 as it is (not
 * _KERNEL: it takes <stdint.h> and <string.h>, the latter from include/), and the four string.h
 * functions it needs. Byte loops: -mstrict-align, and every access is to Device memory anyway.
 */
#include <stddef.h>

void *
memcpy(void *dst, const void *src, size_t n)
{
	unsigned char *d = dst;
	const unsigned char *s = src;

	while (n--)
		*d++ = *s++;
	return dst;
}

void *
memset(void *dst, int c, size_t n)
{
	unsigned char *d = dst;

	while (n--)
		*d++ = (unsigned char)c;
	return dst;
}

int
memcmp(const void *a, const void *b, size_t n)
{
	const unsigned char *x = a, *y = b;

	for (; n; n--, x++, y++)
		if (*x != *y)
			return *x < *y ? -1 : 1;
	return 0;
}

size_t
strlen(const char *s)
{
	size_t n = 0;

	while (s[n])
		n++;
	return n;
}

#include "../../hackios/wifi/lx/src/lx_wpa.c"
