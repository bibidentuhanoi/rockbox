/*
        assert.h
*/

#include "_ansi.h"

#undef assert

#ifdef NDEBUG           /* required by ANSI standard */
#define assert(p)       ((void)0)
#else

#ifdef __STDC__
#define assert(e)       ((e) ? (void)0 : __assert(__FILE__, __LINE__, #e))
#else   /* PCC */
#define assert(e)       ((e) ? (void)0 : __assert(__FILE__, __LINE__, "e"))
#endif

#endif /* NDEBUG */

void _EXFUN(__assert,(const char *, int, const char *));

/* Provide static_assert for C11+ so ESP-IDF headers that do
 * #include "assert.h" (and find this file before the system one)
 * still get a working static_assert / _Static_assert alias. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#ifndef static_assert
#define static_assert _Static_assert
#endif
#endif

