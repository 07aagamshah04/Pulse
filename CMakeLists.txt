# =============================================================================
# MONTH 1 / WEEK 1 — Build system
# -----------------------------------------------------------------------------
# This file is NOT C++ code. It's input to a separate tool called CMake, which
# reads it and GENERATES the actual build files (Makefiles on Linux) for your
# platform. `make` then reads those generated files and runs the real compiler
# (g++) with the right flags, in the right order.
#
# Why not just call g++ directly? Because real projects have many .cpp files,
# header search paths, compiler flags, and (later) test binaries — manually
# typing one giant g++ command that lists every file gets unmanageable fast,
# and it would be a completely different command on Windows/macOS. CMake is
# the layer that manages "which files, which flags, what order" once, in a way
# that works the same everywhere.
# =============================================================================

# Minimum CMake version this file requires, and the project's name + language.
cmake_minimum_required(VERSION 3.16)
project(pulse CXX)

# Month 1 / Week 1 note: this is the C++ standard version — C++17, matching
# the "TECHNICAL CONSTRAINTS" section of the project spec (C++17 or newer).
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)   # fail loudly if the compiler can't do C++17, instead of silently using an older standard

# Generates compile_commands.json — a file some editors/tools (clangd, VS Code
# IntelliSense) read to understand your project's include paths and flags for
# things like autocomplete and jump-to-definition. Not required to build.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# -Wall -Wextra turns on extra compiler warnings beyond the bare minimum —
# e.g. an unused variable, a comparison that's always true. These are not
# errors, but on a learning project you want the compiler to point out every
# suspicious thing it notices, not stay silent.
add_compile_options(-Wall -Wextra)

# The actual thing being built: an executable named "pulse", made by compiling
# these .cpp files together and linking them into one binary. Each .cpp
# is compiled to an object file independently (see Part 1 of the Week 1 doc —
# "Compilation and linking"), then the linker combines them here.
#
# MONTH 1 / WEEK 2: http_response.cpp, http_parser.cpp, and router.cpp are
# new this week. http_request.hpp has no matching .cpp (it's header-only —
# see the comment at the top of that file for why) so it doesn't need
# listing here; headers never do, only the .cpp files that need compiling.
add_executable(pulse
    src/main.cpp
    src/socket.cpp
    src/tcp_server.cpp
    src/http_response.cpp
    src/http_parser.cpp
    src/router.cpp
)

# Tells the compiler to also look in include/ when a .cpp file writes
# #include "socket.hpp" — otherwise it would only look in the same folder
# as the .cpp file doing the including.
target_include_directories(pulse PRIVATE include)
