#include "tcp_server.hpp"

#include "http_parser.hpp" // Month 1 / Week 2: HttpParser::parse — see handle_client() below
#include <arpa/inet.h>     // htons, ntohs, inet_ntoa — byte-order + address-printing helpers
#include <cstring>         // strerror — turns an errno number into a human-readable message
#include <iostream>
#include <netinet/in.h> // struct sockaddr_in
#include <stdexcept>
#include <sys/socket.h> // socket(), bind(), listen(), accept(), setsockopt()
#include <unistd.h>     // read(), write()
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

        // Month 1 / Week 1: wrap the raw client_fd in a Socket immediately, then hand ownership into handle_client() — from here on we never have to remember to close this fd manually
        handle_client(Socket(client_fd));
    }
}

/* Month 1 / Week 1 — C++ note: `client` is taken BY VALUE, meaning this
   function becomes the sole owner of that Socket (ownership was moved in
   from run() above). When this function returns — however it returns —
   `client` goes out of scope right here, its destructor runs, and the
   connection is closed automatically. That's RAII in action; see Part 1
   of the doc and socket.hpp/.cpp. */
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
            std::cout << "Parsed (" << raw_request.size() << " bytes total): "
                      << result.request.method << " " << result.request.path << " " << result.request.version << "\n";
            response = router_.route(result.request); // Month 1 / Week 2 (unchanged): 200 / 404 / 405 — see router.cpp
            break;
        }

        if (result.status == ParseStatus::Malformed)
        {
            std::cout << "Malformed request (" << raw_request.size() << " bytes so far) — responding 400 Bad Request.\n";
            break; // `response` is already the bad_request() default from above
        }

        // Month 1 / Week 3: ParseStatus::Incomplete — not done yet, but also not stuck forever (see kMaxRequestSize above)
        if (raw_request.size() > kMaxRequestSize)
        {
            std::cout << "Request exceeded " << kMaxRequestSize << " bytes without completing — responding 400 Bad Request.\n";
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
            std::cerr << "read: " << std::strerror(errno) << "\n"; // n == -1 means an error — check errno
            return;                                                // `client` destructs here -> closes automatically, no response sent
        }
        if (n == 0)
        {
            /* Month 1 / Week 1 concept, Week 3 nuance: n == 0 still means
               "the client closed their end" (Part 6 of the Week 1 doc) —
               but now it can happen PARTWAY through an otherwise-valid
               request, not only "before sending anything." Either way,
               there's no complete request to respond to, so we just close. */
            std::cout << "Client disconnected before completing a request (" << raw_request.size() << " bytes received so far).\n";
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
