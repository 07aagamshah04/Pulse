/* =============================================================================
 * MONTH 3 / WEEK 1 — Experiment A (non-blocking side) + Experiment B (busy polling)
 * -----------------------------------------------------------------------------
 * Pulse question: what changes if the client_fd Pulse gets from accept() is
 * switched to non-blocking? Does read() still trap the thread? And if read()
 * stops waiting, how does the code find out when data HAS arrived?
 *
 * The spec's steps, in order:
 *   1. create a TCP server (port 9090)
 *   2. accept one client
 *   3. set the accepted socket to non-blocking with fcntl()
 *   4. call read() before the client sends anything
 *   5. observe the return value and errno
 *   6-8. send data from the client, read() again, see the difference
 *
 * Because read() no longer waits, the program has to call it AGAIN and AGAIN
 * to notice when data arrives. Two ways to do that, chosen on the command line:
 *
 * Usage:  ./nonblocking.out [patient|busy]
 *   patient (default) try read() once per second, print EVERY attempt.
 *                     Cheap on CPU, but data can sit unnoticed for up to 1 s.
 *   busy              try read() again IMMEDIATELY, forever (busy polling).
 *                     Prints a one-line summary per second instead of every
 *                     attempt (there will be far too many to print).
 *
 * Client:  nc localhost 9090      (type a line + Enter to send; Ctrl+D = close)
 *
 * Build:
 *   g++ -std=c++17 -Wall -Wextra Month3/Week1/nonblocking_socket_experiment.cpp -o Month3/Week1/nonblocking.out
 * ============================================================================= */

#include <arpa/inet.h>  // htons
#include <cerrno>       // errno, EAGAIN, EWOULDBLOCK, EINTR
#include <chrono>       // std::chrono::steady_clock
#include <cstdio>       // std::printf, std::perror
#include <cstdlib>      // std::exit
#include <cstring>      // std::strcmp, std::strerror
#include <fcntl.h>      // fcntl, F_GETFL, F_SETFL, O_NONBLOCK — the point of this week
#include <netinet/in.h> // sockaddr_in
#include <string>       // std::string
#include <sys/socket.h> // socket, bind, listen, accept, setsockopt
#include <unistd.h>     // read, close, sleep, getpid

namespace
{
    constexpr uint16_t kPort = 9090;

    using Clock = std::chrono::steady_clock;
    Clock::time_point g_start;

    double since_start()
    {
        return std::chrono::duration<double>(Clock::now() - g_start).count();
    }

    // Same as in blocking_socket_experiment.cpp — see the comment there.
    int make_listener(uint16_t port)
    {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd == -1) { std::perror("socket"); std::exit(1); }

        int opt = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);
        if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == -1) { std::perror("bind"); std::exit(1); }
        if (::listen(fd, 16) == -1) { std::perror("listen"); std::exit(1); }
        return fd;
    }

    std::string visible(const char *data, ssize_t n)
    {
        std::string out;
        for (ssize_t i = 0; i < n; ++i)
        {
            if (data[i] == '\n') out += "\\n";
            else if (data[i] == '\r') out += "\\r";
            else out += data[i];
        }
        return out;
    }

    /* MONTH 3 / WEEK 1 — the "is this -1 actually a problem?" test.
       EAGAIN / EWOULDBLOCK: "nothing to read RIGHT NOW, try later" — the
                             connection is perfectly healthy.
       EINTR:                "a signal interrupted me" — also healthy.
       Anything else (ECONNRESET, EBADF, ...): a real error. */
    bool is_try_again(int err)
    {
        return err == EAGAIN || err == EWOULDBLOCK || err == EINTR;
    }

    const char *errno_name(int err)
    {
        if (err == EAGAIN) return "EAGAIN"; // on Linux EWOULDBLOCK is the SAME number, so this covers both
        if (err == EINTR) return "EINTR";
        return "other";
    }

    /* MONTH 3 / WEEK 1 — patient mode: one read() attempt per second. */
    void run_patient(int fd)
    {
        char buf[4096];
        for (int attempt = 1;; ++attempt)
        {
            Clock::time_point before = Clock::now();
            ssize_t n = ::read(fd, buf, sizeof(buf));
            int err = errno; // save before anything else can overwrite it
            long took_us = static_cast<long>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - before).count());

            if (n > 0)
            {
                std::printf("[%7.3f s] attempt #%d: read() returned %zd (in %ld us): \"%s\"\n",
                            since_start(), attempt, n, took_us, visible(buf, n).c_str());
            }
            else if (n == 0)
            {
                std::printf("[%7.3f s] attempt #%d: read() returned 0 (in %ld us): client closed its side (EOF). Done.\n",
                            since_start(), attempt, took_us);
                return;
            }
            else if (is_try_again(err))
            {
                std::printf("[%7.3f s] attempt #%d: read() returned -1, errno = %s (%d, \"%s\") in %ld us -> nothing yet, try again in 1 s\n",
                            since_start(), attempt, errno_name(err), err, std::strerror(err), took_us);
            }
            else
            {
                std::printf("[%7.3f s] attempt #%d: read() returned -1, errno = %d (\"%s\"): a real error. Giving up.\n",
                            since_start(), attempt, err, std::strerror(err));
                return;
            }
            std::fflush(stdout);
            ::sleep(1); // the "patient" part: the thread sleeps HERE, in sleep(), not inside read()
        }
    }

    /* MONTH 3 / WEEK 1 — busy mode (Experiment B): retry with no pause at
       all. Every second, print how many read() calls happened. */
    void run_busy(int fd)
    {
        char buf[4096];
        long calls = 0, eagains = 0, bytes = 0;
        Clock::time_point window_start = Clock::now();

        for (;;)
        {
            ssize_t n = ::read(fd, buf, sizeof(buf));
            int err = errno;
            ++calls;

            if (n > 0)
            {
                bytes += n;
                std::printf("[%7.3f s] DATA after %ld read() calls this second: \"%s\"\n",
                            since_start(), calls, visible(buf, n).c_str());
            }
            else if (n == 0)
            {
                std::printf("[%7.3f s] read() returned 0: client closed its side (EOF). Done.\n", since_start());
                return;
            }
            else if (is_try_again(err))
            {
                ++eagains;
            }
            else
            {
                std::printf("[%7.3f s] read() returned -1, errno = %d (\"%s\"): a real error. Giving up.\n",
                            since_start(), err, std::strerror(err));
                return;
            }

            // Once a second, a summary line. (Reading the clock is cheap on Linux — no syscall.)
            Clock::time_point now = Clock::now();
            if (now - window_start >= std::chrono::seconds(1))
            {
                std::printf("[%7.3f s] last second: %ld read() calls, %ld returned EAGAIN, %ld bytes received\n",
                            since_start(), calls, eagains, bytes);
                std::fflush(stdout);
                calls = eagains = bytes = 0;
                window_start = now;
            }
        }
    }
}

int main(int argc, char *argv[])
{
    const char *mode = argc > 1 ? argv[1] : "patient";
    const bool busy = std::strcmp(mode, "busy") == 0;
    if (!busy && std::strcmp(mode, "patient") != 0)
    {
        std::printf("usage: %s [patient|busy]\n", argv[0]);
        return 1;
    }

    std::printf("PID %d, mode: %s\n", ::getpid(), mode);
    std::printf("On this system: EAGAIN = %d, EWOULDBLOCK = %d\n", EAGAIN, EWOULDBLOCK);

    int listen_fd = make_listener(kPort);
    std::printf("Waiting in accept() on port %u ... connect with:  nc localhost %u\n", kPort, kPort);
    std::fflush(stdout);

    /* accept() itself is still BLOCKING here: only the accepted socket is
       switched below. (The listening socket can be made non-blocking too —
       that's Week 3, when poll() tells us when to accept.) */
    int client_fd = ::accept(listen_fd, nullptr, nullptr);
    if (client_fd == -1) { std::perror("accept"); return 1; }
    g_start = Clock::now();

    /* -------------------------------------------------------------------
     * MONTH 3 / WEEK 1 — fcntl(): THE change this week.
     *
     * Every open fd carries a small set of "file status flags" — one int,
     * one bit per flag. O_NONBLOCK is one of those bits.
     *
     *   F_GETFL -> read the current flags (the whole int)
     *   F_SETFL -> replace them with a new int
     *
     * So "turn O_NONBLOCK on" is read-modify-write: take the flags that are
     * already there, OR in the one bit, write them all back. Calling
     * fcntl(fd, F_SETFL, O_NONBLOCK) on its own would mean "set the flags
     * to ONLY O_NONBLOCK" — clearing any other status flag that was on
     * (e.g. O_APPEND on a file). Sockets usually have none set, but the
     * habit protects you on every other kind of fd.
     * ------------------------------------------------------------------- */
    int flags = ::fcntl(client_fd, F_GETFL);
    if (flags == -1) { std::perror("fcntl(F_GETFL)"); return 1; }
    std::printf("[%7.3f s] accepted client, fd %d. flags before = 0x%x (O_NONBLOCK %s)\n",
                since_start(), client_fd, flags, (flags & O_NONBLOCK) ? "ON" : "OFF");

    if (::fcntl(client_fd, F_SETFL, flags | O_NONBLOCK) == -1) { std::perror("fcntl(F_SETFL)"); return 1; }

    int after = ::fcntl(client_fd, F_GETFL);
    std::printf("[%7.3f s] flags after  = 0x%x (O_NONBLOCK %s, O_NONBLOCK bit = 0x%x)\n",
                since_start(), after, (after & O_NONBLOCK) ? "ON" : "OFF", O_NONBLOCK);
    std::fflush(stdout);

    if (busy)
        run_busy(client_fd);
    else
        run_patient(client_fd);

    ::close(client_fd);
    ::close(listen_fd);
    return 0;
}
