#pragma once
/* Month 1 / Week 1 — C++ note: #pragma once tells the compiler "if this file
   gets #include'd more than once while building one .cpp, only process it
   the first time." Without it, including the same header twice (which
   happens easily once headers include other headers) would redefine the
   class twice and fail to compile. */

/* =============================================================================
 * C++ BACKGROUND — lvalues, rvalues, and why & and && are different
 * -----------------------------------------------------------------------------
 * Forget Socket for a moment. In C++, every value is either an LVALUE or an
 * RVALUE:
 *
 *   - an LVALUE has a name and a lasting location — a variable. In
 *     `int x = 5;`, x is an lvalue: it has a name, it sticks around, you
 *     could take its address (&x).
 *
 *   - an RVALUE is a temporary with no name, about to disappear — the
 *     literal `5` itself, or the result of `x + 1`, or a freshly-built
 *     object written inline like `Socket(6)`. Nothing refers to it after
 *     the current expression finishes.
 *
 * THE MOTIVATION — why && needs to exist at all, not just &:
 * Before C++ had &&, the only way to hand an object to a function or
 * constructor was to COPY it. For a class like Socket that owns a raw
 * resource (an fd), copying is nonsensical — you'd get two C++ objects
 * both thinking they own the SAME fd, and whichever is destroyed first
 * closes it out from under the other (exactly why the copy constructor is
 * deleted further down). But we still need SOME way to hand a Socket from
 * one place to another (e.g. from run() into handle_client() in
 * tcp_server.cpp). If all we had was &, we could look at someone else's
 * still-alive Socket, but we could never just reach in and steal its fd —
 * the caller might still need it. && is the language's way of marking "this
 * particular object is safe to gut, because it's disposable anyway" — a
 * compile-time signal meaning "nobody needs this value after this point, so
 * stealing its insides instead of copying them is fine."
 *
 *   Analogy: if a friend is still living in their apartment (a named
 *   variable — someone's using it), you can't just walk in and take their
 *   furniture — you'd have to buy your own copy of each item. But if a
 *   friend is moving out today and the apartment's about to be demolished
 *   (a temporary — nobody needs it after this moment), you can just carry
 *   their actual furniture into your place — far cheaper, and nobody's
 *   harmed since that apartment was being emptied anyway. "&" only ever
 *   refers to "someone's still living there" apartments. "&&" exists
 *   specifically to mark "this one's being vacated, take what you want."
 *
 * THE RULE: "&" (a normal/lvalue reference) can ONLY bind to lvalues.
 * "&&" (an rvalue reference) can ONLY bind to rvalues.
 *
 *     int x = 5;
 *
 *     int& r1 = x;    // OK    — x is an lvalue, & can bind to it
 *     int& r2 = 5;    // ERROR — 5 is a temporary (rvalue), & refuses it
 *
 *     int&& r3 = 5;   // OK    — 5 is a temporary, && exists exactly for this
 *     int&& r4 = x;   // ERROR — x is an lvalue, && refuses it
 *
 * WHAT std::move(x) ACTUALLY DOES — this is the part that trips everyone
 * up: it does NOTHING by itself. It doesn't move any bytes, doesn't call
 * any function, doesn't touch x at all. It is purely a RELABEL — roughly
 * equivalent to `static_cast<Socket&&>(x)`. `x` is a real, named variable
 * that the compiler would normally protect from && binding to it.
 * std::move(x) is you, the programmer, personally overriding that
 * protection: "I know x still has a name, but treat it as disposable
 * anyway — I promise not to use it in its old form after this line." That
 * relabeling is ALL std::move does. It just changes which constructor
 * overload resolution picks:
 *
 *     int&& r5 = std::move(x);   // OK now — std::move disguised x as a temporary
 *
 * The ACTUAL stealing happens one step later, inside whichever function
 * body gets selected as a result — NOT inside std::move itself. Mapped
 * onto the two Socket constructors declared just below this comment block:
 *
 *     Socket(const Socket& other);   // takes an LVALUE — a real, still-existing Socket (deleted, further down)
 *     Socket(Socket&& other);        // takes an RVALUE — a temporary, about-to-vanish Socket (this is the move constructor)
 *
 *     Socket a(5);
 *     Socket b(a);               // a is an lvalue -> needs the copy constructor -> deleted -> COMPILE ERROR
 *     Socket b(std::move(a));    // step 1: std::move(a) is JUST a cast/relabel — a's fd_ is untouched so far, it only makes `a` eligible to bind to Socket(Socket&&)
 *                                // step 2: THIS is where the real work happens, inside Socket::Socket(Socket&& other)'s body:
 *                                //         fd_(std::exchange(other.fd_, -1))  — reads other.fd_ (= a's fd_), copies it into b's fd_, sets a's fd_ to -1
 *     Socket c(Socket(6));       // Socket(6) is ALREADY a temporary/rvalue -> calls the move constructor directly, no std::move needed
 *
 * So: std::move is a permission slip, not a mover. It only tells the
 * compiler which constructor is ALLOWED to run; the constructor's own body
 * is what actually transfers the fd and empties the source. That last
 * example line is exactly what tcp_server.cpp does in
 * `handle_client(Socket(client_fd))` — `Socket(client_fd)` is a temporary,
 * so it binds to the && overload, and ownership moves into the function
 * instead of requiring a (deleted) copy.
 * ============================================================================= */

/* =============================================================================
 * MONTH 1 / WEEK 1 — RAII around a raw file descriptor
 * -----------------------------------------------------------------------------
 * Recall from the Week 1 doc (Part 1, "RAII"): socket()/accept() hand you back
 * a plain int (a file descriptor). Nothing forces you to call close() on it —
 * if you forget on some code path (an early return, an exception), that fd is
 * leaked for the life of the process. RAII fixes this by tying the fd's
 * lifetime to an OBJECT's lifetime: as long as a Socket object exists, it owns
 * exactly one fd, and the instant that object is destroyed (goes out of
 * scope, whatever the reason), its destructor closes the fd automatically.
 * You will see this pay off directly in tcp_server.cpp.
 * ============================================================================= */
class Socket
{
public:
   /* Month 1 / Week 1 — C++ note: "noexcept" is a promise to the compiler
      that this function will never throw an exception. It's appropriate
      here because these constructors do nothing that can fail (no
      syscalls, just storing an int). */

   Socket() noexcept; // an "empty" Socket — holds no real fd (fd_ == -1)

   /* Month 1 / Week 1 — C++ note: "explicit" stops this constructor from
      being used for silent, implicit conversions. Without it, a plain int
      could get quietly turned into a Socket anywhere one is expected —
      e.g. calling some_function(42) would silently.+ construct a temporary
      Socket(42), which then takes ownership of fd 42 and will close() it
      when that temporary is destroyed. That's exactly the kind of "closed
      a file descriptor I didn't mean to hand over" bug this class exists
      to prevent. "explicit" forces the conversion to be written out loud:
      some_function(Socket(42)) instead of some_function(42) — the
      ownership transfer becomes visible at the call site instead of
      happening invisibly. */
   explicit Socket(int fd) noexcept; // takes ownership of an fd you already obtained (e.g. from ::socket() or ::accept())

   ~Socket(); // MONTH 1 / WEEK 1: this is where close() actually gets called — see socket.cpp

   /* Month 1 / Week 1 — C++ note: "= delete" means "this function does not
      exist — do not let anyone call it." We delete the copy constructor and
      copy-assignment on purpose: if you could copy a Socket, you'd end up
      with two separate C++ objects both believing they own and must close
      the SAME underlying fd. Whichever one gets destroyed first closes the
      fd; the second one then closes an already-closed (or worse,
      reused-by-someone-else) fd. Deleting these functions makes that
      mistake a compile error instead of a runtime bug. */
   Socket(const Socket &) = delete;
   Socket &operator=(const Socket &) = delete;

   /* Month 1 / Week 1 — C++ note: MOVING is different from copying. A move
      transfers ownership from one object to another — the source object is
      left empty (fd_ = -1) afterward, so only ONE object ever owns the real
      fd at a time. This is what lets us do things like
      `handle_client(Socket(client_fd))` in tcp_server.cpp — the temporary
      Socket's ownership moves into the function parameter instead of being
      copied (which isn't even allowed) or leaking.

      C++ note on & vs &&: "&" after a type (as in the deleted
      `const Socket&` above, or the `Socket&` return type below) is an
      LVALUE reference — it binds to an existing, named object. "&&" here
      is an RVALUE reference — it only binds to TEMPORARIES (things with no
      persistent name, like `Socket(client_fd)` written inline, or anything
      wrapped in std::move(...)). Having both a `Socket(const Socket&)`
      overload (deleted, above) and a `Socket(Socket&&)` overload (this
      one) is what lets the compiler pick "copy" for a real, persisting
      object and "move" for a disposable temporary automatically — that
      choice is the entire mechanism behind move semantics. */
   Socket(Socket &&other) noexcept;
   Socket &operator=(Socket &&other) noexcept;

   /* Month 1 / Week 1 — C++ note: "const" here (after the parameter list)
      is a promise that calling this function will NOT modify the Socket
      it's called on — it only reads fd_, never writes it. This isn't just
      documentation; the compiler enforces it — writing "fd_ = -1;" inside
      a const function like this simply would not compile. It also matters
      practically: a const function can be called on a `const Socket&`,
      whereas a non-const one couldn't be. fd() and valid() are pure
      getters, so marking them const is both accurate and required for
      them to be usable in read-only contexts. */
   /* Month 1 / Week 1 — C++ note: this function's body ({ return fd_; }) is
      written directly here, inside the class — not just declared here with
      the real body off in socket.cpp (compare to close(), declared below
      and defined only in socket.cpp). Because this header gets #include'd
      by multiple .cpp files (socket.cpp, tcp_server.cpp, and transitively
      main.cpp), each one compiles its OWN copy of this body — which would
      normally cause the exact "multiple definition" linker error described
      for to_lower_ascii in http_util.hpp. It doesn't, because C++ has a
      specific rule for exactly this case: a member function written
      entirely inside its class definition is AUTOMATICALLY treated as if
      you had typed "inline" in front of it — the compiler grants that
      permission silently. close() needs no such permission at all, because
      its one real definition lives in exactly one .cpp file and is
      therefore only ever compiled once, no matter how many files include
      this header. */
   int fd() const noexcept { return fd_; } // read-only access to the raw fd, for passing into the actual POSIX calls (::read, ::write, ::bind, etc.) which only know how to work with plain ints, not our C++ class

   bool valid() const noexcept { return fd_ != -1; } // true if this Socket currently owns a real, open fd

   /* Month 1 / Week 1: closes the fd right now instead of waiting for the
      object to be destroyed. Safe to call more than once (calling it on an
      already-closed Socket does nothing) — see the fd_ != -1 check in the
      .cpp file. */
   void close() noexcept;

private:
   int fd_; // the ONE thing this class manages: a raw POSIX file descriptor, or -1 if empty
};
