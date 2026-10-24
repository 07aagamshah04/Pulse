#pragma once

#include <cstdint>

/* =============================================================================
 * MONTH 2 / WEEK 3 — the first shared state that worker threads WRITE
 * -----------------------------------------------------------------------------
 * Everything threads shared in Week 2 was read-only (router_). These
 * counters are written by every worker thread — so this class is where
 * Week 3's whole topic lives: lost updates, data races, mutex vs atomic.
 * (Month 2 / Week 3 doc, Parts 1–8.)
 *
 * ONE class, THREE implementations, chosen when Pulse is BUILT:
 *
 *   cmake -DPULSE_STATS_MODE=unsafe ..   plain uint64_t, no protection at all
 *                                        — the spec's "initially implement it
 *                                        without synchronization". A DATA
 *                                        RACE: may lose updates (doc Part 3),
 *                                        and ThreadSanitizer will report it.
 *   cmake -DPULSE_STATS_MODE=mutex ..    plain uint64_t + one std::mutex
 *                                        around every read and write. The
 *                                        DEFAULT — see below for why.
 *   cmake -DPULSE_STATS_MODE=atomic ..   each counter is a std::atomic<uint64_t>
 *
 * CMakeLists.txt turns that choice into a preprocessor definition
 * (PULSE_STATS_UNSAFE / PULSE_STATS_MUTEX / PULSE_STATS_ATOMIC), and the
 * #if blocks below and in server_stats.cpp pick the matching code.
 *
 * C++ note — the preprocessor: lines starting with # (#if, #elif, #else,
 * #endif, #error) are handled BEFORE the compiler proper ever sees the
 * code. `#if defined(PULSE_STATS_MUTEX)` means "keep the lines up to the
 * next #elif/#else/#endif only if PULSE_STATS_MUTEX was defined" — the
 * other branches are deleted from the code entirely, as if never written.
 * So each build contains exactly ONE of the three implementations. Same
 * mechanism as `#pragma once` and `#include`, which you've used since Week 1.
 *
 * Why build-time instead of a runtime switch? So that the unsafe build is
 * GENUINELY unsafe, and the atomic build GENUINELY has no mutex anywhere —
 * each binary is exactly the thing being measured, with no extra branches.
 *
 * WHY THE DEFAULT IS mutex (doc Part 8): /stats reads six counters. With a
 * mutex, it copies all six under the lock, so the numbers it reports always
 * agree with each other — in particular total_requests ==
 * responses_2xx + responses_4xx + responses_5xx, because on_response()
 * updates total_requests and the bucket under the SAME lock. With six
 * separate atomics, each number is correct but /stats reads them one at a
 * time, so a snapshot can be caught between two related updates. Week 3's
 * test 7 measures what each choice costs; correctness came first.
 * ============================================================================= */

// Pick exactly one mode, or fail the build loudly instead of silently guessing.
#if !defined(PULSE_STATS_UNSAFE) && !defined(PULSE_STATS_MUTEX) && !defined(PULSE_STATS_ATOMIC)
#error "Define one of PULSE_STATS_UNSAFE / PULSE_STATS_MUTEX / PULSE_STATS_ATOMIC (CMakeLists.txt does this from PULSE_STATS_MODE)"
#endif

#if defined(PULSE_STATS_MUTEX)
#include <mutex>
#elif defined(PULSE_STATS_ATOMIC)
#include <atomic>
#endif

/* MONTH 2 / WEEK 3: a plain copy of all six numbers at one moment — what
   /stats prints. It's a struct of ordinary uint64_t values with no locks or
   atomics inside: once snapshot() has returned it, it belongs to the caller
   alone (it's on the caller's stack), so nothing about it is shared. */
struct StatsSnapshot
{
    uint64_t total_connections = 0;
    uint64_t active_connections = 0;
    uint64_t total_requests = 0;
    uint64_t responses_2xx = 0;
    uint64_t responses_4xx = 0;
    uint64_t responses_5xx = 0;
};

class ServerStats
{
public:
    /* MONTH 2 / WEEK 3 — what each call means (decisions documented here,
       as the spec's Week 3 deliverable asks):

       on_connection_opened(): a worker has started on a new connection.
           total_connections += 1, active_connections += 1.
       on_connection_closed(): that worker is finishing — on EVERY way it can
           end (response sent, client vanished, 400, caught exception).
           active_connections -= 1. Called by ActiveConnectionGuard's
           destructor (below), never directly, so it can't be forgotten.
       on_response(status): Pulse is about to send a response with this
           status code. total_requests += 1 AND the matching 2xx/4xx/5xx
           += 1. So "total_requests" means "requests Pulse answered" —
           including 400s for malformed input. A client that disconnects
           before sending a complete request gets no response and is NOT a
           request (it still counts as a connection). That choice keeps the
           invariant total_requests == 2xx + 4xx + 5xx.
       snapshot(): a copy of all six numbers, for GET /stats. */
    void on_connection_opened();
    void on_connection_closed();
    void on_response(int status_code);
    StatsSnapshot snapshot() const;

    // "unsafe" / "mutex" / "atomic" — printed at startup and by /stats, so
    // every test output says which build produced it.
    static const char* mode_name();

private:
#if defined(PULSE_STATS_ATOMIC)
    /* MONTH 2 / WEEK 3 — std::atomic<uint64_t> (doc Part 7).
       Each ++ / -- on these compiles to one `lock add` / `lock sub`
       instruction — an indivisible read-modify-write across all CPU cores.
       C++ note: `{0}` initializes the atomic to 0. (std::atomic can't be
       copied, so the `= 0` style used below for plain integers doesn't
       work for it in C++17.) */
    std::atomic<uint64_t> total_connections_{0};
    std::atomic<uint64_t> active_connections_{0};
    std::atomic<uint64_t> total_requests_{0};
    std::atomic<uint64_t> responses_2xx_{0};
    std::atomic<uint64_t> responses_4xx_{0};
    std::atomic<uint64_t> responses_5xx_{0};
#else
    /* unsafe and mutex modes: plain integers. In unsafe mode nothing guards
       them (a data race). In mutex mode every access happens while holding
       mutex_ below. */
    uint64_t total_connections_ = 0;
    uint64_t active_connections_ = 0;
    uint64_t total_requests_ = 0;
    uint64_t responses_2xx_ = 0;
    uint64_t responses_4xx_ = 0;
    uint64_t responses_5xx_ = 0;
#endif

#if defined(PULSE_STATS_MUTEX)
    /* MONTH 2 / WEEK 3 — `mutable` (doc Part 6).
       snapshot() is a const member function (reading stats doesn't change
       them), but it has to LOCK this mutex, and locking modifies the mutex.
       `mutable` lets this one member change even inside const functions —
       locking is bookkeeping, not a change to the stats' visible values. */
    mutable std::mutex mutex_;
#endif
};

/* =============================================================================
 * MONTH 2 / WEEK 3 — ActiveConnectionGuard: RAII for active_connections
 * -----------------------------------------------------------------------------
 * active_connections must go back down on EVERY way a worker can end —
 * normal response, client disconnect, malformed request, exception. That's
 * exactly the problem Socket solves for close() (Month 1 / Week 1), so the
 * answer is the same: tie it to a destructor.
 *
 *   constructor -> on_connection_opened()   (+1)
 *   destructor  -> on_connection_closed()   (-1) — runs on normal return AND
 *                                            during exception unwinding
 *
 * Created as the first line of TcpServer::serve_connection(), so it lives
 * exactly as long as the worker is handling its connection.
 * ============================================================================= */
class ActiveConnectionGuard
{
public:
    explicit ActiveConnectionGuard(ServerStats& stats) : stats_(stats) { stats_.on_connection_opened(); }
    ~ActiveConnectionGuard() { stats_.on_connection_closed(); }

    /* Not copyable: a copy would run the destructor twice -> -2 for one
       connection. Same reason Socket's copy constructor is deleted. */
    ActiveConnectionGuard(const ActiveConnectionGuard&) = delete;
    ActiveConnectionGuard& operator=(const ActiveConnectionGuard&) = delete;

private:
    /* C++ note: a REFERENCE member — the guard doesn't own the stats, it just
       points at the one shared ServerStats object. That object lives in
       main() and outlives every worker (Week 2 doc, Part 5 reasoning). */
    ServerStats& stats_;
};
