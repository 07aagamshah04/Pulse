#include "tcp_server.hpp"

#include <cstdlib>
#include <iostream>

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
        TcpServer server(static_cast<uint16_t>(port));
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