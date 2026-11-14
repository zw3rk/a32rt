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

/* Darwin SIGABRT (ios_guest.h only names SIGSEGV/SIGUSR1). */
#define ABRT_SIG 6

static volatile int abrt_got = 0;
static int abrt_pipe_w = -1;

static void abrt_handler(int sig, void *info, void *ctx)
{
    char c = 'A';
    (void)info;
    (void)ctx;
    if (sig != ABRT_SIG) {
        return;
    }
    abrt_got = sig;
    if (abrt_pipe_w >= 0) {
        (void)write(abrt_pipe_w, &c, 1);
    }
}

static int check_abort(void)
{
    uint32_t r1 = 0U;
    uint32_t carry = 0U;
    int32_t pr = 0;
    int rfd = -1;
    int wfd = -1;
    int32_t pid = 0;
    uint32_t fr1 = 0U;
    uint32_t fcarry = 0U;
    pr = ios_bsd0_r01((uint32_t)IOS_SYS_pipe, &r1, &carry);
    if (carry != 0U) {
        printf("FAIL abort pipe carry\n");
        return 1;
    }
    rfd = pr;
    wfd = (int)r1;
    if (rfd < 0 || wfd < 0) {
        printf("FAIL abort pipe fds\n");
        return 1;
    }
    abrt_got = 0;
    abrt_pipe_w = wfd;
    pid = ios_bsd0_r01((uint32_t)IOS_SYS_fork, &fr1, &fcarry);
    if (fcarry != 0U) {
        printf("FAIL abort fork\n");
        close(rfd);
        close(wfd);
        return 1;
    }
    if (pid == 0) {
        ios_sigaction_t sa;
        ios_sigaction_t old;
        int i = 0;
        if (fr1 != 1U) {
            _exit(98);
        }
        close(rfd);
        sa.sa_sigaction = abrt_handler;
        sa.sa_mask = 0U;
        sa.sa_flags = IOS_SA_SIGINFO;
        for (i = 0; i < (int)sizeof(old); i++) {
            ((char *)&old)[i] = 0;
        }
        if (sigaction(ABRT_SIG, &sa, &old) != 0) {
            _exit(96);
        }
        abrt_got = 0;
        abort();
        _exit(99);
    }
    close(wfd);
    abrt_pipe_w = -1;
    {
        char c = 0;
        int got = 0;
        int status = 0;
        int32_t w = 0;
        while (got < 1) {
            int n = read(rfd, &c, 1);
            if (n <= 0) {
                break;
            }
            got += n;
        }
        close(rfd);
        if (got != 1 || c != 'A') {
            printf("FAIL abort handler byte got=%d\n", got);
            return 1;
        }
        carry = 0U;
        w = ios_bsd6((uint32_t)IOS_SYS_wait4, (uint32_t)pid,
            (uint32_t)(uintptr_t)&status, 0U, 0U, 0U, 0U, &carry);
        if (carry != 0U || w != pid) {
            printf("FAIL abort wait4\n");
            return 1;
        }
        if ((status & 0x7F) != 0) {
            printf("FAIL abort signaled status=%d\n", status);
            return 1;
        }
        if (((status >> 8) & 0xFF) != 134) {
            printf("FAIL abort exit status=%d\n", status);
            return 1;
        }
    }
    return 0;
}

/* Synthetic cond stubs are outside the minimal link-time TBD, so reach them
 * through the stub island (mov.w r12, #id; svc #0x81; bx lr): the same
 * trampolines bound symbols resolve to at runtime. */
#define IOS_STUB_ISLAND_VA 0xFFFF2000U
#define IOS_STUB_COND_INIT 97U
#define IOS_STUB_COND_WAIT 98U
#define IOS_STUB_COND_SIGNAL 99U
#define IOS_STUB_COND_BROADCAST 100U
#define IOS_STUB_COND_DESTROY 101U

typedef struct test_cond {
    uint32_t words[8];
} test_cond_t;

static int stub_cond0(uint32_t id, uint32_t a0)
{
    int (*fn)(uint32_t) =
        (int (*)(uint32_t))(uintptr_t)(IOS_STUB_ISLAND_VA + id * 8U + 1U);
    return fn(a0);
}

static int stub_cond_wait(uint32_t cond_va, uint32_t mutex_va)
{
    int (*fn)(uint32_t, uint32_t) = (int (*)(uint32_t, uint32_t))(uintptr_t)
        (IOS_STUB_ISLAND_VA + (uint32_t)IOS_STUB_COND_WAIT * 8U + 1U);
    return fn(cond_va, mutex_va);
}

/* Zero-initialized static mutex/cond objects materialize host state lazily. */
static ios_pthread_mutex_t g_static_mu;
static test_cond_t g_static_cv;
static ios_pthread_mutex_t g_cv_mu;
static volatile int g_cv_flag = 0;

static int check_static_mutex(void)
{
    static ios_pthread_mutex_t sig_mu;
    if (pthread_mutex_lock(&g_static_mu) != 0) {
        printf("FAIL static mutex lock\n");
        return 1;
    }
    if (pthread_mutex_unlock(&g_static_mu) != 0) {
        printf("FAIL static mutex unlock\n");
        return 1;
    }
    sig_mu.__opaque[0] = 0x32AAABA7U;
    if (pthread_mutex_lock(&sig_mu) != 0) {
        printf("FAIL sig mutex lock\n");
        return 1;
    }
    if (pthread_mutex_unlock(&sig_mu) != 0) {
        printf("FAIL sig mutex unlock\n");
        return 1;
    }
    return 0;
}

static void *signaler(void *arg)
{
    uint32_t cv_va = (uint32_t)(uintptr_t)&g_static_cv;
    (void)arg;
    pthread_mutex_lock(&g_cv_mu);
    g_cv_flag = 1;
    stub_cond0(IOS_STUB_COND_SIGNAL, cv_va);
    pthread_mutex_unlock(&g_cv_mu);
    return (void *)0;
}

static int check_static_cond(void)
{
    uint32_t cv_va = (uint32_t)(uintptr_t)&g_static_cv;
    uint32_t mu_va = (uint32_t)(uintptr_t)&g_cv_mu;
    ios_pthread_t th = 0U;
    void *ret = 0;
    if (stub_cond0(IOS_STUB_COND_INIT, cv_va) != 0) {
        printf("FAIL static cond init\n");
        return 1;
    }
    if (stub_cond0(IOS_STUB_COND_BROADCAST, cv_va) != 0) {
        printf("FAIL static cond broadcast\n");
        return 1;
    }
    g_cv_flag = 0;
    if (pthread_mutex_init(&g_cv_mu, 0) != 0) {
        printf("FAIL cv mutex init\n");
        return 1;
    }
    if (pthread_create(&th, 0, signaler, 0) != 0) {
        printf("FAIL create signaler\n");
        return 1;
    }
    pthread_mutex_lock(&g_cv_mu);
    while (g_cv_flag == 0) {
        if (stub_cond_wait(cv_va, mu_va) != 0) {
            printf("FAIL static cond wait\n");
            pthread_mutex_unlock(&g_cv_mu);
            return 1;
        }
    }
    pthread_mutex_unlock(&g_cv_mu);
    if (pthread_join(th, &ret) != 0 || ret != 0) {
        printf("FAIL join signaler\n");
        return 1;
    }
    if (stub_cond0(IOS_STUB_COND_DESTROY, cv_va) != 0) {
        printf("FAIL static cond destroy\n");
        return 1;
    }
    if (pthread_mutex_destroy(&g_cv_mu) != 0) {
        printf("FAIL cv mutex destroy\n");
        return 1;
    }
    return 0;
}

static int check_self_join(void)
{
    if (pthread_join(pthread_self(), 0) != 11) {
        printf("FAIL self join not EDEADLK\n");
        return 1;
    }
    return 0;
}

static ios_pthread_key_t g_dtor_key = 0U;
static volatile int g_dtor_ran = 0;
static volatile uint32_t g_dtor_val = 0U;

static void dtor_fn(void *v)
{
    g_dtor_val = (uint32_t)(uintptr_t)v;
    g_dtor_ran = 1;
}

static void *tsd_worker(void *arg)
{
    if (pthread_setspecific(g_dtor_key, arg) != 0) {
        return (void *)1;
    }
    return (void *)0;
}

static int check_tsd_dtor(void)
{
    ios_pthread_t th = 0U;
    void *ret = 0;
    g_dtor_ran = 0;
    g_dtor_val = 0U;
    if (pthread_key_create(&g_dtor_key, dtor_fn) != 0) {
        printf("FAIL dtor key_create\n");
        return 1;
    }
    if (pthread_create(&th, 0, tsd_worker, (void *)0x1234U) != 0) {
        printf("FAIL create tsd worker\n");
        return 1;
    }
    if (pthread_join(th, &ret) != 0 || ret != 0) {
        printf("FAIL join tsd worker\n");
        return 1;
    }
    if (g_dtor_ran != 1 || g_dtor_val != 0x1234U) {
        printf("FAIL dtor ran=%d val=%lx\n", g_dtor_ran,
            (unsigned long)g_dtor_val);
        return 1;
    }
    return 0;
}

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
    if (check_abort() != 0) {
        return 1;
    }
    if (check_static_mutex() != 0) {
        return 1;
    }
    if (check_self_join() != 0) {
        return 1;
    }
    if (run_threads && check_threads() != 0) {
        return 1;
    }
    if (run_threads && check_static_cond() != 0) {
        return 1;
    }
    if (run_threads && check_tsd_dtor() != 0) {
        return 1;
    }
    printf("PASS test_ios_signals_threads\n");
    return 0;
}
