#include "tcp_server.hpp"

#include "http_parser.hpp" // Month 1 / Week 2: HttpParser::parse — see handle_client() below
#include <arpa/inet.h>     // htons, ntohs, inet_ntoa — byte-order + address-printing helpers
#include <cstring>         // strerror — turns an errno number into a human-readable message
#include <iostream>
#include <netinet/in.h> // struct sockaddr_in
#include <stdexcept>
#include <sys/socket.h> // socket(), bind(), listen(), accept(), setsockopt()
#include <thread>       // MONTH 2 / WEEK 2: std::thread — one per connection, see run()
#include <unistd.h>     // read(), write(); MONTH 2 / WEEK 2: also gettid() — the Linux thread ID used in log lines
#include <utility>      // std::move

namespace
{
    /* Month 1 / Week 1 — C++ note: an anonymous namespace makes everything
       inside it visible ONLY within this .cpp file — it's the C++ way of
       saying "this is a private implementation detail, not part of
       TcpServer's public interface, and no other file can accidentally
       call it." throw_errno and write_all below are helpers, not part of
       the class. */

    /* Month 1 / Week 1: EVERY syscall in this file follows the same
       pattern — returns -1 on failure, and the REASON is stored in the
       global variable `errno`. strerror(errno) turns that numeric reason
       into English (e.g. "Address already in use"). This function exists
       so we don't repeat "check for -1, build an error message, throw"
       after every single call. */
    [[noreturn]] void throw_errno(const char *what)
    {
        // [[noreturn]]: a promise to the compiler that this function always throws, never returns normally
        throw std::runtime_error(std::string(what) + ": " + std::strerror(errno));
    }

    /* Month 1 / Week 1 — see Part 4 of the doc, the read()/write() section:
       write() is only guaranteed to send SOME of your bytes per call, not
       necessarily all of them, especially for larger buffers. This loops,
       re-calling write() with whatever is left, until everything requested
       has actually gone out (or an error happens). */
    void write_all(int fd, const char *data, size_t len)
    {
        size_t sent = 0;
        while (sent < len)
        {
            ssize_t n = ::write(fd, data + sent, len - sent);
            if (n < 0)
            {
                throw_errno("write");
            }
            sent += static_cast<size_t>(n);
        }
    }

    /* Month 1 / Week 3: a hard cap on how large the accumulated buffer in
       handle_client() is allowed to grow while still waiting for a request
       to complete. Without this, a client that never sends a valid
       "\r\n\r\n" (or claims a huge Content-Length and then never finishes
       sending the body) would make the read loop below grow raw_request
       forever, one read() at a time, consuming unbounded memory for a
       single connection. This is a defensive limit, not a feature — see
       Part 4 of the Week 3 doc for the related "leftover bytes" edge case
       this same loop deliberately does NOT try to solve. */
    constexpr size_t kMaxRequestSize = 1024 * 1024; // 1 MiB
}

// Month 1 / Week 2: `router` taken by value and moved into router_ below — TcpServer becomes the sole owner, main.cpp doesn't need its copy anymore after this call
TcpServer::TcpServer(uint16_t port, Router router, int backlog) : port_(port), router_(std::move(router))
{
    /* -------------------------------------------------------------------
     * MONTH 1 / WEEK 1 — socket()
     * Creates a new socket and gives us back a file descriptor for it.
     * Nothing is bound or listening yet — see Part 4 of the doc.
     *   AF_INET     = use IPv4 addresses
     *   SOCK_STREAM = TCP-style: reliable, ordered byte stream (not UDP)
     *   0           = "pick the default protocol for this combination" (TCP)
     * ------------------------------------------------------------------- */
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1)
    {
        throw_errno("socket");
    }
    listen_socket_ = Socket(fd); // hand ownership of this fd to our RAII wrapper immediately

    /* -------------------------------------------------------------------
     * MONTH 1 / WEEK 1 — setsockopt(), SO_REUSEADDR
     * Without this, restarting the server soon after stopping it often
     * fails with "Address already in use", because the kernel is still
     * holding the port in TIME_WAIT from the previous run. This does NOT
     * let two live processes share a port at once — see Part 4 of the doc
     * for the full "common misread" explanation.
     * ------------------------------------------------------------------- */
    int opt = 1;
    if (::setsockopt(listen_socket_.fd(), SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1)
    {
        throw_errno("setsockopt");
    }

    /* -------------------------------------------------------------------
     * MONTH 1 / WEEK 1 — building the address for bind()
     * struct sockaddr_in describes WHICH local address+port we want to
     * claim. See Part 4 of the doc for what each field means.
     * ------------------------------------------------------------------- */
    sockaddr_in addr{};
    addr.sin_family = AF_INET;         // must match the domain we gave socket() above
    addr.sin_addr.s_addr = INADDR_ANY; // "any local interface" — not one specific IP
    addr.sin_port = htons(port_);      // htons: host byte order -> network byte order. Skipping this binds the WRONG port number — see Part 4 of the doc.

    /* -------------------------------------------------------------------
     * MONTH 1 / WEEK 1 — bind()
     * Claims that address for this socket. reinterpret_cast is required
     * because bind() takes a generic `sockaddr*` (so the same function
     * works for IPv4, IPv6, Unix sockets, etc.) — see Part 4 of the doc,
     * "why struct sockaddr* and not struct sockaddr_in*".
     * ------------------------------------------------------------------- */
    if (::bind(listen_socket_.fd(), reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == -1)
    {
        throw_errno("bind");
    }

    /* -------------------------------------------------------------------
     * MONTH 1 / WEEK 1 — listen()
     * Flips the socket from "can only initiate outgoing connections" to
     * "passive, willing to accept incoming ones." backlog = how many
     * handshake-completed-but-not-yet-accept()ed connections the kernel
     * will queue for us.
     * ------------------------------------------------------------------- */
    if (::listen(listen_socket_.fd(), backlog) == -1)
    {
        throw_errno("listen");
    }

    std::cout << "Pulse listening on port " << port_ << "...\n";
}

void TcpServer::run()
{
    for (;;) // Month 1 / Week 1 — C++ note: an intentional infinite loop, "for ever" — stopped only by Ctrl+C (SIGINT) from outside the program
    {
        sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);
        /* Month 1 / Week 1: addr_len is an IN/OUT parameter to accept()
           below — "in": we tell it our buffer's capacity
           (sizeof(client_addr)); "out": accept() overwrites it with how
           much it actually wrote. See Part 4 of the doc, the accept() row
           about addrlen. */

        /* ---------------------------------------------------------------
         * MONTH 1 / WEEK 1 — accept()
         * BLOCKS here — the program does nothing until a client's TCP
         * handshake completes. Returns a NEW fd representing that one
         * connection; listen_socket_'s own fd is untouched and can
         * accept() again right after. This is the "two file descriptors"
         * idea from Part 5 of the doc.
         * --------------------------------------------------------------- */
        int client_fd = ::accept(listen_socket_.fd(), reinterpret_cast<sockaddr *>(&client_addr), &addr_len);
        if (client_fd == -1)
        {
            // Month 1 / Week 1: one failed accept() shouldn't kill the whole server — log it and go back to waiting for the next client
            std::cerr << "accept: " << std::strerror(errno) << "\n";
            continue;
        }

        // Month 1 / Week 1: client_addr is now filled in with the CLIENT's real IP/port — proof each connection is independently tracked by the kernel (see Part 4 of the doc's accept() example)
        std::cout << "Client connected: " << inet_ntoa(client_addr.sin_addr)
                  << ":" << ntohs(client_addr.sin_port) << "\n";

        /* Month 1 / Week 1: wrap the raw client_fd in a Socket immediately —
           from here on we never have to remember to close this fd manually.
           (MONTH 2 / WEEK 2: now a named variable instead of a temporary,
           because it has to be std::move'd into the thread below.) */
        Socket client(client_fd);

        /* ---------------------------------------------------------------
         * MONTH 2 / WEEK 2 — thread-per-connection: THE change this week.
         *
         * Month 1 called handle_client() right here, on the main thread, so
         * the main thread was stuck with this client until it was done —
         * one silent client froze everyone (Experiment 0). Now the main
         * thread hands the connection to a NEW thread and goes straight
         * back to accept(). (Month 2 / Week 2 doc, Part 2.)
         *
         * Reading the line left to right:
         *
         *   std::thread( &TcpServer::serve_connection,   <- WHICH function: a pointer to a member
         *                                                   function (doc Part 4). It belongs to the
         *                                                   class, so it needs an object to run on...
         *                this,                           <- ...this one: the TcpServer that lives in
         *                                                   main(). Safe because run() never returns,
         *                                                   so it's never destroyed (doc Part 5).
         *                std::move(client) )             <- the connection itself, MOVED, not copied
         *              .detach();                        <- don't wait for it; let it run on its own
         *
         * OWNERSHIP (doc Parts 3 & 4, the "Ticket" demo): std::move(client)
         * makes std::thread MOVE the Socket into the new thread's storage
         * (move #1, here on the main thread), and the new thread then moves
         * it into serve_connection()'s parameter (move #2). After this line
         * our `client` is EMPTY (fd -1), so when it goes out of scope at the
         * end of this loop iteration its destructor closes nothing. The main
         * thread cannot accidentally close the worker's connection — the
         * type makes it impossible. Exactly one owner at every moment; the
         * worker closes it when it's done.
         *
         * C++ note: without std::move this wouldn't compile — std::thread
         * would try to COPY a Socket, and Socket's copy constructor is
         * deleted (socket.hpp). Forgetting the hand-over is a compile error.
         *
         * Why .detach() and not .join(): join() would make the main thread
         * wait for this connection to finish — Month 1 again with extra
         * steps (Month 2 / Week 1 doc, Part 8). And it MUST be one or the
         * other: this std::thread is a temporary, destroyed at the end of
         * this line, and destroying a joinable std::thread calls
         * std::terminate() — forgetting .detach() would crash Pulse on the
         * very first connection.
         *
         * Why the try/catch: creating a thread can FAIL (e.g. the system
         * has hit its limit on threads) — std::thread then throws
         * std::system_error. Without a catch here that would escape run()
         * and end the whole server. With it, we drop just this one
         * connection: the Socket was already moved into std::thread's
         * storage, and when construction fails that storage is destroyed,
         * which closes the fd — so nothing leaks. (If the failure happens
         * before the move, `client` still owns it and closes it at the end
         * of this iteration instead. Either way, exactly one close.)
         * --------------------------------------------------------------- */
        try
        {
            std::thread(&TcpServer::serve_connection, this, std::move(client)).detach();
        }
        catch (const std::exception &e)
        {
            std::cerr << "could not start a thread for this connection: " << e.what() << " — dropping it\n";
        }
    }
}

/* =============================================================================
 * MONTH 2 / WEEK 2 — the starting point of every worker thread
 * -----------------------------------------------------------------------------
 * Every connection's thread starts HERE — this is the bottom frame of that
 * thread's stack. Its only job is to make sure nothing that goes wrong with
 * one client can take down the rest of the server.
 *
 * Without this try/catch (Month 2 / Week 2 doc, Part 6):
 *   - Month 1 already had a hidden bug: write_all() throws on a failed
 *     write(), nothing caught it until main(), and the WHOLE server exited
 *     with "Fatal error".
 *   - With threads it's worse: an exception that escapes a thread's starting
 *     function calls std::terminate() and aborts the process — every
 *     connection, mid-request. main()'s try/catch can't catch it, because an
 *     exception only unwinds the stack of the thread it was thrown on, and
 *     main()'s try is on a DIFFERENT thread's stack.
 *
 * With it: the exception unwinds handle_client()'s frame (running ~Socket()
 * on the way, so the fd is still closed — RAII works during unwinding too),
 * lands here, gets logged, and only THIS connection ends.
 *
 * C++ note: `catch (const std::exception& e)` catches anything derived from
 * std::exception — including the std::runtime_error from throw_errno().
 * `catch (...)` — three literal dots — catches ANYTHING at all, even things
 * that aren't std::exceptions; it's the final safety net, with no `e` to
 * ask for a message.
 *
 * std::move(client): move #3 — from this function's parameter into
 * handle_client()'s by-value parameter. Ownership keeps travelling forward;
 * it's never shared.
 * ============================================================================= */
void TcpServer::serve_connection(Socket client) const
{
    try
    {
        handle_client(std::move(client));
    }
    catch (const std::exception &e)
    {
        std::cerr << "[TID " << gettid() << "] connection error: " << e.what() << " — closing this connection, server keeps running\n";
    }
    catch (...)
    {
        std::cerr << "[TID " << gettid() << "] connection error: unknown exception — closing this connection, server keeps running\n";
    }
}

/* Month 1 / Week 1 — C++ note: `client` is taken BY VALUE, meaning this
   function becomes the sole owner of that Socket (ownership was moved in
   from run() above). When this function returns — however it returns —
   `client` goes out of scope right here, its destructor runs, and the
   connection is closed automatically. That's RAII in action; see Part 1
   of the doc and socket.hpp/.cpp.

   MONTH 2 / WEEK 2: this whole function now runs on a WORKER thread, not
   the main thread — called from serve_connection(). Its body is unchanged:
   every local below (raw_request, chunk, response) lives on the worker's
   own stack, so two connections running this at the same time never touch
   each other's data (Month 2 / Week 1 doc, Part 4). It READS the shared
   router_, which is safe because nothing writes to it. `client` is still
   the one owner of the connection, so the close at the end now happens on
   the worker thread.

   MONTH 2 / WEEK 2: every log line below is prefixed with [TID n] — the
   Linux thread ID of the worker (gettid(), same number as the SPID/TID
   column in ps -T / ps -L) — so when several connections are handled at
   once you can tell which thread printed what. (Expect lines from different
   threads to interleave, even mid-line — Month 2 / Week 1, Experiment 1b.
   The fix for that, a mutex, is Week 3.) */
void TcpServer::handle_client(Socket client) const
{
    /* Month 1 / Week 3: raw_request is now an ACCUMULATION buffer, not a
       one-shot snapshot. Week 1/2 used a fixed char buf[4096] and trusted
       one read() to fill it with a complete request — Part 1 of the Week 3
       doc walks through exactly why that assumption is a real bug. This
       std::string grows across as many read() calls as it takes. `chunk`
       below is just the temporary landing spot for each individual
       read() call, not the request itself. */
    std::string raw_request;
    char chunk[4096];

    HttpResponse response = HttpResponse::bad_request(); // default assumption — overwritten below the moment we actually know more

    /* -------------------------------------------------------------------
     * MONTH 1 / WEEK 3 — the read-accumulate-reparse loop
     * This is the fix for Part 1-3 of the Week 3 doc. Each iteration:
     *   1. ask the parser "is what we have SO FAR a complete request?"
     *   2. Complete or Malformed -> we have our answer, stop looping
     *   3. Incomplete -> read() more bytes, append them, try again
     * Week 1's single read() call and Week 2's single parse() call are
     * both still present below — they're just now inside a loop instead
     * of running exactly once.
     * ------------------------------------------------------------------- */
    for (;;)
    {
        ParseResult result = HttpParser::parse(raw_request);

        if (result.status == ParseStatus::Complete)
        {
            std::cout << "[TID " << gettid() << "] Parsed (" << raw_request.size() << " bytes total): "
                      << result.request.method << " " << result.request.path << " " << result.request.version << "\n";
            response = router_.route(result.request); // Month 1 / Week 2 (unchanged): 200 / 404 / 405 — see router.cpp
            break;
        }

        if (result.status == ParseStatus::Malformed)
        {
            std::cout << "[TID " << gettid() << "] Malformed request (" << raw_request.size() << " bytes so far) — responding 400 Bad Request.\n";
            break; // `response` is already the bad_request() default from above
        }

        // Month 1 / Week 3: ParseStatus::Incomplete — not done yet, but also not stuck forever (see kMaxRequestSize above)
        if (raw_request.size() > kMaxRequestSize)
        {
            std::cout << "[TID " << gettid() << "] Request exceeded " << kMaxRequestSize << " bytes without completing — responding 400 Bad Request.\n";
            break;
        }

        /* -----------------------------------------------------------
         * MONTH 1 / WEEK 1 — read() itself is UNCHANGED: still the same
         * syscall, same meaning (n bytes actually received, which can
         * be less than sizeof(chunk)). What's new is that this call now
         * sits inside a loop instead of running exactly once.
         * ----------------------------------------------------------- */
        ssize_t n = ::read(client.fd(), chunk, sizeof(chunk));

        if (n < 0)
        {
            std::cerr << "[TID " << gettid() << "] read: " << std::strerror(errno) << "\n"; // n == -1 means an error — check errno
            return;                                                // `client` destructs here -> closes automatically, no response sent
        }
        if (n == 0)
        {
            /* Month 1 / Week 1 concept, Week 3 nuance: n == 0 still means
               "the client closed their end" (Part 6 of the Week 1 doc) —
               but now it can happen PARTWAY through an otherwise-valid
               request, not only "before sending anything." Either way,
               there's no complete request to respond to, so we just close. */
            std::cout << "[TID " << gettid() << "] Client disconnected before completing a request (" << raw_request.size() << " bytes received so far).\n";
            return;
        }

        raw_request.append(chunk, static_cast<size_t>(n)); // Month 1 / Week 3: append, don't overwrite — this is the actual "accumulate" step
    }

    /* -------------------------------------------------------------------
     * MONTH 1 / WEEK 1 — write() (via write_all, defined above), UNCHANGED
     * Still loops until every byte is actually sent. Still sends a real
     * serialized HTTP response (Part 5 of the Week 2 doc).
     * ------------------------------------------------------------------- */
    std::string response_bytes = response.to_string();
    write_all(client.fd(), response_bytes.data(), response_bytes.size());

    /* Month 1 / Week 1: no explicit close() call here — see the comment
       above this function's definition. `client`'s destructor
       (Socket::~Socket in socket.cpp) runs the moment this function
       returns and calls ::close() for us. */
}
