#ifndef _CONFIGLIGHTNING_H
#define _CONFIGLIGHTNING_H

#if defined(__x86_64__) || defined(_M_X64) || defined(_WIN64)
#define LIGHTNING_X86_64
#define __WORDSIZE 64
#elif defined(__i386__) || defined(_M_IX86)
#define LIGHTNING_I386
#define __WORDSIZE 32
#elif defined(__arm__) || defined(__thumb__) || defined(__thumb2__)
#define LIGHTNING_ARM
#define __WORDSIZE 32
#elif defined(__mips__)
#define LIGHTNING_MIPS
#define __WORDSIZE 32
#elif defined(__mips64__)
#define LIGHTNING_MIPS64
#define __WORDSIZE 64
#elif defined(__ppc__)
#define LIGHTNING_PPC
#define __WORDSIZE 32
#elif defined(__sparc__)
#define LIGHTNING_SPARC
#define __WORDSIZE 32
#else
#error "ERROR: unsupported target platform"
#endif

#endif
