#include "server_stats.hpp"

/* =============================================================================
 * MONTH 2 / WEEK 3 — the three implementations of ServerStats
 * -----------------------------------------------------------------------------
 * Exactly ONE of the three blocks below is compiled into any given Pulse
 * binary — the preprocessor deletes the other two (see server_stats.hpp).
 * Reading them side by side is the point: same four functions, three
 * different answers to "what happens when two threads call this at once?"
 * ============================================================================= */

#if defined(PULSE_STATS_UNSAFE)

/* -----------------------------------------------------------------------------
 * UNSAFE — no synchronization at all (doc Parts 2–4).
 *
 * Every ++ below is READ -> MODIFY -> WRITE (three instructions at Pulse's
 * -O0: mov / add / mov). Two workers incrementing at the same moment can both
 * read the same old value and one update is lost. This is a DATA RACE —
 * undefined behavior in C++ — and ThreadSanitizer reports it.
 *
 * active_connections_-- has the same problem in reverse: a lost -1 leaves
 * active_connections stuck above 0 after all clients are gone, and a lost
 * +1 followed by both -1s makes it go BELOW 0 — which, for an unsigned
 * uint64_t, wraps around to 18446744073709551615. Either one, seen in
 * /stats after all traffic stops, is direct proof of a lost update.
 *
 * Kept on purpose — the spec's step "implement it without synchronization,
 * try to observe incorrect results". Never the build to actually run.
 * --------------------------------------------------------------------------- */

void ServerStats::on_connection_opened()
{
    total_connections_++;
    active_connections_++;
}

void ServerStats::on_connection_closed()
{
    active_connections_--;
}

void ServerStats::on_response(int status_code)
{
    total_requests_++;
    if (status_code >= 200 && status_code < 300) responses_2xx_++;
    else if (status_code >= 400 && status_code < 500) responses_4xx_++;
    else if (status_code >= 500 && status_code < 600) responses_5xx_++;
}

StatsSnapshot ServerStats::snapshot() const
{
    // Reads while other threads may be writing — also part of the data race.
    StatsSnapshot s;
    s.total_connections = total_connections_;
    s.active_connections = active_connections_;
    s.total_requests = total_requests_;
    s.responses_2xx = responses_2xx_;
    s.responses_4xx = responses_4xx_;
    s.responses_5xx = responses_5xx_;
    return s;
}

const char* ServerStats::mode_name() { return "unsafe"; }

#elif defined(PULSE_STATS_MUTEX)

/* -----------------------------------------------------------------------------
 * MUTEX — every access to the counters happens while holding mutex_
 * (doc Part 6).
 *
 * std::lock_guard<std::mutex> lock(mutex_);
 *   constructor = mutex_.lock()   — if another worker holds it, this thread
 *                                   sleeps (futex_wait_queue) until it's free
 *   destructor  = mutex_.unlock() — at the closing }, on EVERY exit path
 *
 * Each critical section is a few increments — nanoseconds. No I/O, no
 * logging, no sleeping while holding it (doc Part 6's rule: a slow holder
 * blocks every other worker, Month 1's problem moved into a lock).
 *
 * The thing only this mode guarantees: on_response() updates
 * total_requests_ AND a bucket inside ONE critical section, and snapshot()
 * copies all six inside one critical section, so /stats can never observe
 * the moment between them. total_requests == 2xx + 4xx + 5xx always holds
 * in what /stats shows.
 * --------------------------------------------------------------------------- */

void ServerStats::on_connection_opened()
{
    std::lock_guard<std::mutex> lock(mutex_);
    total_connections_++;
    active_connections_++;
}

void ServerStats::on_connection_closed()
{
    std::lock_guard<std::mutex> lock(mutex_);
    active_connections_--;
}

void ServerStats::on_response(int status_code)
{
    std::lock_guard<std::mutex> lock(mutex_);
    total_requests_++;
    if (status_code >= 200 && status_code < 300) responses_2xx_++;
    else if (status_code >= 400 && status_code < 500) responses_4xx_++;
    else if (status_code >= 500 && status_code < 600) responses_5xx_++;
}

StatsSnapshot ServerStats::snapshot() const
{
    /* const function + lock: only possible because mutex_ is `mutable`
       (server_stats.hpp). All six copied under one lock = one consistent
       moment. */
    std::lock_guard<std::mutex> lock(mutex_);
    StatsSnapshot s;
    s.total_connections = total_connections_;
    s.active_connections = active_connections_;
    s.total_requests = total_requests_;
    s.responses_2xx = responses_2xx_;
    s.responses_4xx = responses_4xx_;
    s.responses_5xx = responses_5xx_;
    return s;
}

const char* ServerStats::mode_name() { return "mutex"; }

#elif defined(PULSE_STATS_ATOMIC)

/* -----------------------------------------------------------------------------
 * ATOMIC — each counter is a std::atomic<uint64_t> (doc Part 7).
 *
 * `x++` / `x--` on a std::atomic is ONE indivisible read-modify-write
 * (`lock add` / `lock sub`), so no single counter can lose an update.
 * C++ note: it must be ++ / -- / += / fetch_add — NOT `x = x + 1`, which is
 * an atomic read, then a separate atomic write, with a gap between them
 * (doc Part 7's pitfall).
 *
 * What atomics do NOT give: several counters changing "together".
 * on_response() does two separate atomic increments (total_requests_, then
 * a bucket), and snapshot() reads six atomics one at a time — so /stats can
 * catch a moment where total_requests is already +1 but the bucket isn't
 * yet. Each number is right; they may briefly not add up. Once traffic
 * stops, they always do.
 * --------------------------------------------------------------------------- */

void ServerStats::on_connection_opened()
{
    total_connections_++;
    active_connections_++;
}

void ServerStats::on_connection_closed()
{
    active_connections_--;
}

void ServerStats::on_response(int status_code)
{
    total_requests_++;
    if (status_code >= 200 && status_code < 300) responses_2xx_++;
    else if (status_code >= 400 && status_code < 500) responses_4xx_++;
    else if (status_code >= 500 && status_code < 600) responses_5xx_++;
}

StatsSnapshot ServerStats::snapshot() const
{
    // C++ note: .load() = read the atomic's current value. Six separate loads,
    // so six separate moments in time (see the block comment above).
    StatsSnapshot s;
    s.total_connections = total_connections_.load();
    s.active_connections = active_connections_.load();
    s.total_requests = total_requests_.load();
    s.responses_2xx = responses_2xx_.load();
    s.responses_4xx = responses_4xx_.load();
    s.responses_5xx = responses_5xx_.load();
    return s;
}

const char* ServerStats::mode_name() { return "atomic"; }

#endif
