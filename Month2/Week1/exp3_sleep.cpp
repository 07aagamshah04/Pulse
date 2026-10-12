/* =============================================================================
 * MONTH 2 / WEEK 1 — Experiment 3: N threads that just SLEEP
 * -----------------------------------------------------------------------------
 * Pulse question: in Week 2, a connection thread waiting in read() for a slow
 * client is ASLEEP (state S — Concepts doc, Part 5; Experiment 0 showed
 * Pulse's one thread as "S wait_woken"). What does a sleeping thread cost?
 * CPU? Time? Memory? And can you have MORE sleeping threads than CPUs?
 *
 * This is Experiment 2 with one change: instead of a busy loop, each thread
 * calls std::this_thread::sleep_for(). Compare every number with Exp 2.
 *
 * Usage:  ./exp3_sleep.out <number_of_threads> [seconds]
 *         e.g. ./exp3_sleep.out 16        -> 16 threads, each sleeps 10 s
 *              ./exp3_sleep.out 100 3     -> 100 threads, each sleeps 3 s
 *
 * Build:
 *   g++ -std=c++17 -Wall -pthread Month2/Week1/exp3_sleep.cpp -o Month2/Week1/exp3_sleep.out
 * ============================================================================= */

#include <chrono>     // std::chrono::seconds, steady_clock
#include <cstdio>     // std::printf
#include <cstdlib>    // std::atoi
#include <thread>     // std::thread, std::this_thread::sleep_for
#include <unistd.h>   // getpid(), gettid()
#include <vector>     // std::vector

namespace
{
    /* Higher cap than Exp 2 (64). Sleeping threads use no CPU, so 100+ is
       safe here — and seeing that it's safe is part of the point. Still
       capped, per the spec's "don't destabilize your machine" warning. */
    constexpr int kMaxThreads = 200;

    /* MONTH 2 / WEEK 1 — the function every thread runs.
       `index` and `seconds` are COPIED into the new thread by std::thread
       (Concepts doc, Part 7), same as in exp2_busy.cpp. */
    void sleepy_work(int index, int seconds)
    {
        /* MONTH 2 / WEEK 1 — sleep_for (Concepts doc, Part 7).
           Puts THIS thread to sleep: it tells the kernel "don't run me
           again until this much time has passed." The thread moves to
           state S (sleeping) and the scheduler stops considering it at all
           — it is not waiting in line for a CPU (that was Exp 2's %wait),
           it's not in the line at all. A timer in the kernel wakes it up
           when the time is up.

           That is the same situation as a Pulse thread inside read()
           waiting for a slow client — the only difference is WHAT wakes it
           up: here a timer, in Pulse the network card delivering bytes.

           C++ note: std::chrono::seconds(seconds) turns a plain int into a
           typed duration. sleep_for(10) on its own would not compile — C++
           makes you say the unit (seconds? milliseconds?) on purpose. */
        std::this_thread::sleep_for(std::chrono::seconds(seconds));

        /* Only prints for small thread counts — 100 lines of "finished"
           would bury the output that matters. (Rule of thumb, not a
           threading reason.) */
        if (index < 16)
        {
            std::printf("thread %2d (TID %d) woke up and finished\n", index, gettid());
        }
    }
}

int main(int argc, char* argv[])
{
    if (argc != 2 && argc != 3)
    {
        std::printf("Usage: %s <number_of_threads> [seconds]\n", argv[0]);
        return 1;
    }

    int num_threads = std::atoi(argv[1]);
    if (num_threads < 1 || num_threads > kMaxThreads)
    {
        std::printf("number_of_threads must be between 1 and %d\n", kMaxThreads);
        return 1;
    }

    /* C++ note: ternary operator, same as exp2 — default 10 seconds. */
    int seconds = (argc == 3) ? std::atoi(argv[2]) : 10;
    if (seconds < 1 || seconds > 60)
    {
        std::printf("seconds must be between 1 and 60\n");
        return 1;
    }

    std::printf("PID %d: starting %d sleeping thread(s), %d s each, on a machine with %u CPUs\n",
                getpid(), num_threads, seconds, std::thread::hardware_concurrency());

    auto start = std::chrono::steady_clock::now();

    /* Same create-ALL-then-join-ALL pattern as Exp 2 (Concepts doc, Part 8).
       emplace_back builds each move-only std::thread directly in the
       vector; each thread starts sleeping the moment it's constructed. */
    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; ++i)
    {
        threads.emplace_back(sleepy_work, i, seconds);
    }

    /* The main thread now ALSO sleeps — inside join(), waiting for thread 0
       to finish. So during this experiment EVERY thread in the process is
       asleep: N workers in sleep_for, plus main in join(). */
    for (auto& t : threads)
    {
        t.join();
    }

    std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
    std::printf("all %d thread(s) done in %.2f s\n", num_threads, elapsed.count());
}
