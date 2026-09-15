# Pulse — Month 1

## Project Goal

Build a small HTTP/1.1 server in C++ from the ground up using Linux/POSIX networking APIs.

The purpose of this project is NOT to build a production-grade web server.

The purpose is to understand what actually happens underneath a backend API:

```text
Browser / curl
      |
      | HTTP
      v
   Pulse
      |
      | Linux/POSIX APIs
      v
   Linux Kernel
      |
      v
     TCP
      |
      v
   Network
```

By the end of Month 1, Pulse should accept a real HTTP request from `curl` or a browser and return a valid HTTP response.

---

# IMPORTANT RULE FOR CLAUDE

You are my systems-programming mentor and coding partner.

Do NOT blindly write the complete solution for me.

I am using this project to learn Linux, operating systems, computer networking and C++.

When I ask for help:

1. First explain the underlying concept.
2. Ask me what I think is happening when useful.
3. Let me attempt the implementation.
4. Review my code.
5. Point out bugs and conceptual mistakes.
6. Give hints before giving complete code.
7. Only provide complete code when I explicitly ask for it or when the code is trivial/boilerplate.
8. Never hide important system behaviour behind abstractions just to make the code shorter.
9. Prefer understanding over premature optimization.
10. Do not introduce frameworks such as Boost.Asio, libuv, Crow, Pistache, etc. during Month 1.

I should be able to explain every important line of the final server.

---

# TECHNICAL CONSTRAINTS

Language:

C++17 or newer.

Platform:

Linux.

Allowed:

- C++ standard library
- POSIX/Linux system calls
- POSIX networking APIs
- basic build tools
- CMake
- gcc/g++
- clang++
- curl
- netcat (`nc`)
- standard Linux debugging/inspection tools

Important standard library facilities we MAY use:

```cpp
std::string
std::string_view
std::vector
std::array
std::unordered_map
std::optional
std::thread
std::mutex
std::condition_variable
std::atomic
std::unique_ptr
```

Do not unnecessarily reimplement standard containers.

The educational goal is to understand the server/networking layer, not to reinvent `std::vector`.

Linux/POSIX APIs we will learn:

```text
socket()
bind()
listen()
accept()
read()
write()
close()
setsockopt()
```

We will NOT implement TCP itself.

Linux already provides TCP/IP.

We are building the server/application layer that uses Linux networking facilities.

---

# WHAT WE ARE BUILDING

The final Month 1 architecture should look approximately like:

```text
                    curl / Browser
                          |
                          | HTTP request
                          v
                 +------------------+
                 |   Pulse Server   |
                 |                  |
                 |  socket          |
                 |  bind            |
                 |  listen          |
                 |  accept          |
                 |  read            |
                 |                  |
                 |  HTTP Parser     |
                 |                  |
                 |  Router          |
                 |                  |
                 |  Response        |
                 |  write           |
                 +--------+---------+
                          |
                    Linux/POSIX
                          |
                    Linux Kernel
                          |
                         TCP
```

The user should eventually be able to run:

```bash
./pulse 8080
```

and then:

```bash
curl http://localhost:8080/hello
```

and receive:

```text
Hello from Pulse!
```

---

# MONTH 1 OUTCOME

At the end of Month 1 I should be able to:

1. Explain what a server is.
2. Explain client/server communication.
3. Explain IP address and port.
4. Explain TCP at a high level.
5. Explain what a socket is.
6. Explain what a file descriptor is.
7. Explain:

```text
socket()
bind()
listen()
accept()
read()
write()
close()
```

8. Explain what HTTP is.
9. Explain the structure of an HTTP request.
10. Parse a basic HTTP request.
11. Generate a valid HTTP response.
12. Serve multiple basic routes.
13. Test the server with curl.
14. Test the server with a browser.
15. Inspect connections using Linux tools.
16. Understand that TCP is a byte stream and does NOT understand HTTP.

---

# MONTH 1 SCOPE

DO NOT add these yet:

- epoll
- non-blocking sockets
- thread pools
- async I/O
- io_uring
- DPDK
- AF_XDP
- custom allocator
- lock-free queues
- TLS
- HTTP/2
- database
- distributed systems
- HFT/order book

Those are future milestones.

Month 1 is about understanding the basic path:

```text
HTTP
 ↓
socket
 ↓
Linux
 ↓
TCP
```

---

# WEEK 1 — UNDERSTAND + TCP SERVER

## Learning

Learn:

### C++ basics needed for systems programming

- compilation
- linking
- header/source files
- pointers
- references
- arrays
- strings
- error handling
- RAII basics
- `const`
- `std::string`
- `std::vector`

Do not spend the week studying all of C++.

Only learn what the project requires.

---

## Linux concepts

Learn:

- process
- user space
- kernel space
- system call
- file descriptor
- stdin/stdout/stderr
- `/proc` at a basic level

Understand:

```text
Your C++ program
      |
      | system call
      v
Linux Kernel
```

---

## Networking concepts

Learn:

- IP address
- localhost
- port
- client
- server
- TCP
- connection
- TCP handshake at a conceptual level

Understand:

```text
Client
  |
  | TCP connection
  v
Server
```

---

# WEEK 1 BUILD

Build a TCP server.

Initial target:

```text
client
  |
  | "hello"
  v
server
  |
  | "hello"
  v
client
```

The server should:

1. Create a socket.
2. Bind to a port.
3. Listen.
4. Accept one client.
5. Read bytes.
6. Print the bytes.
7. Send a response.
8. Close the connection.

---

# WEEK 1 TESTS

Test with:

```bash
nc localhost 8080
```

Type:

```text
hello
```

The server should receive it.

Test another client.

Inspect the listening socket:

```bash
ss -ltnp
```

Find the process:

```bash
ps aux
```

Inspect open file descriptors:

```bash
ls -l /proc/<PID>/fd
```

Questions I must be able to answer:

- Why did `socket()` return an integer?
- What does that integer represent?
- Why does the server need `bind()`?
- What does `listen()` actually change?
- Why does `accept()` return another FD?
- Why are there two FDs?
- What is the difference between the listening socket and client socket?
- What happens when the client disconnects?

---

# WEEK 2 — HTTP

Now turn the TCP server into an HTTP server.

Understand:

```http
GET /hello HTTP/1.1
Host: localhost:8080
Connection: close
```

Break it into:

```text
Method  = GET
Path    = /hello
Version = HTTP/1.1

Headers:
Host = localhost:8080
Connection = close
```

Important concept:

TCP gives us bytes.

HTTP gives those bytes meaning.

```text
TCP:
"Here are some bytes."

HTTP:
"These bytes represent GET /hello."
```

---

# WEEK 2 BUILD

Implement a basic HTTP parser.

At minimum support:

```text
GET
POST
HTTP/1.1
headers
```

Build internal representations such as:

```cpp
Request
Response
```

Conceptually:

```text
Raw bytes
    |
    v
HTTP Parser
    |
    v
Request
    |
    v
Application
    |
    v
Response
    |
    v
Raw HTTP bytes
```

---

# WEEK 2 ROUTING

Implement a very small router.

Support:

```text
GET /
GET /hello
GET /health
```

Example:

```text
GET /
```

returns:

```text
Hello from Pulse!
```

```text
GET /health
```

returns:

```text
OK
```

Unknown routes:

```text
GET /something
```

return:

```text
HTTP/1.1 404 Not Found
```

---

# WEEK 2 TESTING

Use:

```bash
curl -v http://localhost:8080/
```

```bash
curl -v http://localhost:8080/hello
```

```bash
curl -v http://localhost:8080/health
```

Test 404:

```bash
curl -v http://localhost:8080/random
```

Test POST:

```bash
curl -v -X POST http://localhost:8080/hello
```

Use a browser:

```text
http://localhost:8080/hello
```

Inspect the raw HTTP request using:

```bash
nc localhost 8080
```

Manually type:

```http
GET /hello HTTP/1.1
Host: localhost

```

Remember the blank line after headers.

---

# WEEK 2 CONCEPT CHECK

I should be able to explain:

- HTTP vs TCP
- Why TCP does not know HTTP
- What an HTTP request is
- What an HTTP response is
- Why headers exist
- Status codes
- Content-Length
- Why a blank line separates headers from body
- Why TCP can split one HTTP request across multiple reads
- Why one `read()` is NOT guaranteed to return one complete HTTP request

---

# WEEK 3 — MAKE THE SERVER MORE CORRECT

Improve the HTTP implementation.

Implement:

```text
400 Bad Request
404 Not Found
405 Method Not Allowed
200 OK
```

Support:

```text
Content-Length
Content-Type
Connection
```

Implement basic request body handling for POST.

Example:

```http
POST /echo HTTP/1.1
Content-Length: 5

hello
```

Response:

```text
hello
```

---

# IMPORTANT TCP EXPERIMENT

This is one of the most important experiments of Month 1.

Do NOT assume:

```text
one send()
=
one read()
```

Test this.

Create a client that sends a request in multiple pieces:

```text
"GET /hel"
"lo HTTP/1.1\r\n"
"Host: localhost\r\n"
"\r\n"
```

Your server must understand that TCP is a stream.

It may receive:

```text
GET /hel
```

then:

```text
lo HTTP/1.1
```

then:

```text
...
```

The HTTP parser must accumulate bytes until enough information exists to parse the request.

This experiment is extremely important.

---

# WEEK 3 LINUX EXPERIMENTS

While Pulse is running:

```bash
ss -ltnp
```

Find its PID.

Then:

```bash
ls -l /proc/<PID>/fd
```

Observe that the process has file descriptors.

Use:

```bash
strace ./pulse 8080
```

Observe system calls such as:

```text
socket
bind
listen
accept
read
write
close
```

The goal is NOT to memorize strace output.

The goal is to connect:

```text
YOUR C++ CODE
     |
     v
socket()
     |
     v
SYSTEM CALL
     |
     v
LINUX KERNEL
```

---

# WEEK 4 — CLEANUP + DOCUMENTATION + BENCHMARK

Do not add major features.

Make the existing server clean and understandable.

Organize code approximately:

```text
pulse/
│
├── CMakeLists.txt
├── README.md
│
├── include/
│   ├── server.hpp
│   ├── http.hpp
│   └── router.hpp
│
├── src/
│   ├── main.cpp
│   ├── server.cpp
│   ├── http.cpp
│   └── router.cpp
│
└── tests/
    └── ...
```

The exact structure can evolve.

Do not create unnecessary abstractions.

---

# BASIC BENCHMARK

Use curl or a simple load generator.

Measure:

```text
requests
requests/sec
latency
CPU usage
memory
```

Do NOT claim the server is "high performance" yet.

At this stage the objective is:

> Establish a baseline.

Record:

```text
Machine:
CPU:
RAM:
OS:

Concurrency:
Requests:
Throughput:
Latency:
```

---

# FINAL MONTH 1 DEMO

At the end, I should be able to demonstrate:

## 1. Start server

```bash
./pulse 8080
```

## 2. Browser

Open:

```text
http://localhost:8080/hello
```

## 3. API

```bash
curl http://localhost:8080/health
```

## 4. POST

```bash
curl -X POST http://localhost:8080/echo -d "hello"
```

## 5. 404

```bash
curl http://localhost:8080/does-not-exist
```

## 6. Inspect Linux

```bash
ss -ltnp
```

## 7. Inspect system calls

```bash
strace ./pulse 8080
```

## 8. Explain the entire request

I should be able to draw:

```text
Browser
   |
   | TCP connection
   v
Linux TCP stack
   |
   v
listen socket
   |
 accept()
   |
   v
client socket / FD
   |
 read()
   |
   v
HTTP bytes
   |
   v
HTTP parser
   |
   v
Router
   |
   v
Handler
   |
   v
HTTP response
   |
 write()
   |
   v
Linux TCP stack
   |
   v
Browser
```

---

# MONTH 1 SUCCESS CRITERIA

Month 1 is COMPLETE if:

- [ ] C++ project builds cleanly
- [ ] Server listens on a configurable port
- [ ] TCP connection works
- [ ] HTTP GET works
- [ ] HTTP POST works at a basic level
- [ ] HTTP headers are parsed
- [ ] HTTP response is generated
- [ ] Routing works
- [ ] 404 works
- [ ] curl works
- [ ] Browser works
- [ ] Multiple sequential clients work
- [ ] Partial TCP reads are handled correctly
- [ ] `ss` has been used to inspect the server
- [ ] `strace` has been used to inspect the server
- [ ] README contains architecture and learnings
- [ ] Basic benchmark has been recorded

---

# WHAT NOT TO WORRY ABOUT YET

Do NOT worry about:

```text
epoll
threads
async
io_uring
lock-free
custom allocator
cache optimization
NUMA
DPDK
HFT
distributed systems
```

Those come later.

Right now:

```text
                  MONTH 1

              "How does a
             request reach
             my C++ code?"
                    |
                    v
             TCP + sockets
                    |
                    v
               Linux APIs
                    |
                    v
               HTTP parser
                    |
                    v
                 Router
                    |
                    v
               Response
```

---

# LEARNING METHOD

For every new concept use this loop:

```text
1. Learn the concept
       ↓
2. Predict what should happen
       ↓
3. Implement
       ↓
4. Run it
       ↓
5. Break it intentionally
       ↓
6. Observe Linux/network behaviour
       ↓
7. Explain why
       ↓
8. Fix it
       ↓
9. Document it
```

Do not measure progress by lines of code.

Measure progress by:

> "Can I explain what happened?"

---

# AI USAGE RULES

AI is allowed and encouraged.

But use AI primarily as:

```text
teacher
debugger
reviewer
question generator
experiment designer
```

not:

```text
code generator
```

Good prompt:

> "Explain why accept() returns a different file descriptor from the listening socket. Do not give me code."

Good prompt:

> "Here is my HTTP parser. Find conceptual bugs but don't rewrite it."

Good prompt:

> "Give me an experiment that proves TCP is a byte stream."

Good prompt:

> "Ask me five interview questions about my server."

Bad prompt:

> "Build the entire HTTP server for me."

If I ask for code, still encourage me to understand it line-by-line.

---

# FINAL PRINCIPLE

The project is not valuable because it is a complicated GitHub repository.

It is valuable because I can eventually open it in an interview and explain:

> "I started with Linux sockets, accepted TCP connections, implemented HTTP parsing myself, built routing, observed the actual system calls with strace, inspected sockets with ss, and learned why TCP stream semantics require buffering."

That understanding is the actual goal.

Future months will extend this same server rather than throwing it away.