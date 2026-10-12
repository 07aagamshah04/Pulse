/* =============================================================================
 * MONTH 2 / WEEK 1 — Experiment 1b: A and B, 2000 times each
 * -----------------------------------------------------------------------------
 * Same as exp1a_ab.cpp, but each thread prints its letter 2000 times instead
 * of once. In 1a each thread's whole job takes a few microseconds, so it is
 * often finished before the other thread even starts. Here each thread runs
 * long enough that both are likely to be running AT THE SAME TIME — so this
 * shows what concurrent threads writing to one shared std::cout look like.
 *
 * Pulse question: in Week 2, several connection threads will all log to the
 * same std::cout. What does that output look like? (Concepts doc, Part 9)
 *
 * Build:  g++ -std=c++17 -Wall -pthread Month2/Week1/exp1b_ab_many.cpp -o Month2/Week1/exp1b_ab_many.out
 * ============================================================================= */

#include <iostream> // std::cout
#include <thread>   // std::thread

int main()
{
    /* MONTH 2 / WEEK 1 — a lambda with more than one statement in its body.
       Same [] { ... } lambda as in 1a; the body is just a for loop now
       instead of one print.

       C++ note: 'A' in SINGLE quotes is one char (a single character).
       "A" in DOUBLE quotes would be a string (an array of chars ending in
       '\0'). std::cout prints either; a char is the simplest thing to print
       one at a time.

       C++ note: no "\n" inside the loop, on purpose — all 2000 letters go on
       one line, so you can SEE whether the A's and B's stay in two clean
       blocks or get mixed together. */
    std::thread t1([]
                   {
        for (int i = 0; i < 2000; ++i)
        {
            std::cout << 'A';
        } });

    std::thread t2([]
                   {
        for (int i = 0; i < 2000; ++i)
        {
            std::cout << 'B';
        } });

    /* Both created first, then both joined — same reasoning as 1a. */
    t1.join();
    t2.join();

    /* MONTH 2 / WEEK 1: runs only after BOTH threads have finished (that's
       what the joins above guarantee), so this newline always comes last.
       It just ends the line so your terminal prompt starts on a fresh line. */
    std::cout << "\n";
}
