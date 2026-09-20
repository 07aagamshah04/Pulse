#pragma once

#include "http_util.hpp"
#include <optional>
#include <string>
#include <unordered_map>

/* =============================================================================
 * MONTH 1 / WEEK 2 — the parsed representation of an HTTP request
 * -----------------------------------------------------------------------------
 * See Part 3 of the Week 2 doc, "Designing Request and Response": this is the
 * "standardized form" the parser fills out after reading the raw envelope
 * (Part 1's request-line + headers + body grammar). Everything AFTER parsing
 * (the router, your route handlers) works with this struct, never with the
 * raw bytes directly — that separation is the entire point of having a
 * parser at all.
 *
 * This is deliberately a plain struct, not a class with private fields and
 * getters/setters: there's no invariant to protect here (any combination of
 * method/path/version/headers/body is a perfectly valid, if perhaps
 * semantically odd, HttpRequest), so a struct with public fields is the
 * honest representation — adding encapsulation machinery with nothing to
 * actually encapsulate would be exactly the kind of unnecessary abstraction
 * to avoid. It's also why this stays header-only, with no matching .cpp:
 * the one real piece of logic (header(), below) is two lines, and a .cpp
 * file for that would just be empty boilerplate.
 * ============================================================================= */
struct HttpRequest
{
    std::string method;    // e.g. "GET" — see Part 1 of the doc, the request line
    std::string path;      // e.g. "/hello"
    std::string version;   // e.g. "HTTP/1.1"

    /* Month 1 / Week 2: keys are ALWAYS stored lowercase (the parser
       lowercases every header name before inserting — see http_parser.cpp).
       Do not read this map directly by a literal name like headers["Host"]
       — it may not be there even if the client sent "Host:", because the
       parser stored it as "host". Always go through header() below, which
       lowercases the lookup key to match. */
    std::unordered_map<std::string, std::string> headers;

    std::string body;      // raw bytes after the blank line, if any (see Part 5 of the doc, Content-Length)

    /* Month 1 / Week 2: case-insensitive header lookup. Returns std::nullopt
       if the header wasn't sent at all — using std::optional here instead of
       returning an empty string means "not present" and "present but empty"
       are distinguishable, which matters for headers like Content-Length
       where "absent" and "0" are genuinely different situations. */
    std::optional<std::string> header(const std::string& name) const
    {
        auto it = headers.find(to_lower_ascii(name));
        if (it == headers.end())
        {
            return std::nullopt;
        }
        return it->second;
    }
};
