#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(_MSC_VER) && !defined(__clang__)
  #define PORT_MSVC 1
  #include <intrin.h>
  #include <stdlib.h>
  /* No <Windows.h>: this header reaches every lifted chunk (via ppu_recomp.h),
     where its cost and its min/max macros are unwelcome. Include it yourself. */
#else
  #define PORT_MSVC 0
#endif

#ifdef __cplusplus
  #define PORT_INLINE inline
#else
  #define PORT_INLINE static inline
#endif

#if PORT_MSVC
  #define PORT_NOINLINE __declspec(noinline)
#else
  #define PORT_NOINLINE __attribute__((noinline))
#endif


/* ---------------------------------------------------------------------------
 * 1. Weak symbols
 *
 * Usage -- the default implementation, in exactly one translation unit:
 *
 *     void PORT_WEAK_DEF(on_event)(void) { }
 *     PORT_WEAK_ALIAS(on_event, on_event_default);
 *
 * Any other TU may then define a plain `void on_event(void)` and it wins.
 * If nobody does, calls land on the default.
 *
 * MSVC caveats:
 *   - The pragma needs the *decorated* name. The macro assumes __cdecl C
 *     linkage; wrap C++ functions in extern "C" or the mangled name won't
 *     match. For __stdcall on x86 you need "_name@N" and must spell it out.
 *   - This is link-time substitution: it does not work across a DLL boundary,
 *     because the import library defines the symbol.
 * ------------------------------------------------------------------------ */
#if PORT_MSVC
  #define PORT_WEAK_DEF(name) name##_default
  #if defined(_M_IX86)
    #define PORT_WEAK_ALIAS(ret, name) \
        __pragma(comment(linker, "/alternatename:_" #name "=_" #name "_default")) \
        extern "C" ret name##_default
  #else
    #define PORT_WEAK_ALIAS(ret, name) \
        __pragma(comment(linker, "/alternatename:" #name "=" #name "_default")) \
        extern "C" ret name##_default
  #endif
#else
  #define PORT_WEAK_DEF(name) __attribute__((weak)) name
  #define PORT_WEAK_ALIAS(ret, name) extern "C" ret name##_default
#endif

/* Multiple TUs may define this initialized global; the linker keeps one. */
#if PORT_MSVC
  #define PORT_SELECTANY __declspec(selectany)
#else
  #define PORT_SELECTANY __attribute__((weak))
#endif


/* ---------------------------------------------------------------------------
 * 2. Return addresses
 *
 * PORT_RETURN_ADDRESS() is cheap (one load). PORT_RETURN_ADDRESS_N(n) walks
 * the unwinder on MSVC and costs hundreds of instructions per frame.
 *
 * Mark any function using these PORT_NOINLINE. If it gets inlined, both
 * compilers silently report the caller of whatever it was folded into.
 *
 * Level n may legitimately be unavailable (top of thread, frames without
 * unwind data) -- PORT_RETURN_ADDRESS_N returns NULL there on MSVC, whereas
 * __builtin_return_address returns garbage or faults. Always null-check.
 * ------------------------------------------------------------------------ */
#if PORT_MSVC
  #pragma intrinsic(_ReturnAddress, _AddressOfReturnAddress)
  #define PORT_RETURN_ADDRESS()      _ReturnAddress()
  #define PORT_ADDRESS_OF_RETURN_ADDRESS() _AddressOfReturnAddress()

  /* Declared by hand to avoid dragging <Windows.h> into every consumer.
     Matches winnt.h exactly, so including both is fine. */
  #ifdef __cplusplus
  extern "C"
  #endif
  __declspec(dllimport) unsigned short __stdcall RtlCaptureStackBackTrace(
      unsigned long FramesToSkip, unsigned long FramesToCapture,
      void **BackTrace, unsigned long *BackTraceHash);

  PORT_NOINLINE PORT_INLINE void *port_return_address_n(unsigned n)
  {
      void *frames[64];
      unsigned short got;
      if (n >= 63u) return NULL;
      /* Skip 1 to drop this helper, aligning frames[0] with level 0. */
      got = RtlCaptureStackBackTrace(1ul, (unsigned long)(n + 1u), frames, NULL);
      return (got > n) ? frames[n] : NULL;
  }
  /* n must be a constant on GCC, so keep the call shape identical. */
  #define PORT_RETURN_ADDRESS_N(n) port_return_address_n((unsigned)(n))
#else
  #define PORT_RETURN_ADDRESS()      __builtin_return_address(0)
  #define PORT_ADDRESS_OF_RETURN_ADDRESS() \
      ((void *)((char *)__builtin_frame_address(0) + sizeof(void *)))
  /* GCC warns (-Wframe-address) on every nonzero level, since the result is
     only meaningful when the whole chain has frame pointers. That is a real
     caveat, but it belongs in the docs above, not in each caller's build log. */
  #define PORT_RETURN_ADDRESS_N(n)                                        \
      __extension__ ({                                                    \
          _Pragma("GCC diagnostic push")                                  \
          _Pragma("GCC diagnostic ignored \"-Wframe-address\"")           \
          void *port_ra_ = __builtin_return_address(n);                   \
          _Pragma("GCC diagnostic pop")                                   \
          port_ra_;                                                       \
      })
#endif


/* ---------------------------------------------------------------------------
 * 3. Byte swapping
 *
 * The PORT_BSWAPnn_CONST forms are usable in constant expressions everywhere;
 * MSVC's intrinsics are not constant-foldable, so a constexpr context needs
 * these. The optimizer still emits bswap/rev for them at runtime.
 *
 * In C++23, prefer std::byteswap from <bit>.
 * ------------------------------------------------------------------------ */
#define PORT_BSWAP16_CONST(v) \
    ((uint16_t)((((uint16_t)(v) & 0x00FFu) << 8) | \
                (((uint16_t)(v) & 0xFF00u) >> 8)))

#define PORT_BSWAP32_CONST(v) \
    ((uint32_t)((((uint32_t)(v) & 0x000000FFu) << 24) | \
                (((uint32_t)(v) & 0x0000FF00u) <<  8) | \
                (((uint32_t)(v) & 0x00FF0000u) >>  8) | \
                (((uint32_t)(v) & 0xFF000000u) >> 24)))

#define PORT_BSWAP64_CONST(v) \
    ((uint64_t)((((uint64_t)(v) & 0x00000000000000FFull) << 56) | \
                (((uint64_t)(v) & 0x000000000000FF00ull) << 40) | \
                (((uint64_t)(v) & 0x0000000000FF0000ull) << 24) | \
                (((uint64_t)(v) & 0x00000000FF000000ull) <<  8) | \
                (((uint64_t)(v) & 0x000000FF00000000ull) >>  8) | \
                (((uint64_t)(v) & 0x0000FF0000000000ull) >> 24) | \
                (((uint64_t)(v) & 0x00FF000000000000ull) >> 40) | \
                (((uint64_t)(v) & 0xFF00000000000000ull) >> 56)))

#if PORT_MSVC
  /* Sized by Windows type widths: unsigned long is 32-bit here, not 64. */
  #pragma intrinsic(_byteswap_ushort, _byteswap_ulong, _byteswap_uint64)
  #define PORT_BSWAP16(v) _byteswap_ushort((unsigned short)(v))
  #define PORT_BSWAP32(v) _byteswap_ulong((unsigned long)(v))
  #define PORT_BSWAP64(v) _byteswap_uint64((unsigned __int64)(v))
#else
  #define PORT_BSWAP16(v) __builtin_bswap16((uint16_t)(v))
  #define PORT_BSWAP32(v) __builtin_bswap32((uint32_t)(v))
  #define PORT_BSWAP64(v) __builtin_bswap64((uint64_t)(v))
#endif


/* ---------------------------------------------------------------------------
 * 4. Sequentially consistent atomics (32- and 64-bit)
 *
 * Every operation here is seq_cst, matching __ATOMIC_SEQ_CST. Pointers must
 * be naturally aligned.
 *
 * In C++, std::atomic is a better choice than any of this -- it emits a plain
 * mov for the load and xchg for the store, which is optimal on x86-64. These
 * wrappers exist for C, where <stdatomic.h> needs VS 2022 17.5+ with
 * /std:c11 (and on some toolsets /experimental:c11atomics).
 *
 * Implementation note: MSVC has no interlocked load, so the load below is
 * _InterlockedOr(p, 0) -- a full-barrier RMW. Correct on x86 and ARM64, but
 * it dirties the cache line and cannot be used on read-only memory. On
 * x86/x64 only, a plain aligned load is already seq_cst provided stores go
 * through xchg, so you can substitute one if loads are hot.
 * ------------------------------------------------------------------------ */
#if PORT_MSVC

PORT_INLINE int32_t port_atomic_load32(volatile int32_t *p)
{ return (int32_t)_InterlockedOr((volatile long *)p, 0); }

PORT_INLINE void port_atomic_store32(volatile int32_t *p, int32_t v)
{ (void)_InterlockedExchange((volatile long *)p, (long)v); }

PORT_INLINE void port_atomic_storeu32(volatile uint32_t *p, uint32_t v)
{ (void)_InterlockedExchange((volatile long *)p, (long)v); }

PORT_INLINE int32_t port_atomic_exchange32(volatile int32_t *p, int32_t v)
{ return (int32_t)_InterlockedExchange((volatile long *)p, (long)v); }

PORT_INLINE int32_t port_atomic_fetch_add32(volatile int32_t *p, int32_t v)
{ return (int32_t)_InterlockedExchangeAdd((volatile long *)p, (long)v); }

PORT_INLINE uint32_t port_atomic_fetch_addu32(volatile uint32_t*p, uint32_t v)
{ return (uint32_t)_InterlockedExchangeAdd((volatile long *)p, (long)v); }

PORT_INLINE int32_t port_atomic_add_fetch32(volatile int32_t* p, int32_t v)
{
    uint32_t old = (uint32_t)_InterlockedExchangeAdd((volatile long*)p, (long)v);
    return (int32_t)(old + (uint32_t)v);
}

PORT_INLINE int32_t port_atomic_fetch_sub32(volatile int32_t *p, int32_t v)
{ return (int32_t)_InterlockedExchangeAdd((volatile long *)p, -(long)v); }

PORT_INLINE int32_t port_atomic_fetch_or32(volatile int32_t *p, int32_t v)
{ return (int32_t)_InterlockedOr((volatile long *)p, (long)v); }

PORT_INLINE int32_t port_atomic_fetch_and32(volatile int32_t *p, int32_t v)
{ return (int32_t)_InterlockedAnd((volatile long *)p, (long)v); }

PORT_INLINE int32_t port_atomic_fetch_xor32(volatile int32_t *p, int32_t v)
{ return (int32_t)_InterlockedXor((volatile long *)p, (long)v); }

/* GCC-shaped CAS: returns success, and writes the observed value back into
   *expected on failure. _InterlockedCompareExchange itself takes
   (dest, exchange, comparand) and returns the old value, so the loop around
   it has to be rewritten rather than macro-substituted. */
PORT_INLINE int port_atomic_cas32(volatile int32_t *p, int32_t *expected, int32_t desired)
{
    long old = _InterlockedCompareExchange((volatile long *)p,
                                           (long)desired, (long)*expected);
    if (old == (long)*expected) return 1;
    *expected = (int32_t)old;
    return 0;
}

PORT_INLINE int port_atomic_casU32(volatile uint32_t* p, uint32_t* expected, uint32_t desired)
{
    long old = _InterlockedCompareExchange((volatile long*)p,
        (long)desired, (long)*expected);
    if (old == (long)*expected) return 1;
    *expected = (uint32_t)old;
    return 0;
}


/* 64-bit. On 32-bit x86 only CompareExchange64 exists (cmpxchg8b), so the
   rest are built from CAS loops. */
PORT_INLINE int port_atomic_cas64(volatile int64_t *p, int64_t *expected, int64_t desired)
{
    __int64 old = _InterlockedCompareExchange64((volatile __int64 *)p,
                                                (__int64)desired,
                                                (__int64)*expected);
    if (old == (__int64)*expected) return 1;
    *expected = (int64_t)old;
    return 0;
}

PORT_INLINE int port_atomic_casU64(volatile uint64_t* p, uint64_t* expected, uint64_t desired)
{
    __int64 old = _InterlockedCompareExchange64((volatile __int64*)p,
        (__int64)desired,
        (__int64)*expected);
    if (old == (__int64)*expected) return 1;
    *expected = (uint64_t)old;
    return 0;
}

PORT_INLINE int64_t port_atomic_load64(volatile int64_t *p)
{
    int64_t expected = 0;
    (void)port_atomic_cas64(p, &expected, 0); /* expected := observed value */
    return expected;
}

#if defined(_M_IX86)
  PORT_INLINE int64_t port_atomic_exchange64(volatile int64_t *p, int64_t v)
  {
      int64_t e = port_atomic_load64(p);
      while (!port_atomic_cas64(p, &e, v)) { }
      return e;
  }
  PORT_INLINE int64_t port_atomic_fetch_add64(volatile int64_t *p, int64_t v)
  {
      int64_t e = port_atomic_load64(p);
      while (!port_atomic_cas64(p, &e, e + v)) { }
      return e;
  }
#else
  PORT_INLINE int64_t port_atomic_exchange64(volatile int64_t *p, int64_t v)
  { return (int64_t)_InterlockedExchange64((volatile __int64 *)p, (__int64)v); }
  PORT_INLINE int64_t port_atomic_fetch_add64(volatile int64_t *p, int64_t v)
  { return (int64_t)_InterlockedExchangeAdd64((volatile __int64 *)p, (__int64)v); }
#endif

PORT_INLINE void port_atomic_store64(volatile int64_t *p, int64_t v)
{ (void)port_atomic_exchange64(p, v); }

PORT_INLINE void port_atomic_thread_fence(void)
{
#if defined(_M_ARM64) || defined(_M_ARM)
    __dmb(0xB /* _ARM64_BARRIER_ISH */);
#else
    long tmp = 0;
    (void)_InterlockedOr(&tmp, 0);  /* locked op == full barrier on x86 */
#endif
}

#else  /* GCC / Clang */

PORT_INLINE int32_t port_atomic_load32(volatile int32_t *p)
{ return __atomic_load_n(p, __ATOMIC_SEQ_CST); }

PORT_INLINE void port_atomic_store32(volatile int32_t *p, int32_t v)
{ __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }

PORT_INLINE int32_t port_atomic_exchange32(volatile int32_t *p, int32_t v)
{ return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }

PORT_INLINE int32_t port_atomic_fetch_add32(volatile int32_t *p, int32_t v)
{ return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST); }

PORT_INLINE int32_t port_atomic_add_fetch32(volatile int32_t* p, int32_t v)
{ return __atomic_add_fetch(p, v, __ATOMIC_RELAXED) }

PORT_INLINE int32_t port_atomic_fetch_sub32(volatile int32_t *p, int32_t v)
{ return __atomic_fetch_sub(p, v, __ATOMIC_SEQ_CST); }

PORT_INLINE int32_t port_atomic_fetch_or32(volatile int32_t *p, int32_t v)
{ return __atomic_fetch_or(p, v, __ATOMIC_SEQ_CST); }

PORT_INLINE int32_t port_atomic_fetch_and32(volatile int32_t *p, int32_t v)
{ return __atomic_fetch_and(p, v, __ATOMIC_SEQ_CST); }

PORT_INLINE int32_t port_atomic_fetch_xor32(volatile int32_t *p, int32_t v)
{ return __atomic_fetch_xor(p, v, __ATOMIC_SEQ_CST); }

PORT_INLINE int port_atomic_cas32(volatile int32_t *p, int32_t *expected,
                                  int32_t desired)
{ return __atomic_compare_exchange_n(p, expected, desired, 0,
                                     __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }

PORT_INLINE int64_t port_atomic_load64(volatile int64_t *p)
{ return __atomic_load_n(p, __ATOMIC_SEQ_CST); }

PORT_INLINE void port_atomic_store64(volatile int64_t *p, int64_t v)
{ __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }

PORT_INLINE int64_t port_atomic_exchange64(volatile int64_t *p, int64_t v)
{ return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }

PORT_INLINE int64_t port_atomic_fetch_add64(volatile int64_t *p, int64_t v)
{ return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST); }

PORT_INLINE int port_atomic_cas64(volatile int64_t *p, int64_t *expected,
                                  int64_t desired)
{ return __atomic_compare_exchange_n(p, expected, desired, 0,
                                     __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }

PORT_INLINE void port_atomic_thread_fence(void)
{ __atomic_thread_fence(__ATOMIC_SEQ_CST); }

#endif /* atomics */


#if defined(_MSC_VER) && !defined(__clang__)
#define PORT_LIKELY(x)   (!!(x))
#define PORT_UNLIKELY(x) (!!(x))
#else
#define PORT_LIKELY(x)   __builtin_expect(!!(x), 1)
#define PORT_UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif


/* ---------------------------------------------------------------------------
 * 5. GCC __atomic_* builtins for cl.exe
 *
 * Upstream code uses the GCC/Clang __atomic builtins directly. cl has none of
 * them, so provide them here under their own names -- call sites from upstream
 * then build unchanged. Every operation is sequentially consistent whatever
 * order is passed (always at least as strong as asked), except that a relaxed or
 * release store is a plain store, which is all x64 needs. Sizes 1/2/4/8 only.
 *
 * The result type follows the pointee: _Generic in C, a template in C++.
 * Pointer-typed pointees come back as void* in C, which converts implicitly.
 * ------------------------------------------------------------------------ */
#if PORT_MSVC && !defined(PS3_MSVC_ATOMIC_BUILTINS)
#define PS3_MSVC_ATOMIC_BUILTINS 1

#define __ATOMIC_RELAXED 0
#define __ATOMIC_CONSUME 1
#define __ATOMIC_ACQUIRE 2
#define __ATOMIC_RELEASE 3
#define __ATOMIC_ACQ_REL 4
#define __ATOMIC_SEQ_CST 5

PORT_INLINE long long ps3a_load(size_t n, const volatile void* p)
{
    long long v;
    switch (n) {   /* x64: aligned loads are atomic; volatile keeps them in order */
    case 1:  v = *(const volatile char*)p; break;
    case 2:  v = *(const volatile short*)p; break;
    case 4:  v = *(const volatile long*)p; break;
    default: v = *(const volatile long long*)p; break;
    }
    _ReadWriteBarrier();
    return v;
}
PORT_INLINE void ps3a_store(size_t n, volatile void* p, long long v)
{
    _ReadWriteBarrier();
    switch (n) {   /* x64: aligned stores are atomic and already release-ordered */
    case 1:  *(volatile char*)p = (char)v; break;
    case 2:  *(volatile short*)p = (short)v; break;
    case 4:  *(volatile long*)p = (long)v; break;
    default: *(volatile long long*)p = v; break;
    }
}
PORT_INLINE long long ps3a_xchg(size_t n, volatile void* p, long long v)
{
    switch (n) {
    case 1:  return _InterlockedExchange8((volatile char*)p, (char)v);
    case 2:  return _InterlockedExchange16((volatile short*)p, (short)v);
    case 4:  return _InterlockedExchange((volatile long*)p, (long)v);
    default: return _InterlockedExchange64((volatile long long*)p, v);
    }
}
PORT_INLINE long long ps3a_fadd(size_t n, volatile void* p, long long v)
{
    switch (n) {
    case 1:  return _InterlockedExchangeAdd8((volatile char*)p, (char)v);
    case 2:  return _InterlockedExchangeAdd16((volatile short*)p, (short)v);
    case 4:  return _InterlockedExchangeAdd((volatile long*)p, (long)v);
    default: return _InterlockedExchangeAdd64((volatile long long*)p, v);
    }
}
PORT_INLINE long long ps3a_fand(size_t n, volatile void* p, long long v)
{
    switch (n) {
    case 1:  return _InterlockedAnd8((volatile char*)p, (char)v);
    case 2:  return _InterlockedAnd16((volatile short*)p, (short)v);
    case 4:  return _InterlockedAnd((volatile long*)p, (long)v);
    default: return _InterlockedAnd64((volatile long long*)p, v);
    }
}
PORT_INLINE long long ps3a_for(size_t n, volatile void* p, long long v)
{
    switch (n) {
    case 1:  return _InterlockedOr8((volatile char*)p, (char)v);
    case 2:  return _InterlockedOr16((volatile short*)p, (short)v);
    case 4:  return _InterlockedOr((volatile long*)p, (long)v);
    default: return _InterlockedOr64((volatile long long*)p, v);
    }
}
/* Strong CAS: on failure *expected receives the current value. */
PORT_INLINE int ps3a_cas(size_t n, volatile void* p, void* expected, long long desired)
{
    switch (n) {
    case 1: { char e = *(char*)expected, o = _InterlockedCompareExchange8((volatile char*)p, (char)desired, e);
              if (o == e) return 1; *(char*)expected = o; return 0; }
    case 2: { short e = *(short*)expected, o = _InterlockedCompareExchange16((volatile short*)p, (short)desired, e);
              if (o == e) return 1; *(short*)expected = o; return 0; }
    case 4: { long e = *(long*)expected, o = _InterlockedCompareExchange((volatile long*)p, (long)desired, e);
              if (o == e) return 1; *(long*)expected = o; return 0; }
    default: { long long e = *(long long*)expected, o = _InterlockedCompareExchange64((volatile long long*)p, desired, e);
              if (o == e) return 1; *(long long*)expected = o; return 0; }
    }
}

#ifdef __cplusplus
#include <cstring>
#include <type_traits>
extern "C++" {   /* templates need C++ linkage, even inside an extern "C" block */
template <class P> using ps3a_val_t = std::remove_cv_t<std::remove_reference_t<P>>;
template <class T> static inline T ps3a_ret(long long v) { T r; std::memcpy(&r, &v, sizeof(T)); return r; }
template <class T> static inline long long ps3a_arg(T v) { long long r = 0; std::memcpy(&r, &v, sizeof(T)); return r; }
}
#define PS3A_RET(p, v) ps3a_ret<ps3a_val_t<decltype(*(p))>>(v)
#define PS3A_ARG(p, v) ps3a_arg<ps3a_val_t<decltype(*(p))>>((ps3a_val_t<decltype(*(p))>)(v))
#else
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 201112L
#error "ps3emu/portable_builtins.h: the __atomic shim needs C11 _Generic -- compile C with /std:c11 or /std:c17"
#endif
#define PS3A_RET(p, v) _Generic(*(p), \
    _Bool: (_Bool)(v), char: (char)(v), unsigned char: (unsigned char)(v), /* cl: signed char == char here */ \
    short: (short)(v), unsigned short: (unsigned short)(v), int: (int)(v), unsigned int: (unsigned int)(v), \
    long: (long)(v), unsigned long: (unsigned long)(v), long long: (long long)(v), \
    unsigned long long: (unsigned long long)(v), default: (void*)(uintptr_t)(v))
#define PS3A_ARG(p, v) ((long long)(v))
#endif

#define __atomic_load_n(p, o)          PS3A_RET(p, ps3a_load(sizeof(*(p)), (p)))
#define __atomic_store_n(p, v, o)      ((o) == __ATOMIC_SEQ_CST \
    ? (void)ps3a_xchg(sizeof(*(p)), (p), PS3A_ARG(p, v)) : ps3a_store(sizeof(*(p)), (p), PS3A_ARG(p, v)))
#define __atomic_exchange_n(p, v, o)   PS3A_RET(p, ps3a_xchg(sizeof(*(p)), (p), PS3A_ARG(p, v)))
#define __atomic_fetch_add(p, v, o)    PS3A_RET(p, ps3a_fadd(sizeof(*(p)), (p), (long long)(v)))
#define __atomic_fetch_sub(p, v, o)    PS3A_RET(p, ps3a_fadd(sizeof(*(p)), (p), -(long long)(v)))
#define __atomic_add_fetch(p, v, o)    PS3A_RET(p, ps3a_fadd(sizeof(*(p)), (p), (long long)(v)) + (long long)(v))
#define __atomic_sub_fetch(p, v, o)    PS3A_RET(p, ps3a_fadd(sizeof(*(p)), (p), -(long long)(v)) - (long long)(v))
#define __atomic_fetch_and(p, v, o)    PS3A_RET(p, ps3a_fand(sizeof(*(p)), (p), (long long)(v)))
#define __atomic_fetch_or(p, v, o)     PS3A_RET(p, ps3a_for(sizeof(*(p)), (p), (long long)(v)))
#define __atomic_compare_exchange_n(p, e, d, w, s, f) \
    ps3a_cas(sizeof(*(p)), (p), (void*)(e), PS3A_ARG(p, d))
#define __atomic_test_and_set(p, o)    (ps3a_xchg(1, (p), 1) != 0)
#define __atomic_clear(p, o)           ((void)ps3a_xchg(1, (p), 0))
#define __atomic_thread_fence(o)       _mm_mfence()

/* Branch hint only: cl has no equivalent, the value is what matters. */
#ifndef __builtin_expect
#define __builtin_expect(x, expected) (x)
#endif

/* Same widths and results as GCC's. */
#ifndef __builtin_bswap16
#define __builtin_bswap16(x) PORT_BSWAP16(x)
#define __builtin_bswap32(x) PORT_BSWAP32(x)
#define __builtin_bswap64(x) PORT_BSWAP64(x)
#endif

#endif /* MSVC __atomic builtins */
