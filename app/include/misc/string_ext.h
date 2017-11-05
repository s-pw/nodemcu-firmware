#ifndef __STRINGEXT_H__
#define __STRINGEXT_H__
#include "c_stdio.h"
#include "c_stdlib.h"
#include "c_string.h"
#include "ctype.h"

char *c_strnstr(const char *haystack, const char *needle, size_t len);
int c_strncasecmp(const char *s1, const char *s2, size_t n);
char *c_strncasestr(const char *haystack, const char *needle, size_t len);

#endif // __STRINGEXT_H__
