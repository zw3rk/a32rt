/*
 * M7 Conformance Suite — musl libc-test, IEEE-754 libm, regex, setjmp & pthreads
 *
 * Dynamically linked against Alpine armv7 musl (/lib/ld-musl-armhf.so.1) and
 * compiled in both A32 (-marm) and Thumb-2 (-mthumb) modes.
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include <errno.h>
#include <fenv.h>
#include <float.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <regex.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

static bool near_f(float a, float b, float eps)
{
    float diff = fabsf(a - b);
    float scale = fmaxf(1.0f, fmaxf(fabsf(a), fabsf(b)));
    return diff <= eps * scale;
}

/* --------------------------------------------------------------------------
 * 1. IEEE-754 & libm Transcendentals, Classification, Subnormals & Rounding
 * -------------------------------------------------------------------------- */
static void test_libm_ieee754(void)
{
    const double pi = 3.14159265358979323846;

    /* Trigonometric & inverse trigonometric */
    CHECK(near_d(sin(0.0), 0.0, 1e-15), "sin(0)");
    CHECK(near_d(cos(0.0), 1.0, 1e-15), "cos(0)");
    CHECK(near_d(sin(pi / 6.0), 0.5, 1e-14), "sin(pi/6)");
    CHECK(near_d(cos(pi / 3.0), 0.5, 1e-14), "cos(pi/3)");
    CHECK(near_d(tan(pi / 4.0), 1.0, 1e-14), "tan(pi/4)");
    CHECK(near_d(asin(0.5), pi / 6.0, 1e-14), "asin(0.5)");
    CHECK(near_d(acos(0.5), pi / 3.0, 1e-14), "acos(0.5)");
    CHECK(near_d(atan(1.0), pi / 4.0, 1e-14), "atan(1.0)");
    CHECK(near_d(atan2(1.0, 1.0), pi / 4.0, 1e-14), "atan2(1,1)");
    CHECK(near_d(atan2(-1.0, -1.0), -3.0 * pi / 4.0, 1e-14), "atan2(-1,-1)");

    /* Hyperbolic */
    CHECK(near_d(sinh(0.0), 0.0, 1e-15), "sinh(0)");
    CHECK(near_d(cosh(0.0), 1.0, 1e-15), "cosh(0)");
    CHECK(near_d(cosh(1.5) * cosh(1.5) - sinh(1.5) * sinh(1.5), 1.0, 1e-13),
          "cosh^2 - sinh^2 == 1");
    CHECK(near_d(tanh(0.5), sinh(0.5) / cosh(0.5), 1e-14), "tanh(0.5)");

    /* Exponential, logarithmic & power */
    CHECK(near_d(exp(0.0), 1.0, 1e-15), "exp(0)");
    CHECK(near_d(exp(1.0), 2.71828182845904523536, 1e-14), "exp(1)");
    CHECK(near_d(exp2(10.0), 1024.0, 1e-14), "exp2(10)");
    CHECK(near_d(expm1(1e-8), 1e-8 + 0.5e-16, 1e-14), "expm1(1e-8)");
    CHECK(near_d(log(1.0), 0.0, 1e-15), "log(1)");
    CHECK(near_d(log(exp(2.5)), 2.5, 1e-14), "log(exp(2.5))");
    CHECK(near_d(log2(65536.0), 16.0, 1e-14), "log2(65536)");
    CHECK(near_d(log10(1000000.0), 6.0, 1e-14), "log10(1e6)");
    CHECK(near_d(log1p(1e-9), 1e-9, 1e-7), "log1p(1e-9)");
    CHECK(near_d(pow(2.0, 10.0), 1024.0, 1e-14), "pow(2,10)");
    CHECK(near_d(pow(9.0, 0.5), 3.0, 1e-14), "pow(9,0.5)");

    /* Roots, hypot, fma, remainder, decomposition */
    CHECK(near_d(sqrt(2.0), 1.41421356237309504880, 1e-15), "sqrt(2)");
    CHECK(near_d(cbrt(27.0), 3.0, 1e-14), "cbrt(27)");
    CHECK(near_d(cbrt(-125.0), -5.0, 1e-14), "cbrt(-125)");
    CHECK(near_d(hypot(3.0, 4.0), 5.0, 1e-14), "hypot(3,4)");
    CHECK(near_d(hypot(3e200, 4e200), 5e200, 1e-14), "hypot overflow-safe");
    CHECK(near_d(fma(1.5, 2.0, 0.5), 3.5, 1e-15), "fma(1.5,2.0,0.5)");
    CHECK(near_f(fmaf(1.5f, 2.0f, 0.5f), 3.5f, 1e-6f), "fmaf(1.5,2.0,0.5)");
    CHECK(near_d(fmod(5.5, 2.0), 1.5, 1e-15), "fmod(5.5,2.0)");
    CHECK(near_d(remainder(5.5, 2.0), -0.5, 1e-15), "remainder(5.5,2.0)");

    int quo = 0;
    double rem = remquo(11.5, 3.0, &quo);
    CHECK(near_d(rem, -0.5, 1e-15) && (quo & 7) == 4, "remquo(11.5,3.0)");

    double ipart = 0.0;
    double fpart = modf(-3.75, &ipart);
    CHECK(ipart == -3.0 && fpart == -0.75, "modf(-3.75)");

    int exp_out = 0;
    double mant = frexp(24.0, &exp_out);
    CHECK(mant == 0.75 && exp_out == 5, "frexp(24.0)");
    CHECK(ldexp(0.75, 5) == 24.0, "ldexp(0.75,5)");
    CHECK(scalbn(1.5, 4) == 24.0, "scalbn(1.5,4)");

    /* Single-precision transcendentals */
    CHECK(near_f(sinf((float)(pi / 6.0)), 0.5f, 1e-6f), "sinf(pi/6)");
    CHECK(near_f(cosf((float)(pi / 3.0)), 0.5f, 1e-6f), "cosf(pi/3)");
    CHECK(near_f(atan2f(1.0f, 1.0f), (float)(pi / 4.0), 1e-6f), "atan2f(1,1)");
    CHECK(near_f(sqrtf(16.0f), 4.0f, 1e-6f), "sqrtf(16)");

    /* IEEE-754 special values, signed zero, subnormals */
    volatile double pos_zero = +0.0;
    volatile double neg_zero = -0.0;
    volatile double pos_inf  = INFINITY;
    volatile double nan_val  = NAN;
    CHECK(!signbit(pos_zero) && signbit(neg_zero), "signbit(+/-0.0)");
    CHECK(copysign(5.0, neg_zero) == -5.0, "copysign(5.0, -0.0)");
    CHECK(isinf(pos_inf) && !isnan(pos_inf) && !isfinite(pos_inf), "isinf(INF)");
    CHECK(isnan(nan_val) && !isinf(nan_val) && !isfinite(nan_val), "isnan(NAN)");
    CHECK(isnan(nan_val + 1.0) && !(nan_val == nan_val), "NaN propagation");

    volatile double min_norm = DBL_MIN;
    volatile double subnorm = min_norm / 16.0;
    CHECK(isnormal(min_norm), "DBL_MIN is normal");
    CHECK(subnorm > 0.0 && !isnormal(subnorm) && isfinite(subnorm), "subnormal double");
    CHECK(subnorm * 16.0 == min_norm, "subnormal * 16 == DBL_MIN");

    /* fenv.h rounding modes (FPSCR RMode bits [23:22]) */
    int orig_round = fegetround();
    volatile double a = 1.0;
    volatile double b = 3.0;
    volatile double p19 = 1.9;
    volatile double m19 = -1.9;
    volatile double p11 = 1.1;
    volatile double m11 = -1.1;

    CHECK(fesetround(FE_TOWARDZERO) == 0 && fegetround() == FE_TOWARDZERO, "FE_TOWARDZERO");
    volatile double r_zero_pos = a / b;
    volatile double r_zero_neg = (-a) / b;
    CHECK(rint(p19) == 1.0 && rint(m19) == -1.0, "rint FE_TOWARDZERO");

    CHECK(fesetround(FE_UPWARD) == 0 && fegetround() == FE_UPWARD, "FE_UPWARD");
    volatile double r_up_pos = a / b;
    volatile double r_up_neg = (-a) / b;
    CHECK(rint(p11) == 2.0 && rint(m19) == -1.0, "rint FE_UPWARD");

    CHECK(fesetround(FE_DOWNWARD) == 0 && fegetround() == FE_DOWNWARD, "FE_DOWNWARD");
    volatile double r_down_pos = a / b;
    volatile double r_down_neg = (-a) / b;
    CHECK(rint(p19) == 1.0 && rint(m11) == -2.0, "rint FE_DOWNWARD");

    CHECK(r_up_pos > r_zero_pos && r_zero_pos == r_down_pos, "directed rounding +1/3");
    CHECK(r_down_neg < r_zero_neg && r_zero_neg == r_up_neg, "directed rounding -1/3");

    CHECK(fesetround(orig_round) == 0, "restore orig rounding mode");
}

/* --------------------------------------------------------------------------
 * 2. String, Memory & Number Formatting / Parsing Round-Trips
 * -------------------------------------------------------------------------- */
static void test_strings_and_formatting(void)
{
    static const double test_vals[] = {
        0.0, -0.0, 1.0, -1.0, 3.14159265358979323846,
        1e-300, 1e+300, DBL_MIN, DBL_MAX, 0x1.fffffep+127
    };
    char buf[128];

    for (size_t i = 0; i < sizeof(test_vals) / sizeof(test_vals[0]); i++) {
        int n = snprintf(buf, sizeof(buf), "%.13a", test_vals[i]);
        CHECK(n > 0 && (size_t)n < sizeof(buf), "snprintf %a");
        char *endp = NULL;
        double parsed = strtod(buf, &endp);
        CHECK(endp != buf && *endp == '\0', "strtod hex float endp");
        CHECK(parsed == test_vals[i] && signbit(parsed) == signbit(test_vals[i]),
              "strtod hex float exact round-trip");
    }

    int n = snprintf(buf, sizeof(buf), "%lld %llu",
                     (long long)LLONG_MIN, (unsigned long long)ULLONG_MAX);
    CHECK(n > 0, "snprintf 64-bit int");
    char *p1 = NULL;
    char *p2 = NULL;
    long long v_min = strtoll(buf, &p1, 10);
    unsigned long long v_max = strtoull(p1, &p2, 10);
    CHECK(v_min == LLONG_MIN && v_max == ULLONG_MAX, "strtoll/strtoull 64-bit limits");

    /* Overlapping memmove */
    char mbuf[32] = "0123456789abcdef";
    memmove(mbuf + 4, mbuf, 12);
    CHECK(memcmp(mbuf, "01230123456789ab", 16) == 0, "memmove forward overlap");
    memmove(mbuf, mbuf + 4, 12);
    CHECK(memcmp(mbuf, "0123456789ab89ab", 16) == 0, "memmove backward overlap");

    /* strstr, strspn, strcspn, strtok_r */
    const char *hay = "alpha::beta::gamma::delta";
    CHECK(strstr(hay, "gamma") == hay + 13, "strstr");
    CHECK(strspn("12345abc", "0123456789") == 5U, "strspn");
    CHECK(strcspn("hello:world", ":") == 5U, "strcspn");

    char tokbuf[32];
    snprintf(tokbuf, sizeof(tokbuf), "%s", hay);
    char *save = NULL;
    char *t0 = strtok_r(tokbuf, ":", &save);
    char *t1 = strtok_r(NULL, ":", &save);
    char *t2 = strtok_r(NULL, ":", &save);
    char *t3 = strtok_r(NULL, ":", &save);
    char *t4 = strtok_r(NULL, ":", &save);
    CHECK(t0 && strcmp(t0, "alpha") == 0 &&
          t1 && strcmp(t1, "beta") == 0 &&
          t2 && strcmp(t2, "gamma") == 0 &&
          t3 && strcmp(t3, "delta") == 0 &&
          t4 == NULL, "strtok_r");
}

/* --------------------------------------------------------------------------
 * 3. qsort / bsearch, Multi-Level setjmp/longjmp & POSIX Regex
 * -------------------------------------------------------------------------- */
typedef struct {
    uint32_t key;
    uint32_t payload;
} item_t;

static int cmp_item(const void *a, const void *b)
{
    const item_t *ia = (const item_t *)a;
    const item_t *ib = (const item_t *)b;
    return (ia->key > ib->key) - (ia->key < ib->key);
}

static jmp_buf g_jmp_outer;
static jmp_buf g_jmp_inner;

__attribute__((noinline))
static void jump_leaf(int val)
{
    longjmp(g_jmp_inner, val * 3);
}

__attribute__((noinline))
static void jump_middle(int val)
{
    volatile double callee_d = 42.5;
    int r = setjmp(g_jmp_inner);
    if (r == 0) {
        jump_leaf(val + 1);
    }
    CHECK(callee_d == 42.5, "VFP preserved across inner longjmp");
    longjmp(g_jmp_outer, r + 100);
}

static void test_qsort_setjmp_regex(void)
{
    item_t items[256];
    uint32_t rng = 0x13579BDFU;
    for (uint32_t i = 0; i < 256U; i++) {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        items[i].key = rng;
        items[i].payload = i;
    }
    uint32_t target_key = items[127].key;
    qsort(items, 256U, sizeof(items[0]), cmp_item);
    for (size_t i = 1; i < 256U; i++) {
        if (items[i - 1].key > items[i].key) {
            CHECK(false, "qsort ordering");
            break;
        }
    }
    item_t needle = { target_key, 0U };
    item_t *found = (item_t *)bsearch(&needle, items, 256U, sizeof(items[0]), cmp_item);
    CHECK(found != NULL && found->key == target_key, "bsearch found key");

    /* Nested setjmp / longjmp */
    volatile int local_var = 77;
    volatile double local_d = 3.125;
    int jr = setjmp(g_jmp_outer);
    if (jr == 0) {
        local_var = 88;
        local_d = 6.25;
        jump_middle(10);
        CHECK(false, "unreachable after longjmp");
    }
    CHECK(jr == 133 && local_var == 88 && local_d == 6.25, "nested setjmp/longjmp state");

    /* POSIX Extended Regex */
    regex_t re;
    int rc = regcomp(&re, "^([a-z0-9_]+)-([0-9]+)\\.([0-9]+)$", REG_EXTENDED);
    CHECK(rc == 0, "regcomp");
    if (rc == 0) {
        regmatch_t pm[4];
        const char *sample = "a32rt_m7-2026.42";
        CHECK(regexec(&re, sample, 4, pm, 0) == 0, "regexec match");
        CHECK(pm[1].rm_so == 0 && pm[1].rm_eo == 8, "regex group 1");
        CHECK(pm[2].rm_so == 9 && pm[2].rm_eo == 13, "regex group 2");
        CHECK(pm[3].rm_so == 14 && pm[3].rm_eo == 16, "regex group 3");
        CHECK(regexec(&re, "invalid-string!", 4, pm, 0) == REG_NOMATCH, "regexec nomatch");
        regfree(&re);
    }
}

/* --------------------------------------------------------------------------
 * 4. Pthreads, Condvars, TLS (__thread) & Pipe + poll()
 * -------------------------------------------------------------------------- */
static __thread uint32_t g_tls_magic = 0x11223344U;

typedef struct {
    pthread_mutex_t mtx;
    pthread_cond_t  cv;
    uint32_t        accum;
    unsigned        done_count;
} worker_sync_t;

typedef struct {
    worker_sync_t *sync;
    uint32_t       id;
} worker_arg_t;

static void *worker_fn(void *arg)
{
    worker_arg_t *wa = (worker_arg_t *)arg;
    CHECK(g_tls_magic == 0x11223344U, "TLS initial value in thread");
    g_tls_magic = 0xCAFE0000U | wa->id;

    /* Exercise VFP in thread alongside TLS */
    volatile double x = (double)wa->id * 1.5;
    uint32_t contrib = (uint32_t)(sqrt(x * x) * 2.0 + 0.5);

    pthread_mutex_lock(&wa->sync->mtx);
    wa->sync->accum += contrib;
    wa->sync->done_count++;
    pthread_cond_broadcast(&wa->sync->cv);
    pthread_mutex_unlock(&wa->sync->mtx);

    CHECK(g_tls_magic == (0xCAFE0000U | wa->id), "TLS thread isolation");
    return (void *)(uintptr_t)(wa->id * 10U);
}

static void test_pthreads_and_poll(void)
{
    worker_sync_t sync;
    pthread_mutex_init(&sync.mtx, NULL);
    pthread_cond_init(&sync.cv, NULL);
    sync.accum = 0U;
    sync.done_count = 0U;

    pthread_t th[4];
    worker_arg_t args[4];
    for (uint32_t i = 0; i < 4U; i++) {
        args[i].sync = &sync;
        args[i].id = i + 1U;
        CHECK(pthread_create(&th[i], NULL, worker_fn, &args[i]) == 0, "pthread_create");
    }

    pthread_mutex_lock(&sync.mtx);
    while (sync.done_count < 4U) {
        pthread_cond_wait(&sync.cv, &sync.mtx);
    }
    pthread_mutex_unlock(&sync.mtx);

    for (uint32_t i = 0; i < 4U; i++) {
        void *ret = NULL;
        CHECK(pthread_join(th[i], &ret) == 0, "pthread_join");
        CHECK((uintptr_t)ret == (i + 1U) * 10U, "pthread return value");
    }
    CHECK(sync.accum == (3U + 6U + 9U + 12U), "pthread condvar accum");
    CHECK(g_tls_magic == 0x11223344U, "main thread TLS untouched");

    pthread_cond_destroy(&sync.cv);
    pthread_mutex_destroy(&sync.mtx);

    /* Pipe + poll() */
    int fds[2];
    CHECK(pipe(fds) == 0, "pipe");
    const char msg[] = "m7-poll-ok";
    CHECK(write(fds[1], msg, sizeof(msg)) == (ssize_t)sizeof(msg), "pipe write");
    struct pollfd pfd;
    pfd.fd = fds[0];
    pfd.events = POLLIN;
    pfd.revents = 0;
    CHECK(poll(&pfd, 1, 1000) == 1 && (pfd.revents & POLLIN) != 0, "poll POLLIN");
    char rbuf[32] = {0};
    CHECK(read(fds[0], rbuf, sizeof(rbuf)) == (ssize_t)sizeof(msg), "pipe read");
    CHECK(strcmp(rbuf, msg) == 0, "pipe message match");
    close(fds[0]);
    close(fds[1]);
}

int main(void)
{
    test_libm_ieee754();
    test_strings_and_formatting();
    test_qsort_setjmp_regex();
    test_pthreads_and_poll();

    if (g_failures != 0) {
        fprintf(stderr, "M7 libc/math conformance: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("M7 libc/math conformance (IEEE-754 libm, fenv, regex, setjmp, pthreads): PASS\n");
    return 0;
}
