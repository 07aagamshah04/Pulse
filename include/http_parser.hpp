#pragma once

#include "http_request.hpp"
#include <optional>
#include <string>

/* =============================================================================
 * MONTH 1 / WEEK 2 — turning raw bytes into an HttpRequest
 * -----------------------------------------------------------------------------
 * This is the actual "customs officer" from the Part 3 analogy in the Week 2
 * doc: it reads the raw envelope (request line + headers + blank line +
 * body, per Part 1's grammar) and fills out the standardized form
 * (HttpRequest).
 *
 * IMPORTANT scope note, matching the Week 2 doc exactly: this parser expects
 * to be handed a buffer that ALREADY contains a complete request (request
 * line, all headers, and the full blank-line separator). It does NOT handle
 * a request that arrived split across multiple read() calls — see Part 2 of
 * the Week 2 doc and Week 1's "IMPORTANT TCP EXPERIMENT". Making this robust
 * against partial reads is Week 3's dedicated job, on purpose.
 * ============================================================================= */
class HttpParser
{
public:
    /* Month 1 / Week 2: returns std::nullopt if `raw` isn't a well-formed
       request (bad request line, no blank-line separator found, a header
       line with no colon, ...). TcpServer turns a nullopt here into a
       400 Bad Request response — see Part 5 of the doc's status code table. */
    //    raw bytes
    //     ↓
    //     parse()
    //     ↓
    //     ┌─────────────────────┐
    //     │ valid HTTP request? │
    //     └─────────────────────┘
    //         ↓          ↓
    //         YES         NO
    //         ↓           ↓
    //     HttpRequest   std::nullopt
    static std::optional<HttpRequest> parse(const std::string &raw);
};
