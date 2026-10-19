/*
 * M8 Conformance Suite — Debian bookworm armhf glibc 2.36 & ld-linux-armhf.so.3
 *
 * Dynamically linked against Debian armhf glibc 2.36 (/lib/ld-linux-armhf.so.3,
 * libc.so.6, libm.so.6, libpthread.so.0, libdl.so.2) and compiled in both
 * A32 (-marm) and Thumb-2 (-mthumb) modes.
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#define _GNU_SOURCE

#include <dlfcn.h>
#include <gnu/libc-version.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>

static int g_failures = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, (msg));  \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

static bool near_d(double a, double b, double eps)
{
    double diff = fabs(a - b);
    double scale = fmax(1.0, fmax(fabs(a), fabs(b)));
    return diff <= eps * scale;
}

/* --------------------------------------------------------------------------
 * 1. glibc Version & Auxiliary Vector (getauxval)
 * -------------------------------------------------------------------------- */
static void test_glibc_version_and_auxv(void)
{
    const char *ver = gnu_get_libc_version();
    CHECK(ver != NULL && ver[0] != '\0', "gnu_get_libc_version non-empty");
    if (ver != NULL) {
        char *endp = NULL;
        long major = strtol(ver, &endp, 10);
        long minor = (endp && *endp == '.') ? strtol(endp + 1, NULL, 10) : 0L;
        CHECK(major > 2L || (major == 2L && minor >= 30L),
              "glibc version >= 2.30");
    }

    const char *plat = (const char *)(uintptr_t)getauxval(AT_PLATFORM);
    CHECK(plat != NULL &&
          (strcmp(plat, "v7l") == 0 || strcmp(plat, "v8l") == 0),
          "getauxval(AT_PLATFORM) is v7l/v8l");

    unsigned long hwcap = getauxval(AT_HWCAP);
    /* ARM_HWCAP_VFP = (1 << 6), ARM_HWCAP_NEON = (1 << 12) */
    CHECK((hwcap & (1UL << 6)) != 0UL, "AT_HWCAP has VFP bit");
    CHECK((hwcap & (1UL << 12)) != 0UL, "AT_HWCAP has NEON bit");
}

/* --------------------------------------------------------------------------
 * 2. Dynamic Linker Runtime Resolution (dlopen / dlsym / dlclose)
 * -------------------------------------------------------------------------- */
static void test_dlopen_dlsym(void)
{
    void *h = dlopen("libm.so.6", RTLD_LAZY);
    CHECK(h != NULL, "dlopen(libm.so.6)");
    if (h != NULL) {
        (void)dlerror();
        void *sym = dlsym(h, "cos");
        CHECK(sym != NULL && dlerror() == NULL, "dlsym(libm, cos)");
        if (sym != NULL) {
            double (*fn_cos)(double) = NULL;
            memcpy(&fn_cos, &sym, sizeof(fn_cos));
            double c0 = fn_cos(0.0);
            double cpi3 = fn_cos(3.14159265358979323846 / 3.0);
            CHECK(near_d(c0, 1.0, 1e-14), "dlsym cos(0) == 1.0");
            CHECK(near_d(cpi3, 0.5, 1e-14), "dlsym cos(pi/3) == 0.5");
        }
        CHECK(dlclose(h) == 0, "dlclose(libm.so.6)");
    }
}

/* --------------------------------------------------------------------------
 * 3. glibc NPTL Pthreads, Mutex, Condvar & __thread TLS
 * -------------------------------------------------------------------------- */
static __thread uint32_t g_tls_cookie = 0x55AA1234U;
static __thread uint64_t g_tls_counter = 7ULL;

typedef struct {
    pthread_mutex_t mtx;
    pthread_cond_t  cv;
    uint32_t        sum;
    unsigned        ready_workers;
} nptl_sync_t;

typedef struct {
    nptl_sync_t *sync;
    uint32_t     worker_id;
} nptl_arg_t;

static void *nptl_worker(void *arg)
{
    nptl_arg_t *wa = (nptl_arg_t *)arg;
    CHECK(g_tls_cookie == 0x55AA1234U, "NPTL initial TLS cookie");
    CHECK(g_tls_counter == 7ULL, "NPTL initial TLS counter");

    g_tls_cookie = 0xABCD0000U | wa->worker_id;
    g_tls_counter = (uint64_t)wa->worker_id * 100ULL;

    double v = hypot((double)wa->worker_id * 3.0, (double)wa->worker_id * 4.0);
    uint32_t contrib = (uint32_t)(v + 0.5);

    pthread_mutex_lock(&wa->sync->mtx);
    wa->sync->sum += contrib;
    wa->sync->ready_workers++;
    pthread_cond_broadcast(&wa->sync->cv);
    pthread_mutex_unlock(&wa->sync->mtx);

    CHECK(g_tls_cookie == (0xABCD0000U | wa->worker_id), "NPTL TLS cookie isolated");
    CHECK(g_tls_counter == (uint64_t)wa->worker_id * 100ULL, "NPTL TLS counter isolated");
    return (void *)(uintptr_t)(wa->worker_id * 11U);
}

static void test_nptl_pthreads_tls(void)
{
    nptl_sync_t sync;
    CHECK(pthread_mutex_init(&sync.mtx, NULL) == 0, "pthread_mutex_init");
    CHECK(pthread_cond_init(&sync.cv, NULL) == 0, "pthread_cond_init");
    sync.sum = 0U;
    sync.ready_workers = 0U;

    pthread_t threads[4];
    nptl_arg_t args[4];
    for (uint32_t i = 0; i < 4U; i++) {
        args[i].sync = &sync;
        args[i].worker_id = i + 1U;
        CHECK(pthread_create(&threads[i], NULL, nptl_worker, &args[i]) == 0,
              "pthread_create");
    }

    pthread_mutex_lock(&sync.mtx);
    while (sync.ready_workers < 4U) {
        pthread_cond_wait(&sync.cv, &sync.mtx);
    }
    pthread_mutex_unlock(&sync.mtx);

    for (uint32_t i = 0; i < 4U; i++) {
        void *ret = NULL;
        CHECK(pthread_join(threads[i], &ret) == 0, "pthread_join");
        CHECK((uintptr_t)ret == (i + 1U) * 11U, "pthread_join return value");
    }

    /* Sum of 5 * (1 + 2 + 3 + 4) = 50 */
    CHECK(sync.sum == 50U, "NPTL worker sum == 50");
    CHECK(g_tls_cookie == 0x55AA1234U && g_tls_counter == 7ULL,
          "main thread TLS preserved");

    pthread_cond_destroy(&sync.cv);
    pthread_mutex_destroy(&sync.mtx);
}

/* --------------------------------------------------------------------------
 * 4. glibc qsort, bsearch, snprintf/sscanf & libm (hypot, atan2, erf, tgamma)
 * -------------------------------------------------------------------------- */
static int cmp_u32(const void *a, const void *b)
{
    uint32_t va = *(const uint32_t *)a;
    uint32_t vb = *(const uint32_t *)b;
    return (va > vb) - (va < vb);
}

static void test_algorithms_stdio_libm(void)
{
    uint32_t arr[64];
    uint32_t state = 0x9E3779B9U;
    for (size_t i = 0; i < 64U; i++) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        arr[i] = state;
    }
    uint32_t needle = arr[29];
    qsort(arr, 64U, sizeof(arr[0]), cmp_u32);
    for (size_t i = 1; i < 64U; i++) {
        CHECK(arr[i - 1] <= arr[i], "qsort ascending");
    }
    uint32_t *found = (uint32_t *)bsearch(&needle, arr, 64U, sizeof(arr[0]), cmp_u32);
    CHECK(found != NULL && *found == needle, "bsearch found element");

    char buf[128];
    int n = snprintf(buf, sizeof(buf), "%.8f %.6e", 3.14159265, -2.5e-4);
    CHECK(n > 0 && (size_t)n < sizeof(buf), "snprintf float");
    double v1 = 0.0;
    double v2 = 0.0;
    CHECK(sscanf(buf, "%lf %lf", &v1, &v2) == 2, "sscanf float");
    CHECK(near_d(v1, 3.14159265, 1e-8) && near_d(v2, -2.5e-4, 1e-8),
          "snprintf/sscanf round-trip");

    CHECK(near_d(hypot(5.0, 12.0), 13.0, 1e-14), "hypot(5, 12)");
    CHECK(near_d(atan2(1.0, 1.0), 3.14159265358979323846 / 4.0, 1e-14),
          "atan2(1, 1)");
    CHECK(near_d(erf(0.0), 0.0, 1e-15), "erf(0)");
    CHECK(near_d(erf(1.0), 0.84270079294971486934, 1e-12), "erf(1)");
    CHECK(near_d(tgamma(5.0), 24.0, 1e-13), "tgamma(5) == 4!");
    CHECK(near_d(tgamma(6.0), 120.0, 1e-12), "tgamma(6) == 5!");
}

int main(void)
{
    test_glibc_version_and_auxv();
    test_dlopen_dlsym();
    test_nptl_pthreads_tls();
    test_algorithms_stdio_libm();

    if (g_failures != 0) {
        fprintf(stderr, "M8 glibc 2.36 conformance: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("M8 glibc 2.36 (ld-linux-armhf.so.3, dlopen, NPTL pthreads, TLS, libm): PASS\n");
    return 0;
}
