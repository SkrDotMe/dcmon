#!/bin/sh
# Benchmark helper (not used at runtime).  Runs dcmon inside a pseudo
# terminal via script(1) and prints measured numbers only.
#
#   tests/bench.sh [BINARY] [SECONDS] [COLSxROWS] [EXTRA DCMON OPTIONS]
#
# Environment: INTERVALS="250 1000" limits the refresh intervals tested.
#
# Per refresh interval:
#   cpu_%, rss, output   from `dcmon --stats` (getrusage + bytes written),
#                        untraced run of SECONDS seconds
#   syscalls/s           measured with build/sccount (ptrace): the count of a
#                        long run minus the count of a short run, divided by
#                        the time difference, so start-up is excluded
set -eu
BIN=${1:-./dcmon}
SECS=${2:-60}
GEOM=${3:-120x40}
EXTRA=${4:-}
INTERVALS=${INTERVALS:-250 500 1000 2000}
COLS=${GEOM%x*}
ROWS=${GEOM#*x}
SC=build/sccount
SHORT=5
LONG=25
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

command -v script >/dev/null || { echo "script(1) is required (util-linux)"; exit 1; }
[ -x "$SC" ] || { mkdir -p build; ${CC:-gcc} -O2 -o "$SC" tests/sccount.c; }

run() { # $1 = output file, rest = command
    f=$1
    shift
    script -qfc "stty cols $COLS rows $ROWS; $*" "$f" >/dev/null 2>&1 || true
}
sc_total() { grep -ao 'sccount: total [0-9]*' "$1" | awk '{print $3}' | tail -1; }

echo "binary   : $BIN ($(stat -c %s "$BIN") bytes, $(size "$BIN" | awk 'NR==2 {print "text " $1 ", data " $2 ", bss " $3}'))"
echo "terminal : ${COLS}x${ROWS} pseudo terminal, ${SECS}s per untraced run"
echo "kernel   : $(uname -r), $(nproc) CPUs"
echo "options  : ${EXTRA:-(none)}"
echo
printf '%-8s %8s %8s %9s %11s %9s %9s %11s %10s\n' \
    interval frames cpu_s cpu_% max_rss_kib out_B/s B/frame syscalls/s sys/refresh

for IV in $INTERVALS; do
    run "$OUT/s.$IV" "$BIN $EXTRA --interval $IV --duration $SECS --stats"
    STATS=$(tr -d '\r' < "$OUT/s.$IV" | grep -a -A1 'dcmon stats' | tr '\n' ' ')
    FRAMES=$(echo "$STATS" | sed -n 's/.*frames \([0-9]*\).*/\1/p')
    BPS=$(echo "$STATS" | sed -n 's/.*B\/frame, \([0-9.]*\) B\/s.*/\1/p')
    BPF=$(echo "$STATS" | sed -n 's/.*(\([0-9.]*\) B\/frame.*/\1/p')
    CPU=$(echo "$STATS" | sed -n 's/.*cpu \([0-9.]*\) s.*/\1/p')
    PCT=$(echo "$STATS" | sed -n 's/.*(\([0-9.]*\)% of one core.*/\1/p')
    RSS=$(echo "$STATS" | sed -n 's/.*max RSS \([0-9]*\) KiB.*/\1/p')

    run "$OUT/a.$IV" "$SC $BIN $EXTRA --interval $IV --duration $SHORT"
    run "$OUT/b.$IV" "$SC $BIN $EXTRA --interval $IV --duration $LONG"
    A=$(sc_total "$OUT/a.$IV")
    B=$(sc_total "$OUT/b.$IV")
    SPS=$(awk -v a="$A" -v b="$B" -v d=$((LONG - SHORT)) 'BEGIN {printf "%.2f", (b - a) / d}')
    SPR=$(awk -v s="$SPS" -v iv="$IV" 'BEGIN {printf "%.1f", s * iv / 1000}')
    printf '%-8s %8s %8s %9s %11s %9s %9s %11s %10s\n' \
        "${IV}ms" "$FRAMES" "$CPU" "$PCT" "$RSS" "$BPS" "$BPF" "$SPS" "$SPR"
    [ "$IV" = 1000 ] && cp "$OUT/b.$IV" "$OUT/detail"
done

echo
grep -ao 'sccount: main.*' "$OUT/detail" | tr -d '\r' | sed 's/^sccount: /threads (1000 ms run): /' || true

echo
echo "syscalls by number, 1000 ms run of ${LONG}s (x86-64 numbers):"
grep -ao 'sccount: nr [0-9]* [0-9]*' "$OUT/detail" | awk '{print $4, $3}' | sort -rn | head -12 |
    while read -r n nr; do
        case $nr in
        0) name=read ;; 1) name=write ;; 2) name=open ;; 3) name=close ;; 7) name=poll ;;
        9) name=mmap ;; 13) name=rt_sigaction ;; 14) name=rt_sigprocmask ;; 16) name=ioctl ;;
        17) name=pread64 ;; 21) name=access ;; 56) name=clone ;; 57) name=fork ;;
        61) name=wait4 ;; 137) name=statfs ;; 228) name=clock_gettime ;; 257) name=openat ;;
        262) name=newfstatat ;; 286) name=timerfd_settime ;; 293) name=pipe2 ;; 217) name=getdents64 ;;
        *) name="nr $nr" ;;
        esac
        printf '  %-16s %6s\n' "$name" "$n"
    done
