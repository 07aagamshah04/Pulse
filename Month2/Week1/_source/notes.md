---
title: "Pulse — Month 2, Week 1: Experiment Log"
subtitle: "What I ran, what I predicted, what actually happened, and why"
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
.pending { background:#f3f3f3; border:1px dashed #aaa; border-radius:6px; padding:0.6em 1em; color:#555; }
details { background:#fafafa; border:1px solid #ddd; border-radius:6px; padding:0.3em 1.2em 1em; margin:1.5em 0; }
details[open] { padding-bottom:1.2em; }
summary { cursor:pointer; font-weight:600; padding:0.8em 0; }
</style>

# How this file works

This is the permanent record of Week 1's experiments: what was run, every command explained, what was predicted, what really happened, and why.

For each experiment:

1. **Before running:** write predictions in `notes.txt` (the scratchpad next to this file).
2. **Run** the commands listed under that experiment.
3. **Paste** the full raw output into the chat.
4. The results get written up here: predictions vs reality, every field explained, raw output kept at the bottom of the section.

| # | Experiment | Status |
|---|---|---|
| 0 | One silent client freezes Month 1 Pulse | **Done** |
| 1 | A and B: who runs first? | **Done** |
| 2 | 1 / 2 / 4 / 8 / 16 busy threads | **Done** |
| 3 | Sleeping threads | **Done** |

---

# Setup: commands used in every experiment

```bash
wsl -d Ubuntu
cd /mnt/c/Users/aagam/OneDrive/Desktop/Pulse
```

- **`wsl -d Ubuntu`**: from Windows, open a shell inside the Ubuntu Linux VM. `-d` = "distribution": which Linux install to start, since WSL can have several.
- **`cd /mnt/c/...`**: inside WSL, your Windows `C:` drive appears at `/mnt/c`. The project folder is the same files Windows sees.

Compiling an experiment:

```bash
g++ -std=c++17 -Wall -pthread FILE.cpp -o FILE.out
```

| Piece | Meaning |
|---|---|
| `g++` | The C++ compiler |
| `-std=c++17` | Use the C++17 language version (same as Pulse's `CMakeLists.txt`) |
| `-Wall` | "Warnings: all". Report suspicious code, not just errors |
| `-pthread` | Link the POSIX threads library that `std::thread` is built on (Concepts doc, Part 7). Without it, thread code can fail to link |
| `-o FILE.out` | Name of the output program. `.out` because `.gitignore` ignores `*.out`, so binaries never get committed |

**No `-O2`.** At the default optimization level (`-O0`), the compiler leaves busy loops alone. With `-O2` it can notice a loop's answer can be calculated without looping and delete the work, and then Experiment 2 would measure nothing.

---

# Experiment 0 — One silent client freezes Month 1 Pulse

**Pulse question:** what does "only one thread" look like from the outside?
**Why it matters:** this is the **before** picture. In Week 2, the exact same commands run against thread-per-connection Pulse, and the two results get compared side by side.

## What happened, in plain English

<div class="tldr">
<b>What I did:</b> opened a connection to Pulse that sends nothing (<code>nc</code>), then sent a perfectly normal <code>GET /hello</code> with <code>curl</code>.
<br><br>
<b>What happened:</b> <code>/hello</code>, which normally answers in about <b>0.02 seconds</b>, waited the full <b>30 seconds</b> and got <b>nothing</b>. Pulse wasn't busy and wasn't using any CPU. Its one and only thread was asleep inside <code>read()</code>, waiting for the silent client to say something. The moment the silent client was closed, <code>/hello</code> answered instantly again.
<br><br>
<b>The key insight:</b> the kernel had already done everything for curl: connection set up, all 83 bytes of the request received. Pulse just never came to pick it up, because its one thread was stuck somewhere else.
</div>

## The 3 numbers that matter

<div class="stat-row">
<div class="stat-card">
<div class="label">Time for GET /hello</div>
<div class="values">0.023s <span class="up">&rarr;</span> 30.04s</div>
<div class="note">normal vs. with a silent client connected (gave up, no reply)</div>
</div>
<div class="stat-card">
<div class="label">Threads in Pulse</div>
<div class="values"><span class="flat">1</span></div>
<div class="note">only one place it can wait from at a time</div>
</div>
<div class="stat-card">
<div class="label">curl's request, unread in the kernel</div>
<div class="values">83 bytes</div>
<div class="note">fully arrived, never read by Pulse</div>
</div>
</div>

<div class="analogy">
<b>Analogy:</b> a shop with one shopkeeper. Customer 1 walks to the counter and just stands there silently. The shopkeeper, by the rules of this shop, waits for them to speak. Customer 2 is in the queue holding a written note with their full order (the 83 bytes). The shopkeeper is not busy, they're just waiting, and customer 2 eventually gives up and leaves. Week 2's fix is a shopkeeper per customer.
</div>

## Predictions vs reality

| Prediction | What I predicted | What actually happened | Verdict |
|---|---|---|---|
| Threads | *(left blank)* | `1` | n/a |
| curl /hello with silent client connected | times out | timed out after 30.04 s, `http_code=000`, exit code `28` | <span class="verdict-right">Right</span> |
| STAT and WCHAN of Pulse's thread | Sleep, waiting | `S+`, `wait_woken` | <span class="verdict-right">Right</span> |
| LISTEN `Recv-Q` | 0 | **1** | <span class="verdict-wrong">Wrong</span> |
| curl connection's `Recv-Q` | 83 | 83 | <span class="verdict-right">Right</span> |

### The wrong one, and why it's the most useful result here

The prediction was LISTEN `Recv-Q 0`, probably reasoning "Pulse hasn't accepted curl, so curl isn't connected yet, so nothing's there." The actual answer is **1**, and the reason is:

**The kernel connects clients by itself, without Pulse.** When curl connects, the TCP handshake (SYN → SYN-ACK → ACK) is done entirely by the Linux kernel. Pulse's code isn't involved at all. Once the handshake finishes, the kernel puts that finished connection in the **accept queue** (the backlog you set with `listen(fd, 128)`). `accept()` doesn't create a connection. It just **picks up an already-finished one from that queue**.

So:

- LISTEN `Recv-Q 1` = one fully finished connection (curl's) sitting in the accept queue, waiting for Pulse to call `accept()`.
- curl's connection shows up as `ESTABLISHED`, with `Recv-Q 83`, **even though Pulse never accepted it**. The kernel even received its 83-byte request and buffered it.

> **"Connected" and "accepted" are different things.** Connected = the kernel finished the handshake. Accepted = your program picked it up. In Month 1 Pulse, the gap between them can be as long as the slowest client before it.

### The blank one: Threads

`1`. Month 1 Pulse never creates a thread, so it only has the one every process starts with, the **main thread**, running `main()`. Even with two clients connected, there is one thread, so there is only one place Pulse can be waiting at any moment.

## What happened, step by step (with Pulse's own log)

Pulse's log from Terminal 1:

```
Pulse listening on port 8080...
Client connected: 127.0.0.1:33978
Client disconnected before completing a request (0 bytes received so far).
Client connected: 127.0.0.1:38914
Parsed (83 bytes total): GET /hello HTTP/1.1
Client connected: 127.0.0.1:44260
Parsed (83 bytes total): GET /hello HTTP/1.1
```

The numbers after `127.0.0.1:` are each **client's port**: a random port the client's OS picked for its side of the connection. That's how you tell clients apart in the log and in `ss`:

| Client port | Who | What Pulse did |
|---|---|---|
| `33978` | `nc`, the silent client | `accept()`ed it, went into `handle_client`, and sat in `read()` waiting. **This is where Pulse was stuck for 30 seconds.** |
| `38914` | **First** curl (the one that timed out) | Not accepted until `nc` was closed. |
| `44260` | Second curl (after `nc` was closed) | Accepted and answered immediately: 0.023 s. |

The full timeline:

1. **`nc` connects (33978).** Pulse `accept()`s it, calls `handle_client`, reaches `::read()` and **sleeps**. nc sends nothing.
2. **curl connects (38914).** The kernel finishes the handshake on its own and puts the connection in the accept queue (LISTEN `Recv-Q 1`). curl sends its 83-byte request, and the kernel stores it (curl's `Recv-Q 83`). Pulse knows none of this. It's asleep in `read()` for nc.
3. **30 seconds pass.** curl gives up (`-m 30`), closes its side, prints `http_code=000`, exit code `28`.
4. **Ctrl+C on nc.** nc closes its connection. Pulse's `read()` wakes up and returns `0` (= "client closed"), so Pulse logs *"Client disconnected before completing a request (0 bytes received so far)"*, closes it, and loops back to `accept()`.
5. **Pulse finally `accept()`s 38914**, the first curl, which **already gave up**. Its 83 bytes are still waiting in the kernel, so Pulse reads them, logs *"Parsed (83 bytes total): GET /hello"*, and writes `Hello from Pulse!` back to a client that's no longer listening.
6. **Second curl (44260)** arrives with nothing ahead of it, so it's accepted, parsed, and answered in 0.023 s.

Step 5 is worth noticing: Pulse did the work for the first curl **30 seconds late**, for nobody. With one thread, slow clients don't just delay others. They can make the server do work that's already useless by the time it happens.

## Every command, explained

### Terminal 1: build and run Pulse

```bash
mkdir -p build && cd build && cmake .. && make && ./pulse 8080
```

- **`&&`** = "run the next command **only if** the previous one succeeded." If `make` fails, `./pulse` never runs with an old binary.
- **`mkdir -p build`**: make the `build` folder. `-p` = no error if it already exists.
- **`cmake ..`** then **`make`**: generate the build files from `CMakeLists.txt` one folder up (`..`), then compile (explained in the README).
- **`./pulse 8080`**: run the program from *this* folder (`./`), with port `8080` as `argv[1]`.

### Terminal 2: the silent client

```bash
nc localhost 8080
```

- **`nc`** (netcat) opens a raw TCP connection and connects your keyboard to it. Nothing is sent until you type. By not typing, it becomes a client that's connected but silent: the "slow client" in its purest form.

### Terminal 3: the timed request

```bash
time curl -s -m 30 -w "\nhttp_code=%{http_code}\n" localhost:8080/hello; echo "curl exit code: $?"
```

| Piece | Meaning |
|---|---|
| `time` | Run the command after it, then print how long it took (`real`, `user`, `sys`, below) |
| `curl` | Make an HTTP request |
| `-s` | Silent: don't show curl's progress bar |
| `-m 30` | Max time: give up after 30 seconds total |
| `-w "...%{http_code}..."` | Write out extra info after the response. `%{http_code}` = the status code curl received (`200`, `404`…), or `000` if it never got one |
| `;` | Run the next command **no matter what** (unlike `&&`). We want the exit code even when curl fails |
| `echo "... $?"` | `$?` = the **exit code** of the previous command. `0` = success, anything else = a specific failure |

**curl's exit codes:** `0` = success (second run). `28` = "Operation timeout" (first run): the `-m 30` limit was hit.

**Reading `time`'s three numbers.** This connects directly to threads:

| | Stuck run | Fast run | Meaning |
|---|---|---|---|
| `real` | **30.044 s** | 0.023 s | Wall-clock time: what a stopwatch would show |
| `user` | 0.014 s | 0.004 s | CPU time spent running curl's own code |
| `sys` | 0.022 s | 0.017 s | CPU time the kernel spent working for curl (syscalls) |

In the stuck run, `real` was 30 s but `user + sys` was only **0.036 s**. curl spent 99.9% of those 30 seconds **asleep**, waiting, using no CPU. Pulse was doing exactly the same thing at the same time (`STAT S`, below). Two programs, both asleep, both waiting on someone else, with an idle CPU. That's the waste Month 2 removes.

### Terminal 4: looking inside Pulse

```bash
P=$(pgrep -x pulse)
```

- **`pgrep -x pulse`**: print the PID of the process named exactly (`-x`) `pulse`. Without `-x`, it would also match anything with "pulse" *in* its name.
- **`$( ... )`** = **command substitution**: run the command inside, and put its output here.
- **`P=...`**: store it in a shell variable named `P`. (No spaces around `=`, which bash requires.) After this, **`$P`** means "Pulse's PID," so you don't have to copy the number by hand.

```bash
grep Threads /proc/$P/status
```

- **`/proc`** is a fake filesystem: the files aren't on disk. The kernel generates them on the fly when you read them. **`/proc/<PID>/status`** is a live, readable summary of one process.
- **`grep Threads`**: show only lines containing "Threads".
- **Output `Threads: 1`**: the number of threads in Pulse right now.

```bash
ps -L -o tid,stat,wchan:22,comm -p $P
```

| Piece | Meaning |
|---|---|
| `ps` | List processes |
| `-L` | One row **per thread** instead of per process |
| `-o tid,stat,wchan:22,comm` | Choose exactly which columns to show |
| `-p $P` | Only this process |

Output: `773   S+   wait_woken   pulse`

| Column | Value | Meaning |
|---|---|---|
| `TID` | `773` | The thread's ID. With only one thread, it's the main thread, so TID = PID (Concepts doc, Part 6) |
| `STAT` | `S` | **Sleeping**: blocked waiting for an event, using **zero CPU** (Concepts doc, Part 5) |
|  | `+` | It belongs to the **foreground** of its terminal (it's the program you launched in Terminal 1 and are "in"). Not about threads, just where it's running |
| `WCHAN` | `wait_woken` | "Wait channel": **the kernel function the thread is parked in**. `wait_woken` = generic "sleep until someone wakes me." Here that someone is the network code, when data arrives on the socket. It's the kernel-side half of the `::read()` call on line 232 of `tcp_server.cpp` |
| `COMMAND` | `pulse` | Program name |

`:22` in `wchan:22` just makes that column 22 characters wide so long function names don't get cut off.

```bash
ss -ltn "sport = :8080"
```

- **`ss`** = "socket statistics": list the sockets the kernel knows about.
- **`-l`** = listening sockets only, **`-t`** = TCP only, **`-n`** = show numbers (`8080`) instead of guessing service names (like `http-alt`).
- **`"sport = :8080"`** = a filter: only sockets whose **s**ource (local) **port** is 8080.

Output: `LISTEN   1   128   0.0.0.0:8080   0.0.0.0:*`

| Column | Value | Meaning **for a LISTEN socket** |
|---|---|---|
| `State` | `LISTEN` | Pulse's listening socket (fd 3) |
| `Recv-Q` | **`1`** | Finished connections waiting in the accept queue for `accept()`. That's curl. |
| `Send-Q` | `128` | The **maximum** size of that queue: your `backlog = 128` from `TcpServer`'s constructor |
| Local Address | `0.0.0.0:8080` | Listening on all interfaces (`INADDR_ANY`), port 8080 |
| Peer Address | `0.0.0.0:*` | No specific peer: a listening socket waits for anyone |

```bash
ss -tn state established "sport = :8080"
```

- **`state established`**: only fully connected sockets (handshake done). There's no `-l`, because these aren't listening sockets.

Output:

```
Recv-Q   Send-Q   Local Address:Port   Peer Address:Port
0        0        127.0.0.1:8080       127.0.0.1:33978     ← nc
83       0        127.0.0.1:8080       127.0.0.1:38914     ← first curl
```

| Column | Meaning **for an ESTABLISHED socket** (different from LISTEN!) |
|---|---|
| `Recv-Q` | Bytes that have **arrived** from the client but your program **hasn't `read()` yet** |
| `Send-Q` | Bytes your program has written that the client **hasn't confirmed receiving** yet |

- **nc's row, `Recv-Q 0`**: it sent nothing, so there's nothing to read. Pulse is asleep in `read()` on *this* one, waiting for bytes that never come.
- **curl's row, `Recv-Q 83`**: its entire request, `GET /hello HTTP/1.1` + headers = 83 bytes (the same "83 bytes total" Pulse logged later), sitting in the kernel, unread.

`Recv-Q` and `Send-Q` mean different things depending on the socket's state. On LISTEN they count *connections*. On ESTABLISHED they count *bytes*. Same column name, different unit.

## What Week 2 should show instead (write this down now, check it next week)

Same experiment, thread-per-connection Pulse:

| | Month 1 (measured above) | Week 2 (expected) |
|---|---|---|
| `curl /hello` with `nc` connected | 30.04 s, timed out | ≈ 0.02 s, `200` |
| `Threads:` while nc is connected | 1 | at least 2 (main thread in `accept()` + nc's thread in `read()`) |
| `ps -L` | one row: `S wait_woken` | main thread asleep in `accept()`, nc's thread asleep in `read()` |
| LISTEN `Recv-Q` | 1 (curl stuck in the queue) | 0 (accepted right away) |
| curl's `Recv-Q` | 83 (never read) | 0 (read immediately) |

## Noticed for later (not this week's problem)

**Writing to a client that already left.** In step 5, Pulse wrote a response to curl after curl had already closed its side. That worked here without an error. The first `write()` to a closed peer usually succeeds (the bytes go into the kernel's send buffer, and the other side answers with a TCP reset). But a **second** write to that same socket after the reset arrives would raise the **`SIGPIPE`** signal, and by default `SIGPIPE` **kills the whole process**. Pulse's responses are small enough to be one `write()`, so it hasn't happened yet. It's a real latent bug, though, and with threads it would kill every connection at once, not just one. It belongs with error handling and robustness later. Logged here so it isn't forgotten.

<details>
<summary>Raw output: Experiment 0 (click to expand)</summary>

**Terminal 1 (Pulse)**

```
aagam@Galu-PC:/mnt/c/Users/aagam/OneDrive/Desktop/Pulse/build$ ./pulse 8080
Pulse listening on port 8080...
Client connected: 127.0.0.1:33978
Client disconnected before completing a request (0 bytes received so far).
Client connected: 127.0.0.1:38914
Parsed (83 bytes total): GET /hello HTTP/1.1
Client connected: 127.0.0.1:44260
Parsed (83 bytes total): GET /hello HTTP/1.1
```

**Terminal 4 (while curl was stuck)**

```
aagam@Galu-PC:/mnt/c/Users/aagam/OneDrive/Desktop/Pulse$ P=$(pgrep -x pulse)
grep Threads /proc/$P/status
ps -L -o tid,stat,wchan:22,comm -p $P
ss -ltn "sport = :8080"
ss -tn state established "sport = :8080"
Threads:        1
    TID STAT WCHAN                  COMMAND
    773 S+   wait_woken             pulse
State           Recv-Q          Send-Q                   Local Address:Port                   Peer Address:Port
LISTEN          1               128                            0.0.0.0:8080                        0.0.0.0:*
Recv-Q            Send-Q                        Local Address:Port                         Peer Address:Port
0                 0                                 127.0.0.1:8080                            127.0.0.1:33978
83                0                                 127.0.0.1:8080                            127.0.0.1:38914
```

**Terminal 3 (curl, both runs)**

```
aagam@Galu-PC:/mnt/c/Users/aagam/OneDrive/Desktop/Pulse$ time curl -s -m 30 -w "\nhttp_code=%{http_code}\n" localhost:8080/hello; echo "curl exit code: $?"

http_code=000

real    0m30.044s
user    0m0.014s
sys     0m0.022s
curl exit code: 28
aagam@Galu-PC:/mnt/c/Users/aagam/OneDrive/Desktop/Pulse$ time curl -s -m 30 -w "\nhttp_code=%{http_code}\n" localhost:8080/hello; echo "curl exit code: $?"
Hello from Pulse!
http_code=200

real    0m0.023s
user    0m0.004s
sys     0m0.017s
curl exit code: 0
```

</details>

---

# Experiment 1 — A and B: who runs first?

**Files:** `Month2/Week1/exp1a_ab.cpp` and `Month2/Week1/exp1b_ab_many.cpp` (Concepts doc Parts 7, 8, 9). Both are written and fully commented. Read the comments in each file before running.
**Pulse question:** if connection A's thread is created before connection B's, does A's code run first? And what does output from several threads at once look like?

## What the programs do

- **1a (`exp1a_ab.cpp`):** creates thread `t1` that prints `A`, then thread `t2` that prints `B`, then joins both. Each thread's whole job is one tiny print.
- **1b (`exp1b_ab_many.cpp`):** same, but each thread prints its letter **2000 times** with no newline. Each thread now runs long enough that both are likely running *at the same time*.

## What I ran (from the Pulse folder in WSL)

```bash
# compile both
g++ -std=c++17 -Wall -pthread Month2/Week1/exp1a_ab.cpp      -o Month2/Week1/exp1a_ab.out
g++ -std=c++17 -Wall -pthread Month2/Week1/exp1b_ab_many.cpp -o Month2/Week1/exp1b_ab_many.out

# 1a: run once to see it, then 100 times and count each order
./Month2/Week1/exp1a_ab.out
for i in $(seq 100); do ./Month2/Week1/exp1a_ab.out | tr -d '\n'; echo; done | sort | uniq -c

# 1b: run 3 times
for i in 1 2 3; do ./Month2/Week1/exp1b_ab_many.out; echo "-----"; done
```

### New commands, explained

| Piece | Meaning |
|---|---|
| `for i in $(seq 100); do ...; done` | A bash loop: `seq 100` prints the numbers 1 to 100, and the loop runs its body once for each |
| <code>&#124;</code> (pipe) | Feed the output of the left command into the input of the right command |
| `tr -d '\n'` | "translate, delete": remove newline characters, so one run's `A` and `B` lines become one line: `AB` or `BA` |
| `echo` | Print a newline, ending that run's line |
| `sort` | Sort all 100 lines, so identical ones end up next to each other |
| `uniq -c` | Collapse runs of identical lines into one, with a **c**ount in front, e.g. `<count> AB` and `<count> BA` |

## What happened, in plain English

<div class="tldr">
<b>1a:</b> out of 100 runs, <b>98 printed <code>AB</code> and 2 printed <code>BA</code></b>. The thread created first <i>usually</i> runs first, but not always. Nothing in the code forces the order.
<br><br>
<b>1b:</b> with 2000 letters each, some runs came out as two clean blocks (<code>AAAA…BBBB</code>), and others had the A's and B's <b>mixed together</b> in the middle. No two runs looked the same.
<br><br>
<b>The key insight:</b> "creation order" is not "execution order". It only gives a head start. And a 98% result is the dangerous kind: a test can pass 98 times and fail the 99th, with no change to the code.
</div>

## The numbers that matter

<div class="stat-row">
<div class="stat-card">
<div class="label">1a: runs that printed A first</div>
<div class="values">98 / 100</div>
<div class="note">2 runs printed B first, from the same program</div>
</div>
<div class="stat-card">
<div class="label">1b: your 3 runs</div>
<div class="values">2 mixed, 1 clean</div>
<div class="note">same program, three different outputs</div>
</div>
<div class="stat-card">
<div class="label">1b: blocks per run (10 extra runs)</div>
<div class="values">2 &rarr; 376</div>
<div class="note">2 = perfectly clean, 376 = heavily mixed</div>
</div>
</div>

<div class="analogy">
<b>Analogy:</b> two runners, but the starter says "go" to runner A, then spends a moment walking over to runner B to say "go" again. In a 10-metre race (1a: one print), A has almost always finished before B has even started, so it looks like "A always wins". But B still wins occasionally, maybe because A tripped (the scheduler paused A's thread). In a long race (1b: 2000 prints), A's head start gets eaten up, and for a stretch both are running side by side, overtaking each other. That's the mixed middle section.
</div>

## Predictions vs reality

| Prediction | What I predicted | What actually happened | Verdict |
|---|---|---|---|
| 1a: AB vs BA out of 100 | "can't be determined" | 98 AB, 2 BA | <span class="verdict-right">Right in principle</span>, but the split was much more lopsided than "can't tell" suggests (see below) |
| 1a: does creating t1 first matter? | No | It **biases** the result heavily (98%), but doesn't **guarantee** it (2 runs went BA) | <span class="verdict-right">Half right</span>: it doesn't *decide* the order, but it *does* matter |
| 1b: clean blocks or mixed? | Mixed | Runs 1 and 2 mixed. Run 3 looked like two clean blocks | <span class="verdict-right">Right</span>: mixing happens, but not on every run |
| 1b: will all 3 runs look the same? | *(unclear answer)* | No, all 3 different | n/a |
| Week 2: Pulse's log with two threads printing | Mixed | That's exactly what 1b shows (explained below) | <span class="verdict-right">Right</span> |

### The interesting one: "can't be determined" vs 98 / 2

The prediction was correct in the way that matters. **For any single run, you can't know** whether it will be AB or BA. But over 100 runs the result wasn't 50/50. It was 98/2. Why so lopsided?

Look at what the main thread does, in order:

```
main:  [create t1 ......][create t2 ......][join t1][join t2]
t1:                     [print A]                             ← starts here, done in ~microseconds
t2:                                       [print B]           ← can't start until t2 is even created
```

1. `std::thread t1(...)` creates t1. Creating a thread is a real system call (`clone()`, Concepts doc Part 6), and it takes time, on the order of tens of microseconds.
2. **t1 starts running immediately**, while main is *still* busy creating t2. t1's whole job is to print one letter, which takes a few microseconds.
3. So by the time t2 even *exists*, t1 has usually already finished printing `A`.

That's a **head start**, not a rule. In the 2 `BA` runs, something got in t1's way. For example, the scheduler hadn't put t1 on a CPU yet when t2 started, or t1 got paused for a moment by something else running on the machine. Nothing in the code prevented it, so occasionally it happened.

> **Why 98% is the dangerous number.** If the code *needed* A to come first (say, A sets something up that B uses), it would work 98 times out of 100. You'd test it a few times, see it working, and ship it. Then it fails "randomly". This is the same lesson as Month 1 / Week 3's single `read()`: "usually works in testing" is not "correct". The only way to *guarantee* an order between threads is to make them coordinate, which is Week 3 (mutex, condition_variable).

## 1b: why the output looks the way it does

### The shape of a mixed run

Your runs 1 and 2 both have the same three-part shape:

```
AAAAAAAAAAAAAAAA……AAAAAAAAAA  ABBABAABBBBAAAB……ABBBA  BBBBBBBBBBB……BBBBBBBBBBB
└── t1 alone ──────────────┘  └── both running ────┘  └── t2 alone ──────────┘
   (head start, same reason      (t1 and t2 really        (t1 has finished its
    as 1a: t2 doesn't exist       running at the same      2000, only t2 is left)
    yet)                          time)
```

1. **Long block of A's at the start**: t1's head start. t2 hasn't been created yet, exactly as in 1a.
2. **Mixed middle**: both threads are alive and printing *at the same time*. With 12 CPUs, they're probably on two different CPUs, so this is truly **parallel**, not just taking turns. Whichever thread gets to `std::cout` first puts its letter in next.
3. **Long block of B's at the end**: t1 has printed all 2000 and finished, and t2 is finishing on its own.

Run 3 looked like two clean blocks: t1 printed all 2000 A's before t2 printed its first B. 2000 single-character prints is fast enough that sometimes t1 finishes its whole job before t2 gets going. Then it behaves exactly like 1a.

### Measuring it instead of eyeballing it

To put numbers on "how mixed", I ran 1b ten more times, counting the **blocks** in each run (an unbroken run of the same letter = 1 block). 2 blocks means perfectly clean `AAAA…BBBB`. More blocks means more mixing.

```
run 1: blocks=2    first=Ax2000  last=Bx2000  total=4000
run 2: blocks=60   first=Ax1600  last=Bx1915  total=4000
run 3: blocks=2    first=Ax2000  last=Bx2000  total=4000
run 4: blocks=2    first=Ax2000  last=Bx2000  total=4000
run 5: blocks=2    first=Ax2000  last=Bx2000  total=4000
run 6: blocks=2    first=Ax2000  last=Bx2000  total=4000
run 7: blocks=376  first=Ax638   last=Bx296   total=4000
run 8: blocks=2    first=Ax2000  last=Bx2000  total=4000
run 9: blocks=5    first=Ax1417  last=Ax582   total=4000
run 10: blocks=2   first=Ax2000  last=Bx2000  total=4000
```

| Field | Meaning |
|---|---|
| `blocks` | How many unbroken runs of one letter. `2` = clean, `376` = the threads switched back and forth 375 times |
| `first=Ax1600` | The first block was 1600 A's: t1 got 1600 letters out before t2 printed anything (the head start) |
| `last=Bx1915` | The last block was 1915 B's: t2 printed that many alone after t1 had finished |
| `total=4000` | Always exactly 4000 letters (2000 + 2000). **No letter was ever lost or garbled**, only reordered |

Three things stand out:

- **Run 7** had only a 638-letter head start, then 376 blocks. The two threads were neck and neck for most of the run.
- **Run 9 *ended* with A's** (`last=Ax582`). t2 finished its 2000 B's **before** t1 finished its A's, even though t1 started first with a 1417-letter lead. Being created first doesn't even guarantee finishing first.
- **7 of 10 were clean here, but 2 of your 3 were mixed.** My runs sent the output into a pipe; yours printed straight to the terminal. Where the output goes changes how long each print takes, which changes how much the threads overlap. That's one more sign the result depends on **timing**, not on the code.

### Why letters mix but never get garbled

Each `std::cout << 'A'` is one separate call. The C++ standard guarantees that two threads using `std::cout` at the same time won't *corrupt* it: every letter comes out whole, and the total is always 4000. It does **not** guarantee whose letter goes next. So between any two of t1's letters, a letter from t2 can slip in.

## What this means for Pulse (Week 2)

**1. The order connections are handled in is not guaranteed.** If client A connects slightly before client B, A's thread is created first and *usually* runs first, the same 98% head start as 1a. But Pulse's code must never *depend* on that. Each connection must be handled correctly no matter which thread runs when.

**2. Pulse's logs will mix, like 1b.** One log line in `handle_client` is about ten separate `<<` calls:

```cpp
std::cout << "Parsed (" << raw_request.size() << " bytes total): "
          << result.request.method << " " << result.request.path << " " << result.request.version << "\n";
```

1b mixed at single-letter granularity, so Pulse's ten pieces can mix too, when two connections log at the same moment:

```
Parsed (Parsed (83 bytes total): 83 bytes total): GET GET /hello /health HTTP/1.1
HTTP/1.1
```

**3. It will be rare in light testing and common under load.** 1a (short job) almost never overlapped. 1b (longer job) overlapped often. A single `curl` at a time will almost never show mixed logs. A benchmark with many simultaneous clients will. That's the same "rare in testing, real in production" pattern again.

The fix (making each whole log line one uninterruptible unit, using a mutex) is Week 3. For Week 2, mixed logs are expected and harmless. They're the first visible sign of threads sharing something, here `std::cout`.

<details>
<summary>Raw output: Experiment 1 (click to expand)</summary>

**1a**

```
aagam@Galu-PC:/mnt/c/Users/aagam/OneDrive/Desktop/Pulse$ ./Month2/Week1/exp1a_ab.out
for i in $(seq 100); do ./Month2/Week1/exp1a_ab.out | tr -d '\n'; echo; done | sort | uniq -c
A
B
     98 AB
      2 BA
```

**1b**: 3 runs of 4000 letters each. The full 12,000-character output is too long to keep here letter by letter, so this is a description of each run's shape (the complete output is in the chat):

| Run | Shape |
|---|---|
| 1 | Long block of A's → a mixed section where A and B alternate in short clumps → a stretch of mostly A's → long block of B's to the end |
| 2 | Long block of A's → a mixed section → a long block of B's, broken by one more short mixed patch near the end → B's to the end |
| 3 | Looks like two clean blocks: A's, then B's, with no visible switching back |

To keep exact numbers next time, use the block-counting command from the "Measuring it" section above. It turns a run into one line of numbers instead of 4000 letters:

```bash
out=$(./Month2/Week1/exp1b_ab_many.out); echo -n "$out" | grep -o 'A\+\|B\+' | wc -l
```

</details>

---

# Experiment 2 — 1 / 2 / 4 / 8 / 16 busy threads


**File:** `Month2/Week1/exp2_busy.cpp`, written and fully commented (Concepts doc Parts 5, 6, 7, 8). Read the comments before running.
**Pulse question:** what happens when threads actually *need* the CPU at the same time? Does `join()` make them run one after another? And what changes when there are more threads than CPUs?


## What the program does

```
./exp2_busy.out <threads> [rounds]
```

- Creates `<threads>` threads, all stored in a `std::vector<std::thread>`.
- Each thread does pure CPU work: 1.5 billion additions per round, with **no waiting at all** (no `read()`, no sleep). One thread, one round ≈ **2.5 s** on this machine.
- `main` creates **all** the threads first, then joins all of them, then prints the total time.
- Each thread prints its index and its **TID** when it finishes, so you can match it to the rows in `ps -L`.
- `[rounds]` is optional (default 1). It repeats the work so a run lasts long enough to watch with `ps`/`pidstat`. It's only used in Parts B and C.

## What to run

All from the Pulse folder in WSL (`cd /mnt/c/Users/aagam/OneDrive/Desktop/Pulse`).

### Part A: timing 1 / 2 / 4 / 8 / 16 threads

```bash
g++ -std=c++17 -Wall -pthread Month2/Week1/exp2_busy.cpp -o Month2/Week1/exp2_busy.out

for n in 1 2 4 8 16; do echo "===== $n threads"; time ./Month2/Week1/exp2_busy.out $n; done
```

This takes about a minute in total. Each run prints the program's own lines, then `time`'s `real` / `user` / `sys`.

### Part B: watch 8 threads (fewer threads than CPUs)

Two terminals, both in the Pulse folder.

```bash
# Terminal 1: start it (4 rounds, so it lasts long enough to look at)
./Month2/Week1/exp2_busy.out 8 4
```

```bash
# Terminal 2: run these straight away while Terminal 1 is still working
P=$(pgrep -n exp2_busy)
grep Threads /proc/$P/status
ps -L -o tid,psr,stat,pcpu,comm -p $P
sleep 2; ps -L -o tid,psr,stat,pcpu,comm -p $P
pidstat -u -t -p $P 1 1
pidstat -w -t -p $P 1 1
```

### Part C: watch 16 threads (more threads than CPUs)

Exactly the same as Part B, but in Terminal 1:

```bash
./Month2/Week1/exp2_busy.out 16 4
```

Then the same 6 commands in Terminal 2.

### Optional: watch it live (no need to paste)

While a Part B or C run is going: `top -H -p $P` (press `1` to see every CPU separately, `q` to quit), or `htop` (`F4` → type `exp2` to filter, `H` toggles showing threads).

### New commands, explained

| Piece | Meaning |
|---|---|
| `for n in 1 2 4 8 16; do ...; done` | Loop over exactly that list of values; `$n` is the current one |
| `time ./prog` | Run the program, then print 3 times: **`real`** = wall-clock (stopwatch) time, **`user`** = CPU time spent running the program's own code, **added up across all its threads**, **`sys`** = CPU time the kernel spent working for it |
| `P=$(pgrep -n exp2_busy)` | `pgrep -n` = PID of the **n**ewest process whose name matches `exp2_busy` |
| `grep Threads /proc/$P/status` | How many threads the process has right now |
| `ps -L -o tid,psr,stat,pcpu,comm -p $P` | One row per thread (`-L`). `PSR` = which CPU (0–11) the thread last ran on. `STAT` = state (`R` running, `S` sleeping; an extra `l` means "multi-threaded process", `+` means foreground). `%CPU` = its CPU use |
| `sleep 2; ps ...` | Wait 2 seconds and look again: did any thread move to a different CPU? |
| `pidstat -u -t -p $P 1 1` | `-u` = CPU usage, `-t` = per thread, for process `$P`, sampled over `1` second, `1` time |
| `pidstat -w -t -p $P 1 1` | `-w` = **context switches** per thread (Concepts doc Part 5): `cswch/s` = voluntary (the thread gave up the CPU itself, e.g. waiting), `nvcswch/s` = non-voluntary (the scheduler took the CPU away from it) |

Why `pidstat -w` and not `grep ctxt /proc/$P/status`: the counters in `/proc/<PID>/status` belong to the **main thread only**, not the whole process. Per-thread counts live in `/proc/<PID>/task/<TID>/status`. `pidstat -t` collects them for every thread at once.

## What happened, in plain English

<div class="tldr">
<b>What I did:</b> ran 1, 2, 4, 8 and 16 threads, each doing the same chunk of pure CPU work (about 1.1 s for one thread alone), and timed the whole thing. Then I watched an 8-thread run and a 16-thread run with <code>ps</code> and <code>pidstat</code>.
<br><br>
<b>What happened:</b> 16 threads finished in <b>3.4 seconds</b>, not 16 × 1.1 = 17.6 seconds. The threads ran <b>at the same time</b>, on different CPUs. <code>join()</code> did not make them take turns.
<br><br>
<b>But it wasn't free.</b> With 8 threads (fewer than the 12 CPUs), every thread had a CPU to itself: 100% each, no waiting, no switching. With 16 threads (more than 12 CPUs), every CPU was full, each thread spent 15–35% of its time <b>waiting in line for a CPU</b>, and the scheduler was forcibly switching threads about <b>930 times per second</b>.
<br><br>
<b>The key insight:</b> threads only run in parallel up to the number of CPUs. Past that, they take turns, and taking turns costs context switches. And on this laptop, "12 CPUs" is really 6 cores with 2 hyperthreads each, so even below 12 the speed-up isn't perfect.
</div>

## The numbers that matter

<div class="stat-row">
<div class="stat-card">
<div class="label">16 threads: real time</div>
<div class="values">3.41 s</div>
<div class="note">vs. 17.6 s if they had run one after another</div>
</div>
<div class="stat-card">
<div class="label">CPUs in use (pidstat %CPU)</div>
<div class="values">800% <span class="up">&rarr;</span> 1193%</div>
<div class="note">8 threads: 8 CPUs. 16 threads: all 12 full</div>
</div>
<div class="stat-card">
<div class="label">Forced context switches / s</div>
<div class="values">~1 <span class="up">&rarr;</span> ~930</div>
<div class="note">8 threads vs. 16 threads (sum of nvcswch/s)</div>
</div>
</div>

<div class="analogy">
<b>Analogy:</b> 12 checkout counters in a supermarket, and each thread is a shopper with a full trolley that takes 1 minute to scan. 8 shoppers: everyone walks straight to a free counter, and they're all done in about a minute. Nobody waits, and nobody gets interrupted. 16 shoppers: all 12 counters are busy and 4 shoppers are always standing in line. To be fair, the store manager (the scheduler) keeps stopping a shopper halfway through, sending them back to the line, and letting someone waiting take the counter. Everyone gets a fair share, but there's a lot of stopping and swapping (non-voluntary context switches), and everyone finishes later. One more twist: these 12 counters are really 6 double counters sharing one scanner each (hyperthreads), so two shoppers at the same double counter slow each other down.
</div>

## Predictions vs reality

Only prediction 1 was filled in, so it's the one to look at closely. The rest are answered below from the output.

| Prediction | What I predicted | What actually happened | Verdict |
|---|---|---|---|
| 1. real time for 2 / 4 / 8 / 16 threads | 5 / 10 / 20 / 40 s | **1.19 / 1.38 / 1.84 / 3.41 s** | <span class="verdict-wrong">Wrong</span>, and in the most useful way (below) |
| 2. 8 threads: user vs real | *(blank)* | user 14.06 s vs real 1.84 s: **user is 7.6× real** | n/a |
| 3. `Threads:` with 8 workers | *(blank)* | **9** (8 workers + the main thread) | n/a |
| 4. `ps -L`, 8 workers | *(blank)* | workers `Rl+` at 100%. Main thread `Sl+` at 0.1–0.3% | n/a |
| 5. pidstat total %CPU, 8 / 16 threads | *(blank)* | **800% / 1193%** | n/a |
| 6. same CPU (PSR) between snapshots? | *(blank)* | 8 threads: 7 of 8 stayed put. 16 threads: 12 of 16 moved | n/a |
| 7. finish in order 0, 1, 2…? | *(blank)* | No. Different order every run | n/a |
| 8. `cswch/s` vs `nvcswch/s` | *(blank)* | `cswch/s` was 0 everywhere. `nvcswch/s` ≈ 0 with 8 threads, **25–94 per thread** with 16 | n/a |

### The wrong one: "5 / 10 / 20 / 40" assumed the threads take turns

The predicted numbers double every time the thread count doubles: 2.5 s × number of threads. That's what you'd get if the threads ran **one after another**: thread 0 finishes, then thread 1 starts, and so on. That's exactly the misunderstanding in the spec's important question, *"why doesn't `t1.join(); t2.join();` make it single-threaded?"* (Concepts doc, Part 10).

What actually happened:

```
main:      [create 0..15][ join(0): wait ............ ][join(1)]…[join(15)]   ← each join returns instantly once that thread is done
thread 0:       [======== work ========]
thread 1:        [======== work ========]
thread 2:         [======== work ========]       all running AT THE SAME TIME,
   …                                               on different CPUs
thread 15:                [======== work ========]
```

`exp2_busy.cpp` creates **all** the threads first, then joins them. By the time `main` reaches the first `join()`, every thread is already running. `join()` only makes `main` wait. It doesn't start anything or put the threads in a line. So total time ≈ the time for one thread's work (when there are enough CPUs), not the sum.

**If they had run one after another**, using the real 1.1 s per thread: 2.2 / 4.4 / 8.8 / 17.6 s. Actual: 1.19 / 1.38 / 1.84 / 3.41 s. 16 threads did **17.6 seconds of single-thread work in 3.41 seconds**, a 5.2× speed-up.

(Also: one thread took **1.1 s**, not the 2.5 s I measured on another day. The same program's speed changes with what else Windows is running and how hot the laptop is. That's why each run's own 1-thread number is the baseline, not a figure from a different day.)

## Part A, explained: the timing table

| Threads | `real` | `user` | `user ÷ real` | CPU time per thread (`user ÷ threads`) |
|---|---|---|---|---|
| 1 | 1.155 s | 1.111 s | 0.96 | 1.11 s |
| 2 | 1.188 s | 2.336 s | 1.97 | 1.17 s |
| 4 | 1.377 s | 5.291 s | 3.84 | 1.32 s |
| 8 | 1.838 s | 14.062 s | 7.65 | 1.76 s |
| 16 | 3.410 s | 38.162 s | 11.19 | 2.39 s |

**`real` vs `user`, and why `user` can be bigger than `real`.** `real` is the stopwatch. `user` is CPU time **added up across all threads**. With 8 threads each busy for about 1.8 s at the same time, that's 8 × 1.76 ≈ 14 s of CPU time packed into 1.84 s of stopwatch time. So **`user ÷ real` ≈ how many CPUs were busy on average**: about 1, 2, 4, 7.7, and 11.2 for 16 threads (close to all 12). A single-threaded program can never have `user` bigger than `real`. Seeing it is direct proof of parallelism.

**`sys`** stayed tiny (0.008–0.070 s): almost no kernel work, because the threads never make system calls in the loop. The biggest `sys` is the 16-thread run (0.070 s), the one where the scheduler was switching threads the most.

**Finish order** was different in every run (8 threads: 3, 7, 1, 4, 5, 6, 0, 2). It's the same lesson as Experiment 1: creation order says nothing about which thread finishes first.

### The surprise: why didn't `real` stay flat at 1.1 s up to 12 threads?

If each thread had a CPU to itself, 2, 4 and 8 threads should all take about 1.1 s. Instead, 8 threads took 1.84 s, and each thread used **1.76 s** of CPU for the same work that took 1.11 s alone (last column of the table). The same work cost more CPU time when more threads ran at once. Two reasons:

1. **Your "12 CPUs" are 6 real cores.** `lscpu` on this machine:

    ```
    Model name:            AMD Ryzen 5 5600H with Radeon Graphics
    CPU(s):                12
    Core(s) per socket:    6
    Thread(s) per core:    2
    ```

    Each physical core runs **2 hardware threads** ("hyperthreading", which AMD calls SMT). Linux counts each one as a CPU, hence 12. But the two hyperthreads on one core **share that core's actual calculating hardware**. One thread alone gets a whole core. Two busy threads on the same core each run noticeably slower. Up to about 6 threads, the scheduler can give each one its own core. Past 6, threads start sharing cores. That's why the per-thread cost jumps at 8 (1.76 s) and 16 (2.39 s, about 2× the solo cost: each thread is getting roughly half a core).
2. **The CPU slows its clock when many cores are busy.** With one core busy, a laptop chip can run that core at its highest "boost" speed. With all cores busy, it has to stay within its power and heat limits, so every core runs slower. This is likely part of why even 4 threads cost a bit more (1.32 s vs 1.11 s).

So for pure-CPU work, this laptop behaves more like **6 fast cores plus some extra from hyperthreading** than like 12 full CPUs.

## Part B, explained: 8 threads (fewer than 12 CPUs)

```
Threads:        9
```
**9, not 8.** The 8 workers plus the **main thread** (the one running `main()`). Every process starts with one thread, and creating 8 more makes 9. This is the "careful" in prediction 3.

```
    TID PSR STAT %CPU COMMAND
    826   4 Sl+   0.3 exp2_busy.out      ← main thread
    827   7 Rl+   100 exp2_busy.out      ← worker (thread 0)
    ...
```

| Column | Main thread (826) | Workers (827–834) | Meaning |
|---|---|---|---|
| `TID` | 826 = the PID | 827–834 | Main thread's TID = PID (Concepts doc Part 6). Workers got the next numbers. They match the `(TID …)` printed in Terminal 1 |
| `PSR` | 4 | 7, 9, 11, 1, 4, 3, 6, 10 | Which CPU it last ran on: 8 workers spread across 8 different CPUs |
| `STAT` | **`S`**`l+` | **`R`**`l+` | **`S` = sleeping**: main is blocked in `join()`, waiting for thread 0 to finish. **`R` = running.** `l` = "this process has multiple threads". `+` = running in the terminal's foreground |
| `%CPU` | 0.3 / 0.1 | 100 | Main uses almost nothing while it waits in `join()`. Each worker uses one full CPU |

That main-thread row shows a **sleeping thread costs nothing**: blocked in `join()`, it uses 0.1% CPU while 8 other threads work flat out. Pulse's threads waiting in `read()` will look like this row. That's Experiment 3.

**PSR between the two snapshots (2 seconds apart):** 7 of 8 workers were on the **same CPU** both times. Only 828 moved (CPU 9 → 8). When there's a free CPU for everyone, the scheduler prefers to leave a thread where it is, because its data is already in that CPU's caches. Moving it would mean re-loading them.

**`pidstat -u`:**

| Column | Value | Meaning |
|---|---|---|
| `TGID` row (`826 -`) | `%CPU 800.00` | The whole process = **8 CPUs' worth**. The 8 workers at 100% each add up to 800% |
| `%usr` | 100 per worker | Time spent running the program's own code (the loop) |
| `%system` | 0 | Time the kernel spent on its behalf: none, because the loop never makes a system call |
| `%wait` | **0** | Time the thread was **ready to run but waiting for a free CPU**. Zero: there were always enough CPUs |
| `CPU` | 7, 8, 11, … | The CPU it was on during the sample |
| 834: `101.00` | | Just measurement rounding. A thread can't really use more than 100% of one CPU |

**`pidstat -w`:** `cswch/s` and `nvcswch/s` are **0 for every thread** (one thread had a single non-voluntary switch). Every thread had its own CPU and kept it the whole time. Nobody waited, so nobody gave up the CPU (`cswch`), and nobody had it taken away (`nvcswch`).

## Part C, explained: 16 threads (more than 12 CPUs)

`Threads: 17` (16 workers + main). Now there are more runnable threads than CPUs, and everything changes:

**pidstat -u, the clearest picture:**

```
TID    %usr   %wait   %CPU
985   85.58   15.38   85.58
988   63.46   33.65   63.46
1000  61.54   35.58   61.54
...
TGID row:  %CPU 1193.27
```

- **Process `%CPU` = 1193%**: about **all 12 CPUs**, completely full. It can't go higher, because there are only 12.
- **Each worker got 61–86%** of a CPU instead of 100%. 12 CPUs shared by 16 threads = 12 ÷ 16 = **75% each on average**, and the per-thread numbers average out to about 74%.
- **`%wait` appeared: 14–36% per thread.** That's time the thread was **ready to run but had no CPU**: standing in line. Look at any row: `%usr + %wait ≈ 100%` (985: 85.58 + 15.38 ≈ 101). A busy thread is always either running or waiting for a CPU. It never stops wanting one.
- **`%system` 2.88%** for the whole process: a little kernel time, versus 0 with 8 threads.

**`ps -L` `%CPU` (62–88%) vs `pidstat` `%CPU` (61–86%):** close but not identical, because they measure different time windows. `ps` shows the average **since the thread started**. `pidstat` shows the **last 1 second** only.

**PSR:** in the first snapshot, some CPUs show up **twice** (CPU 10: threads 988 and 991; CPU 9: 985 and 992; CPU 0: 989 and 994; …). That's two threads taking turns on one CPU. Between snapshots, **12 of 16 threads changed CPU**, versus 1 of 8 in Part B. With more threads than CPUs, the scheduler keeps moving threads around to give everyone a fair share.

**`pidstat -w`, the cost:**

| | 8 threads | 16 threads |
|---|---|---|
| `cswch/s` (voluntary) | 0 | 0 |
| `nvcswch/s` (non-voluntary), per thread | 0 (one thread: 1) | **25 to 94** |
| `nvcswch/s`, all workers added up | ~1 per second | **~927 per second** |

- **`cswch/s` is still 0.** These threads never *choose* to stop. There's nothing for them to wait for, no `read()`, no sleep.
- **`nvcswch/s` exploded.** With 16 threads and 12 CPUs, the scheduler has to **forcibly take the CPU away** from a running thread (its time slice is up) to let a waiting one run. That happened about 927 times per second across the process. Each one is a context switch (Concepts doc Part 5): save the registers, load another thread's, and continue with cold caches.

**Time:** 16 threads × 4 rounds took 13.70 s, vs 8.28 s for 8 threads × 4 rounds. **Double the work took 1.65× the time.** That's not double, because 8 threads left 4 CPUs idle that 16 threads could use. But past 12, adding threads adds no more CPUs, only more waiting and switching.

## What this means for Pulse

**1. `join()` / `detach()` don't serialize anything. Threads really do run at the same time.** That's what Week 2 relies on: the silent client's thread and curl's thread will both be alive at once.

**2. More threads only help up to the number of CPUs, for CPU work.** Past that (here, really past about 6 for full speed), threads take turns, and taking turns costs forced context switches: about 0 → 930 per second just by going from 8 to 16 threads.

**3. Pulse is NOT like this experiment, and that's the point.** These threads wanted the CPU 100% of the time. Pulse's threads will spend almost all their time **asleep** in `read()`/`accept()`, like the main thread's row here (`S`, 0.1% CPU), because the Month 1 benchmark showed Pulse isn't CPU-bound. So Pulse can probably have many more threads than CPUs without the waiting and switching seen in Part C, because sleeping threads aren't in line for a CPU. Experiment 3 tests exactly that, and **Week 4** finds out where the limit for Pulse really is (memory per thread, thread count, the scheduler) by pushing hundreds of connections.

**4. The tools to spot "too many busy threads" in Week 4:** `%wait` above 0 in `pidstat -u`, `nvcswch/s` climbing in `pidstat -w`, and the process's `%CPU` stuck at about `nproc × 100%`.

<details>
<summary>Raw output: Experiment 2, Part A (timing)</summary>

```
aagam@Galu-PC:/mnt/c/Users/aagam/OneDrive/Desktop/Pulse$ for n in 1 2 4 8 16; do echo "===== $n threads"; time ./Month2/Week1/exp2_busy.out $n; done
===== 1 threads
PID 694: starting 1 thread(s), 1 round(s) each, on a machine with 12 CPUs
thread  0 (TID 695) finished, sum = 1124999999250000000
all 1 thread(s) done in 1.10 s

real    0m1.155s
user    0m1.111s
sys     0m0.035s
===== 2 threads
PID 696: starting 2 thread(s), 1 round(s) each, on a machine with 12 CPUs
thread  0 (TID 697) finished, sum = 1124999999250000000
thread  1 (TID 698) finished, sum = 1124999999250000000
all 2 thread(s) done in 1.17 s

real    0m1.188s
user    0m2.336s
sys     0m0.009s
===== 4 threads
PID 699: starting 4 thread(s), 1 round(s) each, on a machine with 12 CPUs
thread  0 (TID 700) finished, sum = 1124999999250000000
thread  3 (TID 703) finished, sum = 1124999999250000000
thread  2 (TID 702) finished, sum = 1124999999250000000
thread  1 (TID 701) finished, sum = 1124999999250000000
all 4 thread(s) done in 1.36 s

real    0m1.377s
user    0m5.291s
sys     0m0.008s
===== 8 threads
PID 704: starting 8 thread(s), 1 round(s) each, on a machine with 12 CPUs
thread  3 (TID 708) finished, sum = 1124999999250000000
thread  7 (TID 712) finished, sum = 1124999999250000000
thread  1 (TID 706) finished, sum = 1124999999250000000
thread  4 (TID 709) finished, sum = 1124999999250000000
thread  5 (TID 710) finished, sum = 1124999999250000000
thread  6 (TID 711) finished, sum = 1124999999250000000
thread  0 (TID 705) finished, sum = 1124999999250000000
thread  2 (TID 707) finished, sum = 1124999999250000000
all 8 thread(s) done in 1.82 s

real    0m1.838s
user    0m14.062s
sys     0m0.013s
===== 16 threads
PID 713: starting 16 thread(s), 1 round(s) each, on a machine with 12 CPUs
thread  7 (TID 721) finished, sum = 1124999999250000000
thread  6 (TID 720) finished, sum = 1124999999250000000
thread  1 (TID 715) finished, sum = 1124999999250000000
thread  2 (TID 716) finished, sum = 1124999999250000000
thread 15 (TID 729) finished, sum = 1124999999250000000
thread  5 (TID 719) finished, sum = 1124999999250000000
thread  8 (TID 722) finished, sum = 1124999999250000000
thread  0 (TID 714) finished, sum = 1124999999250000000
thread 10 (TID 724) finished, sum = 1124999999250000000
thread 11 (TID 725) finished, sum = 1124999999250000000
thread  9 (TID 723) finished, sum = 1124999999250000000
thread  4 (TID 718) finished, sum = 1124999999250000000
thread 14 (TID 728) finished, sum = 1124999999250000000
thread 12 (TID 726) finished, sum = 1124999999250000000
thread  3 (TID 717) finished, sum = 1124999999250000000
thread 13 (TID 727) finished, sum = 1124999999250000000
all 16 thread(s) done in 3.39 s

real    0m3.410s
user    0m38.162s
sys     0m0.070s
```

</details>

<details>
<summary>Raw output: Experiment 2, Part B (8 threads, 4 rounds)</summary>

**Terminal 1**

```
aagam@Galu-PC:/mnt/c/Users/aagam/OneDrive/Desktop/Pulse$ ./Month2/Week1/exp2_busy.out 8 4
PID 826: starting 8 thread(s), 4 round(s) each, on a machine with 12 CPUs
thread  4 (TID 831) finished, sum = 4499999997000000000
thread  5 (TID 832) finished, sum = 4499999997000000000
thread  6 (TID 833) finished, sum = 4499999997000000000
thread  1 (TID 828) finished, sum = 4499999997000000000
thread  3 (TID 830) finished, sum = 4499999997000000000
thread  2 (TID 829) finished, sum = 4499999997000000000
thread  7 (TID 834) finished, sum = 4499999997000000000
thread  0 (TID 827) finished, sum = 4499999997000000000
all 8 thread(s) done in 8.28 s
```

**Terminal 2**

```
aagam@Galu-PC:/mnt/c/Users/aagam/OneDrive/Desktop/Pulse$ P=$(pgrep -n exp2_busy)
grep Threads /proc/$P/status
ps -L -o tid,psr,stat,pcpu,comm -p $P
sleep 2; ps -L -o tid,psr,stat,pcpu,comm -p $P
pidstat -u -t -p $P 1 1
pidstat -w -t -p $P 1 1
Threads:        9
    TID PSR STAT %CPU COMMAND
    826   4 Sl+   0.3 exp2_busy.out
    827   7 Rl+   100 exp2_busy.out
    828   9 Rl+   100 exp2_busy.out
    829  11 Rl+   100 exp2_busy.out
    830   1 Rl+   100 exp2_busy.out
    831   4 Rl+   100 exp2_busy.out
    832   3 Rl+   100 exp2_busy.out
    833   6 Rl+   100 exp2_busy.out
    834  10 Rl+   100 exp2_busy.out
    TID PSR STAT %CPU COMMAND
    826   4 Sl+   0.1 exp2_busy.out
    827   7 Rl+   100 exp2_busy.out
    828   8 Rl+   100 exp2_busy.out
    829  11 Rl+   100 exp2_busy.out
    830   1 Rl+   100 exp2_busy.out
    831   4 Rl+   100 exp2_busy.out
    832   3 Rl+   100 exp2_busy.out
    833   6 Rl+   100 exp2_busy.out
    834  10 Rl+   100 exp2_busy.out
Linux 6.6.87.2-microsoft-standard-WSL2 (Galu-PC)        10/07/26        _x86_64_   (12 CPU)

02:04:12      UID      TGID       TID    %usr %system  %guest   %wait    %CPU   CPU  Command
02:04:13     1000       826         -  800.00    0.00    0.00    0.00  800.00     4  exp2_busy.out
02:04:13     1000         -       826    0.00    0.00    0.00    0.00    0.00     4  |__exp2_busy.out
02:04:13     1000         -       827  100.00    0.00    0.00    0.00  100.00     7  |__exp2_busy.out
02:04:13     1000         -       828  100.00    0.00    0.00    0.00  100.00     8  |__exp2_busy.out
02:04:13     1000         -       829  100.00    0.00    0.00    0.00  100.00    11  |__exp2_busy.out
02:04:13     1000         -       830  100.00    0.00    0.00    0.00  100.00     1  |__exp2_busy.out
02:04:13     1000         -       831  100.00    0.00    0.00    0.00  100.00     4  |__exp2_busy.out
02:04:13     1000         -       832  100.00    0.00    0.00    0.00  100.00     3  |__exp2_busy.out
02:04:13     1000         -       833  100.00    0.00    0.00    0.00  100.00     6  |__exp2_busy.out
02:04:13     1000         -       834  101.00    0.00    0.00    0.00  101.00    10  |__exp2_busy.out
Average:     1000       826         -  800.00    0.00    0.00    0.00  800.00     -  exp2_busy.out
Average:     1000         -       826    0.00    0.00    0.00    0.00    0.00     -  |__exp2_busy.out
Average:     1000         -       827  100.00    0.00    0.00    0.00  100.00     -  |__exp2_busy.out
Average:     1000         -       828  100.00    0.00    0.00    0.00  100.00     -  |__exp2_busy.out
Average:     1000         -       829  100.00    0.00    0.00    0.00  100.00     -  |__exp2_busy.out
Average:     1000         -       830  100.00    0.00    0.00    0.00  100.00     -  |__exp2_busy.out
Average:     1000         -       831  100.00    0.00    0.00    0.00  100.00     -  |__exp2_busy.out
Average:     1000         -       832  100.00    0.00    0.00    0.00  100.00     -  |__exp2_busy.out
Average:     1000         -       833  100.00    0.00    0.00    0.00  100.00     -  |__exp2_busy.out
Average:     1000         -       834  101.00    0.00    0.00    0.00  101.00     -  |__exp2_busy.out
Linux 6.6.87.2-microsoft-standard-WSL2 (Galu-PC)        10/07/26        _x86_64_   (12 CPU)

02:04:13      UID      TGID       TID   cswch/s nvcswch/s  Command
02:04:14     1000       826         -      0.00      0.00  exp2_busy.out
02:04:14     1000         -       826      0.00      0.00  |__exp2_busy.out
02:04:14     1000         -       827      0.00      0.00  |__exp2_busy.out
02:04:14     1000         -       828      0.00      0.00  |__exp2_busy.out
02:04:14     1000         -       829      0.00      0.00  |__exp2_busy.out
02:04:14     1000         -       830      0.00      0.00  |__exp2_busy.out
02:04:14     1000         -       831      0.00      0.00  |__exp2_busy.out
02:04:14     1000         -       832      0.00      0.00  |__exp2_busy.out
02:04:14     1000         -       833      0.00      1.00  |__exp2_busy.out
02:04:14     1000         -       834      0.00      0.00  |__exp2_busy.out
Average:     1000       826         -      0.00      0.00  exp2_busy.out
Average:     1000         -       826      0.00      0.00  |__exp2_busy.out
Average:     1000         -       827      0.00      0.00  |__exp2_busy.out
Average:     1000         -       828      0.00      0.00  |__exp2_busy.out
Average:     1000         -       829      0.00      0.00  |__exp2_busy.out
Average:     1000         -       830      0.00      0.00  |__exp2_busy.out
Average:     1000         -       831      0.00      0.00  |__exp2_busy.out
Average:     1000         -       832      0.00      0.00  |__exp2_busy.out
Average:     1000         -       833      0.00      1.00  |__exp2_busy.out
Average:     1000         -       834      0.00      0.00  |__exp2_busy.out
```

</details>

<details>
<summary>Raw output: Experiment 2, Part C (16 threads, 4 rounds)</summary>

**Terminal 1**

```
aagam@Galu-PC:/mnt/c/Users/aagam/OneDrive/Desktop/Pulse$ ./Month2/Week1/exp2_busy.out 16 4
PID 984: starting 16 thread(s), 4 round(s) each, on a machine with 12 CPUs
thread 10 (TID 995) finished, sum = 4499999997000000000
thread 11 (TID 996) finished, sum = 4499999997000000000
thread  1 (TID 986) finished, sum = 4499999997000000000
thread  5 (TID 990) finished, sum = 4499999997000000000
thread 12 (TID 997) finished, sum = 4499999997000000000
thread  0 (TID 985) finished, sum = 4499999997000000000
thread 14 (TID 999) finished, sum = 4499999997000000000
thread  8 (TID 993) finished, sum = 4499999997000000000
thread  2 (TID 987) finished, sum = 4499999997000000000
thread  6 (TID 991) finished, sum = 4499999997000000000
thread  7 (TID 992) finished, sum = 4499999997000000000
thread  9 (TID 994) finished, sum = 4499999997000000000
thread 15 (TID 1000) finished, sum = 4499999997000000000
thread  3 (TID 988) finished, sum = 4499999997000000000
thread 13 (TID 998) finished, sum = 4499999997000000000
thread  4 (TID 989) finished, sum = 4499999997000000000
all 16 thread(s) done in 13.70 s
```

**Terminal 2**

```
aagam@Galu-PC:/mnt/c/Users/aagam/OneDrive/Desktop/Pulse$ P=$(pgrep -n exp2_busy)
grep Threads /proc/$P/status
ps -L -o tid,psr,stat,pcpu,comm -p $P
sleep 2; ps -L -o tid,psr,stat,pcpu,comm -p $P
pidstat -u -t -p $P 1 1
pidstat -w -t -p $P 1 1
Threads:        17
    TID PSR STAT %CPU COMMAND
    984   6 Sl+   0.0 exp2_busy.out
    985   9 Rl+  73.2 exp2_busy.out
    986   5 Rl+  82.8 exp2_busy.out
    987  11 Rl+  76.7 exp2_busy.out
    988  10 Rl+  83.0 exp2_busy.out
    989   0 Rl+  79.8 exp2_busy.out
    990   3 Rl+  76.1 exp2_busy.out
    991  10 Rl+  78.8 exp2_busy.out
    992   9 Rl+  72.2 exp2_busy.out
    993   5 Rl+  75.3 exp2_busy.out
    994   0 Rl+  87.8 exp2_busy.out
    995   4 Rl+  73.8 exp2_busy.out
    996   2 Rl+  81.4 exp2_busy.out
    997   6 Rl+  65.3 exp2_busy.out
    998   8 Rl+  61.9 exp2_busy.out
    999   7 Rl+  69.4 exp2_busy.out
   1000   7 Rl+  64.4 exp2_busy.out
    TID PSR STAT %CPU COMMAND
    984   6 Sl+   0.0 exp2_busy.out
    985   9 Rl+  69.6 exp2_busy.out
    986   5 Rl+  79.4 exp2_busy.out
    987  10 Rl+  75.0 exp2_busy.out
    988   4 Rl+  78.2 exp2_busy.out
    989   0 Rl+  79.2 exp2_busy.out
    990   3 Rl+  76.9 exp2_busy.out
    991  11 Rl+  78.7 exp2_busy.out
    992   8 Rl+  71.0 exp2_busy.out
    993   6 Rl+  75.0 exp2_busy.out
    994   1 Rl+  86.5 exp2_busy.out
    995   5 Rl+  74.0 exp2_busy.out
    996   3 Rl+  81.8 exp2_busy.out
    997   7 Rl+  69.4 exp2_busy.out
    998   0 Rl+  63.2 exp2_busy.out
    999   2 Rl+  70.8 exp2_busy.out
   1000  10 Rl+  66.3 exp2_busy.out
Linux 6.6.87.2-microsoft-standard-WSL2 (Galu-PC)        10/07/26        _x86_64_   (12 CPU)

02:05:26      UID      TGID       TID    %usr %system  %guest   %wait    %CPU   CPU  Command
02:05:27     1000       984         - 1190.38    2.88    0.00    0.00 1193.27     6  exp2_busy.out
02:05:27     1000         -       984    0.00    0.00    0.00    0.00    0.00     6  |__exp2_busy.out
02:05:27     1000         -       985   85.58    0.00    0.00   15.38   85.58     8  |__exp2_busy.out
02:05:27     1000         -       986   79.81    0.00    0.00   20.19   79.81     5  |__exp2_busy.out
02:05:27     1000         -       987   84.62    0.00    0.00   14.42   84.62    10  |__exp2_busy.out
02:05:27     1000         -       988   63.46    0.00    0.00   33.65   63.46    11  |__exp2_busy.out
02:05:27     1000         -       989   63.46    0.00    0.00   34.62   63.46     0  |__exp2_busy.out
02:05:27     1000         -       990   80.77    0.00    0.00   17.31   80.77     3  |__exp2_busy.out
02:05:27     1000         -       991   71.15    0.00    0.00   25.96   71.15    10  |__exp2_busy.out
02:05:27     1000         -       992   79.81    0.00    0.00   17.31   79.81     9  |__exp2_busy.out
02:05:27     1000         -       993   75.00    0.00    0.00   22.12   75.00     6  |__exp2_busy.out
02:05:27     1000         -       994   62.50    0.00    0.00   34.62   62.50     1  |__exp2_busy.out
02:05:27     1000         -       995   71.15    0.00    0.00   24.04   71.15     4  |__exp2_busy.out
02:05:27     1000         -       996   81.73    0.00    0.00   14.42   81.73     2  |__exp2_busy.out
02:05:27     1000         -       997   77.88    0.96    0.00   18.27   78.85     7  |__exp2_busy.out
02:05:27     1000         -       998   68.27    0.00    0.00   27.88   68.27     1  |__exp2_busy.out
02:05:27     1000         -       999   63.46    0.00    0.00   32.69   63.46     8  |__exp2_busy.out
02:05:27     1000         -      1000   61.54    0.00    0.00   35.58   61.54     7  |__exp2_busy.out
Average:     1000       984         - 1190.38    2.88    0.00    0.00 1193.27     -  exp2_busy.out
Average:     1000         -       984    0.00    0.00    0.00    0.00    0.00     -  |__exp2_busy.out
Average:     1000         -       985   85.58    0.00    0.00   15.38   85.58     -  |__exp2_busy.out
Average:     1000         -       986   79.81    0.00    0.00   20.19   79.81     -  |__exp2_busy.out
Average:     1000         -       987   84.62    0.00    0.00   14.42   84.62     -  |__exp2_busy.out
Average:     1000         -       988   63.46    0.00    0.00   33.65   63.46     -  |__exp2_busy.out
Average:     1000         -       989   63.46    0.00    0.00   34.62   63.46     -  |__exp2_busy.out
Average:     1000         -       990   80.77    0.00    0.00   17.31   80.77     -  |__exp2_busy.out
Average:     1000         -       991   71.15    0.00    0.00   25.96   71.15     -  |__exp2_busy.out
Average:     1000         -       992   79.81    0.00    0.00   17.31   79.81     -  |__exp2_busy.out
Average:     1000         -       993   75.00    0.00    0.00   22.12   75.00     -  |__exp2_busy.out
Average:     1000         -       994   62.50    0.00    0.00   34.62   62.50     -  |__exp2_busy.out
Average:     1000         -       995   71.15    0.00    0.00   24.04   71.15     -  |__exp2_busy.out
Average:     1000         -       996   81.73    0.00    0.00   14.42   81.73     -  |__exp2_busy.out
Average:     1000         -       997   77.88    0.96    0.00   18.27   78.85     -  |__exp2_busy.out
Average:     1000         -       998   68.27    0.00    0.00   27.88   68.27     -  |__exp2_busy.out
Average:     1000         -       999   63.46    0.00    0.00   32.69   63.46     -  |__exp2_busy.out
Average:     1000         -      1000   61.54    0.00    0.00   35.58   61.54     -  |__exp2_busy.out
Linux 6.6.87.2-microsoft-standard-WSL2 (Galu-PC)        10/07/26        _x86_64_   (12 CPU)

02:05:27      UID      TGID       TID   cswch/s nvcswch/s  Command
02:05:28     1000       984         -      0.00      0.00  exp2_busy.out
02:05:28     1000         -       984      0.00      0.00  |__exp2_busy.out
02:05:28     1000         -       985      0.00     76.24  |__exp2_busy.out
02:05:28     1000         -       986      0.00     51.49  |__exp2_busy.out
02:05:28     1000         -       987      0.00     24.75  |__exp2_busy.out
02:05:28     1000         -       988      0.00     34.65  |__exp2_busy.out
02:05:28     1000         -       989      0.00     74.26  |__exp2_busy.out
02:05:28     1000         -       990      0.00     74.26  |__exp2_busy.out
02:05:28     1000         -       991      0.00     28.71  |__exp2_busy.out
02:05:28     1000         -       992      0.00     94.06  |__exp2_busy.out
02:05:28     1000         -       993      0.00     82.18  |__exp2_busy.out
02:05:28     1000         -       994      0.00     55.45  |__exp2_busy.out
02:05:28     1000         -       995      0.00     49.50  |__exp2_busy.out
02:05:28     1000         -       996      0.00     25.74  |__exp2_busy.out
02:05:28     1000         -       997      0.00     67.33  |__exp2_busy.out
02:05:28     1000         -       998      0.00     31.68  |__exp2_busy.out
02:05:28     1000         -       999      0.00     71.29  |__exp2_busy.out
02:05:28     1000         -      1000      0.00     85.15  |__exp2_busy.out
Average:     1000       984         -      0.00      0.00  exp2_busy.out
Average:     1000         -       984      0.00      0.00  |__exp2_busy.out
Average:     1000         -       985      0.00     76.24  |__exp2_busy.out
Average:     1000         -       986      0.00     51.49  |__exp2_busy.out
Average:     1000         -       987      0.00     24.75  |__exp2_busy.out
Average:     1000         -       988      0.00     34.65  |__exp2_busy.out
Average:     1000         -       989      0.00     74.26  |__exp2_busy.out
Average:     1000         -       990      0.00     74.26  |__exp2_busy.out
Average:     1000         -       991      0.00     28.71  |__exp2_busy.out
Average:     1000         -       992      0.00     94.06  |__exp2_busy.out
Average:     1000         -       993      0.00     82.18  |__exp2_busy.out
Average:     1000         -       994      0.00     55.45  |__exp2_busy.out
Average:     1000         -       995      0.00     49.50  |__exp2_busy.out
Average:     1000         -       996      0.00     25.74  |__exp2_busy.out
Average:     1000         -       997      0.00     67.33  |__exp2_busy.out
Average:     1000         -       998      0.00     31.68  |__exp2_busy.out
Average:     1000         -       999      0.00     71.29  |__exp2_busy.out
Average:     1000         -      1000      0.00     85.15  |__exp2_busy.out
```

</details>

---

# Experiment 3 — Sleeping threads

**File:** `Month2/Week1/exp3_sleep.cpp`, written and fully commented (Concepts doc Parts 5, 6). Read the comments before running.
**Pulse question:** a Pulse thread waiting in `read()` for a slow client is **asleep** (Experiment 0 showed Pulse's thread as `S wait_woken`). What does a sleeping thread cost in CPU, time and memory? And can a process have *more* sleeping threads than CPUs, unlike Experiment 2's busy ones?


## What the program does

```
./exp3_sleep.out <threads> [seconds]
```

- Exactly Experiment 2's structure (create all threads, then join all), with one change: each thread calls `std::this_thread::sleep_for(...)` instead of a busy loop. Default 10 seconds.
- A sleeping thread is in state `S`. The scheduler doesn't consider it at all until a kernel timer wakes it up. In Pulse, the thing that wakes a thread up is data arriving from the network instead of a timer. Otherwise it's the same situation.
- Allows up to 200 threads (Experiment 2 stopped at 64), because sleeping threads use no CPU.
- Prints `woke up and finished` only for the first 16 threads, so 100 lines don't bury the output that matters.

## What to run

All from the Pulse folder in WSL (`cd /mnt/c/Users/aagam/OneDrive/Desktop/Pulse`).

### Part A: timing 1 / 8 / 16 / 100 threads (3-second sleep each, ~15 s total)

```bash
g++ -std=c++17 -Wall -pthread Month2/Week1/exp3_sleep.cpp -o Month2/Week1/exp3_sleep.out

for n in 1 8 16 100; do echo "===== $n threads"; time ./Month2/Week1/exp3_sleep.out $n 3; done
```

### Part B: watch 16 sleeping threads (more than 12 CPUs, the case that hurt in Experiment 2)

```bash
# Terminal 1 (sleeps 10 s, so there's time to look)
./Month2/Week1/exp3_sleep.out 16
```

```bash
# Terminal 2, straight away
P=$(pgrep -n exp3_sleep)
grep -E "Threads|VmSize|VmRSS" /proc/$P/status
ps -L -o tid,psr,stat,wchan:22,pcpu,comm -p $P
pidstat -u -t -p $P 1 1
pidstat -w -t -p $P 1 1
```

### Part C: watch 100 sleeping threads (8× more threads than CPUs)

```bash
# Terminal 1
./Month2/Week1/exp3_sleep.out 100
```

```bash
# Terminal 2, straight away
P=$(pgrep -n exp3_sleep)
grep -E "Threads|VmSize|VmRSS" /proc/$P/status
ps -L -o stat= -p $P | sort | uniq -c
ps -L -o wchan:22= -p $P | sort | uniq -c
pidstat -u -p $P 1 1
```

### New commands, explained

| Piece | Meaning |
|---|---|
| <code>grep -E "Threads&#124;VmSize&#124;VmRSS"</code> | Three lines from `/proc/<PID>/status`: thread count, **VmSize** = virtual memory *reserved* (address space), **VmRSS** = physical RAM *actually used* (Concepts doc Part 6) |
| `ps -L -o ...,wchan:22,...` | `WCHAN` = the kernel function each thread is asleep in (seen in Experiment 0 as `wait_woken`) |
| `ps -L -o stat= -p $P` | Just the `STAT` column, one line per thread. The `=` after `stat` means "no header line" |
| <code>&#124; sort &#124; uniq -c</code> | Same trick as Experiment 1: sort the lines so identical ones are together, then count each kind. 101 rows become a couple of lines like `<count> Sl+` |
| `pidstat -u -p $P 1 1` | CPU use for the **whole process** (no `-t`, so no per-thread rows, which would be 101 lines here) |

## What happened, in plain English

<div class="tldr">
<b>What I did:</b> ran 1, 8, 16 and 100 threads that each just <b>sleep</b> for 3 seconds, timed it, then watched 16 sleeping threads with <code>ps</code> and <code>pidstat</code>.
<br><br>
<b>What happened:</b> every run took about <b>3 seconds</b>, whether 1 thread or 100. They used essentially <b>zero CPU</b> (<code>user</code> ≈ 0.000 s). With 16 threads on 12 CPUs there was <b>no waiting for a CPU and no context switching at all</b>, the exact opposite of Experiment 2's 16 busy threads.
<br><br>
<b>The key insight:</b> a sleeping thread isn't waiting in line for a CPU. It isn't in the line at all. So you can have far more sleeping threads than CPUs at almost no CPU cost. What each thread <i>does</i> cost is <b>memory reserved for its stack</b> (8 MiB of address space each) and a little kernel time to create and destroy it. Pulse's threads, waiting in <code>read()</code> for clients, are this kind of thread.
</div>

## The numbers that matter

<div class="stat-row">
<div class="stat-card">
<div class="label">real time, 1 vs 100 threads</div>
<div class="values"><span class="flat">3.05 s &rarr; 3.08 s</span></div>
<div class="note">100× the threads, basically the same time</div>
</div>
<div class="stat-card">
<div class="label">CPU used (user), 100 threads</div>
<div class="values"><span class="flat">0.000 s</span></div>
<div class="note">Exp 2's 16 busy threads: 38.2 s</div>
</div>
<div class="stat-card">
<div class="label">16 threads: reserved vs real memory</div>
<div class="values">135 MB vs 3.9 MB</div>
<div class="note">VmSize 138,040 kB vs VmRSS 3,968 kB</div>
</div>
</div>

<div class="analogy">
<b>Analogy:</b> back to the 12 supermarket checkout counters. In Experiment 2, every shopper had a full trolley, so they all needed a counter, and with 16 shoppers there was a queue. Here, every "shopper" is sitting in the café waiting for a phone call (the timer, or in Pulse, the client's data). Someone sitting in the café doesn't need a counter and isn't in the queue. You could have 100 of them in the café and the counters would sit empty. What they <i>do</i> take is a seat each (memory for the stack). The café can run out of seats long before the counters get busy. That's Week 4's question.
</div>

## Predictions vs reality

No Experiment 3 predictions were written in `notes.txt`, so each question is answered from the output instead.

| Question | Answer from the output |
|---|---|
| 1. `real` for 1 / 8 / 16 / 100 threads (3 s sleep) | **3.048 / 3.017 / 3.022 / 3.079 s**: always about 3 s |
| 2. `user` time | **0.018 / 0.000 / 0.004 / 0.000 s**: nothing. Experiment 2 was 1.1 to 38.2 s |
| 3. `Threads:` | 16 workers: **17**. 100 workers: **101** (always workers + main) |
| 4. `STAT` | Workers **`Sl+`**, main **`Sl+`**: everyone asleep |
| 5. `WCHAN` | Workers **`hrtimer_nanosleep`**, main **`futex_wait_queue`**: **different**, explained below |
| 6. total `%CPU`, `%wait` | **0.00%** CPU, **0.00%** wait, with 16 threads **and** with 100 |
| 7. `cswch/s` / `nvcswch/s` while sleeping | **0 / 0** for every thread |
| 8. `VmSize` / `VmRSS` | 16 threads: 138,040 / 3,968 kB. 100 threads: **826,504 / 4,352 kB**. VmSize grew by **8,196 kB per extra thread**, VmRSS by only **~4.6 kB per extra thread** |
| 9. Could Pulse handle 100 slow clients? | Answered at the end of this section |

## Part A, explained: the timing table

| Threads | `real` | `user` | `sys` |
|---|---|---|---|
| 1 | 3.048 s | 0.018 s | 0.021 s |
| 8 | 3.017 s | 0.000 s | 0.013 s |
| 16 | 3.022 s | 0.004 s | 0.015 s |
| 100 | 3.079 s | 0.000 s | **0.098 s** |

- **`real` ≈ 3 s every time.** Every thread starts sleeping at almost the same moment and wakes 3 s later. The sleeps overlap completely. There's no queue for CPUs because none of them needs one.
- **`user` ≈ 0.** Sleeping runs no program code, so it uses no CPU. Compare Experiment 2's 16 threads: `user` 38.162 s.
- **`sys` with 100 threads: 0.098 s** (vs about 0.015 s for 8 or 16). This is the **cost of creating and destroying threads**. For each thread the kernel has to `clone()` it, map an 8 MiB stack, set up the guard page, and later tear it all down. Here that's well under 1 ms per thread. It's small, but in Week 2 Pulse pays it **once per connection**, every time a client connects. Remember that for Week 4.
- **`real` 3.079 s for 100 vs 3.017 s for 8**: the extra ~60 ms is the time spent creating 100 threads before the last one starts sleeping, plus joining them.

**Finish order is almost in order this time** (16 threads: 0, 1, 2, **4, 3**, 5, 6, 7, **9, 8**, 10…). In Experiment 2 it was scrambled. Why the difference? Each thread sleeps exactly 3 s **from the moment it started**, and they started in order (0 first), so their alarms go off in roughly that order, microseconds apart. When two alarms go off almost together, either can run first: **4 before 3, 9 before 8**. So the order is still not guaranteed. It just *looks* ordered because the timers were set in order. That's the same lesson as Experiment 1's 98/2.

## Part B, explained: 16 sleeping threads

```
VmSize:   138040 kB
VmRSS:      3968 kB
Threads:        17
```

**`Threads: 17`**: 16 workers + the main thread, as in Experiment 2.

**`VmSize` 138,040 kB: memory *reserved*.** Do the arithmetic with the stack size from Concepts doc Part 4 (8 MiB + one 4 KiB guard page per thread):

```
16 threads × (8192 kB stack + 4 kB guard page) = 131,136 kB
138,040 − 131,136                               =   6,904 kB   ← the program itself, libc/libstdc++, the main thread's stack
```

So **95% of this process's address space is thread stacks.** Every extra thread should add about 8 MiB to `VmSize`. Part C (100 threads) tests that prediction directly, and it holds to the kilobyte.

**`VmRSS` 3,968 kB: memory *actually in RAM*.** Only about 4 MB, nearly the same as **Pulse's Month 1 baseline RSS of 3,840 kB**. 16 threads have reserved 128 MiB of stack between them, but each one has only *touched* a few KiB (a sleeping thread barely uses its stack). The kernel only gives real RAM to pages that are actually touched (Concepts doc Part 6). **Reserved is not used.**

```
    TID PSR STAT WCHAN                  %CPU COMMAND
   1153   5 Sl+  futex_wait_queue        1.7 exp3_sleep.out     ← main thread
   1154   8 Sl+  hrtimer_nanosleep       0.0 exp3_sleep.out     ← worker
   ...
```

| Column | Main thread (1153) | Workers (1154–1169) | Meaning |
|---|---|---|---|
| `STAT` | `Sl+` | `Sl+` | **Every** thread is sleeping (`S`) |
| `WCHAN` | **`futex_wait_queue`** | **`hrtimer_nanosleep`** | **What** each thread is asleep waiting for (below) |
| `%CPU` | 1.7 | 0.0 | `ps` `%CPU` is the average since the thread started. Main's 1.7% is from the start, when it was busy creating 16 threads. Workers have done nothing |
| `PSR` | 5 | only 3, 7, 8, 11 | All 16 workers last ran on just **4 CPUs** (below) |

### `S` tells you a thread is asleep. `WCHAN` tells you what it's waiting for.

You've now seen three different wait channels, all in state `S`:

| WCHAN | Who | Waiting for |
|---|---|---|
| `hrtimer_nanosleep` | Workers here | A **timer**. `sleep_for` → the `nanosleep` system call → "wake me when this high-resolution timer (`hrtimer`) goes off" |
| `futex_wait_queue` | Main thread here | **Another thread**. `join()` waits on a **futex** ("fast userspace mutex"), the kernel's basic "sleep until another thread signals this spot in memory" mechanism. When a thread exits, the kernel signals its futex, which wakes whoever is in `join()` for it. Futexes are also how `std::mutex` sleeps (Week 3) |
| `wait_woken` | Month 1 Pulse, Experiment 0 | **The network**: data arriving on a socket (`read()`) |

Same state, three different reasons. In Week 2, `ps -L` on Pulse will show the main thread waiting in `accept()` and each slow client's thread in `read()`, so `WCHAN` will tell you exactly which thread is stuck on what.

### PSR: 16 threads "on" 4 CPUs, and it doesn't matter

All 16 workers show `PSR` 3, 7, 8 or 11, with **four threads per CPU**. In Experiment 2 Part C, two threads on one CPU meant **taking turns** (`%wait` 14–36%). Here it means nothing. `PSR` is only the CPU a thread **last** ran on, and these threads ran for a few microseconds (just long enough to start sleeping), then left. None of them is running or waiting to run. When they wake up, the scheduler will put each one on whichever CPU is free.

### pidstat: all zeros, and that *is* the result

- **`pidstat -u`**: `%CPU 0.00` and **`%wait 0.00`** for every thread and the whole process. 16 threads on 12 CPUs, and nobody is in line for a CPU.
- **`pidstat -w`**: `cswch/s 0` and `nvcswch/s 0` for every thread. During that second, **no thread was switched in or out at all**. A sleeping thread does one voluntary switch when it goes to sleep and runs again once when it wakes up. In between, the scheduler doesn't touch it.

## Part C, explained: 100 sleeping threads

```
VmSize:   826504 kB
VmRSS:      4352 kB
Threads:        101
    101 Sl+
      1 futex_wait_queue
    100 hrtimer_nanosleep
```

**`Threads: 101`**: 100 workers + main.

**`sort | uniq -c` turned 101 rows into three lines:**

- `101 Sl+`: **all 101 threads asleep**. Not one is running or waiting for a CPU.
- `1 futex_wait_queue`: the main thread, in `join()`.
- `100 hrtimer_nanosleep`: the 100 workers, in `sleep_for`.

**`pidstat -u`: `%CPU 0.00`, `%wait 0.00`** for the whole process. 100 threads on 12 CPUs, 8× more threads than CPUs, and the CPUs don't notice. With busy threads (Experiment 2), even 16 filled every CPU.

### The memory prediction, checked

From Part B's numbers, the prediction was VmSize ≈ 6,904 + 100 × 8,196 ≈ **826,500 kB**. Actual: **826,504 kB**, off by 4 kB.

| | 16 threads (Part B) | 100 threads (Part C) | Difference ÷ 84 extra threads |
|---|---|---|---|
| `VmSize` (reserved) | 138,040 kB | 826,504 kB | **8,196 kB per thread** = 8,192 kB stack + 4 kB guard page, exactly |
| `VmRSS` (actually in RAM) | 3,968 kB | 4,352 kB | **~4.6 kB per thread** |

So each extra thread **reserves 8 MiB** of address space but **uses about 4.6 KiB** of real RAM: roughly one page (4 KiB) of stack that it actually touched, plus a little bookkeeping. **100 threads cost about 0.4 MB of real memory**, while `VmSize` makes it look like 800 MB.

This is why the "8 MiB per thread" from the Concepts doc doesn't mean 100 threads eat 800 MB of RAM, and why Week 4 has to look at **both** numbers. `VmRSS` tells you real memory pressure. `VmSize` tells you how much address space is being reserved, which is what kernel limits and huge thread counts eventually run into.

**Finish order with 100 threads** was more scrambled at the start (1, 3, 4, 0, 5, 6, 2, …) than with 16. All their alarms go off at almost the same moment, and only microseconds separate the order they were created in. Again: not guaranteed.
## Same thread count, opposite behavior: Experiment 2 vs Experiment 3

Both runs had **16 worker threads on 12 CPUs**:

| | Exp 2, Part C (16 busy) | Exp 3, Part B (16 sleeping) |
|---|---|---|
| Process `%CPU` | 1193% (all 12 CPUs full) | **0%** |
| `%wait` per thread | 14–36% | **0%** |
| Non-voluntary switches / s | ~927 | **0** |
| Threads moving between CPUs | 12 of 16 | none (none are running) |
| What limits it | CPUs | **memory per thread, creation cost** |

> **"How many threads is too many?" depends on what the threads do.** Busy threads: about the number of CPUs (here really about 6 cores). Sleeping threads: many more, limited by memory and kernel bookkeeping, not CPUs.

## What this means for Pulse

**9. Could Week 2 Pulse handle 100 slow clients at once? What would it cost?**

Yes, very likely. A Pulse thread stuck on a slow client is asleep in `read()` (`wait_woken`), exactly like these sleeping threads. Based on this experiment, 100 of them would cost:

| Resource | Cost of 100 slow-client threads | From |
|---|---|---|
| CPU while they wait | ~0 | `user` 0.000 s, `%CPU` 0 |
| Waiting for CPUs / context switches | none while they sleep | `%wait` 0, `nvcswch/s` 0 |
| Reserved address space | about **800 MiB** (100 × 8,196 kB) | Part C: VmSize 826,504 kB |
| Real RAM | small: about 4.6 KiB per sleeping thread here. A Pulse thread also touches `handle_client`'s 4 KiB `chunk` buffer and `raw_request`, so a little more, but kilobytes, not megabytes | Part C: +384 kB of VmRSS for 84 extra threads |
| Kernel time | creating and destroying 100 threads ≈ 0.1 s total `sys` | Part A, 100 threads |

**The catch, for Week 4:** that's 100. At 1,000 or 10,000 connections, reserved stacks reach 8–80 GiB of address space, kernel limits on threads per user start to matter, creating a thread *per connection* adds up, and if many of those threads wake up and need the CPU at once, Pulse turns into Experiment 2 Part C: CPUs full, `%wait` climbing, forced switches climbing. This experiment shows *why* thread-per-connection works well at moderate scale. Week 4 measures where it stops working.


<details>
<summary>Raw output: Experiment 3, Part A (timing)</summary>

```
aagam@Galu-PC:/mnt/c/Users/aagam/OneDrive/Desktop/Pulse$ cd /mnt/c/Users/aagam/OneDrive/Desktop/Pulse
g++ -std=c++17 -Wall -pthread Month2/Week1/exp3_sleep.cpp -o Month2/Week1/exp3_sleep.out

for n in 1 8 16 100; do echo "===== $n threads"; time ./Month2/Week1/exp3_sleep.out $n 3; done
===== 1 threads
PID 971: starting 1 sleeping thread(s), 3 s each, on a machine with 12 CPUs
thread  0 (TID 972) woke up and finished
all 1 thread(s) done in 3.00 s

real    0m3.048s
user    0m0.018s
sys     0m0.021s
===== 8 threads
PID 973: starting 8 sleeping thread(s), 3 s each, on a machine with 12 CPUs
thread  0 (TID 974) woke up and finished
thread  1 (TID 975) woke up and finished
thread  2 (TID 976) woke up and finished
thread  3 (TID 977) woke up and finished
thread  4 (TID 978) woke up and finished
thread  5 (TID 979) woke up and finished
thread  6 (TID 980) woke up and finished
thread  7 (TID 981) woke up and finished
all 8 thread(s) done in 3.00 s

real    0m3.017s
user    0m0.000s
sys     0m0.013s
===== 16 threads
PID 986: starting 16 sleeping thread(s), 3 s each, on a machine with 12 CPUs
thread  0 (TID 987) woke up and finished
thread  1 (TID 988) woke up and finished
thread  2 (TID 989) woke up and finished
thread  4 (TID 991) woke up and finished
thread  3 (TID 990) woke up and finished
thread  5 (TID 992) woke up and finished
thread  6 (TID 993) woke up and finished
thread  7 (TID 994) woke up and finished
thread  9 (TID 996) woke up and finished
thread  8 (TID 995) woke up and finished
thread 10 (TID 997) woke up and finished
thread 11 (TID 998) woke up and finished
thread 12 (TID 999) woke up and finished
thread 13 (TID 1000) woke up and finished
thread 14 (TID 1001) woke up and finished
thread 15 (TID 1002) woke up and finished
all 16 thread(s) done in 3.01 s

real    0m3.022s
user    0m0.004s
sys     0m0.015s
===== 100 threads
PID 1003: starting 100 sleeping thread(s), 3 s each, on a machine with 12 CPUs
thread  0 (TID 1004) woke up and finished
thread  1 (TID 1005) woke up and finished
thread  2 (TID 1006) woke up and finished
thread  3 (TID 1007) woke up and finished
thread  4 (TID 1008) woke up and finished
thread  5 (TID 1009) woke up and finished
thread  6 (TID 1010) woke up and finished
thread  7 (TID 1011) woke up and finished
thread  8 (TID 1012) woke up and finished
thread  9 (TID 1013) woke up and finished
thread 10 (TID 1014) woke up and finished
thread 11 (TID 1015) woke up and finished
thread 12 (TID 1016) woke up and finished
thread 13 (TID 1017) woke up and finished
thread 14 (TID 1018) woke up and finished
thread 15 (TID 1019) woke up and finished
all 100 thread(s) done in 3.07 s

real    0m3.079s
user    0m0.000s
sys     0m0.098s
```

</details>

<details>
<summary>Raw output: Experiment 3, Part B (16 sleeping threads)</summary>

**Terminal 1**

```
aagam@Galu-PC:/mnt/c/Users/aagam/OneDrive/Desktop/Pulse$ ./Month2/Week1/exp3_sleep.out 16
PID 1153: starting 16 sleeping thread(s), 10 s each, on a machine with 12 CPUs
thread  0 (TID 1154) woke up and finished
thread  1 (TID 1155) woke up and finished
thread  2 (TID 1156) woke up and finished
thread  3 (TID 1157) woke up and finished
thread  4 (TID 1158) woke up and finished
thread  5 (TID 1159) woke up and finished
thread  6 (TID 1160) woke up and finished
thread  7 (TID 1161) woke up and finished
thread  8 (TID 1162) woke up and finished
thread  9 (TID 1163) woke up and finished
thread 10 (TID 1164) woke up and finished
thread 11 (TID 1165) woke up and finished
thread 12 (TID 1166) woke up and finished
thread 13 (TID 1167) woke up and finished
thread 14 (TID 1168) woke up and finished
thread 15 (TID 1169) woke up and finished
all 16 thread(s) done in 10.01 s
```

**Terminal 2**

```
aagam@Galu-PC:/mnt/c/Users/aagam/OneDrive/Desktop/Pulse$ P=$(pgrep -n exp3_sleep)
grep -E "Threads|VmSize|VmRSS" /proc/$P/status
ps -L -o tid,psr,stat,wchan:22,pcpu,comm -p $P
pidstat -u -t -p $P 1 1
pidstat -w -t -p $P 1 1
VmSize:   138040 kB
VmRSS:      3968 kB
Threads:        17
    TID PSR STAT WCHAN                  %CPU COMMAND
   1153   5 Sl+  futex_wait_queue        1.7 exp3_sleep.out
   1154   8 Sl+  hrtimer_nanosleep       0.0 exp3_sleep.out
   1155   3 Sl+  hrtimer_nanosleep       0.0 exp3_sleep.out
   1156   7 Sl+  hrtimer_nanosleep       0.0 exp3_sleep.out
   1157  11 Sl+  hrtimer_nanosleep       0.0 exp3_sleep.out
   1158   8 Sl+  hrtimer_nanosleep       0.0 exp3_sleep.out
   1159   3 Sl+  hrtimer_nanosleep       0.0 exp3_sleep.out
   1160   7 Sl+  hrtimer_nanosleep       0.0 exp3_sleep.out
   1161  11 Sl+  hrtimer_nanosleep       0.0 exp3_sleep.out
   1162   8 Sl+  hrtimer_nanosleep       0.0 exp3_sleep.out
   1163   3 Sl+  hrtimer_nanosleep       0.0 exp3_sleep.out
   1164  11 Sl+  hrtimer_nanosleep       0.0 exp3_sleep.out
   1165   7 Sl+  hrtimer_nanosleep       0.0 exp3_sleep.out
   1166   7 Sl+  hrtimer_nanosleep       0.0 exp3_sleep.out
   1167   3 Sl+  hrtimer_nanosleep       0.0 exp3_sleep.out
   1168   8 Sl+  hrtimer_nanosleep       0.0 exp3_sleep.out
   1169   8 Sl+  hrtimer_nanosleep       0.0 exp3_sleep.out
Linux 6.6.87.2-microsoft-standard-WSL2 (Galu-PC)        10/12/26        _x86_64_   (12 CPU)

01:34:42      UID      TGID       TID    %usr %system  %guest   %wait    %CPU   CPU  Command
01:34:43     1000      1153         -    0.00    0.00    0.00    0.00    0.00     5  exp3_sleep.out
01:34:43     1000         -      1153    0.00    0.00    0.00    0.00    0.00     5  |__exp3_sleep.out
01:34:43     1000         -      1154    0.00    0.00    0.00    0.00    0.00     8  |__exp3_sleep.out
01:34:43     1000         -      1155    0.00    0.00    0.00    0.00    0.00     3  |__exp3_sleep.out
01:34:43     1000         -      1156    0.00    0.00    0.00    0.00    0.00     7  |__exp3_sleep.out
01:34:43     1000         -      1157    0.00    0.00    0.00    0.00    0.00    11  |__exp3_sleep.out
01:34:43     1000         -      1158    0.00    0.00    0.00    0.00    0.00     8  |__exp3_sleep.out
01:34:43     1000         -      1159    0.00    0.00    0.00    0.00    0.00     3  |__exp3_sleep.out
01:34:43     1000         -      1160    0.00    0.00    0.00    0.00    0.00     7  |__exp3_sleep.out
01:34:43     1000         -      1161    0.00    0.00    0.00    0.00    0.00    11  |__exp3_sleep.out
01:34:43     1000         -      1162    0.00    0.00    0.00    0.00    0.00     8  |__exp3_sleep.out
01:34:43     1000         -      1163    0.00    0.00    0.00    0.00    0.00     3  |__exp3_sleep.out
01:34:43     1000         -      1164    0.00    0.00    0.00    0.00    0.00    11  |__exp3_sleep.out
01:34:43     1000         -      1165    0.00    0.00    0.00    0.00    0.00     7  |__exp3_sleep.out
01:34:43     1000         -      1166    0.00    0.00    0.00    0.00    0.00     7  |__exp3_sleep.out
01:34:43     1000         -      1167    0.00    0.00    0.00    0.00    0.00     3  |__exp3_sleep.out
01:34:43     1000         -      1168    0.00    0.00    0.00    0.00    0.00     8  |__exp3_sleep.out
01:34:43     1000         -      1169    0.00    0.00    0.00    0.00    0.00     8  |__exp3_sleep.out
Average:     1000      1153         -    0.00    0.00    0.00    0.00    0.00     -  exp3_sleep.out
Average:     1000         -      1153    0.00    0.00    0.00    0.00    0.00     -  |__exp3_sleep.out
Average:     1000         -      1154    0.00    0.00    0.00    0.00    0.00     -  |__exp3_sleep.out
Average:     1000         -      1155    0.00    0.00    0.00    0.00    0.00     -  |__exp3_sleep.out
Average:     1000         -      1156    0.00    0.00    0.00    0.00    0.00     -  |__exp3_sleep.out
Average:     1000         -      1157    0.00    0.00    0.00    0.00    0.00     -  |__exp3_sleep.out
Average:     1000         -      1158    0.00    0.00    0.00    0.00    0.00     -  |__exp3_sleep.out
Average:     1000         -      1159    0.00    0.00    0.00    0.00    0.00     -  |__exp3_sleep.out
Average:     1000         -      1160    0.00    0.00    0.00    0.00    0.00     -  |__exp3_sleep.out
Average:     1000         -      1161    0.00    0.00    0.00    0.00    0.00     -  |__exp3_sleep.out
Average:     1000         -      1162    0.00    0.00    0.00    0.00    0.00     -  |__exp3_sleep.out
Average:     1000         -      1163    0.00    0.00    0.00    0.00    0.00     -  |__exp3_sleep.out
Average:     1000         -      1164    0.00    0.00    0.00    0.00    0.00     -  |__exp3_sleep.out
Average:     1000         -      1165    0.00    0.00    0.00    0.00    0.00     -  |__exp3_sleep.out
Average:     1000         -      1166    0.00    0.00    0.00    0.00    0.00     -  |__exp3_sleep.out
Average:     1000         -      1167    0.00    0.00    0.00    0.00    0.00     -  |__exp3_sleep.out
Average:     1000         -      1168    0.00    0.00    0.00    0.00    0.00     -  |__exp3_sleep.out
Average:     1000         -      1169    0.00    0.00    0.00    0.00    0.00     -  |__exp3_sleep.out
Linux 6.6.87.2-microsoft-standard-WSL2 (Galu-PC)        10/12/26        _x86_64_   (12 CPU)

01:34:43      UID      TGID       TID   cswch/s nvcswch/s  Command
01:34:44     1000      1153         -      0.00      0.00  exp3_sleep.out
01:34:44     1000         -      1153      0.00      0.00  |__exp3_sleep.out
01:34:44     1000         -      1154      0.00      0.00  |__exp3_sleep.out
01:34:44     1000         -      1155      0.00      0.00  |__exp3_sleep.out
01:34:44     1000         -      1156      0.00      0.00  |__exp3_sleep.out
01:34:44     1000         -      1157      0.00      0.00  |__exp3_sleep.out
01:34:44     1000         -      1158      0.00      0.00  |__exp3_sleep.out
01:34:44     1000         -      1159      0.00      0.00  |__exp3_sleep.out
01:34:44     1000         -      1160      0.00      0.00  |__exp3_sleep.out
01:34:44     1000         -      1161      0.00      0.00  |__exp3_sleep.out
01:34:44     1000         -      1162      0.00      0.00  |__exp3_sleep.out
01:34:44     1000         -      1163      0.00      0.00  |__exp3_sleep.out
01:34:44     1000         -      1164      0.00      0.00  |__exp3_sleep.out
01:34:44     1000         -      1165      0.00      0.00  |__exp3_sleep.out
01:34:44     1000         -      1166      0.00      0.00  |__exp3_sleep.out
01:34:44     1000         -      1167      0.00      0.00  |__exp3_sleep.out
01:34:44     1000         -      1168      0.00      0.00  |__exp3_sleep.out
01:34:44     1000         -      1169      0.00      0.00  |__exp3_sleep.out
Average:     1000      1153         -      0.00      0.00  exp3_sleep.out
Average:     1000         -      1153      0.00      0.00  |__exp3_sleep.out
Average:     1000         -      1154      0.00      0.00  |__exp3_sleep.out
Average:     1000         -      1155      0.00      0.00  |__exp3_sleep.out
Average:     1000         -      1156      0.00      0.00  |__exp3_sleep.out
Average:     1000         -      1157      0.00      0.00  |__exp3_sleep.out
Average:     1000         -      1158      0.00      0.00  |__exp3_sleep.out
Average:     1000         -      1159      0.00      0.00  |__exp3_sleep.out
Average:     1000         -      1160      0.00      0.00  |__exp3_sleep.out
Average:     1000         -      1161      0.00      0.00  |__exp3_sleep.out
Average:     1000         -      1162      0.00      0.00  |__exp3_sleep.out
Average:     1000         -      1163      0.00      0.00  |__exp3_sleep.out
Average:     1000         -      1164      0.00      0.00  |__exp3_sleep.out
Average:     1000         -      1165      0.00      0.00  |__exp3_sleep.out
Average:     1000         -      1166      0.00      0.00  |__exp3_sleep.out
Average:     1000         -      1167      0.00      0.00  |__exp3_sleep.out
Average:     1000         -      1168      0.00      0.00  |__exp3_sleep.out
Average:     1000         -      1169      0.00      0.00  |__exp3_sleep.out
```

</details>

<details>
<summary>Raw output: Experiment 3, Part C (100 sleeping threads)</summary>

**Terminal 1**

```
aagam@Galu-PC:/mnt/c/Users/aagam/OneDrive/Desktop/Pulse$ ./Month2/Week1/exp3_sleep.out 100
PID 1546: starting 100 sleeping thread(s), 10 s each, on a machine with 12 CPUs
thread  1 (TID 1548) woke up and finished
thread  3 (TID 1550) woke up and finished
thread  4 (TID 1551) woke up and finished
thread  0 (TID 1547) woke up and finished
thread  5 (TID 1552) woke up and finished
thread  6 (TID 1553) woke up and finished
thread  2 (TID 1549) woke up and finished
thread  7 (TID 1554) woke up and finished
thread  8 (TID 1555) woke up and finished
thread  9 (TID 1556) woke up and finished
thread 10 (TID 1557) woke up and finished
thread 11 (TID 1558) woke up and finished
thread 12 (TID 1559) woke up and finished
thread 13 (TID 1560) woke up and finished
thread 14 (TID 1561) woke up and finished
thread 15 (TID 1562) woke up and finished
all 100 thread(s) done in 10.08 s
```

**Terminal 2**

```
aagam@Galu-PC:/mnt/c/Users/aagam/OneDrive/Desktop/Pulse$ P=$(pgrep -n exp3_sleep)
grep -E "Threads|VmSize|VmRSS" /proc/$P/status
ps -L -o stat= -p $P | sort | uniq -c
ps -L -o wchan:22= -p $P | sort | uniq -c
pidstat -u -p $P 1 1
VmSize:   826504 kB
VmRSS:      4352 kB
Threads:        101
    101 Sl+
      1 futex_wait_queue
    100 hrtimer_nanosleep
Linux 6.6.87.2-microsoft-standard-WSL2 (Galu-PC)        10/12/26        _x86_64_   (12 CPU)

01:38:50      UID       PID    %usr %system  %guest   %wait    %CPU   CPU  Command
01:38:51     1000      1546    0.00    0.00    0.00    0.00    0.00     8  exp3_sleep.out
Average:     1000      1546    0.00    0.00    0.00    0.00    0.00     -  exp3_sleep.out
```

</details>

---

# Week 1 done when

- [x] Experiment 0 recorded (the "before" for Week 2)
- [x] `exp1a_ab.cpp`, `exp1b_ab_many.cpp`, `exp2_busy.cpp`, `exp3_sleep.cpp` written and compiling with no warnings
- [ ] Every prediction written **before** running: done for Experiments 0 and 1. Experiment 2 only had prediction 1, and Experiment 3 had none. Worth doing fully in Week 2: the wrong predictions (Exp 0's `Recv-Q`, Exp 2's "5 / 10 / 20 / 40") taught the most
- [x] Every experiment's results written up here
- [ ] Can answer these without the docs (try first, then open the answers):
    1. Why does Experiment 2's total time stay roughly flat up to some thread count and then grow?
    2. Why does Experiment 3 use ~0% CPU no matter how many threads?
    3. Which of the two is Pulse more like, and why?

<details>
<summary>Answers</summary>

1. Busy threads each need a CPU the whole time. While there's a free CPU for each one, they all run at the same time, so total time ≈ one thread's time. On this laptop the per-thread cost already rises past about 6 threads, because the 12 "CPUs" are 6 real cores with 2 hyperthreads each. Past 12 threads there are no CPUs left: threads wait in line (`%wait` 14–36%), the scheduler forcibly switches them (~930 non-voluntary switches/s), and total time grows with the amount of work.
2. A sleeping thread is in state `S`. It isn't running and isn't in the queue for a CPU, so the scheduler ignores it until its timer fires. 100 sleeping threads = 0% CPU, 0 `%wait`, 0 context switches. What they cost instead is memory reserved for stacks (8,196 kB of `VmSize` each, but only ~4.6 kB of real RAM) and a little kernel time to create and destroy them.
3. Experiment 3. Pulse's threads spend almost all their time asleep in `read()` (Experiment 0 showed `S wait_woken`) waiting on the network, and the Month 1 benchmark showed Pulse isn't CPU-bound. So thread-per-connection can handle many slow clients cheaply. It would only start to look like Experiment 2 if many connections needed real CPU work at the same moment, and Week 4 measures where that happens.

</details>
