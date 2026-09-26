#pragma once

#include "http_request.hpp"
#include <string>

/* =============================================================================
 * MONTH 1 / WEEK 3 — from "parsed or not" to three real outcomes
 * -----------------------------------------------------------------------------
 * Week 2's parser returned std::optional<HttpRequest> — nullopt meant
 * "couldn't parse this," full stop. That conflated two genuinely different
 * situations, which Part 3 of the Week 3 doc asked you to think through:
 *
 *   1. "This will NEVER be valid, no matter how many more bytes arrive" —
 *      a garbled request line, a header with no colon. Truly MALFORMED.
 *   2. "This is a perfectly valid request so far, it just hasn't all
 *      ARRIVED yet" — e.g. no "\r\n\r\n" found yet because the client is
 *      still sending headers, or the full body hasn't landed. INCOMPLETE.
 *
 * Week 2's single read() call meant this distinction never mattered — every
 * request was assumed complete already. Week 3 removes that assumption
 * (see tcp_server.cpp's new read loop), so the caller genuinely needs to
 * tell "keep reading" apart from "give up and respond 400" — a plain
 * std::optional can't express that third state, so this is now a small
 * three-way result type instead.
 * ============================================================================= */
enum class ParseStatus
{
    Complete,     // a full, valid HttpRequest was extracted — safe to route it
    Incomplete,   // not malformed — just needs more bytes; caller should read() again and retry
    Malformed     // never going to parse, regardless of how many more bytes arrive — caller should respond 400 now
};

struct ParseResult
{
    ParseStatus status;
    HttpRequest request;   // Month 1 / Week 3: only meaningful when status == Complete; left default-constructed (empty) otherwise — callers must check status first, never read `request` without checking
};

/* =============================================================================
 * MONTH 1 / WEEK 2 (unchanged from here down except the return type) —
 * turning raw bytes into an HttpRequest
 * -----------------------------------------------------------------------------
 * Still the "customs officer" from the Part 3 analogy in the Week 2 doc: read
 * the raw envelope (request line + headers + blank line + body, per Part 1's
 * grammar) and fill out the standardized form (HttpRequest).
 * ============================================================================= */
class HttpParser
{
public:
    /* Month 1 / Week 3: `raw` is now the ACCUMULATED buffer from
       tcp_server.cpp's read loop, not necessarily a complete request yet.
       This function is expected to be called repeatedly, with more and more
       bytes appended to `raw` each time, until it stops returning
       Incomplete — see tcp_server.cpp's handle_client() for the loop that
       does this. */
    static ParseResult parse(const std::string& raw);
};
