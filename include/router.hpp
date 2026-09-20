#pragma once

#include "http_request.hpp"
#include "http_response.hpp"
#include <functional>
#include <string>
#include <unordered_map>

/* =============================================================================
 * MONTH 1 / WEEK 2 — the router
 * -----------------------------------------------------------------------------
 * See Part 4 of the Week 2 doc, the "hotel switchboard" analogy: given a
 * (method, path) pair, find the right handler, or report that none exists.
 * That's the entire job — routing decides WHICH handler runs, it never
 * contains any handler's actual logic itself.
 * ============================================================================= */
class Router
{
public:
    /* Month 1 / Week 2 — C++ note: std::function<HttpResponse(const HttpRequest&)>
       is a type that can hold ANY callable with that signature — a plain
       function, a lambda, a functor. This is what lets main.cpp register
       routes using short inline lambdas (see main.cpp) instead of needing a
       named function or a class for every single route. */
    using Handler = std::function<HttpResponse(const HttpRequest&)>;

    // Month 1 / Week 2: registers `handler` to run for this exact (method, path) pair
    void add_route(std::string method, std::string path, Handler handler);

    /* Month 1 / Week 2: looks up the request's (method, path) and either
       runs the matching handler, or returns 404/405 per Part 4 and Part 5
       of the doc — implemented in router.cpp. */
    HttpResponse route(const HttpRequest& request) const;

private:
    /* Month 1 / Week 2: path -> (method -> handler). Nesting it this way
       (rather than a single map keyed on a combined "method+path" string)
       is what lets route() below distinguish "no route AT ALL for this
       path" (404) from "this path exists, but not for this method" (405)
       — see Part 5 of the doc's status code table. */
    std::unordered_map<std::string, std::unordered_map<std::string, Handler>> routes_;
};
