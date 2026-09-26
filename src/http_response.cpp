#include "http_response.hpp"

#include <sstream>

std::string HttpResponse::to_string() const
{
    /* Month 1 / Week 2 — C++ note: std::ostringstream lets us build up a
       string piece by piece with the same << syntax as std::cout, instead
       of repeated string concatenation (which would reallocate repeatedly).
       .str() at the end pulls out the finished std::string. */
    std::ostringstream out;

    /* -------------------------------------------------------------------
     * MONTH 1 / WEEK 2 — the status line
     * Always responds as HTTP/1.1 regardless of what the client sent —
     * see Part 5 of the doc for the exact grammar this is producing:
     * "HTTP/1.1 200 OK\r\n".
     * ------------------------------------------------------------------- */
    out << "HTTP/1.1 " << status_code_ << " " << reason_phrase_ << "\r\n";

    // Month 1 / Week 2: each header exactly as Part 1's grammar requires — "Name: value\r\n"
    for (const auto& [name, value] : headers_)
    {
        out << name << ": " << value << "\r\n";
    }

    /* -------------------------------------------------------------------
     * MONTH 1 / WEEK 2 — Content-Length
     * Written unconditionally, even for an empty body ("Content-Length: 0")
     * — see Part 5 of the doc's 404 example, which shows exactly this. This
     * is what lets a client (or a future keep-alive connection) know
     * precisely how many body bytes to read, since TCP itself has no
     * concept of "end of message" (Week 1, Part 3).
     * ------------------------------------------------------------------- */
    out << "Content-Length: " << body_.size() << "\r\n";

    /* -------------------------------------------------------------------
     * MONTH 1 / WEEK 3 — Connection: close
     * See Part 6 of the Week 3 doc: this server has behaved like
     * Connection: close since Week 1 (TcpServer::handle_client closes the
     * socket after exactly one request — see tcp_server.cpp) — it just
     * never SAID so explicitly. Sending this header makes that behavior
     * honest instead of accidental: any real client now knows not to
     * expect this connection to stay open for a second request, instead
     * of finding out the hard way.
     * ------------------------------------------------------------------- */
    out << "Connection: close\r\n";

    out << "\r\n";   // Month 1 / Week 2: the blank line — same rule as requests, this is what separates headers from body
    out << body_;

    return out.str();
}
