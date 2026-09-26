#include "http_parser.hpp"
#include "http_util.hpp"

#include <sstream>
#include <vector>

namespace
{
    // Month 1 / Week 2 — unchanged: splits `text` into lines on "\r\n" (CRLF — HTTP's line ending, not just "\n")
    std::vector<std::string> split_lines(const std::string& text)
    {
        std::vector<std::string> lines;
        size_t pos = 0;
        while (true)
        {
            size_t next = text.find("\r\n", pos);
            if (next == std::string::npos)
            {
                lines.push_back(text.substr(pos));
                break;
            }
            lines.push_back(text.substr(pos, next - pos));
            pos = next + 2;   // +2 to skip past the "\r\n" itself, not land back on it
        }
        return lines;
    }
}

ParseResult HttpParser::parse(const std::string& raw)
{
    /* -------------------------------------------------------------------
     * MONTH 1 / WEEK 3 — the header-boundary check now means something
     * different than it did in Week 2. Back then, "no \r\n\r\n found" was
     * treated as outright malformed, because the whole request was assumed
     * to have arrived already in one read(). Now that tcp_server.cpp
     * accumulates across MULTIPLE read() calls (Part 3 of the Week 3 doc),
     * "no \r\n\r\n yet" just as plausibly means "the client hasn't finished
     * sending headers yet" — genuinely INCOMPLETE, not malformed. The
     * caller (tcp_server.cpp's loop) will call parse() again after reading
     * more bytes.
     * ------------------------------------------------------------------- */
    size_t header_end = raw.find("\r\n\r\n");
    if (header_end == std::string::npos)
    {
        return ParseResult{ParseStatus::Incomplete, HttpRequest{}};
    }

    std::string head = raw.substr(0, header_end);          // request line + headers, no trailing blank line
    std::string body_so_far = raw.substr(header_end + 4);   // +4 skips "\r\n\r\n" — whatever has arrived after it SO FAR (may still be a partial body)

    std::vector<std::string> lines = split_lines(head);
    if (lines.empty() || lines[0].empty())
    {
        /* Month 1 / Week 3: this IS now genuinely malformed, not
           incomplete — we already found the blank line, meaning the
           client considers its headers finished, and yet there's no
           usable request line in them. More bytes arriving later (which
           would only ever be body bytes at this point) can't fix that. */
        return ParseResult{ParseStatus::Malformed, HttpRequest{}};
    }

    /* -------------------------------------------------------------------
     * MONTH 1 / WEEK 2 — unchanged: the request line: "METHOD SP PATH SP VERSION"
     * ------------------------------------------------------------------- */
    std::istringstream request_line(lines[0]);
    std::string method, path, version;
    if (!(request_line >> method >> path >> version))
    {
        return ParseResult{ParseStatus::Malformed, HttpRequest{}};   // couldn't extract all three tokens — malformed, not incomplete: we HAVE the full headers already
    }

    if (version.rfind("HTTP/", 0) != 0)
    {
        return ParseResult{ParseStatus::Malformed, HttpRequest{}};
    }

    HttpRequest req;
    req.method = method;
    req.path = path;
    req.version = version;

    // Month 1 / Week 2 — unchanged: header lines, names lowercased before storing
    for (size_t i = 1; i < lines.size(); ++i)
    {
        const std::string& line = lines[i];
        size_t colon = line.find(':');
        if (colon == std::string::npos)
        {
            return ParseResult{ParseStatus::Malformed, HttpRequest{}};   // not a valid "Name: value" line — again, we have the full headers already, so this can't be fixed by more bytes
        }

        std::string name = line.substr(0, colon);
        std::string value = line.substr(colon + 1);

        size_t value_start = value.find_first_not_of(' ');
        value = (value_start == std::string::npos) ? "" : value.substr(value_start);

        req.headers[to_lower_ascii(name)] = value;
    }

    /* -------------------------------------------------------------------
     * MONTH 1 / WEEK 3 — the body-completeness check, the SECOND half of
     * Part 3's "two layers" in the Week 3 doc. Headers being complete
     * (we got this far) does NOT mean the body has fully arrived — that's
     * a separate question, answered by comparing how many body bytes
     * Content-Length promises against how many we actually have so far.
     * ------------------------------------------------------------------- */
    if (auto content_length = req.header("content-length"))
    {
        size_t len;
        try
        {
            len = std::stoul(*content_length);
        }
        catch (const std::exception&)
        {
            return ParseResult{ParseStatus::Malformed, HttpRequest{}};   // Content-Length wasn't a valid number — no amount of extra bytes fixes that
        }

        if (body_so_far.size() < len)
        {
            /* Month 1 / Week 3: headers are done, but the body isn't fully
               here yet — this is exactly the gap Part 3 of the doc
               describes. NOT malformed: tcp_server.cpp's loop will read()
               again and call parse() on the larger accumulated buffer. */
            return ParseResult{ParseStatus::Incomplete, HttpRequest{}};
        }

        req.body = body_so_far.substr(0, len);

        /* Month 1 / Week 3 — see Part 4 of the doc ("leftover bytes"): if
           body_so_far has MORE than `len` bytes, whatever comes after is
           deliberately ignored here — it would be the start of a next
           pipelined request, which this project doesn't support yet. A
           documented, deliberate simplification, not an oversight. */
    }

    return ParseResult{ParseStatus::Complete, std::move(req)};
}
