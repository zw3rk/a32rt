/*
 * tests/ios/test_ios_signals_threads.c — signals + pthread qualification.
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * SIGUSR1 via sigaction+kill, SIGSEGV on __PAGEZERO with ucontext fix-up
 * (explicit 4-byte ldr.w so pc+=4 is exact), VFP preservation, 2 pthreads.
 */

#include "ios_guest.h"

static volatile int usr1_got = 0;

static void usr1_handler(int sig, void *info, void *ctx)
{
    (void)info;
    (void)ctx;
    usr1_got = sig;
}

static volatile int segv_got = 0;

static void segv_handler(int sig, void *info, void *ctx)
{
    ios_ucontext32_t *uc = (ios_ucontext32_t *)ctx;
    (void)info;
    if (sig != IOS_SIGSEGV) {
        return;
    }
    segv_got = sig;
    if (uc != 0 && uc->uc_mcontext != 0 && uc->uc_mcontext->es.far == 0x2000U) {
        uc->uc_mcontext->ss.pc += 4U;
    }
}

static int check_sigusr1(void)
{
    ios_sigaction_t sa;
    ios_sigaction_t old;
    int i = 0;
    sa.sa_sigaction = usr1_handler;
    sa.sa_mask = 0U;
    sa.sa_flags = IOS_SA_SIGINFO;
    for (i = 0; i < (int)sizeof(old); i++) {
        ((char *)&old)[i] = 0;
    }
    usr1_got = 0;
    if (sigaction(IOS_SIGUSR1, &sa, &old) != 0) {
        printf("FAIL sigaction usr1\n");
        return 1;
    }
    if (kill(getpid(), IOS_SIGUSR1) != 0) {
        printf("FAIL kill usr1\n");
        return 1;
    }
    if (usr1_got != IOS_SIGUSR1) {
        printf("FAIL usr1 got=%d\n", usr1_got);
        return 1;
    }
    return 0;
}

static int check_sigsegv(void)
{
    ios_sigaction_t sa;
    volatile double keep = 3.14159265358979;
    volatile uint32_t *bad = (volatile uint32_t *)(uintptr_t)0x2000U;
    uint32_t v = 0U;
    sa.sa_sigaction = segv_handler;
    sa.sa_mask = 0U;
    sa.sa_flags = IOS_SA_SIGINFO;
    segv_got = 0;
    if (sigaction(IOS_SIGSEGV, &sa, 0) != 0) {
        printf("FAIL sigaction segv\n");
        return 1;
    }
    /* Explicit 32-bit Thumb-2 load: handler advances pc by exactly 4. */
    __asm__ __volatile__("ldr.w %0, [%1]"
                         : "=r"(v)
                         : "r"(bad)
                         : "memory");
    (void)v;
    if (segv_got != IOS_SIGSEGV) {
        printf("FAIL segv not delivered\n");
        return 1;
    }
    if (keep < 3.14159 || keep > 3.14160) {
        printf("FAIL vfp clobbered\n");
        return 1;
    }
    return 0;
}

static ios_pthread_mutex_t g_mu;
static volatile int g_counter = 0;
static ios_pthread_key_t g_key = 0U;

static void *worker(void *arg)
{
    int i = 0;
    if (pthread_setspecific(g_key, arg) != 0) {
        return (void *)1;
    }
    if (pthread_getspecific(g_key) != arg) {
        return (void *)2;
    }
    for (i = 0; i < 1000; i++) {
        pthread_mutex_lock(&g_mu);
        g_counter++;
        pthread_mutex_unlock(&g_mu);
    }
    return (void *)0;
}

static int check_threads(void)
{
    ios_pthread_t t1 = 0U;
    ios_pthread_t t2 = 0U;
    void *r1 = 0;
    void *r2 = 0;
    int marker = 42;
    g_counter = 0;
    if (pthread_mutex_init(&g_mu, 0) != 0) {
        printf("FAIL mutex init\n");
        return 1;
    }
    if (pthread_key_create(&g_key, 0) != 0) {
        printf("FAIL key_create\n");
        return 1;
    }
    if (pthread_self() == 0U) {
        printf("FAIL pthread_self\n");
        return 1;
    }
    if (pthread_create(&t1, 0, worker, &marker) != 0) {
        printf("FAIL create t1\n");
        return 1;
    }
    if (pthread_create(&t2, 0, worker, &marker) != 0) {
        printf("FAIL create t2\n");
        return 1;
    }
    if (pthread_join(t1, &r1) != 0 || r1 != 0) {
        printf("FAIL join t1\n");
        return 1;
    }
    if (pthread_join(t2, &r2) != 0 || r2 != 0) {
        printf("FAIL join t2\n");
        return 1;
    }
    if (g_counter != 2000) {
        printf("FAIL counter=%d\n", g_counter);
        return 1;
    }
    if (pthread_mutex_destroy(&g_mu) != 0) {
        printf("FAIL mutex destroy\n");
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int run_threads = 1;
    if (argc > 1 && argv[1] != 0 && strcmp(argv[1], "--no-threads") == 0) {
        run_threads = 0;
    }
    if (check_sigusr1() != 0) {
        return 1;
    }
    if (check_sigsegv() != 0) {
        return 1;
    }
    if (run_threads && check_threads() != 0) {
        return 1;
    }
    printf("PASS test_ios_signals_threads\n");
    return 0;
}
