# Pulse

A raw-socket HTTP server built from scratch in C++ on Linux/POSIX, as a systems-programming learning project. See `CLAUDE.md — Pulse Month 1.md` for the project's goals and ground rules, and `Month1/` for that month's concept docs.

## Build

```bash
mkdir -p build && cd build
cmake ..
make
```

- `cmake ..` reads `CMakeLists.txt` (the `..` = "one directory up," since you're inside `build/`) and **generates** the actual platform build files — a `Makefile`, on Linux. It does not compile anything itself; it's a translator from CMake's platform-independent instructions to your platform's real build tool.
- `make` reads that generated `Makefile` and does the real work: runs `g++` on each `.cpp` to produce object files, then links them into the `pulse` executable.
- Building inside a separate `build/` folder (instead of the project root) is called an "out-of-source build" — everything generated lands in one folder you can wipe entirely with `rm -rf build` without touching any actual source code. That's why `.gitignore` excludes it.

## Run

```bash
./build/pulse 8080
```

## Test

```bash
nc localhost 8080
```

Type something and press enter — the server echoes it back.

Inspect it while running:

```bash
ss -ltnp
ls -l /proc/<PID>/fd
```
