#pragma once

#include "socket.hpp"
#include <cstdint>

/* =============================================================================
 * MONTH 1 / WEEK 1 — the actual server
 * -----------------------------------------------------------------------------
 * This class does the full sequence from the "WEEK 1 BUILD" section of the
 * project spec: socket -> bind -> listen -> (loop: accept -> read -> print ->
 * respond -> close). One client at a time, blocking, no threads, no HTTP yet —
 * deliberately minimal, matching Month 1 Week 1's scope exactly.
 * ============================================================================= */
class TcpServer
{
public:
    /* Month 1 / Week 1: this constructor performs socket()+setsockopt()+
       bind()+listen() — see tcp_server.cpp for each syscall explained
       inline. It THROWS std::runtime_error if any of those fail. Why throw
       instead of returning a bool/error code? A constructor has no return
       value to give you — throwing is C++'s way of saying "this object
       could not be built, do not treat it as valid." */
    explicit TcpServer(uint16_t port, int backlog = 128);

    /* Month 1 / Week 1: blocks forever, repeatedly doing
       accept -> handle one client -> close -> accept the next.
       Stop the process with Ctrl+C. */
    void run();

private:
    /* Month 1 / Week 1: handles ONE already-accepted connection: read,
       print, echo, and (implicitly, via Socket's destructor) close. Takes
       the Socket BY VALUE on purpose — see the comment above its
       definition in the .cpp.

       C++ note: the trailing "const" means this method promises not to
       modify TcpServer's OWN members (listen_socket_, port_) — it only
       operates on the `client` Socket passed into it, never touches the
       server's own state. Same "const" meaning as Socket::fd() in
       socket.hpp, just applied to a method that also takes a parameter. */
    void handle_client(Socket client) const;

    Socket listen_socket_;   // Month 1 / Week 1: the ONE listening socket — see Part 5 of the doc ("why there are two file descriptors") for why this is separate from each client's socket
    uint16_t port_;          // just remembered for the log message in the constructor
};
