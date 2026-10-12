/* =============================================================================
 * MONTH 2 / WEEK 1 — Experiment 2: N threads doing pure CPU work
 * -----------------------------------------------------------------------------
 * Pulse question: what happens when threads actually NEED the CPU at the same
 * time? And does join() make them run one after another? (Concepts doc,
 * Parts 5, 6, 8)
 *
 * Usage:  ./exp2_busy.out <number_of_threads> [rounds]
 *         e.g. ./exp2_busy.out 4       -> 4 threads, normal amount of work
 *              ./exp2_busy.out 8 4     -> 8 threads, each doing 4x the work
 *                                         (just to make it last long enough
 *                                         to watch with ps/top/pidstat)
 *
 * Each thread runs a loop of kIterations additions per round — about 2.5 s
 * of CPU work for one thread, one round, on this machine (measured).
 * main() creates ALL the threads first, then joins all of them, then prints
 * how long the whole thing took.
 *
 * Build (NO -O2 — see notes.html, "Setup"):
 *   g++ -std=c++17 -Wall -pthread Month2/Week1/exp2_busy.cpp -o Month2/Week1/exp2_busy.out
 * ============================================================================= */

#include <chrono>     // std::chrono::steady_clock — for timing
#include <cstdio>     // std::printf
#include <cstdlib>    // std::atoi
#include <thread>     // std::thread
#include <unistd.h>   // getpid(), gettid()
#include <vector>     // std::vector

namespace
{
    /* MONTH 2 / WEEK 1 — how much work each thread does.
       C++ note: `constexpr` = a constant the compiler knows at compile time.
       `ULL` on the end = "unsigned long long" literal, so the number isn't
       treated as a plain int (1,500,000,000 is close to the limit of a
       32-bit int, and it's compared against an unsigned long long below). */
    constexpr unsigned long long kIterations = 1'500'000'000ULL;
    /* C++ note: the ' marks inside the number are just digit separators
       (C++14 and later), purely for readability — the compiler ignores them. */

    /* Safety cap. The spec's Week 4 warning applies here too: don't create
       an extreme workload that can make the machine unusable. */
    constexpr int kMaxThreads = 64;

    /* MONTH 2 / WEEK 1 — the function every thread runs.

       `index` and `rounds` are passed in by std::thread, and — Concepts doc,
       Part 7 — std::thread COPIES them into the new thread. So each thread
       has its own index, even though main()'s loop variable `i` keeps
       changing. */
    void busy_work(int index, int rounds)
    {
        /* `sum` is a LOCAL variable: it lives on THIS thread's own stack
           (Concepts doc, Part 4 — the notebook, not the whiteboard). Every
           thread has its own `sum`, so the threads never touch each other's
           data. That's why there's no Week 3 problem in this program. */
        unsigned long long sum = 0;

        /* The actual CPU work: kIterations additions. Nothing in this loop
           ever waits — no read(), no sleep — so this thread wants a CPU
           100% of the time until it finishes. That's the difference from
           Experiment 3 (sleeping) and from Pulse (waiting on the network). */
        for (int r = 0; r < rounds; ++r)
        {
            for (unsigned long long i = 0; i < kIterations; ++i)
            {
                sum += i;
            }
        }

        /* Printing `sum` is what keeps this loop alive: the result is
           actually used, so the compiler can't decide the loop is pointless
           and delete it.

           One printf call per line, not several std::cout << pieces —
           Experiment 1b showed separate << pieces from different threads can
           mix together; one printf call building the whole line is much less
           likely to.

           gettid() = this thread's Linux thread ID — the same number as the
           TID/SPID column in `ps -L` / `ps -T`, so you can match lines here
           to rows there. */
        std::printf("thread %2d (TID %d) finished, sum = %llu\n", index, gettid(), sum);
    }
}

int main(int argc, char* argv[])
{
    /* Same argc/argv handling as Pulse's main.cpp does for the port. */
    if (argc != 2 && argc != 3)
    {
        std::printf("Usage: %s <number_of_threads> [rounds]\n", argv[0]);
        return 1;
    }

    int num_threads = std::atoi(argv[1]);
    if (num_threads < 1 || num_threads > kMaxThreads)
    {
        std::printf("number_of_threads must be between 1 and %d\n", kMaxThreads);
        return 1;
    }

    /* Optional second argument. C++ note: `cond ? a : b` is the ternary
       operator — "if cond then a, else b" in one expression. argc == 3 means
       the user gave a rounds value; otherwise default to 1. */
    int rounds = (argc == 3) ? std::atoi(argv[2]) : 1;
    if (rounds < 1 || rounds > 20)
    {
        std::printf("rounds must be between 1 and 20\n");
        return 1;
    }

    /* MONTH 2 / WEEK 1 — hardware_concurrency() (Concepts doc, Part 7):
       how many threads can truly run at the same instant — 12 on this
       WSL2 setup. Printed so the output always shows how the thread count
       compares to the CPU count. getpid() is printed so you can use it
       with ps / top / pidstat in another terminal. */
    std::printf("PID %d: starting %d thread(s), %d round(s) each, on a machine with %u CPUs\n",
                getpid(), num_threads, rounds, std::thread::hardware_concurrency());

    /* C++ note: steady_clock is a clock that only ever moves forward (unlike
       the wall clock, which can be adjusted), which is what you want for
       measuring durations. now() returns a point in time. */
    auto start = std::chrono::steady_clock::now();

    /* MONTH 2 / WEEK 1 — keeping many threads in a vector (Concepts doc,
       Part 7). std::thread is move-only (like Pulse's Socket), so:
         - emplace_back(busy_work, i, rounds) builds the std::thread
           directly INSIDE the vector — no copy is ever made. It's the same
           as `std::thread t(busy_work, i, rounds);`, just constructed in
           place.
         - The thread starts running the moment it's constructed, so by the
           time this loop creates thread 1, thread 0 is already working. */
    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; ++i)
    {
        threads.emplace_back(busy_work, i, rounds);
    }

    /* MONTH 2 / WEEK 1 — join ALL threads, only after creating ALL of them
       (Concepts doc, Part 8). While main waits on thread 0, threads 1..N-1
       keep running — join() only makes main wait, it doesn't stop or
       serialize anyone else.

       C++ note: `auto& t` — the & makes t a REFERENCE to each thread in the
       vector. `for (auto t : threads)` would try to COPY each std::thread,
       which is deleted (move-only), so it wouldn't compile. */
    for (auto& t : threads)
    {
        t.join();
    }

    /* end - start is a duration; duration<double> converts it to seconds as
       a double so it can be printed with %.2f. */
    std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
    std::printf("all %d thread(s) done in %.2f s\n", num_threads, elapsed.count());
}
