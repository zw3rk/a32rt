/*
 * tests/ios/test_ios_bsd_mach.c — dynamic PIE BSD + Mach trap qualification.
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Linked with: ld -arch armv7 -pie -ios_version_min 9.0 ... libSystem.tbd
 * Verifies argv/envp/apple, CPSR.C errno, file I/O, mmap, sysctl, Mach traps.
 */

#include "ios_guest.h"

/* Local trap helpers for arities not covered by ios_guest.h. */
static inline __attribute__((unused)) uint32_t trap1(uint32_t t, uint32_t a0)
{
    register uint32_t r0 __asm__("r0") = a0;
    register uint32_t r12 __asm__("r12") = t;
    __asm__ __volatile__("svc #0x80" : "+r"(r0) : "r"(r12) : "memory", "cc");
    return r0;
}

extern ios_kern_return_t _kernelrpc_mach_vm_allocate_trap(
    ios_mach_port_t target, uint64_t *addr, uint64_t size, int flags);
extern ios_kern_return_t _kernelrpc_mach_vm_deallocate_trap(
    ios_mach_port_t target, uint64_t address, uint64_t size);
extern ios_kern_return_t _kernelrpc_mach_vm_protect_trap(
    ios_mach_port_t target, uint64_t address, uint64_t size, int set_maximum,
    int new_protection);

static ios_kern_return_t call_vm_alloc(ios_mach_port_t target, uint64_t *addr,
    uint64_t size, int flags)
{
    uint32_t sz_lo = (uint32_t)(size & 0xFFFFFFFFULL);
    uint32_t sz_hi = (uint32_t)(size >> 32);
    register uint32_t r0 __asm__("r0") = target;
    register uint32_t r1 __asm__("r1") = (uint32_t)(uintptr_t)addr;
    register uint32_t r2 __asm__("r2") = sz_lo;
    register uint32_t r3 __asm__("r3") = sz_hi;
    register uint32_t r4 __asm__("r4") = (uint32_t)flags;
    register uint32_t r12 __asm__("r12") = (uint32_t)IOS_TRAP_MACH_VM_ALLOCATE;
    __asm__ __volatile__("svc #0x80"
                         : "+r"(r0)
                         : "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r12)
                         : "memory", "cc");
    return (ios_kern_return_t)r0;
}

static ios_kern_return_t call_vm_protect(ios_mach_port_t target,
    uint64_t address, uint64_t size, int set_max, int prot)
{
    uint32_t a_lo = (uint32_t)(address & 0xFFFFFFFFULL);
    uint32_t a_hi = (uint32_t)(address >> 32);
    uint32_t s_lo = (uint32_t)(size & 0xFFFFFFFFULL);
    uint32_t s_hi = (uint32_t)(size >> 32);
    register uint32_t r0 __asm__("r0") = target;
    register uint32_t r1 __asm__("r1") = a_lo;
    register uint32_t r2 __asm__("r2") = a_hi;
    register uint32_t r3 __asm__("r3") = s_lo;
    register uint32_t r4 __asm__("r4") = s_hi;
    register uint32_t r5 __asm__("r5") = (uint32_t)set_max;
    register uint32_t r6 __asm__("r6") = (uint32_t)prot;
    register uint32_t r12 __asm__("r12") = (uint32_t)IOS_TRAP_MACH_VM_PROTECT;
    __asm__ __volatile__("svc #0x80"
                         : "+r"(r0)
                         : "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5),
                           "r"(r6), "r"(r12)
                         : "memory", "cc");
    return (ios_kern_return_t)r0;
}

static ios_kern_return_t call_vm_dealloc(ios_mach_port_t target,
    uint64_t address, uint64_t size)
{
    uint32_t a_lo = (uint32_t)(address & 0xFFFFFFFFULL);
    uint32_t a_hi = (uint32_t)(address >> 32);
    uint32_t s_lo = (uint32_t)(size & 0xFFFFFFFFULL);
    uint32_t s_hi = (uint32_t)(size >> 32);
    register uint32_t r0 __asm__("r0") = target;
    register uint32_t r1 __asm__("r1") = a_lo;
    register uint32_t r2 __asm__("r2") = a_hi;
    register uint32_t r3 __asm__("r3") = s_lo;
    register uint32_t r4 __asm__("r4") = s_hi;
    register uint32_t r12 __asm__("r12") = (uint32_t)IOS_TRAP_MACH_VM_DEALLOCATE;
    __asm__ __volatile__("svc #0x80"
                         : "+r"(r0)
                         : "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r12)
                         : "memory", "cc");
    return (ios_kern_return_t)r0;
}

static uint32_t mach_msg7(ios_mach_msg_header_t *msg, uint32_t opt,
    uint32_t send_size, uint32_t rcv_size, uint32_t rcv_name, uint32_t timeout,
    uint32_t notify)
{
    register uint32_t r0 __asm__("r0") = (uint32_t)(uintptr_t)msg;
    register uint32_t r1 __asm__("r1") = opt;
    register uint32_t r2 __asm__("r2") = send_size;
    register uint32_t r3 __asm__("r3") = rcv_size;
    register uint32_t r4 __asm__("r4") = rcv_name;
    register uint32_t r5 __asm__("r5") = timeout;
    register uint32_t r6 __asm__("r6") = notify;
    register uint32_t r12 __asm__("r12") = (uint32_t)IOS_TRAP_MACH_MSG;
    __asm__ __volatile__("svc #0x80"
                         : "+r"(r0)
                         : "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5),
                           "r"(r6), "r"(r12)
                         : "memory", "cc");
    return r0;
}

#define MACH_SEND_MSG 1U
#define MACH_RCV_MSG 2U

typedef struct ios_msg32 {
    ios_mach_msg_header_t hdr;
    uint32_t payload[2];
} ios_msg32_t;

static int check_args(int argc, char **argv, char **envp, char **apple)
{
    if (argc < 1 || argv == 0 || argv[0] == 0) {
        printf("FAIL args argc/argv\n");
        return 1;
    }
    if (envp == 0) {
        printf("FAIL args envp\n");
        return 1;
    }
    if (apple == 0 || apple[0] == 0) {
        printf("FAIL args apple\n");
        return 1;
    }
    if (strncmp(apple[0], "executable_path=", 16) != 0) {
        printf("FAIL args apple[0]=%s\n", apple[0]);
        return 1;
    }
    return 0;
}

static int check_errno_conv(void)
{
    int fd = open("/nonexistent_ios_path_xyz", IOS_O_RDONLY);
    int *err = __error();
    if (fd != -1) {
        printf("FAIL errno open succeeded\n");
        close(fd);
        return 1;
    }
    if (err == 0 || *err != IOS_ENOENT) {
        printf("FAIL errno got %d\n", err ? *err : -99);
        return 1;
    }
    return 0;
}

static int check_file(void)
{
    char path[64];
    const char *data = "a32rt-ios-0123456789";
    char back[32];
    int pid = getpid();
    int fd = 0;
    int i = 0;
    int64_t off = 0;
    int n = 0;
    ios_guest_stat_t st;
    size_t len = 0;
    for (i = 0; i < (int)sizeof(st); i++) {
        ((char *)&st)[i] = 0;
    }
    snprintf(path, sizeof(path), "/tmp/a32rt_ios_test_%d", pid);
    fd = open(path, IOS_O_RDWR | IOS_O_CREAT | IOS_O_TRUNC, 0600);
    if (fd < 0) {
        printf("FAIL file open %s\n", path);
        return 1;
    }
    len = strlen(data);
    n = write(fd, data, len);
    if (n != (int)len) {
        printf("FAIL file write\n");
        close(fd);
        return 1;
    }
    off = lseek(fd, (int64_t)0, IOS_SEEK_SET);
    if (off != (int64_t)0) {
        printf("FAIL file lseek\n");
        close(fd);
        return 1;
    }
    for (i = 0; i < (int)sizeof(back); i++) {
        back[i] = 0;
    }
    n = read(fd, back, len);
    if (n != (int)len || memcmp(back, data, len) != 0) {
        printf("FAIL file read back\n");
        close(fd);
        return 1;
    }
    if (fstat(fd, &st) != 0) {
        printf("FAIL fstat\n");
        close(fd);
        return 1;
    }
    if (st.st_size != (int64_t)len) {
        printf("FAIL fstat size\n");
        close(fd);
        return 1;
    }
    if (sizeof(st) != 108U) {
        printf("FAIL stat size %u\n", (unsigned)sizeof(st));
        close(fd);
        return 1;
    }
    close(fd);
    if (unlink(path) != 0) {
        printf("FAIL unlink\n");
        return 1;
    }
    return 0;
}

static int check_mmap(void)
{
    char *p = (char *)mmap(0, 4096, IOS_PROT_READ | IOS_PROT_WRITE,
        IOS_MAP_PRIVATE | IOS_MAP_ANON, -1, (int64_t)0);
    if (p == IOS_MAP_FAILED || p == 0) {
        printf("FAIL mmap\n");
        return 1;
    }
    p[0] = (char)0x41;
    p[4095] = (char)0x5A;
    if (mprotect(p, 4096, IOS_PROT_READ) != 0) {
        printf("FAIL mprotect\n");
        munmap(p, 4096);
        return 1;
    }
    if (p[0] != (char)0x41) {
        printf("FAIL mmap readback\n");
        munmap(p, 4096);
        return 1;
    }
    if (munmap(p, 4096) != 0) {
        printf("FAIL munmap\n");
        return 1;
    }
    return 0;
}

static int check_sysctl(void)
{
    char buf[32];
    size_t len = sizeof(buf);
    size_t i = 0;
    for (i = 0; i < sizeof(buf); i++) {
        buf[i] = 0;
    }
    if (sysctlbyname("kern.ostype", buf, &len, 0, 0) != 0) {
        printf("FAIL sysctl\n");
        return 1;
    }
    if (strcmp(buf, "Darwin") != 0) {
        printf("FAIL sysctl value %s\n", buf);
        return 1;
    }
    return 0;
}

static int check_mach(void)
{
    uint32_t task = ios_trap0((uint32_t)IOS_TRAP_MACH_TASK_SELF);
    uint32_t host = ios_trap0((uint32_t)IOS_TRAP_MACH_HOST_SELF);
    uint32_t thread = ios_trap0((uint32_t)IOS_TRAP_MACH_THREAD_SELF);
    uint64_t addr = 0ULL;
    ios_kern_return_t kr = 0;
    ios_mach_timebase_info_t tb;
    ios_mach_port_t reply = 0U;
    ios_msg32_t msg;
    uint32_t i = 0U;
    tb.numer = 0U;
    tb.denom = 0U;
    if (task == 0U || host == 0U || thread == 0U) {
        printf("FAIL mach ports\n");
        return 1;
    }
    kr = call_vm_alloc(task, &addr, 4096ULL, 1);
    if (kr != 0 || addr == 0ULL || addr > 0xFFFFFFFFULL) {
        printf("FAIL vm_allocate\n");
        return 1;
    }
    *(volatile uint32_t *)(uintptr_t)addr = 0x55AA1234U;
    kr = call_vm_protect(task, addr, 4096ULL, 0, IOS_PROT_READ);
    if (kr != 0 || *(volatile uint32_t *)(uintptr_t)addr != 0x55AA1234U) {
        printf("FAIL vm_protect\n");
        return 1;
    }
    kr = call_vm_dealloc(task, addr, 4096ULL);
    if (kr != 0) {
        printf("FAIL vm_deallocate\n");
        return 1;
    }
    kr = (ios_kern_return_t)trap1((uint32_t)IOS_TRAP_MACH_TIMEBASE_INFO,
        (uint32_t)(uintptr_t)&tb);
    if (kr != 0 || tb.numer == 0U || tb.denom == 0U) {
        printf("FAIL timebase\n");
        return 1;
    }
    reply = ios_trap0((uint32_t)IOS_TRAP_MACH_REPLY_PORT);
    if (reply == 0U) {
        printf("FAIL reply port\n");
        return 1;
    }
    for (i = 0U; i < sizeof(msg); i++) {
        ((char *)&msg)[i] = 0;
    }
    msg.hdr.msgh_bits = 0x00001313U;
    msg.hdr.msgh_size = (uint32_t)sizeof(msg);
    msg.hdr.msgh_remote_port = reply;
    msg.hdr.msgh_local_port = reply;
    msg.hdr.msgh_voucher_port = 0U;
    msg.hdr.msgh_id = 0x1234U;
    msg.payload[0] = 0xDEADBEEFU;
    msg.payload[1] = 0xCAFEBABEU;
    kr = (ios_kern_return_t)mach_msg7(&msg.hdr,
        (uint32_t)(MACH_SEND_MSG | MACH_RCV_MSG), (uint32_t)sizeof(msg),
        (uint32_t)sizeof(msg), reply, 0U, 0U);
    if (kr != 0) {
        printf("FAIL mach_msg\n");
        return 1;
    }
    if (msg.payload[0] != 0xDEADBEEFU || msg.payload[1] != 0xCAFEBABEU) {
        printf("FAIL mach_msg payload\n");
        return 1;
    }
    return 0;
}

extern int *__error(void);
extern void *fopen(const char *__path, const char *__mode);
extern int fclose(void *__fp);
extern int fprintf(void *__fp, const char *__fmt, ...);
extern int atexit(void (*__fn)(void));
extern long strtol(const char *__s, char **__end, int __base);
extern unsigned long strtoul(const char *__s, char **__end, int __base);

/* Volatile callees force real libSystem calls so the native leaf
 * stubs (not clang-folded constants) are exercised. */
static size_t (* volatile v_strlen)(const char *) = strlen;
static int (* volatile v_strcmp)(const char *, const char *) = strcmp;
static int (* volatile v_strncmp)(const char *, const char *, size_t) = strncmp;
static int (* volatile v_memcmp)(const void *, const void *, size_t) = memcmp;
static void *(* volatile v_memset)(void *, int, size_t) = memset;
static void *(* volatile v_memcpy)(void *, const void *, size_t) = memcpy;
static void *(* volatile v_memmove)(void *, const void *, size_t) = memmove;

static int check_fprintf(void)
{
    char path[64];
    void *f = 0;
    int pid = getpid();
    int n = 0;
    char back[16];
    int i = 0;
    int fd = 0;
    snprintf(path, sizeof(path), "/tmp/a32rt_ios_fprintf_%d", pid);
    f = fopen(path, "w");
    if (f == 0) {
        printf("FAIL fprintf fopen\n");
        return 1;
    }
    n = fprintf(f, "%s %d\n", "ok", 42);
    if (n != 6) {
        printf("FAIL fprintf ret %d\n", n);
        fclose(f);
        return 1;
    }
    if (fclose(f) != 0) {
        printf("FAIL fprintf fclose\n");
        return 1;
    }
    for (i = 0; i < (int)sizeof(back); i++) {
        back[i] = 0;
    }
    fd = open(path, IOS_O_RDONLY);
    if (fd < 0) {
        printf("FAIL fprintf reopen\n");
        return 1;
    }
    n = read(fd, back, 6);
    close(fd);
    unlink(path);
    if (n != 6 || memcmp(back, "ok 42\n", 6) != 0) {
        printf("FAIL fprintf body\n");
        return 1;
    }
    return 0;
}

static int check_snprintf(void)
{
    char big[5000];
    char small[8];
    int n = 0;
    n = snprintf(big, sizeof(big), "%500d", 7);
    if (n != 500) {
        printf("FAIL snprintf 500 %d\n", n);
        return 1;
    }
    if (big[0] != ' ' || big[499] != '7' || strlen(big) != 500U) {
        printf("FAIL snprintf 500 body\n");
        return 1;
    }
    n = snprintf(small, sizeof(small), "%s", "hello world");
    if (n != 11) {
        printf("FAIL snprintf str %d\n", n);
        return 1;
    }
    if (strcmp(small, "hello w") != 0) {
        printf("FAIL snprintf trunc\n");
        return 1;
    }
    n = snprintf(big, 0U, "hi %d", 5);
    if (n != 4) {
        printf("FAIL snprintf zero %d\n", n);
        return 1;
    }
    return 0;
}

static int check_strto(void)
{
    const char *s = " -12345xyz";
    char *end = 0;
    long v = 0L;
    unsigned long uv = 0UL;
    int *err = __error();
    *err = 0;
    v = strtol(s, &end, 10);
    if (v != -12345L) {
        printf("FAIL strtol val %ld\n", v);
        return 1;
    }
    if (end == 0 || (end - s) != 7 || *end != 'x') {
        printf("FAIL strtol end\n");
        return 1;
    }
    *err = 0;
    v = strtol("99999999999", 0, 10);
    if (v != 2147483647L) {
        printf("FAIL strtol clamp %ld\n", v);
        return 1;
    }
    if (*err != 34) {
        printf("FAIL strtol errno %d\n", *err);
        return 1;
    }
    uv = strtoul("4294967295", 0, 10);
    if (uv != 4294967295UL) {
        printf("FAIL strtoul max %lu\n", uv);
        return 1;
    }
    v = strtol("-42", 0, 10);
    if (v != -42L) {
        printf("FAIL strtol neg %ld\n", v);
        return 1;
    }
    end = 0;
    uv = strtoul("77 bottles", &end, 10);
    if (uv != 77UL || end == 0 || *end != ' ') {
        printf("FAIL strtoul end\n");
        return 1;
    }
    return 0;
}

static int check_div64(void)
{
    volatile int64_t a = 1000000000007LL, b = 3333333LL;
    volatile int64_t q = 0, m = 0, z = 0;
    volatile int64_t mn = (-9223372036854775807LL - 1LL), neg1 = -1LL;
    volatile uint64_t ua = 18446744073709551615ULL, ub = 7ULL;
    volatile uint64_t uq = 0U, um = 0U;
    volatile int x = 1000003, y = 7;
    q = a / b;
    m = a % b;
    if (q != 300000LL || m != 100007LL) {
        printf("FAIL divdi\n");
        return 1;
    }
    q = mn / neg1;
    if (q != mn) {
        printf("FAIL divdi min\n");
        return 1;
    }
    q = a / z;
    if (q != 0) {
        printf("FAIL divdi zdiv\n");
        return 1;
    }
    m = a % z;
    if (m != 0) {
        printf("FAIL moddi zdiv\n");
        return 1;
    }
    uq = ua / ub;
    um = ua % ub;
    if (uq != 2635249153387078802ULL || um != 1ULL) {
        printf("FAIL udivdi\n");
        return 1;
    }
    if (x / y != 142857 || x % y != 4) {
        printf("FAIL divsi\n");
        return 1;
    }
    return 0;
}

static int check_leaf(void)
{
    char d[32];
    char hs[6];
    unsigned i = 0U;
    unsigned n5 = 5U, n8 = 8U, n10 = 10U, n3 = 3U, n2 = 2U;
    hs[0] = 'h';
    hs[1] = 'e';
    hs[2] = 'l';
    hs[3] = 'l';
    hs[4] = 'o';
    hs[5] = '\0';
    for (i = 0U; i < sizeof(d); i++) {
        d[i] = (char)0xA5;
    }
    if (v_strlen(hs) != 5U) {
        printf("FAIL leaf strlen\n");
        return 1;
    }
    if (v_strcmp("abc", "abd") >= 0 || v_strcmp("abd", "abc") <= 0 ||
        v_strcmp("abc", "abc") != 0) {
        printf("FAIL leaf strcmp\n");
        return 1;
    }
    if (v_strncmp("abc", "abd", n2) != 0 ||
        v_strncmp("abc", "abd", n3) >= 0) {
        printf("FAIL leaf strncmp\n");
        return 1;
    }
    if (v_memcmp("abc", "abd", n3) >= 0 ||
        v_memcmp("abc", "abc", n3) != 0) {
        printf("FAIL leaf memcmp\n");
        return 1;
    }
    if (v_memset(d, 0x5A, n8) != d || d[0] != 0x5A || d[7] != 0x5A ||
        d[8] != (char)0xA5) {
        printf("FAIL leaf memset\n");
        return 1;
    }
    for (i = 0U; i < 10U; i++) {
        d[i] = 0;
    }
    if (v_memcpy(d, "0123456789", n10) != d ||
        v_memcmp(d, "0123456789", n10) != 0) {
        printf("FAIL leaf memcpy\n");
        return 1;
    }
    if (v_memmove(d + 2, d, n5) != d + 2 ||
        v_memcmp(d, "0101234789", n10) != 0) {
        printf("FAIL leaf fwd\n");
        return 1;
    }
    for (i = 0U; i < 10U; i++) {
        d[i] = (char)('0' + (int)i);
    }
    if (v_memmove(d, d + 2, n5) != d ||
        v_memcmp(d, "2345656789", n10) != 0) {
        printf("FAIL leaf back\n");
        return 1;
    }
    if (v_memcmp(d, "2345656789", 0U) != 0) {
        printf("FAIL leaf zero\n");
        return 1;
    }
    return 0;
}

static int check_mach_thread_self(void)
{
    uint32_t p1 = (uint32_t)mach_thread_self();
    uint32_t p2 = ios_trap0((uint32_t)IOS_TRAP_MACH_THREAD_SELF);
    if (p1 == 0U || p1 != p2) {
        printf("FAIL thread_self %u %u\n", p1, p2);
        return 1;
    }
    return 0;
}

static int check_special_port(void)
{
    ios_mach_port_t reply = ios_trap0((uint32_t)IOS_TRAP_MACH_REPLY_PORT);
    uint32_t rq[16];
    const uint8_t *rb = 0;
    uint32_t i = 0U;
    ios_kern_return_t kr = 0;
    for (i = 0U; i < 16U; i++) {
        rq[i] = 0U;
    }
    if (reply == 0U) {
        printf("FAIL special reply\n");
        return 1;
    }
    rq[0] = 0x00001313U;
    rq[1] = 36U;
    rq[2] = 0x103U;
    rq[3] = reply;
    rq[4] = 0U;
    rq[5] = 3409U;
    rq[6] = 0U;
    rq[7] = 0U;
    rq[8] = 2U;
    kr = mach_msg7((ios_mach_msg_header_t *)rq,
        (uint32_t)(MACH_SEND_MSG | MACH_RCV_MSG), 36U, 64U, reply, 0U, 0U);
    if (kr != 0) {
        printf("FAIL special msg\n");
        return 1;
    }
    rb = (const uint8_t *)rq;
    if (rq[1] != 40U || (rq[0] & 0x80000000U) == 0U || rq[6] != 1U ||
        rq[7] != 0U || rb[38] != 17U) {
        printf("FAIL special reply shape\n");
        return 1;
    }
    return 0;
}

static volatile unsigned g_ax_mask = 0U;

static void ax_hc(void)
{
    g_ax_mask |= 4U;
}

static void ax_hb(void)
{
    if ((g_ax_mask & 5U) != 5U) {
        puts("FAIL atexit B");
        _exit(11);
    }
    g_ax_mask |= 2U;
}

static void ax_ha(void)
{
    if (g_ax_mask != 7U) {
        puts("FAIL atexit A");
        _exit(12);
    }
    printf("PASS test_ios_bsd_mach\n");
}

int main(int argc, char **argv, char **envp, char **apple)
{
    char stack_buf[64];
    if (check_args(argc, argv, envp, apple) != 0) {
        return 1;
    }
    if (check_errno_conv() != 0) {
        return 1;
    }
    if (check_file() != 0) {
        return 1;
    }
    if (check_mmap() != 0) {
        return 1;
    }
    if (check_sysctl() != 0) {
        return 1;
    }
    if (check_mach() != 0) {
        return 1;
    }
    if (check_fprintf() != 0) {
        return 1;
    }
    if (check_snprintf() != 0) {
        return 1;
    }
    if (check_strto() != 0) {
        return 1;
    }
    if (check_div64() != 0) {
        return 1;
    }
    if (check_leaf() != 0) {
        return 1;
    }
    if (check_mach_thread_self() != 0) {
        return 1;
    }
    if (check_special_port() != 0) {
        return 1;
    }
    /* Exercise the ARM icache trap on our own stack buffer. */
    sys_icache_invalidate(stack_buf, sizeof(stack_buf));
    if (atexit(ax_ha) != 0 || atexit(ax_hb) != 0 || atexit(ax_hc) != 0) {
        printf("FAIL atexit reg\n");
        return 1;
    }
    g_ax_mask = 1U;
    exit(0);
}
