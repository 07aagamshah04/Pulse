#pragma once

#include "router.hpp"
#include "server_stats.hpp" // MONTH 2 / WEEK 3
#include "socket.hpp"
#include <cstdint>

/* =============================================================================
 * MONTH 1 / WEEK 1 — the actual server
 * -----------------------------------------------------------------------------
 * This class does the full sequence from the "WEEK 1 BUILD" section of the
 * project spec: socket -> bind -> listen -> (loop: accept -> read -> print ->
 * respond -> close). One client at a time, blocking, no threads.
 *
 * MONTH 1 / WEEK 2 UPDATE: socket()/bind()/listen()/accept() below are
 * UNCHANGED from Week 1 — still the exact same syscalls, same class. The one
 * real change is what happens to the bytes read() hands back: Week 1 just
 * echoed them; Week 2 now parses them as HTTP and routes them (see
 * handle_client() below and tcp_server.cpp) — this is precisely the "turn
 * the TCP server into an HTTP server" step from the Week 2 doc's intro.
 *
 * MONTH 1 / WEEK 3 UPDATE: socket()/bind()/listen()/accept() are STILL
 * unchanged. handle_client() changes again — Week 2 called read() exactly
 * ONCE and assumed that single call contained a complete request. Week 3
 * fixes the real bug that assumption had (see Part 1 of the Week 3 doc):
 * it now calls read() in a LOOP, accumulating bytes, and only stops once
 * HttpParser::parse() reports the request is actually Complete (or
 * definitely Malformed) — see tcp_server.cpp.
 *
 * MONTH 2 / WEEK 2 UPDATE — thread-per-connection. "One client at a time,
 * no threads" (top of this comment) is no longer true. run() now only
 * accepts: each accepted connection is MOVED into its own new, detached
 * thread, which runs serve_connection() -> handle_client() for that one
 * client, and run() goes straight back to accept(). A slow client or a slow
 * handler now blocks only its own thread. handle_client()'s read/parse/
 * route/write logic is unchanged — only WHICH thread runs it changed. See
 * the Month 2 / Week 2 doc, Parts 2–6.
 *
 * MONTH 2 / WEEK 3 UPDATE — shared state that threads WRITE. Workers now
 * update a ServerStats object (connections, requests, 2xx/4xx/5xx) — the
 * first thing every thread writes, protected according to the build's
 * PULSE_STATS_MODE (server_stats.hpp). All log output goes through
 * log_line()/log_error() (logger.hpp), so lines from different threads can
 * no longer split each other. A route handler that throws now gets a 500
 * response instead of a dropped connection. See the Month 2 / Week 3 doc.
 *
 * SHARED-STATE INVENTORY (the spec's Week 3 deliverable: "documented shared
 * state"). Everything a worker thread touches, and why it's safe:
 *   per-connection (worker's own stack, never shared):
 *     the client Socket, raw_request, chunk, response, parse result
 *   shared, READ-ONLY after run() starts (safe without locks):
 *     router_, port_ — written once before any worker exists
 *   shared, only the MAIN thread uses it:
 *     listen_socket_
 *   shared and WRITTEN by workers — each protected:
 *     stats_ (ServerStats)   -> its own mutex or atomics, per build mode
 *     stdout / stderr        -> log_mutex inside logger.cpp
 * ============================================================================= */
class TcpServer
{
public:
   /* Month 1 / Week 1: this constructor performs socket()+setsockopt()+
      bind()+listen() — see tcp_server.cpp for each syscall explained
      inline. It THROWS std::runtime_error if any of those fail. Why throw
      instead of returning a bool/error code? A constructor has no return
      value to give you — throwing is C++'s way of saying "this object
      could not be built, do not treat it as valid."

      MONTH 1 / WEEK 2: now also takes a Router, built by main.cpp with
      whatever routes it wants registered before the server starts —
      see main.cpp. Taken BY VALUE and moved into router_ (see the .cpp) —
      ownership of the Router transfers into the TcpServer, main.cpp
      doesn't need it afterward.

      MONTH 2 / WEEK 3: also takes a ServerStats& — a REFERENCE to the one
      stats object, which lives in main() (so the /stats route handler,
      built in main.cpp, can read the very same object). TcpServer doesn't
      own it, it just updates it. Safe for the same lifetime reason as
      `server` itself: main() never returns while workers run. */
   explicit TcpServer(uint16_t port, Router router, ServerStats &stats, int backlog = 128);

   /* Month 1 / Week 1: blocks forever, repeatedly doing
      accept -> handle one client -> close -> accept the next.
      Stop the process with Ctrl+C.

      MONTH 2 / WEEK 2: still blocks forever, but now does only
      accept -> hand the connection to a new thread -> accept the next.
      It never waits for a client to finish. */
   void run();

private:
   /* Month 1 / Week 1: handles ONE already-accepted connection. Takes
      the Socket BY VALUE on purpose — see the comment above its
      definition in the .cpp.

      C++ note: the trailing "const" means this method promises not to
      modify TcpServer's OWN members (listen_socket_, port_, router_) — it
      only operates on the `client` Socket passed into it and reads
      (never modifies) router_. Same "const" meaning as Socket::fd() in
      socket.hpp, just applied to a method that also takes a parameter.

      MONTH 1 / WEEK 2: this function's BODY changed — it used to just
      read() and echo the bytes back (Week 1). Now it reads once, hands
      the bytes to HttpParser::parse(), and either responds 400 (parse
      failed) or asks router_ to build the real response — see the .cpp.

      MONTH 1 / WEEK 3: the BODY changes again — "reads once" above is no
      longer true. It now loops: parse what's accumulated so far, and if
      ParseStatus::Incomplete comes back, read() more and try again. Only
      Complete or Malformed ends the loop. See the .cpp for the full loop
      and Part 3 of the Week 3 doc for why this is necessary at all. */
   void handle_client(Socket client) const;

   /* MONTH 2 / WEEK 2: the function every worker thread starts in — the
      top of each worker thread's stack. It just calls handle_client(), but
      inside a try/catch that catches EVERYTHING.

      Why it has to exist (Month 2 / Week 2 doc, Part 6): an exception that
      escapes a thread's starting function calls std::terminate() and aborts
      the WHOLE process — every connection, not just this one — and the
      try/catch in main() can't help, because an exception only unwinds its
      OWN thread's stack. So this is the "last line of defence" for one
      connection: whatever goes wrong with this client is logged here, and
      the server keeps running.

      Takes the Socket BY VALUE for the same ownership reason as
      handle_client(): whoever holds this parameter owns the connection. */
   void serve_connection(Socket client) const;

   Socket listen_socket_; // Month 1 / Week 1: the ONE listening socket — see Part 5 of the doc ("why there are two file descriptors") for why this is separate from each client's socket
   uint16_t port_;        // just remembered for the log message in the constructor
   Router router_;        // Month 1 / Week 2: what handle_client() consults to turn a parsed HttpRequest into an HttpResponse
                          // MONTH 2 / WEEK 2: now READ by many worker threads at once (through `this`). Safe because nobody WRITES to it after run() starts — see Part 5 of the Month 2 / Week 2 doc

   /* MONTH 2 / WEEK 3: the shared statistics, WRITTEN by every worker.
      C++ note: a reference member. Two consequences worth knowing:
        - it must be set in the constructor's initializer list and can never
          be re-pointed at another object afterwards;
        - handle_client()/serve_connection() are `const`, yet they can still
          call non-const ServerStats methods through it. `const` on a member
          function makes the REFERENCE itself unchangeable, not the object it
          refers to. (That's why TcpServer needs no `mutable` here — the
          `mutable std::mutex` lives inside ServerStats instead, where the
          const snapshot() needs it.) */
   ServerStats &stats_;
};
