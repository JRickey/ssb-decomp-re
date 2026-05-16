#ifndef __STDDEF_H__
#define __STDDEF_H__

#include <PR/ultratypes.h>

#if defined(PORT) && !defined(_WINT_T_DECLARED)
typedef __WINT_TYPE__ wint_t;
#define _WINT_T_DECLARED
#endif

#if defined(PORT) && !defined(_PTRDIFF_T_DECLARED)
typedef __PTRDIFF_TYPE__ ptrdiff_t;
#define _PTRDIFF_T_DECLARED
#endif

#ifdef _MSC_VER
/* MSVC: offsetof is a compiler intrinsic */
#ifndef offsetof
#define offsetof(type, member) ((size_t)&(((type*)0)->member))
#endif
#elif !defined(__sgi)
/* GCC/Clang: use built-in offsetof macro */
#define offsetof(type, member) __builtin_offsetof(type, member)
#else
/* IDO: use macro from Indy headers */
#define offsetof(s, m) (size_t)(&(((s*)0)->m))
#endif

#endif /* __STDDEF_H__ */
