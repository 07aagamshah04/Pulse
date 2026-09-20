#include "router.hpp"
#include "tcp_server.hpp"

#include <cstdlib>
#include <iostream>

/* =============================================================================
 * MONTH 1 / WEEK 2 — registering routes
 * -----------------------------------------------------------------------------
 * See Part 4 of the Week 2 doc and the "WEEK 2 ROUTING" section of the
 * project spec: GET / and GET /hello return the same greeting, GET /health
 * returns "OK", and anything not registered here falls through to the
 * router's own 404/405 handling (router.cpp) — nothing extra needed here for
 * that part, it's automatic.
 *
 * WHY an anonymous namespace here — same mechanism as tcp_server.cpp's
 * throw_errno/write_all (see the comment there), but a DIFFERENT reason for
 * needing it. throw_errno/write_all live in a header-shared situation where
 * "inline" territory matters; build_router() below is defined directly in
 * this .cpp file (not a header), so it's only ever compiled once regardless
 * — there's no multiple-definition risk to solve here. The reason for the
 * anonymous namespace THIS time is purely about visibility: it gives
 * build_router() INTERNAL LINKAGE, meaning its name is never exposed to the
 * linker for other .cpp files to see or accidentally collide with. This is
 * a pure implementation detail of main.cpp — nothing else in the project
 * calls it or needs to know it exists — so hiding its name entirely is
 * good hygiene, not a requirement for correctness.
 *
 * WHAT build_router() actually does, step by step:
 *   1. `Router router;` default-constructs an EMPTY router (its internal
 *      routes_ map starts empty — see router.hpp).
 *   2. Each `router.add_route(...)` call inserts one more (method, path) ->
 *      handler entry into that map — see router.cpp.
 *   3. `return router;` hands the finished Router back to whoever called
 *      build_router() (main(), below).
 * ============================================================================= */
namespace
{
    Router build_router()
    {
        Router router;   // Month 1 / Week 2: starts with zero routes registered

        /* Month 1 / Week 2 — C++ note: these are LAMBDAS — small, inline,
           unnamed functions. `[](const HttpRequest&) { ... }` means "a
           function taking a const HttpRequest& and returning whatever the
           body returns" (HttpResponse, inferred here). The `[]` (capture
           list) is empty because these handlers don't need anything from
           their surrounding scope — they just build a fixed response. This
           is exactly what Router::Handler (a std::function) was designed
           to accept — see router.hpp. */
        router.add_route("GET", "/", [](const HttpRequest&) {
            return HttpResponse::ok("Hello from Pulse!");
        });

        router.add_route("GET", "/hello", [](const HttpRequest&) {
            return HttpResponse::ok("Hello from Pulse!");
        });

        router.add_route("GET", "/health", [](const HttpRequest&) {
            return HttpResponse::ok("OK");
        });

        /* Month 1 / Week 2 — C++ note, ties back to the &/&& discussion in
           socket.hpp: `router` has a name, so on its own it's an lvalue.
           But C++ has a special rule specifically for `return local_var;`
           statements: since the compiler knows `router` is a local variable
           about to be destroyed the instant this function ends anyway, it
           treats this return as eligible for a MOVE (or often skips moving
           entirely via an optimization called NRVO — the Router object is
           built directly in the caller's storage in the first place, no
           transfer needed at all). Either way, `router`'s contents are
           never actually copied out here — same underlying idea as
           std::move(a) in socket.hpp, just triggered automatically by the
           return statement instead of you writing std::move yourself. */
        return router;
    }
}

/* Month 1 / Week 1: this is the entry point — where the OS actually starts
   running your program. Everything else (Socket, TcpServer) is just classes
   sitting in memory until main() does something with them. */
int main(int argc, char *argv[])
{
    /* Month 1 / Week 1 — C++ note: argc = argument count, argv = argument
       values. Running `./pulse 8080` gives argc == 2: argv[0] is always the
       program's own name ("./pulse"), argv[1] is "8080". */
    if (argc != 2)
    {
        std::cerr << "Usage: " << argv[0] << " <port>\n";
        return 1; // a non-zero return from main() tells the shell "this program failed"
    }

    int port = std::atoi(argv[1]); // argv[1] is TEXT ("8080"); atoi converts that text to an actual int
    if (port <= 0 || port > 65535)
    {
        std::cerr << "Invalid port: " << argv[1] << "\n"; // 65535 is the largest value a 16-bit port number can hold — see Part 3 of the doc, "Port"
        return 1;
    }

    try
    {
        /* Month 1 / Week 1: TcpServer's constructor does socket+setsockopt+
           bind+listen (see tcp_server.cpp) and THROWS if any of them fail —
           this try/catch is what actually catches that and reports it,
           instead of the whole program crashing with no explanation. */
        /* Month 1 / Week 2: build_router() runs once, here, before the
           server starts accepting anything. Its return value is a
           TEMPORARY (an rvalue — no name, exists only for this expression)
           being passed straight into TcpServer's `Router router` by-value
           parameter — same situation as `Socket c(Socket(6))` in the &/&&
           comment block in socket.hpp: a temporary binding directly into a
           by-value parameter, no std::move needed because it's already an
           rvalue on its own. */
        TcpServer server(static_cast<uint16_t>(port), build_router());
        server.run(); // blocks forever — see tcp_server.cpp
    }
    catch (const std::exception &e)
    {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    }

    return 0; // unreachable in practice today, since run() never returns on its own — only Ctrl+C stops this program
}

/*
cmake .. and make are the two separate steps of the build process CMakeLists.txt sets up — each does a genuinely different job.

cmake .. reads CMakeLists.txt (the .. means "look one directory up" — you're inside build/, and the file lives at the project root above it) and generates the actual build files for your platform — on Linux, that means it writes out a Makefile. It does not compile anything yet. Think of it as a translator: it takes your platform-independent instructions ("build an executable called pulse from these three .cpp files, using C++17") and translates them into the platform-specific recipe (make understands Makefiles; on Windows with Visual Studio, this same CMakeLists.txt would instead generate a .sln file — same source, different output, that's the whole point of using CMake instead of writing a Makefile by hand).

make then reads that generated Makefile and does the actual work: invokes g++ on each .cpp file to produce object files, then invokes the linker to combine them into the pulse executable — this is the compiling-and-linking process from Part 1 of the Week 1 doc, actually happening now.
*/