# Pulse — Month 2

## Concurrency, Thread-per-Connection & Server Architecture

> Month 2 builds directly on the working Month 1 HTTP server.
>
> The goal is NOT to make Pulse "production ready".
> The goal is to understand what happens when multiple clients need to be served concurrently.

---

# 1. Context

Month 1 built a:

- C++17 HTTP/1.1 server
- Linux/POSIX socket based
- single-threaded
- blocking
- HTTP parser
- HTTP router
- GET/POST support
- 404/405 handling
- partial TCP read handling
- request size limit
- basic benchmarking

Current architecture:

Client
|
v
TCP
|
v
accept()
|
v
read()
|
v
HTTP Parser
|
v
Router
|
v
Handler
|
v
HTTP Response
|
v
write()
|
v
Client

The server processes clients sequentially.

---

# 2. What Month 1's Benchmark Taught Us

Month 1 benchmark:

- ~2000 req/s at concurrency 1
- ~5400 req/s at concurrency 10
- CPU remained low
- memory remained flat
- server remained strictly sequential

The important observation is:

The server is NOT CPU-bound.

The main architectural limitation is:

    One connection
         ↓
      handle
         ↓
      finish
         ↓
    next connection

A slow client or slow handler can prevent the server from making progress on other work.

This is the problem Month 2 investigates.

---

# 3. Month 2 Goal

By the end of Month 2:

Pulse should support multiple client connections being processed concurrently using a thread-per-connection architecture.

We should understand:

    blocking I/O
          +
    multiple threads
          =
    concurrent connections

We will then study the problems created by this approach.

Those problems will motivate Month 3.

---

# 4. Important Architectural Principle

Do NOT start Month 2 by asking:

"How do I make Pulse faster?"

Instead ask:

"How can Pulse make progress on multiple connections at the same time?"

Performance measurements come after the architecture is understood.

---

# 5. New Architecture

Month 1:

                    Main Thread
                        |
                        v
                     accept()
                        |
                        v
                   handle_client()
                        |
                        v
                    read/write
                        |
                        v
                     finish

Month 2:

                    Main Thread
                        |
                        v
                     accept()
                        |
            +-----------+-----------+
            |           |           |
            v           v           v
         Thread 1    Thread 2    Thread 3
            |           |           |
            v           v           v
         Client A    Client B    Client C

Each connection can now progress independently.

---

# 6. What We Are Building

Implement a thread-per-connection server.

Conceptually:

while (server_running) {

    client_fd = accept();

    create_thread(
        handle_client,
        client_fd
    );

}

Each worker thread:

1. owns one client connection
2. reads the request
3. parses HTTP
4. invokes the router
5. writes the response
6. closes the connection
7. exits

---

# 7. Why This Architecture?

The purpose is educational.

Thread-per-connection is NOT the final architecture of Pulse.

It is useful because it makes concurrency easy to understand.

It gives us a stepping stone:

Month 1:

    sequential

        ↓

Month 2:

    thread per connection

        ↓

Month 3:

    non-blocking I/O

        ↓

Month 4:

    epoll/event loop

        ↓

Month 5:

    controlled worker pool

---

# 8. Learning Goals

By the end of Month 2 understand:

- process vs thread
- address space
- stack per thread
- shared memory
- thread creation
- thread lifecycle
- join
- detach
- mutex
- race condition
- data race
- atomic variables
- condition_variable
- thread safety
- concurrent vs parallel execution
- context switching
- thread-per-connection architecture
- why unlimited threads are problematic

---

# 9. Week 1 — Linux Threads & C++ std::thread

## Goal

Understand what a thread actually is before changing Pulse.

---

## Concepts

Study:

- process
- thread
- virtual address space
- stack
- registers
- program counter
- shared heap
- thread-local stack
- scheduler

Understand:

    Process
       |
       +---- Thread 1
       |
       +---- Thread 2
       |
       +---- Thread 3

Threads in the same process share:

- code
- heap
- global variables
- file descriptors

But each thread has its own:

- stack
- registers
- execution state

---

## C++ Concepts

Learn:

std::thread

join()

detach()

thread::joinable()

Example experiment:

Create two threads.

Thread A prints:

A

Thread B prints:

B

Observe that output order is not guaranteed.

---

## Important Question

Why doesn't this:

thread1.join();
thread2.join();

make the program single-threaded?

Understand that join() means:

"wait for this thread to finish"

It does NOT mean:

"run threads sequentially."

---

## Experiment

Create:

- 2 threads
- 4 threads
- 8 threads

Each thread performs some work.

Observe:

- scheduling
- execution order
- CPU usage

Use:

top

htop

ps -T

---

## Deliverable

Create:

Month2/Week1/

    Concepts document
    notes.txt
    small thread experiments

---

## Estimated Time

8–10 hours

---

# 10. Week 2 — Thread-per-Connection

## Goal

Modify Pulse so every accepted connection gets its own thread.

---

## Current Code

Conceptually:

client_fd = accept();

handle_client(client_fd);

This blocks until that client is completely handled.

---

## New Code

Conceptually:

client_fd = accept();

std::thread(
handle_client,
client_fd
).detach();

The accept loop can immediately return to accepting new connections.

---

# 11. Important Ownership Question

Before implementing:

Ask:

Who owns client_fd?

The worker thread should own the connection after it is handed over.

The main thread must not accidentally close it.

The worker is responsible for:

    read
    parse
    route
    write
    close

Understand this ownership carefully.

---

# 12. Test Scenario

Create a deliberately slow endpoint.

For example:

GET /slow

The handler waits several seconds.

At the same time:

GET /hello

Expected:

Month 1:

    /slow
       |
       v
    server blocked
       |
       v
    /hello waits

Month 2:

    /slow → Thread 1

    /hello → Thread 2

Therefore:

/hello can complete while /slow is still running.

This is one of the most important experiments of Month 2.

---

# 13. Observe It

Run:

curl http://localhost:8080/slow

while simultaneously:

curl http://localhost:8080/hello

Use:

ps -T -p <PID>

or:

top -H -p <PID>

Observe multiple threads.

---

# 14. Deliverable

Pulse supports:

- multiple simultaneous clients
- independent connection handling
- slow handlers without blocking every other connection

---

# 15. Estimated Time

8–12 hours

---

# 16. Week 3 — Thread Safety & Shared State

Now that multiple threads exist, a new class of problems appears.

---

# 17. Why?

Suppose two requests access:

int request_count;

Thread A:

request_count++;

Thread B:

request_count++;

It looks harmless.

But:

request_count++

is not necessarily one indivisible CPU operation.

Conceptually:

READ
MODIFY
WRITE

Two threads can interfere.

---

# 18. Race Condition

Example:

Initial:

count = 0

Thread A:

read 0

Thread B:

read 0

Thread A:

write 1

Thread B:

write 1

Expected:

2

Actual:

1

This is a race condition.

---

# 19. Build a Request Counter

Add:

total_requests

Increment it for every request.

Initially implement it without synchronization.

Run concurrent requests.

Try to observe incorrect results.

Then fix it.

---

# 20. Learn std::mutex

Understand:

std::mutex

std::lock_guard

std::unique_lock

Basic idea:

lock
|
critical section
|
unlock

Prefer RAII.

Avoid manual:

mutex.lock();
...
mutex.unlock();

unless there is a specific reason.

---

# 21. Learn std::atomic

Compare:

std::mutex

vs

std::atomic

Understand that atomic is useful when the operation can be expressed as an atomic operation.

For example:

std::atomic<uint64_t> request_count;

request_count++;

Do NOT conclude:

"atomics are always faster than mutexes."

Performance must be measured.

---

# 22. Build Shared Server Statistics

Add statistics such as:

- total requests
- total connections
- active connections
- 2xx responses
- 4xx responses
- 5xx responses

Make access thread-safe.

---

# 23. Important Design Question

Ask:

Which data belongs to each connection?

versus:

Which data belongs to the whole server?

Connection-local:

- socket fd
- request buffer
- response buffer
- parser state

Server-global:

- router
- counters
- configuration
- shutdown state

This distinction is fundamental.

---

# 24. Deliverable

Pulse should have:

- thread-safe statistics
- clear ownership
- no obvious data races
- documented shared state

---

# 25. Estimated Time

8–12 hours

---

# 26. Week 4 — Thread Limits, Stress Testing & Understanding the Trade-off

This week is NOT about adding another major feature.

The goal is understanding why thread-per-connection cannot simply scale forever.

---

# 27. Question

What happens if:

10 clients
100 clients
1,000 clients
10,000 clients

connect simultaneously?

Does Pulse create:

10 threads?
100 threads?
1,000 threads?
10,000 threads?

What happens to:

- memory?
- CPU?
- scheduler?
- context switching?
- latency?

---

# 28. Experiment

Create a load test.

Gradually increase concurrent connections.

Measure:

- requests/sec
- p50 latency
- p95 latency
- p99 latency
- CPU
- memory
- number of threads

Record results.

---

# 29. Linux Tools

Use:

ps -T

top -H

htop

pidstat -t

/proc/<pid>/status

/proc/<pid>/task/

---

# 30. Thread Explosion Experiment

Do NOT intentionally create an extreme workload that can destabilize your machine.

Stay within reasonable limits.

Observe how resource usage changes as thread count increases.

---

# 31. Context Switching

Learn why threads are not free.

The scheduler may need to switch:

Thread A
↓
Thread B
↓
Thread C
↓
Thread A

This involves scheduler work and CPU state changes.

Understand:

More threads ≠ automatically more performance.

---

# 32. Benchmark Month 1 vs Month 2

Repeat the Month 1 benchmark.

Record:

                    Month 1      Month 2

Concurrency 1

Concurrency 10

Concurrency 50

Concurrency 100

For each:

- throughput
- p50
- p95
- p99
- CPU
- memory
- thread count

---

# 33. Do Not Chase Numbers

The goal is NOT:

"Month 2 must have 100,000 req/s."

The goal is:

"I understand why the numbers changed."

---

# 34. Important Experiments

## Experiment 1 — Slow Handler

Compare:

Month 1:

/slow blocks /hello

Month 2:

/slow does not block /hello

---

## Experiment 2 — Concurrent Requests

Send many simultaneous requests.

Observe multiple threads.

---

## Experiment 3 — Shared Counter

Implement:

unsafe counter

Then:

mutex counter

Then:

atomic counter

Compare correctness first.

Only benchmark performance afterward.

---

## Experiment 4 — Thread Count

Increase concurrent clients.

Observe:

thread count
memory
CPU
latency

---

# 35. Month 2 Architecture

Final Month 2 architecture:

                         Main Thread
                              |
                              v
                           accept()
                              |
              +---------------+---------------+
              |               |               |
              v               v               v
           Thread 1        Thread 2        Thread 3
              |               |               |
              v               v               v
           Client A        Client B        Client C
              |               |               |
              v               v               v
         HTTP Parser     HTTP Parser     HTTP Parser
              |               |               |
              v               v               v
            Router          Router          Router
              |               |               |
              v               v               v
           Handler          Handler         Handler
              |               |               |
              +---------------+---------------+
                              |
                              v
                       Shared Statistics
                              |
                     mutex / atomic
