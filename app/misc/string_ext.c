#include "misc/string_ext.h"

char *c_strnstr(const char *haystack, const char *needle, size_t len)
{
    int i;
    size_t needle_len;

    if (0 == (needle_len = (size_t) c_strlen(needle)))
        return (char *)haystack;

    for (i=0; i<=(int)(len-needle_len); i++)
    {
        if ((haystack[0] == needle[0]) &&
            (0 == c_strncmp(haystack, needle, needle_len)))
            return (char *)haystack;

        haystack++;
    }
    return NULL;
}

int c_strncasecmp(const char *s1, const char *s2, size_t n) {
    if (n == 0)
        return 0;

    while (n-- != 0 && tolower(*s1) == tolower(*s2)) {
        if (n == 0 || *s1 == '\0' || *s2 == '\0')
            break;
        s1++;
        s2++;
    }

    return tolower(*(unsigned char *) s1) - tolower(*(unsigned char *) s2);
}

char *c_strncasestr(const char *haystack, const char *needle, size_t len)
{
    int i;
    size_t needle_len;

    if (0 == (needle_len = (size_t) c_strlen(needle)))
        return (char *)haystack;

    for (i=0; i<=(int)(len-needle_len); i++)
    {
        if ((tolower(haystack[0]) == tolower(needle[0])) &&
            (0 == strncasecmp(haystack, needle, needle_len)))
            return (char *)haystack;

        haystack++;
    }
    return NULL;
}