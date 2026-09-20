---
title: "Pulse — Month 1, Week 2: Concepts"
subtitle: "HTTP Parsing + Routing"
---

# How to use this document

Same format as Week 1: every concept gets an explanation, a blue **analogy** box, and a green **real example** box you can actually reproduce. This does **not** contain a working HTTP parser or router — building those is Week 2's exercise. Read once end-to-end before writing anything.

Week 1 built a server that reads raw bytes and echoes them back, with no idea what those bytes mean. Week 2's whole job is: give those bytes meaning.

---

# Part 1 — The anatomy of an HTTP request

An HTTP request is just **text**, sent over the TCP byte-stream connection you already built. Nothing magic — it's a specific, agreed-upon format layered on top of "here are some bytes."

```http
GET /hello HTTP/1.1
Host: localhost:8080
User-Agent: curl/8.5.0
Accept: */*

```

Broken into its named parts:

```text
GET /hello HTTP/1.1        <- the REQUEST LINE: method, path, HTTP version
Host: localhost:8080       <- a HEADER: name, colon, value
User-Agent: curl/8.5.0     <- another header
Accept: */*                <- another header
                            <- a BLANK LINE — this ends the headers
(body would go here, if any)
```

<div class="analogy">
<b>Analogy:</b> think of a paper letter. The request line is the envelope's front — "to this address, by this method" (GET/POST, like "by registered mail" vs "by courier"). The headers are metadata written on the envelope itself — sender, date, special instructions — read before you even open it. The blank line is the fold where the envelope ends and the actual letter (body) begins. TCP doesn't understand any of this structure — it just faithfully delivers the sealed envelope byte-for-byte; only your parser knows how to open it.
</div>

**The exact grammar**, since this is what your parser has to actually recognize:

- Request line: `METHOD SP PATH SP HTTP-VERSION CRLF` — three tokens separated by single spaces, ending in `\r\n` (CRLF — carriage return + line feed, not just `\n`; this is HTTP's line ending, even on Linux).
- Each header line: `Name: value CRLF` — a name, a colon, optional whitespace, the value, then `\r\n`.
- Headers end with **one blank line**: `\r\n` immediately after the last header, with nothing between the two `\r\n`s.
- Body (if present) comes immediately after that blank line, and its length is announced by the `Content-Length` header — the parser doesn't guess where the body ends, it's told.

<div class="example">
<b>Real example — see the actual bytes, not just the pretty-printed version:</b> run this in Ubuntu and look closely at the raw output:

```bash
curl -v http://localhost:8080/hello
```

```text
> GET /hello HTTP/1.1
> Host: localhost:8080
> User-Agent: curl/8.5.0
> Accept: */*
>
```

Every line curl marks with `>` is a line it actually sent, byte for byte — including that last bare `>` with nothing after it, which is the blank line. `curl -v` is one of the most useful tools for Week 2: it shows you exactly what bytes go out and come back, with no guessing.
</div>

---

# Part 2 — Why TCP can't do this part for you (recap + why it matters now)

From Week 1: TCP delivers a byte stream, reliably and in order, but has no concept of "requests" or "messages" — it doesn't know where one logically ends and another begins. That was an abstract point in Week 1. In Week 2 it becomes a concrete engineering problem: **your parser is the only thing standing between "a pile of bytes" and "a Method, a Path, and some Headers."**

<div class="example">
<b>Real example, proving it again, HTTP-flavored this time:</b> connect raw and type a request by hand:

```bash
nc localhost 8080
```
```http
GET /hello HTTP/1.1
Host: localhost

```
(press enter twice — once to end the Host header line, once more for the blank line)

Your Week 1 server would print these bytes and echo them back verbatim, with zero understanding that "GET" is a method or "/hello" is a path — they were just bytes. Week 2's parser is what turns this pile of bytes into something your code can actually make decisions about, like "route this to the /hello handler."
</div>

---

# Part 3 — Designing `Request` and `Response`

The project spec calls for internal representations:

```text
Raw bytes -> HTTP Parser -> Request -> Application -> Response -> Raw HTTP bytes
```

A `Request` is just a structured, parsed-out version of what Part 1 described — something like "a method, a path, a version, and a collection of header name/value pairs" (plus a body, once you support one). A `Response` is the mirror image on the way out: a status code, headers, and a body, which then needs to be **serialized** back into the exact text format from Part 1 before it goes out over `write()`.

<div class="analogy">
<b>Analogy:</b> the parser is a customs officer opening that sealed envelope from Part 1 and filling out a standardized form from its contents — "sender: X, method: registered mail, contents: ...". Everything after that point (routing, handling) works with the tidy form, not the original envelope. Before handing a reply back to the courier, someone has to re-seal it into a properly formatted envelope again (the Response, serialized back to bytes) — the courier (TCP/`write()`) doesn't know or care what's inside, same as Week 1.
</div>

**Design questions worth thinking through yourself before coding** (this is the actual Week 2 design exercise, not something to skip past):

- What's the minimal set of fields a `Request` needs to answer "which handler should run, and with what information"?
- Should header lookups be case-sensitive? (Real HTTP header names are case-insensitive — `Host` and `host` must be treated as the same header. Decide how your storage handles that before you build it, not after a bug shows up.)
- What should parsing do when the request is malformed (no blank line, missing version)? This is where `400 Bad Request` in Part 5 comes from.

---

# Part 4 — Routing

A router is just a lookup: given a `(method, path)` pair, find the right handler function, or report "no such route." Conceptually:

```text
GET /        -> handler that returns "Hello from Pulse!"
GET /hello   -> handler that returns "Hello from Pulse!"
GET /health  -> handler that returns "OK"
(anything else) -> 404 Not Found
```

<div class="analogy">
<b>Analogy:</b> a hotel switchboard. Every call comes in on the same phone line (the same listening socket, same `accept()` loop from Week 1) — the switchboard operator's only job is reading the room number the caller asked for (the path) and connecting the call to the right room (handler). The operator doesn't cook the room's dinner or make the bed — routing is purely "which handler runs," not the handler's own logic.
</div>

<div class="example">
<b>Real example of what "no such route" looks like on the wire:</b>

```bash
curl -v http://localhost:8080/does-not-exist
```

A correctly-behaving server responds with a real, well-formed HTTP response — just with a 404 status instead of 200:

```text
< HTTP/1.1 404 Not Found
< Content-Length: 0
<
```

Note this is NOT the connection failing or `curl` erroring out — the TCP connection succeeds fine, your server accepted it, read it, parsed it, and made a deliberate decision to respond with 404. That's the router doing its job correctly, not a bug.
</div>

---

# Part 5 — Status codes and building a real response

An HTTP response has the same shape as a request, mirrored:

```http
HTTP/1.1 200 OK
Content-Type: text/plain
Content-Length: 18

Hello from Pulse!
```

```text
HTTP/1.1 200 OK             <- STATUS LINE: version, status code, reason phrase
Content-Type: text/plain    <- header
Content-Length: 18          <- header — MUST match the body's exact byte length
                             <- blank line, same rule as requests
Hello from Pulse!           <- body
```

**Why `Content-Length` matters so much:** unlike your Week 1 echo server (which just wrote whatever bytes it read, then closed the connection so the client knew "that's everything"), a real HTTP client needs to know *exactly* how many body bytes to expect, because with `Connection: keep-alive` the connection might stay open for more requests afterward — there's no "close the connection to signal I'm done" available anymore. `Content-Length` is what tells the client precisely where this response ends.

<div class="example">
<b>Real example of a Content-Length mismatch bug, worth causing on purpose:</b> hand-write a response with the wrong Content-Length (e.g. claim 20 when the body is actually 18 bytes) and test it with curl. You'll see curl either hang waiting for 2 more bytes that never come, or misbehave in some other way — a very concrete demonstration of why this header isn't optional decoration.
</div>

Status codes you need this week, and when each applies:

| Code | Meaning | When your server sends it |
|---|---|---|
| `200 OK` | Success | A known route, valid request |
| `400 Bad Request` | Client sent malformed HTTP | Parser couldn't make sense of the request line/headers at all |
| `404 Not Found` | No such route | Method+path the router doesn't recognize |
| `405 Method Not Allowed` | Path exists, method doesn't | e.g. `POST /health` if `/health` only supports GET |

---

# Part 6 — Testing this week

```bash
curl -v http://localhost:8080/
curl -v http://localhost:8080/hello
curl -v http://localhost:8080/health
curl -v http://localhost:8080/random          # expect 404
curl -v -X POST http://localhost:8080/hello   # expect 405, once you support it
```

Also test with an actual browser — open `http://localhost:8080/hello` and confirm the page renders. A browser sends real-world headers your hand-typed `nc` tests won't (cookies, `Accept-Language`, etc.) — good for noticing if your parser is too fragile about headers it doesn't recognize (it should ignore unknown headers, not fail on them).

And once more, raw and manual — this is still the most important habit from Week 1, don't drop it just because `curl` is more convenient:

```bash
nc localhost 8080
```
```http
GET /hello HTTP/1.1
Host: localhost

```

---

# Part 7 — Concept check (answer these yourself before moving to Week 3)

**Why can't the parser just split on spaces and call it done?** Because headers can contain values with spaces in them (`User-Agent: curl/8.5.0 libcurl/...`), and a naive space-split doesn't respect the request line vs. header-line vs. blank-line structure — the grammar in Part 1 has to be followed by line first, then by field within each line.

**Why must header name comparisons be case-insensitive?** Because the HTTP spec defines them that way — a client is free to send `host:` or `Host:` or `HOST:` and mean the same thing. If your lookup is case-sensitive, a perfectly valid request can silently fail to match a header your handler expected.

**Why does a malformed request deserve `400`, not just closing the connection?** Because `400` is itself a valid, well-formed HTTP response — it tells the client (and whoever's debugging with `curl -v`) specifically "I understood you were trying to speak HTTP, and what you sent doesn't parse," which is far more diagnosable than the connection just silently dropping.

**Why is `Content-Length` the body's job to announce, rather than the parser detecting "end of body" some other way?** Because TCP is a stream with no built-in message boundaries (Week 1, Part 3) — there's no "end of body" marker in the bytes themselves that isn't ambiguous with real body content. The length has to be stated up front so the reader knows exactly how many bytes to treat as body versus the start of whatever comes next.

**What should happen if two different routes could both match a request?** Trick question, worth thinking about: with the simple `(method, path)` exact-match router this week calls for, this can't actually happen — every `(method, path)` pair maps to at most one handler. This becomes a real design question later if you ever add pattern-matched routes (`/users/:id`) — not this week's problem, but worth noticing why it isn't one yet.
