#include "http_parser.hpp"
#include "http_util.hpp"

#include <sstream>
#include <vector>

namespace
{
    /* Month 1 / Week 2: splits `text` into lines on "\r\n" — note CRLF, not
       just "\n". HTTP's grammar (Part 1 of the doc) uses CRLF as the line
       ending, even though this whole project runs on Linux, where a plain
       "\n" would be the native line ending for everything else. This is a
       protocol rule, not an OS rule — HTTP was standardized this way before
       Linux existed, and every HTTP implementation still follows it today. */
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

std::optional<HttpRequest> HttpParser::parse(const std::string& raw)
{
    /* -------------------------------------------------------------------
     * MONTH 1 / WEEK 2 — find the blank line
     * "\r\n\r\n" is an empty line (\r\n) immediately following the end of
     * the previous line (\r\n) — exactly the "one blank line ends the
     * headers" rule from Part 1 of the doc. Everything before this marker
     * is the request line + headers; everything after it is the body.
     * ------------------------------------------------------------------- */
    size_t header_end = raw.find("\r\n\r\n");
    if (header_end == std::string::npos)
    {
        /* Month 1 / Week 2: no blank line found at all. Per this parser's
           documented scope (see http_parser.hpp), we treat this as a
           malformed request rather than "maybe it just hasn't all arrived
           yet" — that second possibility is Week 3's problem to solve, not
           this parser's. */
        return std::nullopt;
    }

    std::string head = raw.substr(0, header_end);         // request line + headers, no trailing blank line
    std::string body_so_far = raw.substr(header_end + 4);  // +4 skips "\r\n\r\n" — whatever came after it in this same read()

    std::vector<std::string> lines = split_lines(head);
    if (lines.empty() || lines[0].empty())
    {
        return std::nullopt;
    }

    /* -------------------------------------------------------------------
     * MONTH 1 / WEEK 2 — the request line: "METHOD SP PATH SP VERSION"
     * std::istringstream's >> operator splits on whitespace automatically,
     * which is a convenient way to pull exactly three space-separated
     * tokens without manually searching for space characters.
     * ------------------------------------------------------------------- */
    std::istringstream request_line(lines[0]);
    std::string method, path, version;
    if (!(request_line >> method >> path >> version))
    {
        return std::nullopt;   // couldn't extract all three tokens — malformed request line
    }

    // Month 1 / Week 2: a minimal sanity check — a real HTTP version always starts with "HTTP/" (e.g. "HTTP/1.1")
    if (version.rfind("HTTP/", 0) != 0)
    {
        return std::nullopt;
    }

    HttpRequest req;
    req.method = method;
    req.path = path;
    req.version = version;

    /* -------------------------------------------------------------------
     * MONTH 1 / WEEK 2 — header lines
     * Each remaining line must be "Name: value". Names are lowercased
     * before storing — see http_util.hpp and Part 7 of the doc, "why must
     * header name comparisons be case-insensitive."
     * ------------------------------------------------------------------- */
    for (size_t i = 1; i < lines.size(); ++i)
    {
        const std::string& line = lines[i];
        size_t colon = line.find(':');
        if (colon == std::string::npos)
        {
            return std::nullopt;   // not a valid "Name: value" line
        }

        std::string name = line.substr(0, colon);
        std::string value = line.substr(colon + 1);

        // Month 1 / Week 2: trim a single leading space, e.g. "Host: localhost" -> value starts as " localhost", strip the leading space
        size_t value_start = value.find_first_not_of(' ');
        value = (value_start == std::string::npos) ? "" : value.substr(value_start);

        req.headers[to_lower_ascii(name)] = value;
    }

    /* -------------------------------------------------------------------
     * MONTH 1 / WEEK 2 — the body, if Content-Length says there is one
     * See Part 5 of the doc: the length is ANNOUNCED, not detected, because
     * TCP has no built-in "end of body" marker (Week 1, Part 3). We only
     * take what we already have from this one read() — same single-read
     * scope limitation as everywhere else in Week 1/2; Week 3 handles
     * reading more if the body arrived split across multiple read() calls.
     * ------------------------------------------------------------------- */
    if (auto content_length = req.header("content-length"))
    {
        try
        {
            size_t len = std::stoul(*content_length);
            req.body = body_so_far.substr(0, std::min(len, body_so_far.size()));
        }
        catch (const std::exception&)
        {
            return std::nullopt;   // Content-Length wasn't a valid number — malformed request
        }
    }

    return req;
}
