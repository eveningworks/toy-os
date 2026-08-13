#ifndef STRING_H
#define STRING_H

#include <stddef.h>
#include <stdint.h>

size_t k_strlen(const char *s);
int k_strcmp(const char *a, const char *b);
void k_memset(void *dst, uint8_t val, size_t n);
void k_memcpy(void *dst, const void *src, size_t n);
char *k_strcpy(char *dst, const char *src);
int k_strncmp(const char *a, const char *b, size_t n);

#endif
