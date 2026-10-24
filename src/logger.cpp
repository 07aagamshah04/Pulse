#include "logger.hpp"

#include <iostream>
#include <mutex>

namespace
{
    /* MONTH 2 / WEEK 3 — the log mutex.

       ONE mutex for all log output: whoever holds it is the only thread
       writing to the terminal right now.

       Why it lives here, in an anonymous namespace, and not in TcpServer:
       logging is used from several places (TcpServer::run on the main
       thread, handle_client/serve_connection on workers) and has nothing to
       do with any one object — it's genuinely one-per-process state, like
       std::cout itself. The anonymous namespace (Month 1 / Week 1) makes it
       private to this file: nothing else can lock it, or forget to.

       Lifetime: a global like this is constructed before main() starts and
       destroyed after main() returns — longer than any worker thread can
       use it. (Same reasoning as `server` in main.cpp, Week 2 doc Part 5.)

       Deadlock rule (Week 3 doc, Part 10): this mutex is only ever held for
       one write, and nothing that writes the log holds the stats mutex at
       the same time — so the two can never wait on each other. */
    std::mutex log_mutex;
}

void log_line(const std::string& line)
{
    /* MONTH 2 / WEEK 3 — std::lock_guard (Week 3 doc, Part 6).
       Constructor locks log_mutex; destructor unlocks it at the closing }.
       RAII: if the write threw an exception, the unlock would still happen.

       C++ note: the variable MUST have a name (`lock`). An unnamed
       `std::lock_guard<std::mutex>{log_mutex};` would be destroyed at the
       end of its own line — lock, immediately unlock — protecting nothing,
       and it would compile without any warning.

       The critical section is just this one `<<` chain on a string that's
       already fully built, so other threads wait for microseconds at most. */
    std::lock_guard<std::mutex> lock(log_mutex);
    std::cout << line << '\n';
}

void log_error(const std::string& line)
{
    std::lock_guard<std::mutex> lock(log_mutex);   // same mutex as log_line — see the header
    std::cerr << line << '\n';
}
