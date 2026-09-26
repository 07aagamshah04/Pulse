---
title: "Pulse — Month 1, Week 3: Concepts"
subtitle: "Making the Server Correct — Partial Reads, POST Bodies, More Headers"
---

# How to use this document

Same format as Weeks 1 and 2: explanation, a blue **analogy** box, a green **real example** box. No new server code here — this is what you read before writing anything.

Week 3 is different in character from Weeks 1 and 2: it's not mostly new features, it's **fixing a bug your server has had since the day it was born, and hardening the HTTP layer around the edges.** The bug is bigger than it looks.

---

# Part 1 — the bug you've been carrying since Week 1

Every version of Pulse so far does this in `handle_client`:

```cpp
ssize_t n = ::read(client.fd(), buf, sizeof(buf));
// ... treat buf[0..n) as if it's THE ENTIRE request
```

**One `read()` call, assumed to contain one complete request.** This has quietly worked in every test you've run so far — `curl`, a hand-typed `nc` request, a browser — for one specific reason: on `localhost`, over a fast loopback connection, with small requests, the whole request usually *does* arrive in a single `read()`. "Usually works in testing" is exactly the kind of bug that survives until production and then fails unpredictably.

<div class="analogy">
<b>Analogy:</b> imagine reading a letter by opening the mailbox once and assuming whatever's sitting in there right now is the complete letter — even if the postal carrier is still walking up the driveway with the rest of the pages. On a quiet street (loopback, small requests) you'll almost always get lucky and the whole letter arrives before you check. On a busy street (a slow network, a large request, a loaded server), pages arrive at different times, and "check the mailbox once" starts silently truncating letters.
</div>

**Why this has been true since Week 1, not something new in Week 3:** TCP's job (Week 1, Part 3) is delivering an ordered, reliable byte *stream*. It makes zero promises about how those bytes get chunked across individual `read()` calls — the kernel is free to hand you 1 byte, 4096 bytes, or the whole request in one call, based on network timing, packet sizes, and OS buffering that your program has no visibility into or control over.

---

# Part 2 — proving it, concretely

<div class="example">
<b>Real example — reproduce the actual bug</b> (this is the "IMPORTANT TCP EXPERIMENT" from the project spec, and it's worth actually running against your current Week 2 server before fixing anything): write a tiny Python client that deliberately sends a request in pieces, with a delay between them:

```python
import socket, time
s = socket.create_connection(("localhost", 8080))
s.send(b"GET /hel")
time.sleep(1)
s.send(b"lo HTTP/1.1\r\nHost: localhost\r\n\r\n")
print(s.recv(4096))
```

Run this against your current (Week 2) server. Because `handle_client` only calls `read()` once, it's a coin flip whether the kernel hands back `"GET /hel"` alone (arrived before the 1-second sleep ends) or the complete request (if enough time passed). If it's the former, your parser sees `"GET /hel"`, finds no `\r\n\r\n` anywhere in it, and returns `std::nullopt` — the server responds `400 Bad Request` to what was actually a perfectly valid, just slowly-delivered, request. That's a real correctness bug, not a hypothetical one, and you just watched it happen.
</div>

---

# Part 3 — the fix: accumulate until you have enough

The fix is not "read a bigger buffer" — no buffer size is guaranteed large enough, and the fragmentation could happen for any request regardless of size. The fix is a **loop**: keep calling `read()`, appending each call's bytes onto a growing buffer, until you can positively determine you have a *complete* request — then, and only then, parse it.

"Complete" has two layers, matching the two parts of an HTTP message:

1. **Headers complete**: keep reading until the accumulated buffer contains `\r\n\r\n` somewhere in it. Before that point, you don't even know how many headers there are yet, let alone whether there's a body.
2. **Body complete** (only relevant if a body is expected): once you've found the header/body boundary and parsed out `Content-Length`, keep reading until the accumulated bytes *after* that boundary reach at least that many bytes. Before that, the body itself might be sitting there truncated.

<div class="analogy">
<b>Analogy:</b> you're now checking the mailbox repeatedly, not once — and you know how to tell "the letter is complete": first you look for the envelope's closing seal (the blank line, "headers complete"), and if the letter mentions "see attached, 20 pages" (Content-Length), you keep checking until you've actually received all 20 attached pages before considering the whole letter done.
</div>

**What does NOT need to change:** `HttpParser::parse()` itself barely changes — it already takes a `std::string` and returns `std::optional<HttpRequest>`. What changes is *when* you call it: no longer "immediately after one `read()`," but "after a loop has confirmed the accumulated buffer is a complete request." The parser's job (turn complete bytes into a `Request`) and the reading loop's job (figure out when bytes are complete enough) are genuinely separate responsibilities — worth keeping them as separate functions rather than merging the loop into the parser itself.

<div class="example">
<b>Real example of the design question this raises</b> (think this through before coding, don't just copy a pattern): should the "have I read enough yet" check be a completely separate function from `HttpParser::parse`, or should `parse` itself be changed to report "incomplete, need more bytes" as a third outcome (alongside "valid request" and "malformed")? Both are legitimate designs used in real HTTP libraries. Consider: what does the loop need to know that a binary "did it parse or not" `std::optional` currently can't tell it apart — specifically, the difference between "this will never be valid no matter how many more bytes arrive" (truly malformed) and "this isn't complete YET, keep reading" (just needs more bytes)?
</div>

---

# Part 4 — a subtlety: what if the client sends MORE bytes than one request?

Once you're accumulating into a buffer instead of trusting a single `read()`, a new question appears that didn't exist before: what if the accumulated buffer, after you find the `\r\n\r\n` and any body, has **leftover bytes past the end of this one request**? (E.g. a client pipelining two requests back-to-back, or your fixed-size read buffer happening to pull in the start of whatever the client sends next.)

For Week 3's scope, per your project spec, you are **not** required to support persistent/keep-alive connections or pipelining — one request per connection is still the model. But your accumulation loop should at least not silently corrupt or lose those leftover bytes; it's fine to simply ignore/discard anything after the first complete request for now, as long as that's a deliberate, commented decision rather than an accident. This is exactly the kind of edge case worth noticing now even if you're choosing not to fully solve it yet — that's the difference between "simple because the scope says so" and "simple because you didn't think about it."

---

# Part 5 — POST bodies and `/echo`

The project spec's Week 3 example:

```http
POST /echo HTTP/1.1
Content-Length: 5

hello
```

Response:

```text
hello
```

This is now straightforward given what you already have: `HttpRequest::body` (Part 3 of the Week 2 doc) already gets populated by the parser when `Content-Length` is present — Week 3's accumulation fix is what makes that population *reliable* even when the body arrives split across multiple reads. The actual `/echo` route is a short handler: read `request.body`, put it straight into the response body.

<div class="example">
<b>Real example worth testing once you build this:</b> send a POST with a body larger than a single small buffer would comfortably hold in one piece — e.g. write a client that sends the headers, sleeps, then sends the body in two separate `send()` calls. If your accumulation loop is correct, `/echo` still returns the complete body. If you only fixed the "find `\r\n\r\n`" half of Part 3 and forgot the "keep reading until the body itself is complete" half, this test will catch that specific gap.
</div>

---

# Part 6 — headers worth handling more deliberately this week

- **`Content-Type`**: you're already sending this on responses (Week 2). Worth reading it on *requests* now too, at least to notice it exists — a `POST /echo` doesn't strictly need to inspect it to just echo bytes back, but a real API would use it to decide how to interpret the body (JSON vs. form-encoded vs. plain text).
- **`Connection`**: your server already behaves like `Connection: close` — every request gets exactly one response, then the socket closes (Part 5 of `TcpServer::handle_client`, unchanged since Week 1). Worth being explicit about this rather than accidental: consider adding a `Connection: close` header to every response, so any client talking to you knows not to expect the connection to stay open for a second request. This is a small, honest addition — you are not implementing keep-alive, so don't imply you might.

---

# Part 7 — Linux experiments, revisited

Same tools as Weeks 1 and 2, but now look for something new:

```bash
strace -f -e trace=read,write ./pulse 8080
```

Send a request via the slow Python client from Part 2 against your **fixed** Week 3 server. Where Week 1/2's `strace` output showed exactly one `read()` per connection, you should now see **multiple** `read()` calls for that single slow-arriving request — direct, observable proof that your accumulation loop is actually looping, not just theoretically correct on paper.

```bash
ss -ltnp
ls -l /proc/<PID>/fd
```

Nothing changes about the FD story from Week 1's Part 5 — still one listening FD, one FD per active connection — but worth re-confirming after a real refactor, not just assuming it still holds.

---

# Part 8 — testing this week

```bash
# the slow client from Part 2 — must now succeed, not intermittently 400
python3 slow_client.py

# POST with a body
curl -i -X POST http://localhost:8080/echo -d "hello"

# a large-ish POST, to make partial reads more likely without needing an artificial delay
curl -i -X POST http://localhost:8080/echo -d "$(python3 -c 'print("x"*100000)')"

# confirm ordinary Week 2 behavior didn't regress
curl -i http://localhost:8080/
curl -i http://localhost:8080/doesnotexist
```

---

# Part 9 — concept check

**Why doesn't making the read buffer bigger (e.g. 65536 instead of 4096) fix this?** Because no fixed size is guaranteed sufficient — the fragmentation isn't about the buffer being "too small" for the data, it's that TCP makes no promises about chunk boundaries at all, regardless of how much room you leave. A request could arrive in two pieces even if both pieces together are only 20 bytes, purely due to network/OS timing.

**Why is "headers complete" a different check from "body complete"?** Because you cannot know how many body bytes to expect until you've parsed `Content-Length` out of the headers — and you can't safely parse headers at all until you know where they end (the blank line). The two checks have a real dependency order: headers-complete must be confirmed before body-complete is even a meaningful question.

**Why is one `read()` call sometimes enough in testing but not reliably enough in general?** Because on a fast loopback connection with small payloads, the OS very often *does* deliver everything in one call — it's not disallowed, just not guaranteed. Tests that only exercise the common case can pass indefinitely while the bug sits there waiting for different network conditions (a slower connection, a larger request, a busier server) to expose it.

**What real bug would silently accepting "leftover bytes past this request" as if they were garbage cause later, if you added keep-alive connections in a future month?** Those leftover bytes are very likely the START of the client's next legitimate request on the same connection — discarding them wouldn't just be "extra unused data," it would corrupt the next request that connection ever sends. Worth understanding even though Week 3 doesn't require solving it yet.
