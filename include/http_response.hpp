#pragma once

#include <string>
#include <utility>
#include <vector>

/* =============================================================================
 * MONTH 1 / WEEK 2 — building an HTTP response
 * -----------------------------------------------------------------------------
 * The mirror image of HttpRequest: a status code, headers, and a body, which
 * to_string() serializes back into the exact text format from Part 5 of the
 * Week 2 doc (status line, headers, blank line, body) — ready to hand
 * straight to write(). See the "customs officer" analogy in the doc: this
 * class is what re-seals the reply into a properly formatted envelope before
 * it goes back out over the same TCP connection Week 1 built.
 * ============================================================================= */
class HttpResponse
{
public:
    /* Month 1 / Week 2: content_type is only attached as a header when the
       body is non-empty — an empty-bodied response (404, 405) has nothing
       to describe the type of. Content-Length, by contrast, is ALWAYS
       written by to_string() below, even as "0" — see Part 5 of the doc for
       why Content-Length is never optional, even for an empty body. */
    HttpResponse(int status_code, std::string reason_phrase, std::string body = "",
                 std::string content_type = "text/plain")
        : status_code_(status_code), reason_phrase_(std::move(reason_phrase)), body_(std::move(body))
    {
        if (!body_.empty())
        {
            headers_.emplace_back("Content-Type", std::move(content_type));
        }
    }

    /* Month 1 / Week 2 — C++ note: these are STATIC factory functions, not
       constructors. Why not just make more constructor overloads? Because
       `HttpResponse::not_found()` reads, at the call site, as exactly what
       it means — a constructor call like `HttpResponse(404, "Not Found")`
       requires the reader to already know 404 means Not Found. Factories
       also mean the status-code/reason-phrase pairing lives in ONE place
       (here) instead of being retyped correctly at every call site in
       router.cpp/main.cpp. */
    static HttpResponse ok(std::string body, std::string content_type = "text/plain")
    {
        return HttpResponse(200, "OK", std::move(body), std::move(content_type));
    }

    static HttpResponse bad_request()
    {
        return HttpResponse(400, "Bad Request");
    }

    static HttpResponse not_found()
    {
        return HttpResponse(404, "Not Found");
    }

    static HttpResponse method_not_allowed()
    {
        return HttpResponse(405, "Method Not Allowed");
    }

    /* MONTH 2 / WEEK 3 — 500 Internal Server Error: "the SERVER broke while
       handling a request that was itself fine" (unlike 4xx = "your request
       was the problem"). Pulse never sent one before Week 3: a handler that
       threw just had its connection dropped by serve_connection(), so the
       client got nothing at all. Now handle_client() catches a throwing
       handler and answers with this instead — a real reply for the client,
       and something for the 5xx counter to count (Week 3 doc, Part 8). */
    static HttpResponse internal_server_error()
    {
        return HttpResponse(500, "Internal Server Error");
    }

    /* MONTH 2 / WEEK 3: read-only access to the status code, so
       handle_client() can tell ServerStats whether this was a 2xx/4xx/5xx.
       status_code_ itself stays private — callers can read it, never change
       it. C++ note: `const` after the () = calling this doesn't modify the
       response (same meaning as Socket::fd() const). */
    int status_code() const
    {
        return status_code_;
    }

    void add_header(std::string name, std::string value)
    {
        headers_.emplace_back(std::move(name), std::move(value));
    }

    // Month 1 / Week 2: serializes this object back into the exact wire format from Part 5 of the doc — implemented in http_response.cpp
    std::string to_string() const;

private:
    int status_code_;
    std::string reason_phrase_;
    std::vector<std::pair<std::string, std::string>> headers_;   // Month 1 / Week 2: a vector of pairs, not a map — response headers must be written out in a predictable order, and we don't need lookups on this side, only iteration
    std::string body_;
};
