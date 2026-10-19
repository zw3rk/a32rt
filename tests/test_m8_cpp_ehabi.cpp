/*
 * M8 C++17 & ARM EHABI (.ARM.exidx) Exception Unwinding Conformance Suite
 *
 * Tests zero-cost ARM EHABI (.ARM.exidx / .ARM.extab) stack unwinding across
 * multi-frame call chains with callee-saved GPR (r4-r11) and VFP (d8-d15)
 * restoration, RAII destruction ordering, RTTI (dynamic_cast / typeid),
 * nested try/catch with rethrow, STL containers/algorithms, and C++17
 * multithreading (std::thread, std::mutex, std::async, std::future exception
 * propagation across threads).
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <typeinfo>

#ifndef A32RT_SELFHOST_FAST
#include <algorithm>
#include <future>
#include <map>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#endif

static int g_failures = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__,     \
                         (msg));                                               \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

/* --------------------------------------------------------------------------
 * 1. ARM EHABI (.ARM.exidx) Unwinding, RAII Destructors & Callee-Saved Regs
 * -------------------------------------------------------------------------- */
namespace {

struct BaseError : public std::exception {
    int code;
    explicit BaseError(int c) noexcept : code(c) {}
    ~BaseError() noexcept override = default;
    const char *what() const noexcept override { return "BaseError"; }
    virtual int scaled_code() const noexcept { return code; }
};

struct DerivedError : public BaseError {
    const char *detail;
    DerivedError(int c, const char *d) noexcept : BaseError(c), detail(d) {}
    ~DerivedError() noexcept override = default;
    const char *what() const noexcept override { return detail; }
    int scaled_code() const noexcept override { return code * 10; }
};

struct DtorLog {
    int ids[16];
    int count;
};

struct RaiiTracker {
    DtorLog *log;
    int id;
    RaiiTracker(DtorLog *l, int i) : log(l), id(i) {}
    ~RaiiTracker() {
        if (log != nullptr && log->count < 16) {
            log->ids[log->count++] = id;
        }
    }
};

__attribute__((noinline))
static void unwind_frame_6(DtorLog *log, int seed)
{
    RaiiTracker guard(log, 6);
    if (seed > 0) {
        throw DerivedError(seed + 42, "ehabi-frame6-throw");
    }
}

__attribute__((noinline))
static int unwind_frame_5(DtorLog *log, int seed)
{
    RaiiTracker guard(log, 5);
    uint32_t a = static_cast<uint32_t>(seed) ^ 0x55550001U;
    uint32_t b = static_cast<uint32_t>(seed) ^ 0x55550002U;
    double d0 = static_cast<double>(seed) * 1.25;
    double d1 = static_cast<double>(seed) * 2.50;
    __asm__ volatile("" : "+r"(a), "+r"(b), "+w"(d0), "+w"(d1));
    unwind_frame_6(log, seed);
    __asm__ volatile("" : "+r"(a), "+r"(b), "+w"(d0), "+w"(d1));
    return static_cast<int>(a + b) + static_cast<int>(d0 + d1);
}

__attribute__((noinline))
static int unwind_frame_4(DtorLog *log, int seed)
{
    RaiiTracker guard(log, 4);
    /* Clobber / occupy multiple callee-saved GPRs and VFP D registers so the
     * compiler emits VPUSH {d8-d15} and PUSH {r4-r11, lr} in this frame. */
    uint32_t g0 = 0x44440000U + static_cast<uint32_t>(seed);
    uint32_t g1 = g0 + 1U;
    uint32_t g2 = g0 + 2U;
    uint32_t g3 = g0 + 3U;
    uint32_t g4 = g0 + 4U;
    uint32_t g5 = g0 + 5U;
    double v0 = 101.5;
    double v1 = 202.5;
    double v2 = 303.5;
    double v3 = 404.5;
    double v4 = 505.5;
    double v5 = 606.5;
    double v6 = 707.5;
    double v7 = 808.5;
    __asm__ volatile(""
                     : "+r"(g0), "+r"(g1), "+r"(g2), "+r"(g3), "+r"(g4), "+r"(g5),
                       "+w"(v0), "+w"(v1), "+w"(v2), "+w"(v3),
                       "+w"(v4), "+w"(v5), "+w"(v6), "+w"(v7));
    int r = unwind_frame_5(log, seed + 1);
    __asm__ volatile(""
                     : "+r"(g0), "+r"(g1), "+r"(g2), "+r"(g3), "+r"(g4), "+r"(g5),
                       "+w"(v0), "+w"(v1), "+w"(v2), "+w"(v3),
                       "+w"(v4), "+w"(v5), "+w"(v6), "+w"(v7));
    return r + static_cast<int>(g0 + g5) + static_cast<int>(v0 + v7);
}

__attribute__((noinline))
static int unwind_frame_3(DtorLog *log, int seed)
{
    RaiiTracker guard(log, 3);
    uint32_t c0 = 0x33330001U;
    uint32_t c1 = 0x33330002U;
    uint32_t c2 = 0x33330003U;
    uint32_t c3 = 0x33330004U;
    double f0 = -11.25;
    double f1 = -22.50;
    double f2 = -33.75;
    double f3 = -45.00;
    __asm__ volatile(""
                     : "+r"(c0), "+r"(c1), "+r"(c2), "+r"(c3),
                       "+w"(f0), "+w"(f1), "+w"(f2), "+w"(f3));
    int r = unwind_frame_4(log, seed + 1);
    __asm__ volatile(""
                     : "+r"(c0), "+r"(c1), "+r"(c2), "+r"(c3),
                       "+w"(f0), "+w"(f1), "+w"(f2), "+w"(f3));
    return r + static_cast<int>(c0 ^ c3) + static_cast<int>(f0 + f3);
}

__attribute__((noinline))
static int unwind_frame_2(DtorLog *log, int seed)
{
    RaiiTracker guard(log, 2);
    uint32_t x = static_cast<uint32_t>(seed) * 7U;
    double y = static_cast<double>(seed) * 3.5;
    __asm__ volatile("" : "+r"(x), "+w"(y));
    int r = unwind_frame_3(log, seed + 1);
    __asm__ volatile("" : "+r"(x), "+w"(y));
    return r + static_cast<int>(x) + static_cast<int>(y);
}

__attribute__((noinline))
static void unwind_frame_1(void)
{
    DtorLog dtor_log = {{0}, 0};

    uint32_t r4_val = 0x10203040U;
    uint32_t r5_val = 0x20304050U;
    uint32_t r6_val = 0x30405060U;
    uint32_t r7_val = 0x40506070U;
    uint32_t r8_val = 0x50607080U;
    uint32_t r9_val = 0x60708090U;

    double d8_val  = 1.125;
    double d9_val  = 2.250;
    double d10_val = 3.375;
    double d11_val = 4.500;
    double d12_val = 5.625;
    double d13_val = 6.750;
    double d14_val = 7.875;
    double d15_val = 9.000;

    __asm__ volatile(""
                     : "+r"(r4_val), "+r"(r5_val), "+r"(r6_val),
                       "+r"(r7_val), "+r"(r8_val), "+r"(r9_val),
                       "+w"(d8_val), "+w"(d9_val), "+w"(d10_val), "+w"(d11_val),
                       "+w"(d12_val), "+w"(d13_val), "+w"(d14_val), "+w"(d15_val));

    bool caught = false;
    int caught_scaled = 0;
    const char *caught_msg = "";

    try {
        RaiiTracker guard1(&dtor_log, 1);
        int res = unwind_frame_2(&dtor_log, 10);
        (void)res;
        CHECK(false, "unreachable after unwind_frame_2");
    } catch (const BaseError &ex) {
        caught = true;
        caught_scaled = ex.scaled_code();
        caught_msg = ex.what();
    }

    __asm__ volatile(""
                     : "+r"(r4_val), "+r"(r5_val), "+r"(r6_val),
                       "+r"(r7_val), "+r"(r8_val), "+r"(r9_val),
                       "+w"(d8_val), "+w"(d9_val), "+w"(d10_val), "+w"(d11_val),
                       "+w"(d12_val), "+w"(d13_val), "+w"(d14_val), "+w"(d15_val));

    CHECK(caught, "caught BaseError in frame 1");
    /* seed starts at 10 -> frame2(10) -> frame3(11) -> frame4(12) -> frame5(13) -> frame6(13):
     * code = 13 + 42 = 55; scaled_code() = 550 */
    CHECK(caught_scaled == 550, "polymorphic scaled_code() == 550");
    CHECK(std::strcmp(caught_msg, "ehabi-frame6-throw") == 0, "exception what() matches");

    const int expected_dtors[6] = {6, 5, 4, 3, 2, 1};
    bool dtors_ok = (dtor_log.count == 6);
    for (int i = 0; i < 6 && dtors_ok; i++) {
        if (dtor_log.ids[i] != expected_dtors[i]) {
            dtors_ok = false;
        }
    }
    CHECK(dtors_ok, "RAII destructors ran in exact reverse order (6..1)");

    CHECK(r4_val == 0x10203040U && r5_val == 0x20304050U &&
          r6_val == 0x30405060U && r7_val == 0x40506070U &&
          r8_val == 0x50607080U && r9_val == 0x60708090U,
          "callee-saved GPRs restored across ARM EHABI unwind");

    CHECK(d8_val == 1.125 && d9_val == 2.250 &&
          d10_val == 3.375 && d11_val == 4.500 &&
          d12_val == 5.625 && d13_val == 6.750 &&
          d14_val == 7.875 && d15_val == 9.000,
          "callee-saved VFP d8-d15 restored across ARM EHABI unwind");
}

/* --------------------------------------------------------------------------
 * 2. Rethrow (throw;), Nested try/catch & RTTI (dynamic_cast / typeid)
 * -------------------------------------------------------------------------- */
static void test_rethrow_and_rtti(void)
{
    int inner_seen = 0;
    int outer_seen = 0;

    try {
        try {
            throw DerivedError(7, "rethrow-test");
        } catch (const std::exception &e) {
            const auto *as_derived = dynamic_cast<const DerivedError *>(&e);
            CHECK(as_derived != nullptr, "dynamic_cast<const DerivedError *>");
            CHECK(typeid(e) == typeid(DerivedError), "typeid(e) == typeid(DerivedError)");
            if (as_derived != nullptr) {
                inner_seen = as_derived->scaled_code();
            }
            throw;
        }
    } catch (const DerivedError &de) {
        outer_seen = de.code;
        CHECK(std::strcmp(de.detail, "rethrow-test") == 0, "rethrown DerivedError detail");
    }

    CHECK(inner_seen == 70 && outer_seen == 7, "nested catch + rethrow");
}

#ifndef A32RT_SELFHOST_FAST
/* --------------------------------------------------------------------------
 * 3. STL Containers & Algorithms (std::vector, std::map, std::sort, accumulate)
 * -------------------------------------------------------------------------- */
static void test_stl_containers_algorithms(void)
{
    std::vector<int> vec = {42, 7, 19, 3, 88, 12, 5, 31};
    std::sort(vec.begin(), vec.end());
    CHECK(vec.front() == 3 && vec.back() == 88, "std::sort vector");
    int total = std::accumulate(vec.begin(), vec.end(), 0);
    CHECK(total == 207, "std::accumulate vector");

    std::map<std::string, int> kv;
    kv["alpha"] = 10;
    kv["beta"]  = 20;
    kv["gamma"] = 30;
    kv["delta"] = 40;
    CHECK(kv.size() == 4U, "std::map size");
    CHECK(kv.at("gamma") == 30, "std::map::at(gamma)");

    int map_sum = 0;
    std::string concat_keys;
    for (const auto &entry : kv) {
        concat_keys += entry.first + ":";
        map_sum += entry.second;
    }
    CHECK(concat_keys == "alpha:beta:delta:gamma:", "std::map ordered iteration");
    CHECK(map_sum == 100, "std::map value sum");
}

/* --------------------------------------------------------------------------
 * 4. C++17 Multithreading & Cross-Thread Exception Propagation
 * -------------------------------------------------------------------------- */
static void test_cpp17_threads_and_futures(void)
{
    std::mutex mtx;
    int shared_total = 0;
    std::vector<std::thread> workers;

    for (int id = 1; id <= 4; id++) {
        workers.emplace_back([id, &mtx, &shared_total]() {
            int local_val = 0;
            try {
                if (id == 3) {
                    throw std::runtime_error("thread-local-exception");
                }
                local_val = id * 10;
            } catch (const std::runtime_error &) {
                local_val = id * 10;
            }
            std::lock_guard<std::mutex> lock(mtx);
            shared_total += local_val;
        });
    }

    for (auto &t : workers) {
        t.join();
    }
    CHECK(shared_total == 100, "std::thread + std::mutex + thread-local unwind");

    auto fut = std::async(std::launch::async, []() -> int {
        throw std::runtime_error("async-future-exception");
    });

    bool future_caught = false;
    std::string future_msg;
    try {
        (void)fut.get();
    } catch (const std::runtime_error &ex) {
        future_caught = true;
        future_msg = ex.what();
    }
    CHECK(future_caught && future_msg == "async-future-exception",
          "std::async / std::future cross-thread exception propagation");
}
#endif

} // namespace

int main()
{
    unwind_frame_1();
    test_rethrow_and_rtti();
#ifndef A32RT_SELFHOST_FAST
    test_stl_containers_algorithms();
    test_cpp17_threads_and_futures();
#endif

    if (g_failures != 0) {
        std::fprintf(stderr, "M8 C++17 & ARM EHABI suite: %d failure(s)\n",
                     g_failures);
        return 1;
    }
    std::printf("M8 C++17 & ARM EHABI (.ARM.exidx unwind, RTTI, RAII, STL, std::thread/future): PASS\n");
    return 0;
}
