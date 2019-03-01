/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2016 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "../SDL_internal.h"

#if defined(__WIN32__)
#include "../core/windows/SDL_windows.h"
#endif

/* CPU feature detection for SDL */

#include "SDL_cpuinfo.h"

#ifdef HAVE_SYSCONF
#include <unistd.h>
#endif
#ifdef HAVE_SYSCTLBYNAME
#include <sys/types.h>
#include <sys/sysctl.h>
#endif
#if defined(__APPLE__) && (defined(__ppc__) || defined(__ppc64__))
#include <sys/sysctl.h>         /* For AltiVec check */
#elif defined(__OpenBSD__) && defined(__powerpc__)
#include <sys/param.h>
#include <sys/sysctl.h> /* For AltiVec check */
#include <machine/cpu.h>
#endif

#define CPU_HAS_RDTSC   0x00000001
#define CPU_HAS_ALTIVEC 0x00000002
#define CPU_HAS_3DNOW   0x00000008
#define CPU_HAS_SSE     0x00000010
#define CPU_HAS_SSE2    0x00000020
#define CPU_HAS_SSE3    0x00000040
#define CPU_HAS_SSE41   0x00000100
#define CPU_HAS_SSE42   0x00000200
#define CPU_HAS_AVX     0x00000400
#define CPU_HAS_AVX2    0x00000800

static int
CPU_haveCPUID(void)
{
    int has_CPUID = 0;
/* *INDENT-OFF* */
#ifndef SDL_CPUINFO_DISABLED
#if defined(__GNUC__) && defined(i386)
    __asm__ (
"        pushfl                      # Get original EFLAGS             \n"
"        popl    %%eax                                                 \n"
"        movl    %%eax,%%ecx                                           \n"
"        xorl    $0x200000,%%eax     # Flip ID bit in EFLAGS           \n"
"        pushl   %%eax               # Save new EFLAGS value on stack  \n"
"        popfl                       # Replace current EFLAGS value    \n"
"        pushfl                      # Get new EFLAGS                  \n"
"        popl    %%eax               # Store new EFLAGS in EAX         \n"
"        xorl    %%ecx,%%eax         # Can not toggle ID bit,          \n"
"        jz      1f                  # Processor=80486                 \n"
"        movl    $1,%0               # We have CPUID support           \n"
"1:                                                                    \n"
    : "=m" (has_CPUID)
    :
    : "%eax", "%ecx"
    );
#elif defined(__GNUC__) && defined(__x86_64__)
/* Technically, if this is being compiled under __x86_64__ then it has 
   CPUid by definition.  But it's nice to be able to prove it.  :)      */
    __asm__ (
"        pushfq                      # Get original EFLAGS             \n"
"        popq    %%rax                                                 \n"
"        movq    %%rax,%%rcx                                           \n"
"        xorl    $0x200000,%%eax     # Flip ID bit in EFLAGS           \n"
"        pushq   %%rax               # Save new EFLAGS value on stack  \n"
"        popfq                       # Replace current EFLAGS value    \n"
"        pushfq                      # Get new EFLAGS                  \n"
"        popq    %%rax               # Store new EFLAGS in EAX         \n"
"        xorl    %%ecx,%%eax         # Can not toggle ID bit,          \n"
"        jz      1f                  # Processor=80486                 \n"
"        movl    $1,%0               # We have CPUID support           \n"
"1:                                                                    \n"
    : "=m" (has_CPUID)
    :
    : "%rax", "%rcx"
    );
#elif (defined(_MSC_VER) && defined(_M_IX86)) || defined(__WATCOMC__)
    __asm {
        pushfd                      ; Get original EFLAGS
        pop     eax
        mov     ecx, eax
        xor     eax, 200000h        ; Flip ID bit in EFLAGS
        push    eax                 ; Save new EFLAGS value on stack
        popfd                       ; Replace current EFLAGS value
        pushfd                      ; Get new EFLAGS
        pop     eax                 ; Store new EFLAGS in EAX
        xor     eax, ecx            ; Can not toggle ID bit,
        jz      done                ; Processor=80486
        mov     has_CPUID,1         ; We have CPUID support
done:
    }
#elif defined(_MSC_VER) && defined(_M_X64)
    has_CPUID = 1;
#elif defined(__sun) && defined(__i386)
    __asm (
"       pushfl                 \n"
"       popl    %eax           \n"
"       movl    %eax,%ecx      \n"
"       xorl    $0x200000,%eax \n"
"       pushl   %eax           \n"
"       popfl                  \n"
"       pushfl                 \n"
"       popl    %eax           \n"
"       xorl    %ecx,%eax      \n"
"       jz      1f             \n"
"       movl    $1,-8(%ebp)    \n"
"1:                            \n"
    );
#elif defined(__sun) && defined(__amd64)
    __asm (
"       pushfq                 \n"
"       popq    %rax           \n"
"       movq    %rax,%rcx      \n"
"       xorl    $0x200000,%eax \n"
"       pushq   %rax           \n"
"       popfq                  \n"
"       pushfq                 \n"
"       popq    %rax           \n"
"       xorl    %ecx,%eax      \n"
"       jz      1f             \n"
"       movl    $1,-8(%rbp)    \n"
"1:                            \n"
    );
#endif
#endif
/* *INDENT-ON* */
    return has_CPUID;
}

#if defined(__GNUC__) && defined(i386)
#define cpuid(func, a, b, c, d) \
    __asm__ __volatile__ ( \
"        pushl %%ebx        \n" \
"        xorl %%ecx,%%ecx   \n" \
"        cpuid              \n" \
"        movl %%ebx, %%esi  \n" \
"        popl %%ebx         \n" : \
            "=a" (a), "=S" (b), "=c" (c), "=d" (d) : "a" (func))
#elif defined(__GNUC__) && defined(__x86_64__)
#define cpuid(func, a, b, c, d) \
    __asm__ __volatile__ ( \
"        pushq %%rbx        \n" \
"        xorq %%rcx,%%rcx   \n" \
"        cpuid              \n" \
"        movq %%rbx, %%rsi  \n" \
"        popq %%rbx         \n" : \
            "=a" (a), "=S" (b), "=c" (c), "=d" (d) : "a" (func))
#elif (defined(_MSC_VER) && defined(_M_IX86)) || defined(__WATCOMC__)
#define cpuid(func, a, b, c, d) \
    __asm { \
        __asm mov eax, func \
        __asm xor ecx, ecx \
        __asm cpuid \
        __asm mov a, eax \
        __asm mov b, ebx \
        __asm mov c, ecx \
        __asm mov d, edx \
}
#elif defined(_MSC_VER) && defined(_M_X64)
#define cpuid(func, a, b, c, d) \
{ \
    int CPUInfo[4]; \
    __cpuid(CPUInfo, func); \
    a = CPUInfo[0]; \
    b = CPUInfo[1]; \
    c = CPUInfo[2]; \
    d = CPUInfo[3]; \
}
#else
#define cpuid(func, a, b, c, d) \
    a = b = c = d = 0
#endif

static int
CPU_getCPUIDFeatures(void)
{
    int features = 0;
    int a, b, c, d;

    cpuid(0, a, b, c, d);
    if (a >= 1) {
        cpuid(1, a, b, c, d);
        features = d;
    }
    return features;
}

static int
CPU_haveRDTSC(void)
{
    if (CPU_haveCPUID()) {
        return (CPU_getCPUIDFeatures() & 0x00000010);
    }
    return 0;
}

static int
CPU_haveAltiVec(void)
{
    return 0;
}

static int
CPU_haveSSE(void)
{
    if (CPU_haveCPUID()) {
        return (CPU_getCPUIDFeatures() & 0x02000000);
    }
    return 0;
}

static int
CPU_haveSSE2(void)
{
    if (CPU_haveCPUID()) {
        return (CPU_getCPUIDFeatures() & 0x04000000);
    }
    return 0;
}

static int
CPU_haveSSE3(void)
{
    if (CPU_haveCPUID()) {
        int a, b, c, d;

        cpuid(0, a, b, c, d);
        if (a >= 1) {
            cpuid(1, a, b, c, d);
            return (c & 0x00000001);
        }
    }
    return 0;
}

static int
CPU_haveSSE41(void)
{
    if (CPU_haveCPUID()) {
        int a, b, c, d;

        cpuid(0, a, b, c, d);
        if (a >= 1) {
            cpuid(1, a, b, c, d);
            return (c & 0x00080000);
        }
    }
    return 0;
}

static int
CPU_haveSSE42(void)
{
    if (CPU_haveCPUID()) {
        int a, b, c, d;

        cpuid(0, a, b, c, d);
        if (a >= 1) {
            cpuid(1, a, b, c, d);
            return (c & 0x00100000);
        }
    }
    return 0;
}

static uint32_t SDL_CPUFeatures = 0xFFFFFFFF;

static uint32_t
SDL_GetCPUFeatures(void)
{
    if (SDL_CPUFeatures == 0xFFFFFFFF) {
        SDL_CPUFeatures = 0;
        if (CPU_haveRDTSC()) {
            SDL_CPUFeatures |= CPU_HAS_RDTSC;
        }
        if (CPU_haveSSE()) {
            SDL_CPUFeatures |= CPU_HAS_SSE;
        }
        if (CPU_haveSSE2()) {
            SDL_CPUFeatures |= CPU_HAS_SSE2;
        }
        if (CPU_haveSSE3()) {
            SDL_CPUFeatures |= CPU_HAS_SSE3;
        }
        if (CPU_haveSSE41()) {
            SDL_CPUFeatures |= CPU_HAS_SSE41;
        }
        if (CPU_haveSSE42()) {
            SDL_CPUFeatures |= CPU_HAS_SSE42;
        }
    }
    return SDL_CPUFeatures;
}

SDL_bool
SDL_HasRDTSC(void)
{
    if (SDL_GetCPUFeatures() & CPU_HAS_RDTSC) {
        return SDL_TRUE;
    }
    return SDL_FALSE;
}

SDL_bool
SDL_HasAltiVec(void)
{
    return SDL_FALSE;
}

SDL_bool
SDL_Has3DNow(void)
{
    if (SDL_GetCPUFeatures() & CPU_HAS_3DNOW) {
        return SDL_TRUE;
    }
    return SDL_FALSE;
}

SDL_bool
SDL_HasSSE(void)
{
    if (SDL_GetCPUFeatures() & CPU_HAS_SSE) {
        return SDL_TRUE;
    }
    return SDL_FALSE;
}

SDL_bool
SDL_HasSSE2(void)
{
    if (SDL_GetCPUFeatures() & CPU_HAS_SSE2) {
        return SDL_TRUE;
    }
    return SDL_FALSE;
}

SDL_bool
SDL_HasSSE3(void)
{
    if (SDL_GetCPUFeatures() & CPU_HAS_SSE3) {
        return SDL_TRUE;
    }
    return SDL_FALSE;
}

SDL_bool
SDL_HasSSE41(void)
{
    if (SDL_GetCPUFeatures() & CPU_HAS_SSE41) {
        return SDL_TRUE;
    }
    return SDL_FALSE;
}

SDL_bool
SDL_HasSSE42(void)
{
    if (SDL_GetCPUFeatures() & CPU_HAS_SSE42) {
        return SDL_TRUE;
    }
    return SDL_FALSE;
}

/* vi: set ts=4 sw=4 expandtab: */
