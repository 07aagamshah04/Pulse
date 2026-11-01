---
title: "Pulse — Month 2, Week 4: Results"
---

<style>
details { background:#fafafa; border:1px solid #ddd; border-radius:6px; padding:0.3em 1.2em 1em; margin:1.2em 0; }
summary { cursor:pointer; font-weight:600; padding:0.6em 0; }
.key { background:#fff8e1; border-left:4px solid #f0c040; padding:0.8em 1.2em; margin:1.2em 0; }
</style>

# How to run the tests

```bash
cmake -S . -B build && cmake --build build                                         # build Month 2
git worktree add ../pulse-month1 dc0ce76                                            # get Month 1 back from git
cmake -S ../pulse-month1 -B ../pulse-month1/build && cmake --build ../pulse-month1/build
```

Run **one Pulse at a time**, with its log sent to `/dev/null`:

| Test | Terminal 1 | Terminal 2 |
|---|---|---|
| A | `./build/pulse 8080 > /dev/null 2>&1` | `python3 Month2/Week4/hold_connections.py --port 8080 --counts 100 500 1000 2000` |
| B | `./build/pulse 8080 > /dev/null 2>&1` | `bash Month2/Week4/bench.sh month2 8080` |
| C | `../pulse-month1/build/pulse 8080 > /dev/null 2>&1` | `bash Month2/Week4/bench.sh month1 8080` |
| D | repeat B, then C | |

When finished: `git worktree remove ../pulse-month1`.

---

# Test A — What does each open connection cost?

**What we did:** opened 100, then 500, 1,000 and 2,000 connections to Month 2 Pulse, sending nothing on them (like Week 1's silent `nc` client, many times over). We measured Pulse before, while they were open, and after they were closed.

**What we saw:**

| For each connection | Cost |
|---|---|
| Threads | **1** |
| Open files (fds) | **1** |
| Real memory | **about 49 KB** |
| Kernel stack (part of that memory) | 16 KB |
| Reserved memory (not real, just booked) | 8 MB |
| CPU while the connection sits idle | **none** |

**What it means:**

- **Every connection costs the same, every time.** 2,000 connections cost exactly twice what 1,000 did. Nothing gets cheaper with more connections. That's the basic problem with one thread per connection: the cost keeps growing with no ceiling.
- **Idle connections don't use CPU.** Their threads are asleep, waiting for data, so the CPU ignores them. What they do use is memory and file descriptors.
- **2,000 idle connections ≈ 98 MB of real memory**, about 3% of this machine. That's affordable, but it adds up.
- **After closing them**, threads, files and kernel memory went back to normal. "Reserved" memory stayed higher (~499 MB). That isn't a leak: the C library keeps some memory pools and old thread stacks around to reuse for future threads. Real memory went back to about 5 MB.

**Your predictions:**
- Right: threads (1,001), low CPU, real memory (you said ~38 KB; it was ~49 KB).
- Wrong:
  - fds: 1,004, not 1,003. Four are always open before any client: 0, 1, 2, and 3, the listening socket.
  - Reserved memory: 8 MB per connection, not 38 KB. You swapped reserved and real.
  - Kernel stack: 16 KB, not 8 KB.

<details>
<summary>Raw output</summary>

```
===== N = 100 held connections =====
                      BEFORE      DURING       AFTER   DURING-BEFORE    per conn
threads                    1         101           1             100       1.0
open fds                   4         104           4             100       1.0
VmRSS kB                3840        4992        4596            1152      11.5 kB
VmSize kB               7012      826612      301940          819600    8196.0 kB
KernelStack kB          4320        5920        4304            1600      16.0 kB
MemAvailable kB      3188588     3186480     3186100           -2108      21.1 kB used
system context switches/s while all 100 were idle: 42

===== N = 500 held connections =====
threads                    1         501           1             500       1.0
open fds                   4         504           4             500       1.0
VmRSS kB                4596        9844        4972            5248      10.5 kB
VmSize kB             301940     4367288      498680         4065348    8130.7 kB
KernelStack kB          4304       12272        4508            7968      15.9 kB
MemAvailable kB      3186100     3162748     3184804          -23352      46.7 kB used
system context switches/s while all 500 were idle: 43

===== N = 1000 held connections =====
threads                    1        1001           1            1000       1.0
open fds                   4        1004           4            1000       1.0
VmRSS kB                4972       15980        5024           11008      11.0 kB
VmSize kB             498680     8662028      498812         8163348    8163.3 kB
KernelStack kB          4508       20304        4304           15796      15.8 kB
MemAvailable kB      3184804     3136032     3186204          -48772      48.8 kB used
system context switches/s while all 1000 were idle: 40

===== N = 2000 held connections =====
threads                    1        2001           1            2000       1.0
open fds                   4        2004           4            2000       1.0
VmRSS kB                5024       28320        5760           23296      11.6 kB
VmSize kB             498812    16858424      499208        16359612    8179.8 kB
KernelStack kB          4304       36304        4304           32000      16.0 kB
MemAvailable kB      3186204     3088296     3185344          -97908      49.0 kB used
system context switches/s while all 2000 were idle: 27
```

</details>

---

# Test B — Month 2 with more and more clients

**What we did:** `ab` sent 20,000 `/hello` requests to Month 2, first with 1 client at a time, then 10, 50, 100 and 200 clients at once.

**What we saw:**

| Clients at once | 1 | 10 | 50 | 100 | 200 |
|---|---|---|---|---|---|
| Requests per second | 3,471 | 7,777 | 8,746 | 8,032 | 8,382 |
| Typical wait, p50 (ms) | 0 | 1 | 6 | 12 | 23 |
| Slowest 1%, p99 (ms) | 0 | 2 | 7 | 15 | 27 |
| Main thread busy | 31% | 65% | 76% | 75% | 76% |
| Failed | 0 | 0 | 0 | 0 | 0 |

**What it means:**

- **Speed went up from 1 to 10 clients, then stopped at about 8,000 per second.** After that, adding clients doesn't make Pulse answer faster. It just makes each client wait longer: 1 ms, then 6, 12, 23 ms.
- **The wait is easy to predict:** clients ÷ requests per second. For 100 clients: 100 ÷ 8,032 = 12 ms, and that's exactly what was measured.
- **No request was unlucky.** The slowest 1% were only slightly slower than the typical one. Nobody waited seconds.
- **The main thread (the doorman) was not maxed out.** It got to 76% busy, not 100%. And the whole server used only about 1 of the 6 CPU cores.
- **So what stopped it at 8,000? The testing tool itself.** `ab` runs on a single core, and it was at **99%**: it simply couldn't send requests any faster. Pulse had spare capacity. So 8,000 per second is `ab`'s limit, not Pulse's.

**Your predictions:** left blank.

<details>
<summary>Raw output</summary>

```
label        c     req/s   p50   p95   p99    max failed   main%   proc%  peakThr   peakRSS      cs/s
month2       1    3470.9     0     0     0     16      0     31%     32%       18      6372     39021
month2      10    7777.1     1     2     2     17      0     65%     98%       10      6240     65554
month2      50    8745.9     6     6     7     16      0     76%    120%       39      6232     67547
month2     100    8032.4    12    14    15     24      0     75%    120%       71      5756     63653
month2     200    8382.3    23    26    27     33      0     76%    124%      134      6328     65498
```

`ab`'s own CPU, checked in the same session:

```
c=1    req/s=3453.52  ab_cpu=48%  pulse_main=32%  pulse_total=33%
c=10   req/s=7673.82  ab_cpu=99%  pulse_main=65%  pulse_total=98%
c=100  req/s=7924.04  ab_cpu=99%  pulse_main=75%  pulse_total=121%
c=200  req/s=7830.26  ab_cpu=99%  pulse_main=76%  pulse_total=124%
```

</details>

---

# Test C — Month 1 with the same clients, same session

**What we did:** exactly Test B, but against **Month 1** (one thread, handles one client at a time), straight after Test B.

**What we saw:**

| Clients at once | 1 | 10 | 50 | 100 | 200 |
|---|---|---|---|---|---|
| **Month 1** requests/s | **4,774** | **11,737** | **13,838** | **14,051** | **14,053** |
| Month 2 requests/s (Test B) | 3,471 | 7,777 | 8,746 | 8,032 | 8,382 |
| **Month 1** typical wait, p50 (ms) | 0 | 1 | 4 | 7 | 9 |
| Month 2 typical wait, p50 (ms) | 0 | 1 | 6 | 12 | 23 |
| **Month 1** slowest request, max (ms) | 1 | 2 | 5 | 12 | **1,046** |
| Month 2 slowest request, max (ms) | 16 | 17 | 16 | 24 | **33** |
| Month 1 threads | 1 | 1 | 1 | 1 | 1 |

**What it means:**

- **For quick `/hello` requests, Month 1 is faster, at every level.** It's about 40% faster with 1 client, and about 70% faster with 200. That's a surprise, given Month 2 is the "better" design.
- **Why:** Month 2 creates a **brand-new thread for every request**. That costs about as much time as answering `/hello` itself (Week 4 doc, Part 2). Month 1 never creates a thread. It just answers, then takes the next client. For tiny requests, creating the thread *is* most of the work.
- **But look at the slowest request with 200 clients:** Month 1's took **1,046 ms**, more than a full second. Month 2's slowest was 33 ms. With 200 clients, Month 1's waiting line (128 places) filled up, a few clients were turned away, and they had to retry a second later. Month 2 accepts every client straight away, so nobody gets turned away. It's only a few requests (p99 was still 15 ms), but it's the first sign of Month 1's weakness.
- **Both runs hit `ab`'s limit** (`ab` at 98–99%), so neither number is the server's true maximum. But `ab` could push Month 1 almost twice as fast as Month 2, so the difference is real.
- **Month 1 always had exactly 1 thread** and 3.8 MB of memory, whatever the number of clients.

<div class="key">

**The trade-off, in one sentence:** for **fast** requests, thread-per-connection is *slower* (paying for a new thread every time), but for **slow** clients or slow requests it's *far* better. Week 2 showed that: one silent client froze Month 1 for 30 seconds, but Month 2 answered in 18 ms. Month 2 didn't make Pulse faster; it made Pulse **not get stuck**. Months 3–5 aim to get both: no getting stuck, and no per-request thread cost.

</div>

**Your predictions:** left blank.

<details>
<summary>Raw output</summary>

```
label        c     req/s   p50   p95   p99    max failed   main%   proc%  peakThr   peakRSS      cs/s    ab%
month1       1    4774.0     0     0     0      1      0     48%     48%        1      3840     30668    72%
month1      10   11736.8     1     1     1      2      0     58%     58%        1      3840      9241    98%
month1      50   13838.2     4     4     4      5      0     59%     59%        1      3840      3206    98%
month1     100   14050.7     7     8     8     12      0     60%     60%        1      3840      2236    98%
month1     200   14053.4     9    14    15   1046      0     60%     60%        1      3840      1925    98%
```

</details>

---

# Test D — Repeat B and C (run by Claude)

**What we did:** ran Test B (Month 2) and Test C (Month 1) a second time, back to back, a few minutes after the first runs. This checks whether the difference between Month 1 and Month 2 is real, or could just be the numbers moving around by themselves.

**What we saw** (requests per second, first run → second run):

| Clients at once | 1 | 10 | 50 | 100 | 200 |
|---|---|---|---|---|---|
| Month 2 | 3,471 → 3,605 | 7,777 → 8,190 | 8,746 → 8,928 | 8,032 → 8,799 | 8,382 → 8,708 |
| Month 1 | 4,774 → 4,789 | 11,737 → 11,478 | 13,838 → 13,717 | 14,051 → 13,762 | 14,053 → 13,760 |
| Month 1's slowest request (ms) | 1 → 1 | 2 → 3 | 5 → 5 | 12 → 9 | **1,046 → 1,021** |

**What it means:**

- **The same server moves by up to about 10% between two runs** (Month 2 at 100 clients: 8,032 → 8,799). Month 1 moved by less than 3%.
- **The gap between Month 1 and Month 2 is much bigger than that:** Month 1 was 30–60% faster in *both* runs, at every level. So it's real, not noise.
- **Month 1's ~1-second slow request at 200 clients happened again** (1,046 ms, then 1,021 ms). It's not a fluke. Every time, its waiting line overflows and a few clients have to retry a second later. Month 2's slowest request was 33 ms both times.
- **Every conclusion from Tests B and C holds.** Month 1 is faster for quick requests, Month 2 never makes anyone wait seconds, and `ab` was the limit in both cases (98–99% busy).

<details>
<summary>Raw output</summary>

```
label        c     req/s   p50   p95   p99    max failed   main%   proc%  peakThr   peakRSS      cs/s    ab%
month2       1    3605.2     0     0     0      1      0     32%     33%        2      4352     40271    48%
month2      10    8189.6     1     1     2      2      0     66%    101%        7      4708     69087    99%
month2      50    8928.2     6     6     8     15      0     77%    120%       36      5036     67855    99%
month2     100    8799.1    11    12    13     15      0     78%    123%       63      5608     68578    99%
month2     200    8707.6    23    26    27     33      0     78%    127%      132      6320     67944    99%

label        c     req/s   p50   p95   p99    max failed   main%   proc%  peakThr   peakRSS      cs/s    ab%
month1       1    4788.6     0     0     0      1      0     48%     48%        1      3840     29728    72%
month1      10   11477.6     1     1     1      3      0     57%     57%        1      3840      9103    98%
month1      50   13717.1     4     4     4      5      0     58%     58%        1      3840      3180    98%
month1     100   13761.8     7     8     8      9      0     60%     60%        1      3840      2213    98%
month1     200   13760.3    10    14    14   1021      0     59%     59%        1      3840      1889    98%
```

</details>

---

# Week 4 summary

| | Month 1 (one thread) | Month 2 (thread per connection) |
|---|---|---|
| Quick requests (`/hello`) | **faster**: ~14,000 req/s | ~8,500 req/s: pays for a new thread every request |
| Slowest request, 200 clients | ~1 second (waiting line overflows) | **33 ms** |
| A slow or silent client | **freezes everyone** (Week 2: 30 s) | **only affects itself** (18 ms) |
| Memory per open connection | n/a (one at a time) | ~49 KB + 1 thread + 1 fd, growing with every connection |
| Most open connections at once | 1 | ~10,000 on this machine (open-file limit) |

**Month 2 didn't make Pulse faster. It made Pulse not get stuck,** at the price of a thread per connection: extra time for every request, and memory that grows with every open connection. Months 3–5 aim to keep "not getting stuck" without paying for a thread per connection.
