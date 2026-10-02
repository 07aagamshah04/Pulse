---
title: "Pulse — Week 4 Notes"
subtitle: "Benchmark & Testing Log"
---

<style>
.tldr { background:#fff8e1; border:2px solid #f0c040; border-radius:8px; padding:1.2em 1.5em; margin:1.5em 0; font-size:1.05em; line-height:1.6; }
.stat-row { display:flex; gap:1em; flex-wrap:wrap; margin:1.5em 0; }
.stat-card { flex:1; min-width:180px; background:#f5f5f5; border:1px solid #ddd; border-radius:8px; padding:1em 1.2em; text-align:center; }
.stat-card .label { font-size:0.82em; color:#555; text-transform:uppercase; letter-spacing:0.03em; }
.stat-card .values { font-size:1.6em; font-weight:bold; margin:0.3em 0; }
.stat-card .up { color:#c0392b; }
.stat-card .flat { color:#2e7d32; }
.stat-card .note { font-size:0.8em; color:#777; }
.verdict-right { color:#2e7d32; font-weight:bold; }
.verdict-wrong { color:#c0392b; font-weight:bold; }
details { background:#fafafa; border:1px solid #ddd; border-radius:6px; padding:0.3em 1.2em 1em; margin:1.5em 0; }
details[open] { padding-bottom:1.2em; }
summary { cursor:pointer; font-weight:600; padding:0.8em 0; }
</style>

# What actually happened — plain English, no jargon

<div class="tldr">
<b>What I did:</b> sent 1000 requests to my server two different ways — one at a time, then 10 at once — and measured what happened each way.
<br><br>
<b>What happened, in one sentence:</b> sending requests 10-at-a-time let the server get through all 1000 of them faster <i>overall</i>, but each <i>individual</i> request took a bit longer to get its reply — because the server only ever truly works on one client at a time, so the others have to wait their turn.
</div>

## The 3 numbers that actually matter

<div class="stat-row">
<div class="stat-card">
<div class="label">Requests handled per second</div>
<div class="values">2023 <span class="up">&rarr;</span> 5372</div>
<div class="note">went UP a lot with 10 clients at once</div>
</div>
<div class="stat-card">
<div class="label">Time for ONE request to get a reply</div>
<div class="values">0.49ms <span class="up">&rarr;</span> 1.86ms</div>
<div class="note">went up a little — each one waits its turn</div>
</div>
<div class="stat-card">
<div class="label">Requests that failed</div>
<div class="values">0 <span class="flat">&rarr;</span> 0</div>
<div class="note">nothing broke, either way</div>
</div>
</div>

## Why both of those can be true at the same time

<div class="analogy">
<b>One cashier, one line:</b> imagine a single cashier (the server) who can only serve one customer fully at a time. If 10 customers show up together and form a line, the cashier can keep working almost back-to-back with barely a gap — so the <i>store</i> serves more customers per hour overall. But any ONE customer in that line of 10 waits longer for their own turn than someone who walked in completely alone. That's exactly what happened here: more total throughput (a busier cashier), but each individual customer's own wait time goes up a bit. The cashier isn't suddenly working on two customers simultaneously — they're just never standing around with nobody in line.
</div>

## Did my 3 guesses match reality?

| # | I guessed | What actually happened | Verdict |
|---|---|---|---|
| 1 | Latency would go up | ✅ Yes — 0.49ms → 1.86ms (about 3.8x slower per request) | <span class="verdict-right">Right</span> |
| 2 | Throughput would stay flat | ❌ No — it more than doubled (2023 → 5372 req/sec) | <span class="verdict-wrong">Wrong</span> |
| 3 | A queued client uses no CPU, just waits | ✅ Confirmed, twice — `0.0%` at idle, and still only ~12.6% of one core under sustained heavy load (see below) | <span class="verdict-right">Right</span> |

**Why #2 was wrong, in plain terms:** I expected "server does one thing at a time, so more clients shouldn't help." That's true for the server's actual *work* (reading, parsing, responding) — but it's not true for *connecting*. While the server is busy answering client #1, the other 9 clients can already be finishing their initial handshake with the operating system in the background, so they're ready and waiting the instant the server is free — instead of each one having to start that handshake from scratch only after the previous customer fully left. More line-forming, same single cashier.

## CPU and memory — confirming guess #3 with real numbers

<div class="stat-row">
<div class="stat-card">
<div class="label">CPU used by the server</div>
<div class="values flat">0.0%</div>
<div class="note">same reading all 5 times sampled</div>
</div>
<div class="stat-card">
<div class="label">Memory held (RSS)</div>
<div class="values flat">~3.75 MB</div>
<div class="note">identical across every sample — flat, no growth</div>
</div>
</div>

Both numbers stayed exactly the same across all 5 snapshots, 0.5 seconds apart. That's the "memory should stay flat under load, not grow" question from Part 3 of the Week 4 doc, answered: it stayed flat. 3.75 MB is a rounding error against this machine's 3.5 GB of available RAM.

<div class="example"><b>Caveat at the time:</b> this first snapshot wasn't confirmed to be taken <i>while</i> <code>ab</code> was actively hammering the server — turned out it was captured at idle, so "0% CPU" was expected trivially and didn't yet prove "low CPU even under real load." Redone properly below.</div>

## Second check — this time with the server genuinely under load

To get real overlap, the benchmark needed to run long enough for `ps` to actually catch it mid-flight: `ab -n 50000 -c 10` (50,000 requests instead of 1,000) running in one terminal, `ps` sampling every 0.5s in another, started together.

**The benchmark itself, at 50x the volume — results essentially unchanged**, which is itself a good finding (consistent behavior isn't a fluke of a small sample):

| Metric | `-c 10`, 1,000 requests | `-c 10`, 50,000 requests |
|---|---|---|
| Time taken | 0.186 s | 9.275 s |
| Requests/sec | 5371.87 | 5390.55 |
| Time per request (mean) | 1.862 ms | 1.855 ms |
| Failed requests | 0 | 0 |
| 100% (worst case) | 4 ms | 16 ms |

`ab` printed a `WARNING` this run about the "Waiting" time's median and mean not matching a normal distribution — that's `ab` itself flagging that one specific average looked a bit skewed, not an error or something that broke.

**The CPU numbers — climbing, but not for the reason it looks like:**

```
%CPU:  3.3  4.1  4.8  5.6  6.4  7.1  7.8  8.5  9.2  9.9  10.6  11.2  11.9  12.6  12.6
```

<div class="analogy"><b>Why this climbs instead of jumping straight to the real number:</b> <code>ps</code>'s <code>%CPU</code> column isn't "what's happening right now" — it's a running average: <i>(total CPU time used so far) / (total time the process has been alive so far)</i>, recalculated fresh on every check. Pulse had been sitting idle for a while before this benchmark started, using ~0% the whole time. The instant <code>ab</code> starts hammering it, Pulse begins actually burning real (tiny amounts of) CPU time continuously — but that old idle time is still sitting in the average's denominator. Each new sample recalculates using more accumulated busy time, so the number keeps creeping toward the true current rate instead of jumping to it immediately. Nothing is runaway or leaking — this is the average <i>converging</i>, not the real workload growing.</div>

**And even at its peak, `12.6%` is of *one* CPU core** — on this 12-core machine (`nproc`), that's a trivial slice of total capacity. This is a genuinely useful finding: Pulse is nowhere near CPU-bound even under sustained load. The actual bottleneck in this architecture is the sequential, one-client-at-a-time `accept()` loop (Week 1), not a shortage of CPU — exactly the gap a later month's concurrency work (threads/`epoll`) would exist to close.

**Memory held firm at exactly `3840 KB` across all 15 samples** — the same number as the idle test, now confirmed under 50x the load. Stronger evidence for "memory stays flat" than the first test could offer alone.

### What is RSS, in plain terms?

RAM is one shared pool used by every running program on the machine. **RSS = how much of that real, physical RAM this one program is actually holding onto, right now** — not how much it's allowed to use, not disk space, the actual physical memory assigned to it at the moment you check.

<div class="analogy">RAM is one big shared shelf in a warehouse. RSS is how many actual boxes Pulse currently has sitting on that shelf. Other programs have their own boxes on the same shelf. <code>3840 KB</code> means Pulse's boxes, right now, take up 3840 KB — nothing hypothetical, nothing reserved-but-unused.</div>

**What's actually inside those ~3.75 MB:**

1. **The compiled program itself** — the machine instructions that make up `pulse`. Present from the moment it starts, regardless of whether any client has ever connected.
2. **Shared libraries it needs to function** — `libstdc++` (gives you `std::string`, `std::vector`, ...) and `libc` (wraps syscalls like `read`/`write`). Every C++ program needs these loaded.
3. **The stack** — holds local variables and function-call bookkeeping while code runs (e.g. `raw_request` lives here while `handle_client` executes).
4. **The heap** — where things like `std::string`'s actual character data get allocated once they outgrow a tiny built-in size.

Items 1 and 2 are **fixed** — the cost of the program simply existing, unaffected by request count. That's the "constant" part.

**The sharper reason item 4 doesn't make RSS climb per request** — one level deeper than "the C++ objects get destroyed" (true, but not the whole story): when `raw_request` needs space, it doesn't ask the operating system directly — it asks your process's *own* internal memory manager (glibc's allocator). When `handle_client()` returns and `raw_request` is destroyed, that memory goes back to the process's own allocator — but the allocator usually does **not** hand it back to the OS immediately. It keeps it on hand, ready to reuse for the next request.

<div class="analogy">Like a toolbox kept at your desk instead of a hardware store. You take a tool out, use it, put it back in the <i>same</i> box — the box on your desk doesn't get bigger each time, because you're reusing it, not buying a new one per task and stacking them up. The first couple of requests made Pulse grab a small toolbox from the OS. Every request since has just been reusing that same toolbox internally — which is why the OS-reported RSS number never climbs, without any code having to manage that explicitly.</div>

### Why does it actually stay flat? (the full mechanism, not just "it did")

Walking through what `handle_client()` genuinely does, per request:

1. `accept()` returns a client fd, wrapped in a `Socket` (one `int`), handed into `handle_client()`.
2. `std::string raw_request;` is a **local variable** living on that one call's stack frame — it grows via `.append()` as bytes arrive, backed by heap memory that *this specific variable* owns.
3. Once parsing succeeds, a local `HttpRequest`, then a local `HttpResponse`, then a local `response_bytes` string get built — still all local to this one call.
4. `handle_client()` returns. Every one of those locals — `raw_request`, the parsed request, the response, and `client` the `Socket` itself — goes out of scope right there. Their destructors run immediately: heap buffers free back to the allocator, and `Socket`'s destructor closes the fd (the exact RAII mechanism from Week 1).
5. The next connection starts this whole cycle again from zero. Nothing survives — there's no global cache, no connection pool, no shared mutable state anywhere in this codebase that a request could add to and leave behind.

Memory doesn't stay flat because of anything done to "clean up." It stays flat because **there is nothing left to accumulate**, by construction — every byte allocated for one request is scoped to that function call and freed the instant it returns, before the next request even begins.

### What is this test actually trying to catch? (the intuition)

`3840 KB` by itself means nothing. What matters is whether it's the *same* number regardless of how many requests have been thrown at it. The failure mode this test exists to catch is a server where memory climbs a little with every request and never comes back down — a leak, typically caused by something like a global list every request appends to with nothing ever clearing it, or an ever-growing cache. If Pulse had that bug, this exact `ps` loop would show `RSS` climbing across the 15 samples instead of holding at `3840` all fifteen times.

<div class="analogy"><b>Why pin this down now, in Week 4 specifically:</b> Pulse is about to stop being this simple. Future months add exactly the kind of state that <i>can</i> leak — connection pools, caches, keep-alive connections held open across requests, thread pools. Once that complexity exists, "is memory flat?" stops being obviously true just by reading the code — you'd have to actually measure it. Having this Week 4 number on record as the honest "before" picture means that if a future change ever makes memory start climbing under this same benchmark, you'll know precisely which change introduced the regression — instead of discovering a leak months later with no idea when it started.</div>

## Benchmark environment (for the record)

| | |
|---|---|
| Machine | Windows 11 PC, running Pulse inside WSL2 (Ubuntu) |
| CPU | 12 logical processors visible to WSL2 (`nproc`) — not necessarily the full physical core count of the host |
| RAM | 3.5 GiB allocated to the WSL2 VM (`free -h`) — not the host's total physical RAM; WSL2 typically gets a subset |
| OS | Ubuntu 26.04 LTS, under WSL2 on Windows 11 — **not bare-metal Linux**, worth stating since a VM layer can add its own overhead/variance |

---

<details>
<summary>Click to expand: the full raw <code>ab</code> output, explained field by field (optional — only if you want every number)</summary>

### Header fields (identical both runs)

| Field | Value | What it means |
|---|---|---|
| Server Software | *(blank)* | Pulse never sends a `Server:` header — see `HttpResponse::to_string()` in `http_response.cpp`. A deliberate gap, not a bug. |
| Server Hostname / Port | `localhost` / `8080` | Which address `ab` pointed at. |
| Document Path | `/` | The route under test. |
| Document Length | `17 bytes` | Body size only — matches `"Hello from Pulse!"` exactly. |

### Every number from both runs

| Metric | `-c 1` | `-c 10` |
|---|---|---|
| Concurrency Level | 1 | 10 |
| Time taken for tests | 0.494 s | 0.186 s |
| Complete requests | 1000 | 1000 |
| Failed requests | 0 | 0 |
| Total transferred | 101000 B | 101000 B |
| HTML transferred | 17000 B | 17000 B |
| Requests per second | 2023.03 | 5371.87 |
| Time per request (mean) | 0.494 ms | 1.862 ms |
| Time per request (mean, across all concurrent) | 0.494 ms | 0.186 ms |
| Transfer rate | 199.54 KB/s | 529.84 KB/s |

**Total transferred → 101000 / 1000 = 101 bytes per response** (headers + body). Cross-check: Week 2 testing measured `response.to_string()` as exactly 101 bytes for this route — matches.

**HTML transferred → 17000 / 1000 = 17 bytes per response** (body only). `101 - 17 = 84 bytes` of headers per response.

**The two "Time per request" lines**, which look confusingly similar: the plain **mean** (`1.862 ms` at `-c 10`) is how long one real request took, including time spent waiting its turn — use this for latency. The **"mean, across all concurrent requests"** (`0.186 ms`) is just `(time taken) / (requests)` — an average-throughput number, not a real wait time. Relationship: `1.862 / 10 = 0.186`. Sanity check: `1000 / 0.186s = 5371/s`, matching requests/sec exactly.

### Connection Times (ms)

| | Connect | Processing | Waiting | Total |
|---|---|---|---|---|
| **`-c 1`** min/mean[sd]/median/max | 0/0[0.3]/0/8 | 0/0[0.2]/0/7 | 0/0[0.2]/0/7 | 0/0[0.5]/0/**14** |
| **`-c 10`** min/mean[sd]/median/max | 0/0[0.2]/0/1 | 0/1[0.6]/1/4 | 0/1[0.6]/1/4 | 1/2[0.5]/2/**4** |

*Connect* = time for the TCP handshake. *Processing* = request sent → full response received. *Waiting* = request sent → first response byte (nearly identical to Processing here, responses are too small for a gap). *Total* = Connect + Processing.

*min/mean/sd/median/max* are plain statistics: fastest, average, how spread-out the numbers are, the middle value, and worst case, respectively.

<div class="example"><b>Interesting outlier:</b> at <code>-c 1</code>, Connect max = 8ms and Total max = 14ms, despite both means being ~0ms — almost certainly the very first request, before anything "warmed up." At <code>-c 10</code>, max Total is only 4ms — tighter and more consistent.</div>

### Percentage of requests served within a certain time (ms)

| Percentile | `-c 1` | `-c 10` |
|---|---|---|
| 50% / 66% / 75% / 80% / 90% | 0 | 2 |
| 95% / 98% | 1 | 3 |
| 99% | 1 | 4 |
| 100% (longest) | **14** | 4 |

This is a **tail latency** view: "95% → 1" means 95% of all 1000 requests finished in 1ms or less — it shows how bad it gets for the unlucky few, which a single average hides. At `-c 1`, the jump from 99% (1ms) to 100% (14ms) is that same cold-start outlier. At `-c 10`, the worst case (4ms) is barely worse than the 99th percentile — a smoother, more predictable spread, even though every number here is higher than `-c 1`'s typical case (the queuing effect from Prediction 1).

</details>

---

# Commands run this week

```bash
# 1. install benchmark tool
sudo apt install -y apache2-utils

# 2. fresh build
cd /mnt/c/Users/aagam/OneDrive/Desktop/Pulse
mkdir -p build && cd build
cmake ..
make

# 3. start server (left running in its own terminal)
./pulse 8080

# 4. the two benchmark runs
ab -n 1000 -c 1 http://localhost:8080/
ab -n 1000 -c 10 http://localhost:8080/

# 5. CPU/mem snapshots
for i in 1 2 3 4 5; do ps -o pid,%cpu,%mem,rss,cmd -p $(pgrep -f 'pulse 8080' | head -1); sleep 0.5; done

# 6. machine info
nproc
free -h
cat /etc/os-release | grep PRETTY_NAME
```

---

# What's left

- [x] Predictions written and checked against reality
- [x] Benchmark run (`-c 1` and `-c 10`)
- [x] `ps` CPU/mem snapshot (step 5) — caveat: confirm it overlapped with an actual `ab` run
- [x] Machine info (step 6)
- [x] Code review of all files (nothing flagged)
- [ ] README architecture + benchmark section
- [ ] Month 1 Final Demo checklist
- [ ] Month 1 Success Criteria checklist

Next: draft the README's architecture + benchmark section, then go through the two Month 1 checklists together.
