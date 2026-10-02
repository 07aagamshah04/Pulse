---
title: "Pulse — Month 1, Week 4: Concepts"
subtitle: "Cleanup, Documentation, and a Baseline Benchmark"
---

# How to use this document

Same format as Weeks 1-3: explanation, a blue **analogy** box, a green **real example** box. This week is different in kind from the previous three, though — there is no new server feature to build. The exercise is: review what exists, document it honestly, and measure it honestly. Read this once, then do the review/write-up/benchmark yourself.

---

# Part 1 — why "just cleanup" is a real part of the project, not filler

It's tempting to treat Week 4 as the boring week between "real" weeks. It isn't. Look back at your own project's "LEARNING METHOD" loop:

```text
1. Learn -> 2. Predict -> 3. Implement -> 4. Run -> 5. Break ->
6. Observe -> 7. Explain why -> 8. Fix -> 9. Document it
```

Steps 7 and 9 — *explain why* and *document it* — are not afterthoughts tacked onto the real work; they're where understanding actually gets tested. You can write code that works without being able to explain why it works. You cannot write an honest architecture doc, or accurately predict your own benchmark numbers before running them, without actually understanding the system. That's the whole point of this week.

<div class="analogy">
<b>Analogy:</b> a pilot's post-flight debrief isn't paperwork bolted onto flying the plane — it's how a near-miss today becomes a lesson instead of a repeated mistake next month. Skipping the debrief doesn't save real time; it just defers the cost to later, with less context available when it's paid.
</div>

**Why this matters specifically for the "final principle" in your project spec** — the whole point of Pulse is being able to open it in an interview and explain what you built and why. A README that just says "a C++ HTTP server" proves nothing an interviewer couldn't guess from the repo name. A README that explains *why* `accept()` returns a different fd than the listening socket, *why* the parser distinguishes malformed from incomplete, and *what your own benchmark actually measured and why the numbers look the way they do* — that's the artifact that's actually worth having.

---

# Part 2 — code review, not a rewrite

Your project spec explicitly warns: **do not add major features this week.** The target layout it describes:

```text
pulse/
├── CMakeLists.txt
├── README.md
├── include/
│   ├── server.hpp / http.hpp / router.hpp (or your actual split)
├── src/
│   └── ...
└── tests/
```

<div class="example">
<b>Real example — do this as an actual exercise, not a rhetorical question:</b> open every header in <code>include/</code> in order and, for each one, answer out loud: "what is this file's ONE job?" If you can't answer in one sentence, that's a real signal — not necessarily that something's wrong, but that it's worth checking whether a file has quietly picked up more than one responsibility over three weeks of incremental changes. (For reference, walk through what you already have: <code>socket.hpp</code> — RAII ownership of one fd. <code>http_request.hpp</code> — the parsed form of a request. <code>http_response.hpp</code> — building and serializing a response. <code>http_parser.hpp</code> — turning bytes into a Request. <code>router.hpp</code> — matching (method, path) to a handler. <code>tcp_server.hpp</code> — the accept/read/route/respond loop. Each one answers in a sentence — that's what "clean" looks like, not zero code smell, just a clear single job per file.)
</div>

This week is about **noticing rot**, not preventing it from ever existing: a variable name that made sense in Week 1 but is misleading now that Week 3 changed what it holds, a comment that's gone stale because the code it describes changed underneath it, an inconsistency in how errors are reported between two files that grew somewhat independently. Go find at least a few real (not invented) examples of this in your own code before moving on — this is a genuine skill, not busywork.

---

# Part 3 — what a benchmark is actually for, at this stage

Your project spec is explicit and worth repeating verbatim: **"Do NOT claim the server is 'high performance' yet... the objective is: establish a baseline."**

<div class="analogy">
<b>Analogy:</b> the first time you time yourself running a mile, the number isn't a verdict on your fitness — it's a reference point. Every future improvement (better shoes, more training, a different route) gets measured against THIS number, not against some abstract "good" runner's time. A baseline's entire value is being honest and reproducible, not impressive.
</div>

**What to measure**, per the spec:

```text
requests
requests/sec
latency
CPU usage
memory
```

**Why these specific numbers, and what each one is actually telling you:**

- **requests / requests-per-second (throughput)**: how many complete request/response cycles your server finishes per unit time. This is the headline number, but it's meaningless without the next one.
- **latency**: how long a single request takes, end to end. A server can have high throughput and terrible latency (queuing everyone, nobody gets an answer fast) or the reverse — throughput and latency are two different questions, not one.
- **CPU usage**: is the bottleneck the CPU doing real work, or is the process mostly *waiting* (on `accept()`, on `read()`)? For a blocking, single-threaded server like Pulse right now, expect CPU usage to be *low* even under load — it's not fully "using" the machine, it's queued up waiting on I/O and the backlog, which is itself a useful, honest observation to record.
- **memory**: does memory grow with load, or stay flat? For Pulse specifically, this connects directly to the `kMaxRequestSize` cap from Week 3 — worth confirming memory stays bounded even under many/large requests, not just assuming it.

---

# Part 4 — predicting your own results before you run them

This is the "Predict" step from your own learning loop, applied to a benchmark instead of a feature. Before running anything, answer these yourself:

<div class="example">
<b>Real example — actually write down your predictions before running the benchmark, then compare:</b>

- Pulse's <code>run()</code> loop calls <code>accept()</code>, then fully handles ONE client (read, parse, route, write, close) before calling <code>accept()</code> again (Week 1, Part 5; unchanged through Week 3). What do you predict happens to <b>latency</b> for the 10th concurrent client if you send 10 requests at once, versus sending them one at a time? (Think back to the backlog-queue observation from your own Week 1 notes.)
- Given the server is blocking and sequential, do you expect throughput to increase, stay flat, or decrease as you raise concurrency from 1 to 50? Why?
- Do you expect CPU usage to be high or low during this benchmark? What is the process actually spending its time doing while a client is connected but hasn't sent anything yet?

Write your answers down BEFORE running <code>ab</code>. A baseline you predicted correctly means you understand the system. A baseline that surprises you is even more valuable — it means you found a gap between your mental model and reality, which is exactly what Step 6 ("Observe") and Step 7 ("Explain why") of your learning loop are for.
</div>

---

# Part 5 — running the actual benchmark

Install a load generator first (needs your password, run it yourself):

```bash
sudo apt install -y apache2-utils
```

This gives you `ab` (Apache Bench) — simple, ubiquitous, sufficient for a baseline.

```bash
# 1000 requests total, one at a time (concurrency 1) - the simplest possible baseline
ab -n 1000 -c 1 http://localhost:8080/

# same 1000 requests, but 10 at a time - see what changes given the sequential accept() loop
ab -n 1000 -c 10 http://localhost:8080/
```

Read the output for: `Requests per second`, `Time per request` (both the "mean" and the "mean, across all concurrent requests" variants — they mean different things, worth understanding why), and `Failed requests` (should be 0 — if it isn't, that's a real finding, not something to hide from your writeup).

While `ab` runs, in another terminal, capture CPU/memory:

```bash
ps -o pid,%cpu,%mem,rss,cmd -p $(pgrep -f 'pulse 8080' | head -1)
```

Run this a few times during the benchmark (or wrap it in a `while true; do ...; sleep 0.5; done` loop) to see if the numbers move.

<div class="example">
<b>Real example of a specific thing to notice:</b> compare the <code>-c 1</code> and <code>-c 10</code> runs' "Requests per second" numbers. If your prediction from Part 4 was "sequential server, so concurrency shouldn't help much," check whether the actual numbers agree. If they don't, that's worth digging into rather than smoothing over in your writeup — e.g., is the OS-level backlog queue absorbing enough of the burst that it doesn't show up as clearly as expected at this small scale?
</div>

---

# Part 6 — what actually goes in the README this week

Your project spec: *"README contains architecture and learnings."* Concretely, that means:

1. **The request path diagram** — you already have this exact diagram in the Month 1 spec itself (Browser → TCP → socket/accept → read → HTTP parser → Router → Handler → Response → write → TCP → Browser). Put a version of it in the README, in your own words, not copy-pasted.
2. **Which syscalls are used, and one sentence on why each is needed** — `socket`, `bind`, `listen`, `accept`, `read`, `write`, `close`, `setsockopt`. You've written far more than a sentence on each already, across three weeks of doc reading — this is where you compress that down to what actually belongs in a repo's front door.
3. **Deliberate simplifications, stated as decisions, not hidden as gaps** — e.g. "single-threaded, one client at a time by design — concurrency is a later month's topic," "no pipelining/keep-alive support," "a fixed 1 MiB cap on accumulated request size." A senior engineer reading this should come away thinking "they know exactly what they didn't build and why," not wondering if you noticed.
4. **The baseline benchmark numbers themselves**, with the machine/environment they were measured on (your project spec's own template):

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

# Part 7 — the Month 1 checklists (from your own spec — go through them literally)

**Final Month 1 Demo** — you should be able to actually do all of these, live, right now:
1. `./pulse 8080` starts cleanly.
2. A browser loads `http://localhost:8080/hello`.
3. `curl http://localhost:8080/health` returns `OK`.
4. `curl -X POST http://localhost:8080/echo -d "hello"` echoes the body.
5. `curl http://localhost:8080/does-not-exist` returns 404.
6. `ss -ltnp` shows Pulse listening.
7. `strace ./pulse 8080` shows the real syscalls.
8. You can draw the full request path from memory, unaided.

**Month 1 success criteria** — go down this list from the spec and honestly check each box; don't check one you can't currently demonstrate:

```text
[ ] C++ project builds cleanly
[ ] Server listens on a configurable port
[ ] TCP connection works
[ ] HTTP GET works
[ ] HTTP POST works at a basic level
[ ] HTTP headers are parsed
[ ] HTTP response is generated
[ ] Routing works
[ ] 404 works
[ ] curl works
[ ] Browser works
[ ] Multiple sequential clients work
[ ] Partial TCP reads are handled correctly
[ ] ss has been used to inspect the server
[ ] strace has been used to inspect the server
[ ] README contains architecture and learnings
[ ] Basic benchmark has been recorded
```

---

# Part 8 — concept check

**Why is "establish a baseline" the right goal, rather than "make it fast"?** Because you cannot know whether a future change (adding threads, `epoll`, connection pooling) actually helped without a trustworthy "before" number to compare against — optimizing without a baseline is just guessing whether you made things better.

**Why might CPU usage stay low even while the server is "under load" from a benchmark tool's perspective?** Because Pulse's `accept()`/`read()` calls are blocking — the process spends most of its time asleep, waiting on the kernel to hand it work, not actively computing. Low CPU usage under load is not automatically "good performance" — it can just as easily mean the bottleneck is elsewhere (like sequential handling of one client at a time), which the throughput/latency numbers would reveal instead.

**Why does the spec insist on NOT adding features this week?** Because mixing "cleaning up and measuring what exists" with "adding new behavior" makes both worse: your benchmark numbers stop being a clean baseline for the *documented* architecture if the code changes mid-week, and your documentation risks describing a moving target instead of a settled one.
