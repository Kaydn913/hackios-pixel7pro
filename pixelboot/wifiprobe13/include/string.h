/* What hackios/wifi/lx/src/lx_wpa.c needs of <string.h> in a freestanding build; wpa.c has them. */
#ifndef WIFIPROBE12_STRING_H
#define WIFIPROBE12_STRING_H

#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);

#endif
