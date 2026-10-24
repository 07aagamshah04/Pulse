#pragma once

#include <string>

/* =============================================================================
 * MONTH 2 / WEEK 3 — thread-safe logging (Month 2 / Week 3 doc, Part 9)
 * -----------------------------------------------------------------------------
 * The problem (Month 2 / Week 2, Test 6 — a real line from Pulse's log):
 *
 *   [TID [TID 549548] Parsed (] Parsed (87 bytes total): 87 bytes total): GET /helloGET /hello HTTP/1.0
 *
 * One log line used to be ~10 separate `std::cout << ...` operations, and
 * std::cout is ONE object shared by every thread — so pieces from two
 * threads' lines could land in between each other.
 *
 * The fix, in two steps:
 *   1. The CALLER builds the whole line first, privately — in a local
 *      std::ostringstream, on its own thread's stack. No sharing, no lock.
 *   2. log_line() writes that finished line in ONE operation, while holding
 *      a mutex. Only one thread can be inside that write at a time, so a
 *      line can never be split by another thread's line.
 *
 * Step 1 is what keeps the critical section tiny (Week 3 doc, Part 6: hold
 * a lock for as short a time as possible) — the slow part, formatting, is
 * done before locking.
 *
 * Usage pattern, everywhere in Pulse:
 *
 *   std::ostringstream line;
 *   line << "[TID " << gettid() << "] Parsed (" << n << " bytes total)";
 *   log_line(line.str());
 * ============================================================================= */

// Writes one complete line (a '\n' is added) to stdout, as one indivisible unit.
void log_line(const std::string& line);

// Same, but to stderr — for errors. Uses the SAME mutex as log_line(), so an
// error line can't be split by a normal line either (stdout and stderr usually
// end up in the same terminal).
void log_error(const std::string& line);
