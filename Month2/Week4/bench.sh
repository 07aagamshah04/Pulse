#!/usr/bin/env bash
# =============================================================================
# MONTH 2 / WEEK 4 — Experiments B and C: the concurrency sweep
# (Week 4 doc, Parts 4–8)
# -----------------------------------------------------------------------------
# Runs `ab -n <requests> -c C http://localhost:<port>/hello` for each
# concurrency C, against whichever Pulse is running (Month 1 or Month 2), and
# prints ONE line per C with everything the spec's table asks for:
#
#   req/s            ab "Requests per second"
#   p50 p95 p99 max  ab's percentile table, in ms (Week 4 doc, Part 6)
#   failed           ab "Failed requests" (should be 0)
#   main%            CPU used by Pulse's MAIN thread during the run.
#                    THE DOORMAN CHECK (doc Part 5): near 100% = the one
#                    accept loop is the bottleneck.
#   proc%            CPU used by the whole Pulse process (all threads added up;
#                    can go above 100%, Week 1)
#   peakThr          highest thread count seen during the run
#   peakRSS          highest VmRSS (kB) seen during the run
#   cs/s             system-wide context switches per second during the run
#   ab%              CPU used by the CLIENT, ab itself. ab is single-threaded, so
#                    ~99% means ab is maxed out: the measured req/s is then ab's
#                    limit, not Pulse's (added 2026-11-01 after Test B found this)
#
# How CPU is measured: the kernel keeps a running total of CPU time for every
# thread (/proc/<pid>/task/<tid>/stat, fields 14+15 = user+system time, in
# "clock ticks" of 1/100 s). Read it before and after the run:
#   CPU% = ticks used / (seconds elapsed x 100) x 100
# The main thread's TID equals the PID (Week 1 doc, Part 6).
#
# Usage (Pulse already running, log to /dev/null):
#   ./Month2/Week4/bench.sh <label> [port] [requests] [concurrencies...]
#   ./Month2/Week4/bench.sh month2 8080
#   ./Month2/Week4/bench.sh month1 8080 20000 1 10 50 100 200
# =============================================================================

LABEL=${1:?usage: bench.sh <label> [port] [requests] [concurrencies...]}
PORT=${2:-8080}
N=${3:-20000}
# Concurrency levels = any arguments AFTER the first three; default 1 10 50 100 200.
# (Bug fixed 2026-11-01: this used to be `shift 3` + "$*", but `shift 3` does
# nothing at all when fewer than 3 arguments were given — so `bench.sh month2 8080`
# used "month2 8080" as the concurrency list.)
if [ $# -gt 3 ]; then
    shift 3
    CONCS="$*"
else
    CONCS="1 10 50 100 200"
fi
for C in $CONCS; do
    case $C in
        ''|*[!0-9]*) echo "concurrency '$C' is not a number"; exit 1 ;;
    esac
    if [ "$C" -gt 1000 ]; then echo "refusing -c $C (Week 4 doc: stay well below the limits)"; exit 1; fi
done

P=$(pgrep -x pulse)
if [ "$(echo "$P" | wc -w)" -ne 1 ]; then
    echo "Need exactly one running 'pulse' process (found: ${P:-none})."; exit 1
fi

# CPU ticks (user + system) for one thread, or for the whole process.
ticks() { awk '{print $14 + $15}' "$1" 2>/dev/null || echo 0; }
ctxt()  { awk '/^ctxt/ {print $2}' /proc/stat; }

# Background sampler: every 0.1 s, track the highest Threads and VmRSS seen.
# Writes "peakThreads peakRSS" to a file when told to stop (SIGTERM).
sampler() {
    local maxT=0 maxR=0 t r
    trap 'echo "$maxT $maxR" > "$1"; exit 0' TERM
    while :; do
        t=$(awk '/^Threads/ {print $2}' /proc/$P/status 2>/dev/null)
        r=$(awk '/^VmRSS/   {print $2}' /proc/$P/status 2>/dev/null)
        [ -n "$t" ] && [ "$t" -gt "$maxT" ] && maxT=$t
        [ -n "$r" ] && [ "$r" -gt "$maxR" ] && maxR=$r
        sleep 0.1
    done
}

echo "Pulse PID $P | $LABEL | ab -n $N /hello | $(date '+%F %T')"
echo "warm-up run (not measured)..."
ab -q -n 2000 -c 10 "http://localhost:$PORT/hello" >/dev/null 2>&1

printf "%-8s %5s %9s %5s %5s %5s %6s %6s %7s %7s %8s %9s %9s %6s\n" \
    label c req/s p50 p95 p99 max failed main% proc% peakThr peakRSS cs/s ab%
for C in $CONCS; do
    TMP=$(mktemp)
    sampler "$TMP.peak" & SPID=$!

    m0=$(ticks /proc/$P/task/$P/stat); p0=$(ticks /proc/$P/stat); x0=$(ctxt); t0=$(date +%s.%N)
    # /usr/bin/time -f "%P" -o FILE: run ab, and write ab's own CPU% into FILE
    /usr/bin/time -f "%P" -o "$TMP.abcpu" ab -q -n "$N" -c "$C" "http://localhost:$PORT/hello" > "$TMP" 2>&1
    t1=$(date +%s.%N); m1=$(ticks /proc/$P/task/$P/stat); p1=$(ticks /proc/$P/stat); x1=$(ctxt)

    kill -TERM $SPID; wait $SPID 2>/dev/null
    read -r peakT peakR < "$TMP.peak"

    rps=$(awk '/Requests per second/ {print $4}' "$TMP")
    failed=$(awk '/Failed requests/ {print $3}' "$TMP")
    pct() { awk -v k="$1" '$1 == k {print $2}' "$TMP"; }
    el=$(echo "$t1 - $t0" | bc -l)
    mainpct=$(echo "($m1 - $m0) / ($el * 100) * 100" | bc -l)
    procpct=$(echo "($p1 - $p0) / ($el * 100) * 100" | bc -l)
    cs=$(echo "($x1 - $x0) / $el" | bc -l)

    abcpu=$(tail -1 "$TMP.abcpu" 2>/dev/null)

    printf "%-8s %5s %9.1f %5s %5s %5s %6s %6s %6.0f%% %6.0f%% %8s %9s %9.0f %6s\n" \
        "$LABEL" "$C" "${rps:-0}" "$(pct 50%)" "$(pct 95%)" "$(pct 99%)" "$(pct 100%)" "${failed:-?}" \
        "$mainpct" "$procpct" "$peakT" "$peakR" "$cs" "${abcpu:-?}"
    rm -f "$TMP" "$TMP.peak" "$TMP.abcpu"
done
