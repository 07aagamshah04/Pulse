#include "router.hpp"

void Router::add_route(std::string method, std::string path, Handler handler)
{
    // Month 1 / Week 2: routes_[path] default-constructs an empty inner map the first time a given path is registered, then add_route just inserts into it
    routes_[std::move(path)][std::move(method)] = std::move(handler);
}

HttpResponse Router::route(const HttpRequest& request) const
{
    /* -------------------------------------------------------------------
     * MONTH 1 / WEEK 2 — the actual routing decision
     * See Part 4 (routing) and Part 5 (status codes) of the Week 2 doc.
     * ------------------------------------------------------------------- */
    auto path_it = routes_.find(request.path);
    if (path_it == routes_.end())
    {
        return HttpResponse::not_found();   // nobody registered this path at all -> 404
    }

    const auto& methods_for_path = path_it->second;
    auto method_it = methods_for_path.find(request.method);
    if (method_it == methods_for_path.end())
    {
        return HttpResponse::method_not_allowed();   // the path exists, but not for this method -> 405
    }

    return method_it->second(request);   // Month 1 / Week 2: actually calls the registered handler (a lambda from main.cpp) and returns whatever HttpResponse it builds
}
