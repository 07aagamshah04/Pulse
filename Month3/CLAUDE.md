# Pulse — Month 3
## Non-Blocking I/O, Connection State Machines & I/O Multiplexing

**Duration:** 4 weeks  
**Language:** C++17  
**Platform:** WSL2 Ubuntu  
**Build:** CMake  
**Networking:** Linux/POSIX sockets  
**Previous milestone:** Month 2 — Thread-per-Connection Server

---

# 1. Project Context

Pulse is a learning-oriented HTTP/1.1 server written from scratch using Linux/POSIX socket APIs.

The purpose is to understand the systems underneath frameworks such as Go's `net/http`, rather than simply use an existing networking framework.

## Month 1 — Single-threaded blocking server (complete)

Built:

- RAII move-only Socket wrapper.
- TCP socket lifecycle: `socket()`, `bind()`, `listen()`, `accept()`, `read()`, `write()`, `close()`.
- HTTP/1.1 request parser and response serializer.
- Router matching `(method, path)`.
- GET and POST support.
- Partial TCP read handling.
- Request size limit.
- HTTP error handling.

Main limitation:

A slow or silent client blocks the entire server because the server waits inside blocking I/O.

## Month 2 — Thread-per-connection server (complete)

Changed the architecture:

- Main thread accepts connections.
- Every accepted connection gets its own worker thread.
- Worker owns its socket through the move-only RAII Socket wrapper.
- Added thread-safe server statistics.
- Added mutex and atomic experiments.
- Added exception handling and thread-safe logging.
- Tested slow clients, concurrent clients and resource consumption.
- Used ThreadSanitizer to investigate data races.

Important findings:

- Month 2 successfully handles slow clients without blocking unrelated clients.
- Each connection consumes a thread, file descriptor and memory.
- A held-open connection costs approximately 49 KB of real memory in the measured environment.
- At 2,000 idle connections, memory consumption was approximately 98 MB.
- The open-file limit was the first hard limit encountered.
- Thread creation and scheduling introduce overhead.

Month 2 benchmark showed that the thread-per-connection model was not automatically faster for small requests.

**Main conclusion:**

Concurrency solved the responsiveness problem, but introduced a resource cost per connection.

---

# 2. Month 3 Goal

Build a server that can handle multiple connections without blocking on an individual client and without creating one thread per connection.

We will introduce:

1. Non-blocking sockets.
2. `O_NONBLOCK`.
3. `EAGAIN` and `EWOULDBLOCK`.
4. Per-connection state.
5. Input and output buffers.
6. Connection state machines.
7. I/O multiplexing with `poll()`.
8. A comparison with `select()`.

By the end of Month 3, Pulse should use a single event-processing thread to manage many non-blocking connections.

We are not implementing `epoll()` yet.

---

# 3. The Core Problem We Are Solving

## Month 2 architecture

Main thread:

    accept()
       |
       +---- Thread 1 ---- Client A
       |
       +---- Thread 2 ---- Client B
       |
       +---- Thread 3 ---- Client C
       |
       +---- Thread 4 ---- Client D

Every client has its own thread.

If a client connects and never sends a request, its thread remains blocked in `read()`.

Month 2 handles this situation better than Month 1, but it consumes one thread per connection.

## Month 3 architecture

Instead:

    Main Event Loop
          |
          +---- Client A
          |
          +---- Client B
          |
          +---- Client C
          |
          +---- Client D

One thread manages multiple connections.

But how?

The answer involves two concepts:

**Non-blocking I/O:** A socket operation should not make the thread wait indefinitely.

**I/O multiplexing:** The operating system tells us which sockets are ready for an operation.

We will learn these separately before combining them.

---

# 4. Learning Goals

By the end of Month 3, understand:

## Socket fundamentals

- File descriptor.
- Blocking vs non-blocking socket.
- `fcntl()`.
- `O_NONBLOCK`.
- `EAGAIN`.
- `EWOULDBLOCK`.
- `EINTR`.
- Partial reads.
- Partial writes.
- Socket readiness.

## Connection management

- Per-connection state.
- Input buffer.
- Output buffer.
- Connection lifecycle.
- State machine.
- Connection cleanup.
- Client disconnects.

## I/O multiplexing

- Why checking every socket manually is inefficient.
- `select()`.
- `poll()`.
- `pollfd`.
- `POLLIN`.
- `POLLOUT`.
- `POLLERR`.
- `POLLHUP`.
- Timeout handling.

## Architecture

- Single-threaded event processing.
- Event loop.
- Readiness notification.
- Connection state management.
- Why non-blocking I/O and I/O multiplexing are complementary.

---

# 5. Month 3 Ground Rules

1. Continue using C++17.
2. Continue using POSIX/Linux socket APIs.
3. Do not use Boost.Asio, libuv or other networking frameworks.
4. Do not introduce threads in the Month 3 event-loop implementation.
5. Preserve the Month 2 implementation for comparison.
6. Keep the existing HTTP parser and router wherever practical.
7. Do not rewrite the entire project unnecessarily.
8. Every major feature must have a test and an experiment.
9. Write predictions before running benchmarks.
10. Do not optimize just to increase requests per second.

## Important scope boundary

Month 3 will introduce `poll()` and study `select()`.

Month 4 will introduce `epoll()`.

This distinction is intentional.

We first need to understand what readiness notification means before learning Linux's more scalable event notification mechanism.

---

# 6. Week 1 — Non-Blocking Sockets

**Estimated time: 8–10 hours**

## Goal

Understand how to make socket operations return without waiting indefinitely.

Do not redesign the entire server this week.

Start with small experiments.

## Concepts

### 1. Blocking I/O

In Month 2, a worker thread might execute:

    read(client_fd, buffer, sizeof(buffer));

If the client has not sent data, the call can wait.

The thread remains blocked until data arrives, the connection closes, an error occurs, or it is interrupted.

### 2. Non-blocking I/O

With `O_NONBLOCK`, the behavior changes.

If there is no data currently available, `read()` returns immediately with an error indication and `errno` set to `EAGAIN` or `EWOULDBLOCK`.

The thread can then perform other work.

### 3. fcntl()

Learn:

    fcntl(fd, F_GETFL)
    fcntl(fd, F_SETFL, flags | O_NONBLOCK)

Understand:

- What file status flags are.
- Why we retrieve existing flags first.
- Why we should preserve existing flags.
- How non-blocking mode affects socket operations.

### 4. Important return values

| Result | Meaning |
|---|---|
| Positive number | Number of bytes read |
| 0 | Peer has performed an orderly shutdown of its sending direction |
| -1 with EAGAIN/EWOULDBLOCK | No data currently available |
| -1 with EINTR | System call interrupted by a signal |
| Other errors | Actual socket or system error |

Do not treat every `read() == -1` as a fatal connection error.

### 5. Important distinction

Non-blocking does not mean asynchronous.

It means the system call does not wait for the operation to become possible.

The application still has to decide when to retry.

## What to build

Create a standalone experiment:

`nonblocking_socket_experiment.cpp`

Steps:

1. Create a TCP server.
2. Accept one client.
3. Set the accepted socket to non-blocking.
4. Call `read()` before the client sends anything.
5. Observe the return value and `errno`.
6. Send data from the client.
7. Call `read()` again.
8. Observe the difference.

Then create another experiment comparing blocking and non-blocking behavior.

Do not modify Pulse's main architecture yet.

## Experiments

### Experiment A — Blocking vs non-blocking

Prediction to write first:

What happens if the client connects but sends nothing?

Compare the behavior.

### Experiment B — CPU behavior

Run a non-blocking read in a loop without waiting.

Observe CPU usage.

Important question:

If non-blocking operations return immediately, what happens if we continuously retry them?

This introduces the concept of busy polling.

### Experiment C — Error handling

Observe:

- `EAGAIN`.
- `EWOULDBLOCK`.
- `EINTR`.

Understand why they are not all fatal errors.

## Deliverables

- Standalone blocking socket experiment.
- Standalone non-blocking socket experiment.
- Concepts document.
- Notes containing predictions and actual observations.
- Explanation of `O_NONBLOCK` and `EAGAIN`.

## Success criteria

I can explain:

1. What blocking I/O means.
2. What non-blocking I/O changes.
3. What `O_NONBLOCK` does.
4. Why `EAGAIN` is not necessarily a failure.
5. Why busy polling can waste CPU.
6. Why non-blocking I/O alone does not automatically solve connection management.

---

# 7. Week 2 — Per-Connection State Machines

**Estimated time: 8–12 hours**

## Goal

Learn how to manage multiple incomplete HTTP requests without blocking on any particular connection.

This is the most important design change of Month 3.

## The problem

Imagine three clients:

Client A:

    GET /hello HTTP/1.1

Client B:

    Connected, but sends nothing.

Client C:

    GET /hel

All three connections have different progress.

A single thread must remember where each client is.

We cannot simply call `read()` and assume the complete request will arrive.

## Concepts

### 1. Per-connection state

Every client needs its own state.

Conceptually:

    Connection
       |
       +-- socket
       |
       +-- input buffer
       |
       +-- output buffer
       |
       +-- parser state
       |
       +-- connection state

### 2. State machine

A connection moves through states.

For example:

    READING_REQUEST
           |
           v
    REQUEST_COMPLETE
           |
           v
    PROCESSING_REQUEST
           |
           v
    WRITING_RESPONSE
           |
           v
       CLOSED

The exact representation is an implementation decision.

The important point is that the server remembers what needs to happen next.

### 3. Input buffering

Client sends:

    GET /hel

Later:

    lo HTTP/1.1\r\n
    Host: localhost\r\n
    \r\n

The server accumulates bytes.

It must not assume one `read()` equals one HTTP request.

Reuse the Month 1 parser's three-way result:

- Complete.
- Incomplete.
- Malformed.

### 4. Output buffering

Suppose the response is 10 KB.

A `write()` call may send only part of it.

The server must remember:

- Which bytes have already been sent.
- Which bytes remain.
- Whether the socket is ready to accept more output.

This is the purpose of an output buffer and write offset.

## What to build

Introduce a `Connection` abstraction.

Suggested conceptual members:

    Connection
    - Socket
    - input_buffer
    - output_buffer
    - bytes_written
    - connection_state
    - last_activity (optional)

The exact design should be decided after studying the current Pulse code.

Implement:

1. Non-blocking accepted sockets.
2. Per-connection input buffers.
3. Per-connection output buffers.
4. Request parsing without waiting for a complete request.
5. Response generation.
6. Partial-write handling.
7. Connection cleanup.

Initially, a temporary loop or manually controlled connection processing can be used to test state transitions.

Do not introduce `poll()` yet.

## Experiments

### Experiment A — Silent client

Open a connection and send nothing.

Then connect another client and send:

    GET /hello

Prediction:

Will the second client receive a response?

Compare Month 2 and Month 3.

### Experiment B — Fragmented HTTP request

Send:

    GET /hel

Wait.

Then send:

    lo HTTP/1.1\r\nHost: localhost\r\n\r\n

Verify that the parser eventually receives the complete request.

### Experiment C — Multiple incomplete clients

Create several clients.

Some send nothing.

Some send half a request.

Some send complete requests.

Verify that each connection retains its own state.

### Experiment D — Partial writes

Use a sufficiently large response or controlled socket-buffer conditions.

Verify that the output buffer retains unsent bytes.

Do not assume partial writes will happen consistently without controlling the experiment.

## Deliverables

- `Connection` abstraction.
- Connection state machine.
- Input and output buffers.
- Correct partial-read handling.
- Correct partial-write handling.
- Concepts document.
- Test log with predictions and results.

## Success criteria

I can explain:

1. Why every connection needs independent state.
2. Why the server cannot assume a complete request arrives in one read.
3. Why output buffers are necessary.
4. What a connection state machine does.
5. How the server remembers what remains to be read or written.

---

# 8. Week 3 — poll() and the Event Loop

**Estimated time: 10–12 hours**

## Goal

Stop manually checking every connection.

Use `poll()` to let Linux tell the application which sockets are ready.

## The problem before poll()

Suppose we have:

    listening_fd
    client_fd_1
    client_fd_2
    client_fd_3
    client_fd_4

We need to know:

- Which client has sent data?
- Which client can accept output?
- Which client has disconnected?
- Is a new connection waiting?

If we continuously call non-blocking `read()` on every socket, we waste CPU.

This is busy polling.

## Concepts

### 1. I/O multiplexing

I/O multiplexing lets one thread monitor multiple file descriptors.

Instead of repeatedly checking each socket, we ask the operating system to wait until relevant sockets become ready.

### 2. poll()

Learn:

    poll()

And:

    struct pollfd

Important fields:

- fd
- events
- revents

Important event flags:

| Flag | Meaning |
|---|---|
| POLLIN | Data or an event is available for reading |
| POLLOUT | Writing is currently possible |
| POLLERR | Error condition |
| POLLHUP | Hangup condition |
| POLLNVAL | Invalid file descriptor |

Understand that readiness does not guarantee that an entire HTTP request can be read or an entire response can be written.

### 3. Event loop

Conceptually:

    while (server_running) {

        poll(...)

        for each ready descriptor:

            if listening socket:
                accept new connection

            if client readable:
                read available bytes

            if client writable:
                write pending bytes

            if connection closed or failed:
                cleanup

    }

This is the beginning of the event-driven architecture.

### 4. Readiness vs completion

Important:

`poll()` tells us that an operation should not block under the reported readiness conditions.

It does NOT tell us that an entire request or response has been transferred.

We still need non-blocking sockets and careful handling of partial reads and writes.

## What to build

Integrate `poll()` into Pulse.

The new implementation should:

1. Maintain one listening socket.
2. Maintain a collection of active connections.
3. Register the listening socket for read readiness.
4. Register clients for read readiness.
5. Register clients for write readiness only when output is pending.
6. Accept new connections when the listening socket is ready.
7. Read available bytes.
8. Update connection state.
9. Write pending response bytes.
10. Remove closed or failed connections.

Use one event-loop thread.

Do not use one worker thread per client in this implementation.

## Important design rule

Do not keep every client registered for `POLLOUT` all the time.

A connected TCP socket is often writable.

Keeping all sockets permanently registered for write readiness can make `poll()` return immediately over and over.

Enable `POLLOUT` when there is pending output, and disable it when the output buffer is empty.

## Experiments

### Experiment A — Slow client comparison

Connect a client but do not send any request.

Then issue:

    curl http://localhost:8080/hello

Expected:

The active request should complete without waiting for the silent client.

Compare:

- Month 1.
- Month 2.
- Month 3.

### Experiment B — Many idle connections

Use the existing `hold_connections.py`.

Try:

- 100 idle connections.
- 500 idle connections.
- 1,000 idle connections.
- More if your machine allows.

Measure:

- RSS.
- Thread count.
- Open file descriptors.
- CPU usage.

Compare with Month 2.

### Experiment C — Slow requests

Send requests slowly from multiple clients.

Verify that other connections continue to make progress.

### Experiment D — Mixed workload

Run:

- Idle clients.
- Normal `/hello` requests.
- `/slow` requests.
- Fragmented requests.

Check that one connection does not block the event loop indefinitely.

Important consideration:

A handler that calls `sleep()` inside the event-loop thread will block all connections.

For Month 3, do not let `/slow` sleep inside the event loop. Use a controlled test or temporarily adapt/remove that route for the event-loop version.

This is a useful demonstration that non-blocking sockets do not automatically make application handlers non-blocking.

## Deliverables

- `poll()`-based event loop.
- Non-blocking listening socket.
- Non-blocking client sockets.
- Connection collection.
- Read/write event handling.
- Connection cleanup.
- Tests for idle and active clients.
- Concepts document.

## Success criteria

I can explain:

1. Why non-blocking I/O alone can cause busy polling.
2. What I/O multiplexing means.
3. What `poll()` does.
4. What `POLLIN` and `POLLOUT` mean.
5. Why `POLLOUT` should be enabled only when needed.
6. Why `poll()` and non-blocking sockets work together.
7. Why a blocking handler can still freeze an event loop.

---

# 9. Week 4 — select(), Architecture Cleanup & Benchmarking

**Estimated time: 6–10 hours**

## Goal

Understand `select()` as an earlier I/O multiplexing interface, finish the Month 3 architecture, and compare the results against Month 2.

This week is primarily about understanding, testing and documentation.

## Concepts

### 1. select()

Learn:

    select()

And:

    fd_set

Understand:

- FD_ZERO.
- FD_SET.
- FD_CLR.
- FD_ISSET.
- timeval timeout.
- Read, write and exception sets.

### 2. select() vs poll()

Understand the differences:

| Property | select() | poll() |
|---|---|---|
| API model | fd sets | pollfd array |
| Descriptor limit | FD_SETSIZE constraints | No equivalent fixed fd_set limit |
| Interest registration | Rebuild sets each call | Pass pollfd array |
| Result | Ready fd sets | revents flags |
| Kernel interface | Readiness notification | Readiness notification |

Both have to examine the registered descriptors, so neither is the scalable Linux solution we ultimately want.

### 3. Why epoll?

Do not implement it yet.

Understand the motivation:

As the number of connections grows, repeatedly passing and scanning large descriptor collections becomes expensive.

Linux provides `epoll` to offer a more scalable readiness-notification interface.

Month 4 will investigate it properly.

## What to build

- Finish and clean up the poll-based server.
- Ensure connection cleanup is reliable.
- Ensure output buffers are cleared or advanced correctly.
- Ensure error and hangup events are handled.
- Add tests for disconnects and malformed requests.
- Optionally build a tiny standalone `select()` experiment.

Do not build a second full server using `select()`.

The goal is understanding the API and its limitations, not duplicating the entire implementation.

## Experiments

### Experiment A — Idle connection scaling

Compare Month 2 and Month 3 at:

- 100 connections.
- 500 connections.
- 1,000 connections.
- 2,000 connections, if practical.

Record:

- Threads.
- File descriptors.
- RSS.
- CPU.

Expected architectural difference:

Month 2 creates approximately one worker thread per connection.

Month 3 uses one event-loop thread for its connections.

Do not assume memory usage is zero or that file descriptors disappear. Every connection still requires a socket and application state.

### Experiment B — Responsiveness

Repeat the silent-client test.

Keep many clients connected without sending data.

Then send normal `/hello` requests.

Measure how quickly active requests complete.

### Experiment C — Throughput

Run the Month 1 and Month 2 benchmark scripts again.

Run the Month 3 implementation in the same session.

Use the same:

- Request count.
- Concurrency.
- Machine.
- Build configuration.
- Route.
- Test conditions.

Record client CPU as well as server CPU.

ApacheBench is single-threaded and previously became the bottleneck, so do not treat its throughput as Pulse's maximum capacity.

If available, evaluate `wrk` as a separate load generator. Keep the client machine's resource usage visible.

### Experiment D — Descriptor behavior

Observe:

    ss -ltnp

    ls /proc/<PID>/fd | wc -l

    ps -T -p <PID>

Verify that:

- Thread count stays approximately constant.
- File descriptor count grows with connections.
- Descriptors return to baseline after clients disconnect.

### Experiment E — select() vs poll() concepts

Run a small experiment and explain:

- How each represents monitored descriptors.
- What the application must provide on each call.
- Why both are readiness interfaces.
- Why `epoll()` is the next step.

## Deliverables

- Completed poll-based Pulse server.
- Optional standalone select experiment.
- Month 2 vs Month 3 benchmark report.
- Thread and memory comparison.
- Updated architecture diagram.
- Concepts document.
- README update.
- Notes explaining the trade-offs.

## Success criteria

I can explain:

1. What `select()` does.
2. How it differs from `poll()`.
3. Why both are readiness-notification mechanisms.
4. Why `poll()` is a useful stepping stone.
5. Why `epoll()` is worth learning next.
6. Why non-blocking I/O does not mean zero memory or zero resource usage.

---

# 10. Month 3 Benchmark Report

Keep the Month 2 results as the baseline.

Create a comparison table using actual measured values.

| Metric | Month 2 | Month 3 |
|---|---|---|
| Architecture | Thread per connection | Single-threaded poll event loop |
| Threads | Grows with connections | Approximately constant |
| File descriptors | One per connection | One per connection |
| Per-connection thread stack | Yes | No |
| Silent client impact | Isolated by worker thread | Managed by event loop |
| Idle connection CPU | Low | Should be low when poll waits |
| Connection state | Worker stack + local state | Explicit Connection object |
| Memory per connection | Measure | Measure |
| Throughput | Measure | Measure |
| p50 latency | Measure | Measure |
| p99 latency | Measure | Measure |

Do not fill this table with assumed numbers.

The purpose is to discover what actually changed.

---

# 11. Month 3 Final Architecture

The intended architecture:

                  CLIENTS
                     |
          +----------+----------+
          |          |          |
          v          v          v
        Client A   Client B   Client C
          |          |          |
          +----------+----------+
                     |
                     v
             LINUX TCP STACK
                     |
                     v
             NON-BLOCKING SOCKETS
                     |
                     v
              LISTENING SOCKET
                     |
                     v
                POLL LOOP
                     |
          +----------+----------+
          |          |          |
          v          v          v
       Connection Connection Connection
          A          B          C
          |          |          |
          v          v          v
       Input      Input      Input
       Buffer     Buffer     Buffer
          |          |          |
          v          v          v
       HTTP       HTTP       HTTP
       Parser     Parser     Parser
          |          |          |
          v          v          v
       Router     Router     Router
          |          |          |
          v          v          v
       Handler    Handler    Handler
          |          |          |
          v          v          v
       Output     Output     Output
       Buffer     Buffer     Buffer
          |          |          |
          +----------+----------+
                     |
                     v
              NON-BLOCKING WRITE
                     |
                     v
                   CLIENT

All connection processing is coordinated by one event-loop thread.

No thread is created for each connection.

---

# 12. What Month 3 Does NOT Solve

Month 3 is a major improvement in connection management, but it is not the final architecture.

## Problem 1 — poll() scalability

`poll()` needs the application to provide the monitored descriptor collection.

As the number of connections grows, the kernel and application must process that collection repeatedly.

This can become expensive.

Month 4 will investigate `epoll()`.

## Problem 2 — One event loop is still one thread

The event loop can manage many sockets, but it cannot execute CPU-heavy application handlers in parallel.

If a handler blocks or performs expensive work, the event loop becomes unresponsive.

Month 5 will investigate a controlled worker pool.

## Problem 3 — Application-level blocking

Non-blocking sockets do not make every operation non-blocking.

For example:

- `sleep()`.
- CPU-heavy calculations.
- Synchronous file operations.
- Blocking database calls.

Any such operation can stall the event loop.

We must understand this distinction.

## Problem 4 — Connection state complexity

The application now explicitly manages:

- input buffers.
- output buffers.
- parser progress.
- connection state.
- partial writes.
- disconnects.
- cleanup.

This is more complex than a thread-per-connection server.

## Problem 5 — No graceful shutdown

The server still needs a deliberate shutdown design.

We will keep this as a separate future improvement rather than adding unrelated scope to Month 3.

---

# 13. What We Are NOT Implementing

Do not implement these in Month 3:

- epoll.
- EPOLLET / edge-triggered operation.
- EPOLLONESHOT.
- io_uring.
- Thread pools.
- Lock-free queues.
- CPU affinity.
- NUMA.
- Custom allocators.
- DPDK.
- AF_XDP.
- TLS.
- HTTP/2.
- Keep-alive / persistent HTTP connections.
- Database integration.
- Distributed systems.

Keep `Connection: close` and one request per connection for now.

The goal is to understand non-blocking connection management, not to expand HTTP features.

---

# 14. Month 3 Final Questions

Before declaring Month 3 complete, I should be able to explain these without memorizing definitions.

## Non-blocking I/O

1. What is the difference between blocking and non-blocking I/O?
2. What does `O_NONBLOCK` do?
3. Why does `read()` return `EAGAIN`?
4. What is the difference between `EAGAIN` and a fatal socket error?
5. Why can non-blocking code accidentally consume 100% CPU?

## Connection state

6. Why does every connection need its own state?
7. Why is one `read()` not necessarily one HTTP request?
8. Why can one `write()` send only part of a response?
9. What is a state machine?
10. How do we know when a connection can be closed?

## I/O multiplexing

11. What is I/O multiplexing?
12. What does `poll()` do?
13. What does `POLLIN` mean?
14. What does `POLLOUT` mean?
15. Why should we enable `POLLOUT` only when there is pending output?
16. What is the difference between readiness and completion?
17. How does `select()` differ from `poll()`?

## Architecture

18. How is Month 3 different from Month 2?
19. Why does Month 3 not need one thread per client?
20. What happens when a handler blocks inside the event loop?
21. Why is `poll()` not the final scalability solution?
22. Why do we need `epoll()` in Month 4?

---

# 15. Final Month 3 Success Checklist

- [ ] Blocking vs non-blocking experiments completed.
- [ ] `fcntl()` understood.
- [ ] `O_NONBLOCK` implemented.
- [ ] `EAGAIN` and `EWOULDBLOCK` handled correctly.
- [ ] `EINTR` handled appropriately.
- [ ] Connection abstraction implemented.
- [ ] Per-connection input buffer implemented.
- [ ] Per-connection output buffer implemented.
- [ ] Connection state machine implemented.
- [ ] Partial reads handled.
- [ ] Partial writes handled.
- [ ] Non-blocking listening socket implemented.
- [ ] Non-blocking accepted sockets implemented.
- [ ] `poll()` event loop implemented.
- [ ] Read readiness handled.
- [ ] Write readiness handled.
- [ ] Connection cleanup tested.
- [ ] Silent client experiment completed.
- [ ] Many idle connections experiment completed.
- [ ] Month 2 vs Month 3 benchmark completed.
- [ ] `select()` concepts understood.
- [ ] README and architecture updated.
- [ ] I can explain the entire event-loop flow.

---

# 16. Final Lesson

Month 1 taught:

    One client communicates with Linux through sockets.

Month 2 taught:

    Multiple clients can make progress using multiple threads.

Month 3 teaches:

    One thread can manage many connections using non-blocking I/O and readiness notification.

Month 4 will ask:

    Can Linux manage this readiness notification more efficiently using epoll?

Month 5 will ask:

    How can we combine an event loop with a controlled number of worker threads for CPU-heavy work?

The objective is not to collect APIs.

The objective is to understand why each architecture exists, what problem it solves, and what new problem it introduces.

**Build it. Predict its behavior. Test it. Measure it. Explain it.**

That is Pulse.
