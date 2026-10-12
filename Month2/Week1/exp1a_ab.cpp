/* =============================================================================
 * MONTH 2 / WEEK 1 — Experiment 1a: who runs first, A or B?
 * -----------------------------------------------------------------------------
 * Pulse question: in Week 2, if connection A's thread is created before
 * connection B's thread, does A's code run first?
 *
 * This program creates thread t1 (prints "A"), then thread t2 (prints "B"),
 * then waits for both. Run it 100 times and count which order you get —
 * see notes.html, Experiment 1, for the exact command.
 *
 * Build:  g++ -std=c++17 -Wall -pthread Month2/Week1/exp1a_ab.cpp -o Month2/Week1/exp1a_ab.out
 * ============================================================================= */

#include <iostream> // std::cout
#include <thread>   // std::thread — Concepts doc, Part 7

int main()
{
   /* MONTH 2 / WEEK 1 — creating a thread (Concepts doc, Part 7).
      `std::thread t1( ... );` declares a variable t1 of type std::thread AND
      starts a brand-new thread right here, inside the constructor. There is
      no separate .start() call. By the time the next line runs, t1's code
      may already have printed, may be printing, or may not have been
      scheduled yet — nothing guarantees which.

      C++ syntax: the thing in the brackets is a LAMBDA — a small function
      with no name, written inline:
          [ ]          capture list — which outside variables the lambda can
                       use. Empty = it uses nothing from outside (nothing to
                       go wrong if the thread outlives anything).
          { ... }      the body — the code the new thread runs.
      It's the same lambda syntax as the route handlers in src/main.cpp,
      minus the (const HttpRequest&) parameter list — this one takes no
      arguments, so the () can be left out entirely. */
   std::thread t1([]
                  { std::cout << "A\n"; });

   /* Created SECOND. The question this experiment answers is whether
      "created second" means "runs second". */
   std::thread t2([]
                  { std::cout << "B\n"; });

   /* MONTH 2 / WEEK 1 — join (Concepts doc, Part 8).
      join() = "main thread, wait here until that thread has finished."
      It does NOT start or schedule anything — both threads were already
      started on the lines above, so they're running concurrently while
      main waits here.

      Both joins come AFTER both threads are created, on purpose. If we
      wrote t1.join() straight after creating t1, t2 wouldn't exist until
      t1 had finished, and "A first" would be guaranteed by our own code —
      the experiment would measure nothing.

      Leaving out a join() would make t1/t2's destructor run at the end of
      main while still joinable -> std::terminate() -> the program aborts
      with "terminate called without an active exception" (Part 8). */
   t1.join();
   t2.join();
}
