/* =============================================================================
 * MONTH 3 / WEEK 1 — Experiment A (blocking side) + Experiment C (EINTR)
 * -----------------------------------------------------------------------------
 * Pulse question: what EXACTLY happens inside Pulse's handle_client() when it
 * calls read() and the client hasn't sent anything yet? (src/tcp_server.cpp,
 * the `::read(client.fd(), chunk, sizeof(chunk))` line in handle_client.)
 *
 * This program is that one line, on its own, with timestamps around it:
 *   - accept ONE client on port 9090 (Pulse uses 8080, so both can run)
 *   - call read() on it — a normal, BLOCKING socket, exactly like Pulse
 *   - print how long read() kept the thread waiting, and what it returned
 *   - repeat until the client closes (read() returns 0)
 *
 * Usage:  ./blocking.out [plain|eintr|restart]
 *   plain   (default) no signal handler. `kill -USR1 <pid>` just kills it
 *           (that's SIGUSR1's default action).
 *   eintr   a SIGUSR1 handler WITHOUT SA_RESTART. `kill -USR1 <pid>` while
 *           we're inside read() makes read() return -1 with errno = EINTR.
 *   restart a SIGUSR1 handler WITH SA_RESTART. Same signal, but the kernel
 *           quietly restarts read() for us — we never see EINTR.
 *
 * Client:  nc localhost 9090      (type a line + Enter to send; Ctrl+D = close)
 *
 * Build:
 *   g++ -std=c++17 -Wall -Wextra Month3/Week1/blocking_socket_experiment.cpp -o Month3/Week1/blocking.out
 * ============================================================================= */

#include <arpa/inet.h>  // htons
#include <cerrno>       // errno, EINTR
#include <chrono>       // std::chrono::steady_clock — timing how long read() waited
#include <csignal>      // sigaction, SIGUSR1, sig_atomic_t
#include <cstdio>       // std::printf, std::perror
#include <cstdlib>      // std::exit
#include <cstring>      // std::strcmp, std::strerror
#include <fcntl.h>      // fcntl, O_NONBLOCK — only used to PRINT that it's off here
#include <netinet/in.h> // sockaddr_in
#include <string>       // std::string
#include <sys/socket.h> // socket, bind, listen, accept, setsockopt
#include <unistd.h>     // read, close, getpid

namespace
{
    constexpr uint16_t kPort = 9090;

    using Clock = std::chrono::steady_clock;
    Clock::time_point g_start; // set right after accept(); every printed time is "seconds since the client connected"

    double since_start()
    {
        return std::chrono::duration<double>(Clock::now() - g_start).count();
    }

    /* MONTH 3 / WEEK 1 — the same socket()/setsockopt()/bind()/listen()
       sequence as TcpServer's constructor (Month 1 / Week 1). Copied here so
       this experiment is one standalone file. On failure we just exit —
       it's an experiment, not a server. */
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

    /* Print received bytes on one line, with \r and \n made visible, so you
       can see exactly what one read() handed back. */
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

    /* MONTH 3 / WEEK 1 — Experiment C. A signal handler runs IN THE MIDDLE
       of whatever the thread was doing — here, in the middle of read(). So it
       may only do "async-signal-safe" things: no printf, no malloc, no locks.
       Setting a `volatile sig_atomic_t` is the classic safe thing to do.
         sig_atomic_t = an integer type the CPU writes in one step, so main()
                        never sees half of an update
         volatile     = "this can change behind the compiler's back — re-read
                        it from memory every time, don't cache it" */
    volatile sig_atomic_t g_signals = 0;
    void on_sigusr1(int) { g_signals = g_signals + 1; }
}

int main(int argc, char *argv[])
{
    const char *mode = argc > 1 ? argv[1] : "plain";
    const bool eintr = std::strcmp(mode, "eintr") == 0;
    const bool restart = std::strcmp(mode, "restart") == 0;
    if (!eintr && !restart && std::strcmp(mode, "plain") != 0)
    {
        std::printf("usage: %s [plain|eintr|restart]\n", argv[0]);
        return 1;
    }

    /* MONTH 3 / WEEK 1 — sigaction(): "when SIGUSR1 arrives, run on_sigusr1".
         sa_flags = 0          -> a blocking read() that gets interrupted
                                  returns -1, errno = EINTR. YOUR code must
                                  notice and call read() again.
         sa_flags = SA_RESTART -> the kernel calls read() again for you;
                                  your code never sees EINTR.
       In "plain" mode we install nothing, so SIGUSR1 does its default:
       terminate the process. */
    if (eintr || restart)
    {
        struct sigaction sa{};
        sa.sa_handler = on_sigusr1;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = restart ? SA_RESTART : 0;
        ::sigaction(SIGUSR1, &sa, nullptr);
    }

    std::printf("PID %d, mode: %s\n", ::getpid(), mode);
    if (eintr)   std::printf("  SIGUSR1 handler installed WITHOUT SA_RESTART -> try: kill -USR1 %d\n", ::getpid());
    if (restart) std::printf("  SIGUSR1 handler installed WITH SA_RESTART    -> try: kill -USR1 %d\n", ::getpid());

    int listen_fd = make_listener(kPort);
    std::printf("Waiting in accept() on port %u ... connect with:  nc localhost %u\n", kPort, kPort);
    std::fflush(stdout);

    int client_fd = ::accept(listen_fd, nullptr, nullptr);
    if (client_fd == -1) { std::perror("accept"); return 1; }
    g_start = Clock::now();

    /* MONTH 3 / WEEK 1 — just PRINTING the flag here, not changing it. A
       socket fresh out of accept() is blocking — the same as every
       client_fd Pulse has ever had. */
    int flags = ::fcntl(client_fd, F_GETFL);
    std::printf("[%7.3f s] accepted client, fd %d. O_NONBLOCK is %s -> read() will WAIT for data.\n",
                since_start(), client_fd, (flags & O_NONBLOCK) ? "ON" : "OFF");

    char buf[4096];
    for (;;)
    {
        std::printf("[%7.3f s] calling read() ... (if nothing has arrived, this thread now sleeps INSIDE read())\n", since_start());
        std::fflush(stdout); // make sure the line is visible BEFORE we block

        Clock::time_point before = Clock::now();
        ssize_t n = ::read(client_fd, buf, sizeof(buf));
        int err = errno; // save it IMMEDIATELY: the printf below could change errno
        double waited = std::chrono::duration<double>(Clock::now() - before).count();

        if (n > 0)
        {
            std::printf("[%7.3f s] read() returned %zd after waiting %.3f s: \"%s\"",
                        since_start(), n, waited, visible(buf, n).c_str());
            if (restart && g_signals > 0)
                std::printf("  (signals received so far: %d — read() was restarted, we never saw EINTR)", static_cast<int>(g_signals));
            std::printf("\n");
        }
        else if (n == 0)
        {
            std::printf("[%7.3f s] read() returned 0 after waiting %.3f s: the client closed its side (EOF). Done.\n",
                        since_start(), waited);
            break;
        }
        else if (err == EINTR)
        {
            /* MONTH 3 / WEEK 1 — Experiment C. Not a connection error: the
               connection is fine, a signal just woke us up early. The right
               reaction is "try again", which the loop does. Pulse's
               handle_client() today would log "read: Interrupted system
               call" and DROP the connection here — see the concepts doc. */
            std::printf("[%7.3f s] read() returned -1, errno = EINTR (%d, \"%s\") after %.3f s. "
                        "A signal interrupted the wait. Connection is fine -> calling read() again.\n",
                        since_start(), err, std::strerror(err), waited);
        }
        else
        {
            std::printf("[%7.3f s] read() returned -1, errno = %d (\"%s\"): a real error. Giving up.\n",
                        since_start(), err, std::strerror(err));
            break;
        }
    }

    ::close(client_fd);
    ::close(listen_fd);
    return 0;
}
