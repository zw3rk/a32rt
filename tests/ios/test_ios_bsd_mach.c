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
    /* Exercise the ARM icache trap on our own stack buffer. */
    sys_icache_invalidate(stack_buf, sizeof(stack_buf));
    printf("PASS test_ios_bsd_mach\n");
    return 0;
}
