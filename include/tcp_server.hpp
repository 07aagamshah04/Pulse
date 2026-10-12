#pragma once

#include "router.hpp"
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
      doesn't need it afterward. */
   explicit TcpServer(uint16_t port, Router router, int backlog = 128);

   /* Month 1 / Week 1: blocks forever, repeatedly doing
      accept -> handle one client -> close -> accept the next.
      Stop the process with Ctrl+C. */
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

   Socket listen_socket_; // Month 1 / Week 1: the ONE listening socket — see Part 5 of the doc ("why there are two file descriptors") for why this is separate from each client's socket
   uint16_t port_;        // just remembered for the log message in the constructor
   Router router_;        // Month 1 / Week 2: what handle_client() consults to turn a parsed HttpRequest into an HttpResponse
};
