#include "socket.hpp"

#include <unistd.h>   // Month 1 / Week 1: declares ::close() — the actual POSIX syscall wrapper
#include <utility>    // declares std::exchange, used below

/* Month 1 / Week 1 — C++ note: writing "::close" (with the leading ::)
   instead of just "close" means "the close from the GLOBAL namespace" —
   i.e. the real POSIX function, not our own Socket::close() method below,
   even though both are named "close". Without the ::, inside Socket::close()
   the compiler would resolve "close" to itself and infinitely recurse. */

Socket::Socket() noexcept : fd_(-1) {}
/* Month 1 / Week 1: an empty Socket. fd_ = -1 is the sentinel value meaning
   "not a real, open file descriptor" — every check in this file tests
   against exactly this value. */

Socket::Socket(int fd) noexcept : fd_(fd) {}
/* Month 1 / Week 1: takes ownership of an fd that ALREADY EXISTS (created by
   a call to ::socket() or ::accept() elsewhere). This constructor doesn't
   create anything itself — it just starts tracking an fd that was handed to
   it. */

Socket::~Socket()
{
    /* Month 1 / Week 1: THIS is the entire point of the class. Whatever
       caused this object to be destroyed — the function it lived in
       returned normally, returned early, or an exception unwound the
       stack — this line runs regardless, guaranteeing the fd is never
       leaked. */
    close();
}

Socket::Socket(Socket&& other) noexcept : fd_(std::exchange(other.fd_, -1))
{
    /* Month 1 / Week 1 — C++ note: std::exchange(other.fd_, -1) does two
       things in one step: (1) reads other.fd_'s current value to initialize
       OUR fd_, and (2) immediately sets other.fd_ to -1. After this line,
       "other" is an empty Socket and "this" is the new sole owner of the
       fd — exactly the ownership transfer described in the header.

       Notice there's no "this == &other" self-check here, unlike the move
       ASSIGNMENT operator below. That's not an oversight — a constructor's
       entire job is bringing a brand-new object into existence, so at this
       point "this" (the object being built) has no prior state to protect,
       and there is no legal way to construct an object "from itself" (the
       object being built doesn't exist yet, so it can never simultaneously
       be its own source). Self-construction just isn't a scenario a
       constructor can be in — only self-ASSIGNMENT (on two already-living
       objects) is, which is exactly why that check lives below instead. */
}

Socket& Socket::operator=(Socket&& other) noexcept
{
    /* Month 1 / Week 1 — C++ note: "&other" here is the address-of
       operator applied to the parameter "other" (turning a Socket into a
       Socket*) — a different meaning of "&" than the reference-type "&"/
       "&&" seen in the header (see the note there). "this" is already a
       Socket*, so "this != &other" is asking "are these literally the
       same object in memory?" — i.e. guarding against `a = std::move(a)`.

       Why guard here but not in the move constructor above? This operator
       runs on an object that ALREADY EXISTS and may already own a real fd.
       If `this == &other` (self-assignment) and we didn't check: close()
       below would close fd_ — but fd_ IS other.fd_, the same memory — so
       the very next line would read other.fd_ right after it was just
       zeroed out by that close(). The object would silently end up empty
       even though self-assignment is supposed to be a no-op. The guard
       catches "old value and new value are the same object" before the
       release-then-acquire sequence can do that damage. A constructor
       never faces this because it never has an "old value" to release in
       the first place. */
    if (this != &other)
    {
        /* Month 1 / Week 1: if THIS Socket already owned a different fd,
           close it first — otherwise that old fd would be silently leaked
           and overwritten. */
        close();
        fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
}

void Socket::close() noexcept
{
    if (fd_ != -1)
    {
        ::close(fd_);   // Month 1 / Week 1: the actual syscall from Part 4 of the doc — releases the fd back to the kernel, begins connection teardown if this was a socket
        fd_ = -1;        // mark this Socket as empty so a second close() call (or the destructor running later) does nothing
    }
}
