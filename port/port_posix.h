

#include <cpuid.h>

namespace utils::port {
static inline void AsmVolatilePause() {
#if defined(__i386__) || defined(__x86_64__)
    asm volatile("pause");
#elif defined(__aarch64__)
    asm volatile("isb");
#elif defined(__powerpc64__)
    asm volatile("or 27,27,27");
#elif defined(__loongarch64)
    asm volatile("dbar 0");
#endif
    // it's okay for other platforms to be no-ops
}

int PhysicalCoreID() {
#if defined(ROCKSDB_SCHED_GETCPU_PRESENT) && defined(__x86_64__) && \
    (__GNUC__ > 2 || (__GNUC__ == 2 && __GNUC_MINOR__ >= 22))
    // sched_getcpu uses VDSO getcpu() syscall since 2.22. I believe Linux offers
    // VDSO support only on x86_64. This is the fastest/preferred method if
    // available.
    int cpuno = sched_getcpu();
    if (cpuno < 0) {
        return -1;
    }
    return cpuno;
#elif defined(__x86_64__) || defined(__i386__)
    // clang/gcc both provide cpuid.h, which defines __get_cpuid(), for x86_64 and
    // i386.
    unsigned eax, ebx = 0, ecx, edx;
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
        return -1;
    }
    return ebx >> 24;
#else
    // give up, the caller can generate a random number or something.
    return -1;
#endif
}
}