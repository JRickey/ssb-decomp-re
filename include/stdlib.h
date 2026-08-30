#ifndef __STDLIB_H__
#define __STDLIB_H__

#ifndef NULL
#define NULL 0
#endif

#ifdef PORT
#include <stddef.h>
#if defined(_MSC_VER)
__declspec(noreturn) extern void abort(void);
#else
extern void abort(void) __attribute__((noreturn));
#endif
extern void *malloc(size_t size);
extern void free(void *ptr);
extern void *realloc(void *ptr, size_t size);
extern char *getenv(const char *name);
extern unsigned long strtoul(const char *str, char **endptr, int base);
#endif

typedef struct lldiv_t
{
	long long quot;
	long long rem;
} lldiv_t;

typedef struct ldiv_t
{
	long quot;
	long rem;
} ldiv_t;

lldiv_t lldiv(long long num, long long denom);
ldiv_t ldiv(long num, long denom);
#endif /* !__STDLIB_H__ */
