#pragma once

#include <cctype>
#include <string>

/* =============================================================================
 * MONTH 1 / WEEK 2 — shared helper
 * -----------------------------------------------------------------------------
 * HTTP header NAMES are case-insensitive by spec — a client sending "host:"
 * or "Host:" or "HOST:" means the same header (see Part 7 of the Week 2 doc,
 * "why must header name comparisons be case-insensitive"). Both the parser
 * (when storing a header it just read) and HttpRequest::header() (when
 * looking one up) need to agree on ONE canonical form, so this one function
 * is shared by both instead of writing the same lowercasing logic twice.
 * ============================================================================= */
/* Month 1 / Week 2 — C++ note: why "inline" here specifically. This
   function's full body lives in a header, and that header gets #include'd
   into MORE THAN ONE .cpp file (http_parser.cpp, and http_request.hpp which
   is itself included elsewhere). Without "inline", every .cpp that includes
   this header would compile its OWN separate copy of to_lower_ascii with
   normal external linkage — and when the linker combines those object
   files, it would find the same function defined more than once and refuse
   to link ("multiple definition" error). "inline" is a specific exception
   to that rule (the One Definition Rule): it tells the linker "this may
   legitimately appear in several translation units — treat every identical
   copy as one, don't complain." Note that a function defined INSIDE a
   class body (like Socket::fd() in socket.hpp) is implicitly inline
   already, which is why that one never needed the keyword spelled out —
   to_lower_ascii is a free function outside any class, so it does.

   here inline wrote as in header if giving full definition of function in header file, it will not give multiple definition error when linking or you can wrap it in class then inline not needed while in cpp file it is by default wrote
   */
inline std::string to_lower_ascii(std::string s)
{
    for (char &c : s)
    {
        /* Month 1 / Week 2 — C++ note: THREE conversions happen on this one
           line, each for a distinct reason:

           1. char -> unsigned char (explicit, static_cast<unsigned char>):
              plain `char` may be SIGNED on this platform. std::tolower has
              undefined behavior if given a negative value that isn't EOF —
              internally it uses the value as an index into a 256-entry
              table, and a negative index is out of bounds. Converting to
              unsigned char first guarantees the value lands in [0, 255],
              which is safe to pass in.

           2. unsigned char -> int (IMPLICIT — not written explicitly at
              all): std::tolower's actual parameter type is `int`, not
              `unsigned char`, so the compiler automatically promotes the
              argument on the way in. Lossless: every unsigned char value
              fits inside an int with room to spare.

           3. int -> char (explicit, static_cast<char>): std::tolower
              RETURNS an int, but we're storing the result back into `c`,
              which is a char — so it has to be narrowed back down. Safe
              here because a valid ASCII input always yields a result that
              fits in a char. */
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}
