# dcmon

A very small, fast terminal system monitor for Linux, written in pure C17.
It shows CPU usage per core, CPU temperature, RAM, network throughput and
disk space, with coloured bars and scrolling Unicode graphs. It is built to
run all day over SSH with almost no CPU, memory or bandwidth.

Example layout (illustrative values, shortened):

```
┌─ dcmon ─ build-box ──────────────────────────────────────────────── 1000 ms ─┐
│ CPU  12 threads                         load 0.34 0.11 0.04  47.2°C    8.7% │
│ ████████▎░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░ │
│                                                              ▂▃▅▇█▆▄▂▂▂▂▂▂ │
│  0 ░░░░░░░░░░   0%   1 ██████████ 100%   2 ▏░░░░░░░░░   2%   3 ░░░░░░░░░░   0% │
│ RAM                         avail 22.2 GiB  661 MiB / 22.9 GiB         2.8% │
│ ██▋░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░ │
│ NETWORK  eth0  up  10 Gb/s                                   default route │
│ ↓ DOWNLOAD                                      scale 2.00 MiB/s  1.21 MiB/s │
│ ↑ UPLOAD                                        scale 10.0 KiB/s  3.40 KiB/s │
│ DISK USAGE                                                 3 filesystems │
│ /  ext4                       free 949 GiB  6.44 GiB / 1006 GiB          1% │
│ ▋░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░ │
└─ q quit +/- speed h help n net c cores d disks t temp ───────────────────────┘
```

* **No dependencies:** only libc, `/proc`, `/sys` and system calls. No
  ncurses, no libsensors, and no helper processes (`top`, `df`, `ip`,
  `sensors`, …).
* **Small:** the stripped release binary is about 62 KiB (x86-64, gcc 15).
  Its only shared library is libc.
* **Single-threaded event loop:** it sleeps on an absolute-deadline
  `timerfd` and never busy-waits.
* **No root needed.**

## Build and install

Requirements: Linux, GCC (or Clang), GNU make.

```bash
make                 # release build -> ./dcmon  (-std=c17 -Os -flto -s)
make debug           # ./dcmon-debug with -O0 -g3, ASan + UBSan, all warnings
make test            # builds build/tests (sanitized) and runs the whole suite
make clean
sudo make install    # /usr/local/bin/dcmon   (PREFIX=... / DESTDIR=... supported)
```

## Usage

```bash
dcmon                         # defaults: 1000 ms refresh, auto interface, real disks
dcmon --interval 500          # faster refresh (100 ... 60000 ms)
dcmon --interface enp3s0      # pin a network interface
dcmon --mount / --mount /home # only these filesystems
dcmon --all-mounts            # everything, including virtual filesystems
dcmon --no-colors             # monochrome (NO_COLOR is honoured as well)
dcmon --no-temperatures       # skip sensor discovery
dcmon --temp-warn 70 --temp-hot 85
dcmon --ascii                 # plain ASCII drawing
dcmon --duration 60 --stats   # exit after 60 s and print resource usage
dcmon --help | --version
```

Bad arguments print a clear message and exit with status 2.

### Keys

| Key | Action |
|---|---|
| `q` | quit (Ctrl-C works too) |
| `+` / `-` | faster / slower refresh (100, 200, 250, 500 ms, 1, 2, 3, 5, 10, 30, 60 s) |
| `r` | clear the graph history |
| `h` / `?` | help screen (also shows sensor, interface and display information) |
| `n` | switch to the next network interface (this pins it) |
| `d` | detailed disk view (bar + free space) on/off |
| `c` | per-core CPU grid on/off |
| `t` | temperature details (package, hottest core, sensor driver) |
| `↑` `↓` `PgUp` `PgDn` `j` `k` | scroll when the window is too small |

Ctrl-Z suspends dcmon and restores the terminal. `fg` resumes it.

## What is shown and where it comes from

| Section | Source | Notes |
|---|---|---|
| CPU | `/proc/stat` | busy = Δ(user+nice+system+irq+softirq+steal) / Δtotal. idle includes iowait. Guest time is already counted inside user/nice. Handles iowait going backwards, counter resets, and CPU hotplug (a CPU that comes back starts from a fresh baseline). |
| Load | `/proc/loadavg` | |
| Temperature | `/sys/class/hwmon/*`, fallback `/sys/class/thermal/thermal_zone*` | See below. |
| RAM | `/proc/meminfo` | used = MemTotal − MemAvailable, so page cache counts as available. Kernels without `MemAvailable` use Free+Buffers+Cached+SReclaimable. Swap is shown when present. |
| Network | `/proc/net/dev`, `/proc/net/route`, `/proc/net/ipv6_route`, `/sys/class/net/<if>/{operstate,speed}` | One read of `/proc/net/dev` gives the counters for every interface. |
| Disk | `/proc/self/mountinfo` + `statvfs()` | Capacity only. Read/write throughput is not measured. |

### Temperature detection

At startup dcmon scans `/sys/class/hwmon/hwmon*`. It does not rely on the
numbering, which can change between boots. A device is used only when its
`name` is a CPU driver: `coretemp`, `k10temp`, `zenpower`, `k8temp`,
`via_cputemp`, `cpu_thermal` or similar. Motherboard sensors (`acpitz`,
`nct6775`, `it87`), GPU sensors (`amdgpu`, `nouveau`) and NVMe sensors are
never treated as the CPU.

| Driver | Package value | Per-core values |
|---|---|---|
| coretemp (Intel) | `Package id N` (max over sockets) | `Core N` |
| k10temp / zenpower (AMD) | `Tdie`, else `Tctl` | `Tccd*` |
| others | labels containing package/cpu, else `temp1` | labels containing core |

If no CPU hwmon driver exists, dcmon falls back to CPU thermal zones
(`x86_pkg_temp`, or a zone type containing `cpu`). If there is nothing at
all, the display shows `N/A`.

Values are in millidegrees and are shown with one decimal. The colour
thresholds are not hard-coded to one value for every processor. dcmon picks
the first that applies:

1. `--temp-warn` / `--temp-hot`. Giving only one of them sets the other
   15 °C away.
2. The sensor's own `tempN_max`: hot = max, warm = max − 15 °C.
3. The sensor's `tempN_crit` or the zone's critical trip point:
   hot = crit − 10, warm = crit − 25.
4. Otherwise warm 75 °C, hot 90 °C.

Sensor files stay open and are re-read with `pread()`. Per-core sensors are
read only while the `t` details are visible, or when there is no package
sensor. If the driver goes away (for example a module reload), dcmon
rescans after at most 30 s.

### Network

* **Interface choice:** dcmon uses the interface of the default route with
  the lowest metric. It checks IPv4 first, then IPv6. If there is no default
  route, it picks the non-loopback interface that is `up` and has the most
  traffic. Interface names are never guessed from prefixes like `eth`,
  `enp` or `wlan`.
* **Following changes:** in automatic mode the route is checked again every
  15 s, so the display follows a new VPN or a switch from Wi-Fi to cable. If
  the interface disappears (removed or renamed), dcmon chooses again after
  3 s. A pinned interface (`--interface` or `n`) is never replaced; it shows
  as `absent` until it comes back.
* **Rate calculation:** rate = Δbytes / Δt, where t comes from
  `CLOCK_MONOTONIC`. These cases are handled:
  * Less than 1 ms elapsed: the old baseline is kept.
  * Counter went backwards: treated as a reset. No spike is drawn; the graph
    shows a gap (`·`).
  * Legacy 32-bit counter wraps: handled only when the previous value was
    near 2³².
  * Rates above 100 GB/s: discarded as glitches.
* **Graph scale:** each graph scales to its visible history, rounded up to a
  1/2/5 × 1024ⁿ step (at least 10 KiB/s). The scale is printed in the
  header. No percentage of link speed is shown, because the link speed is
  often unknown.

### Disks

`/proc/self/mountinfo` is parsed with the kernel's octal escapes decoded
(`\040` for a space, and so on). Mount names are shown safely: control
characters and invalid UTF-8 become `?`, so a crafted mount point cannot
send escape sequences to your terminal.

By default dcmon hides:

* Pseudo filesystems: proc, sysfs, tmpfs, devtmpfs, cgroup*, debugfs,
  tracefs, bpf, squashfs snaps, and similar.
* Mounts under `/proc`, `/sys`, `/dev`, `/run` (except `/run/media`) and
  `/snap`.
* Container overlay layers.
* Any filesystem whose `statvfs()` reports zero blocks. This catches
  virtual filesystems with unfamiliar names.

Other filesystems are shown, including network ones and unfamiliar types.
When the same filesystem is mounted several times (same `major:minor`, for
example bind mounts), it is shown once. dcmon prefers the mount that exposes
the filesystem root.

Numbers follow `df`:

* used = (f_blocks − f_bfree) × f_frsize
* available = f_bavail × f_frsize, the space unprivileged users can use
* percent = used / (used + available), rounded up

**Unresponsive network filesystems:** NFS, CIFS/SMB, 9p, FUSE, Ceph and
similar mounts can block `statvfs()` indefinitely when the server is gone.
dcmon never calls `statvfs()` on them from the UI process. A short-lived
child process does it and returns the results through a non-blocking pipe
that sits in the same `poll()`. The UI keeps updating. If the probe is still
stuck at the next disk refresh, the mount shows `not responding`. No second
probe is started while one is outstanding, so a hung server cannot pile up
processes. Local filesystems are queried inline.

Disk space is refreshed every 10 s. Mount discovery does **not** run on a
timer: the mountinfo file descriptor is in the `poll()` set, and the kernel
raises `POLLPRI` when the mount table changes. Rediscovery is limited to
once per second.

## Performance design

| Technique | Effect |
|---|---|
| Kernel files opened once and re-read with `pread(fd, …, 0)` | One syscall per file per refresh, with no open/close. A short read counts as EOF, which avoids an extra zero-length read. |
| Separate schedules: CPU/RAM/net at the refresh interval, temperature every max(2 s, interval), disks every max(10 s, interval) | Each source is polled only as often as needed. Timers due within 20 ms are merged into one wake-up. |
| `timerfd` with `TFD_TIMER_ABSTIME` on `CLOCK_MONOTONIC`, plus `signalfd` | A blocking `poll()` with absolute deadlines: no drift, no busy loop, no signal-handler races. |
| Fixed 512-sample ring buffers per graph | No allocation per sample, memory bounded for any run length. History survives terminal resizes. |
| Integer arithmetic for parsing, percentages, byte formatting and temperatures | No floating point in the refresh path. |
| Line-level diffing with a 64-bit FNV-1a hash per screen row | Only changed rows are sent. An idle screen sends 0 bytes. |
| Colour escapes only when the colour changes; consecutive rows joined with `\r\n` instead of cursor addressing | Less output. |
| One 32 KiB output buffer, flushed with one `write()` per frame | |
| Alternate screen, hidden cursor, autowrap off; full clears only after a resize | No flicker. |
| Allocation only when something grows: more CPUs, a larger `/proc` file, a terminal resize | Zero allocations in steady state. |

Every frame writes a row exactly `cols` cells wide. Unicode is used for
glyphs and assumed one column wide. Wide characters in mount names are
counted as two. When the locale is not UTF-8, ASCII glyphs are used
automatically. Colour mode is chosen from `COLORTERM`/`TERM`: 256 colours,
16 colours, or none.

**Layout:**

* 110 columns or wider: two columns (CPU and RAM on the left, network and
  disks on the right).
* Narrower: one column.
* Spare rows make the graphs taller.
* When rows run short, dcmon first drops separators, then disk bars, then
  network totals, and finally enables scrolling.
* Below 40×6 it shows a "terminal too small" message.

## Reliability

* **Terminal state:** the original termios is restored on `q`, Ctrl-C,
  SIGTERM, SIGHUP and SIGQUIT, at `exit()`, and after crashes. A SIGSEGV,
  SIGBUS, SIGFPE, SIGILL or SIGABRT handler restores the terminal using only
  async-signal-safe calls, then re-raises the signal.
* **Session loss:** if the terminal disappears (SSH drops, stdin hangs up,
  a write fails), dcmon exits cleanly.
* **Interrupted syscalls:** every read, open, statvfs and write retries on
  `EINTR`.
* **Bad or missing data:** missing, unreadable or malformed `/proc` and
  `/sys` data shows as `N/A`, `--`, or a gap in the graph. dcmon never
  crashes on it.
* **Errors:** permission errors appear per filesystem, for example
  `Permission denied`.

## Tests

```bash
make test
```

`tests/test.c` contains 587 checks and runs under AddressSanitizer,
LeakSanitizer and UndefinedBehaviorSanitizer. Each module reads a fixture
tree of `/proc` and `/sys` files generated in a temporary directory, so
results do not depend on the host. The suite covers:

* CPU percentages, iowait going backwards, counter resets, hotplug, 2048
  logical CPUs, and malformed `/proc/stat`.
* RAM calculation, the pre-3.14 fallback, and malformed input.
* Network rate maths: resets, 32-bit wrap, zero or tiny elapsed time,
  absurd jumps, large values.
* Interface choice by IPv4 or IPv6 default route or by traffic. Removed and
  missing interfaces, manual cycling.
* mountinfo parsing and octal unescaping. Filtering, bind-mount dedupe,
  `--mount` longest-prefix matching, `--all-mounts`. Real `statvfs()` and
  the asynchronous probe through fork and pipe.
* Temperature: Intel and AMD sensor choice, ignoring non-CPU sensors,
  thermal-zone fallback, threshold choice, malformed and missing sensors.
* The command-line parser, including 18 invalid inputs.
* Layout: 16 terminal sizes from 1×1 to 2000×30, in 6 display modes. The
  test checks that every emitted row is exactly the terminal width, that no
  raw control byte reaches the output, and that an unchanged frame emits 0
  bytes. It also covers scrolling, history kept across resizes, and
  escape-sequence injection through mount names.
* Under heavy `SIGALRM` interruption (`EINTR`): 20,000 reads.
* A 3000-iteration stress run that changes data and terminal sizes, with
  leak checking.
* The real binary in a pseudo terminal, quitting through `q`, Ctrl-C,
  SIGTERM, SIGHUP and every key. The test verifies exit status 0, a
  byte-identical termios afterwards, the alternate screen left, and no
  sanitizer reports.
* Exit codes for invalid arguments and for a non-terminal stdout.

## Benchmarking

```bash
make bench                         # = sh tests/bench.sh ./dcmon 60 120x40
sh tests/bench.sh ./dcmon 300 200x60
```

The script runs dcmon in a pseudo terminal through `script(1)` at 250, 500,
1000 and 2000 ms. For each interval it reports:

* CPU time, peak RSS, and terminal output volume and bytes per frame. These
  come from `dcmon --stats`, which uses `getrusage()` and counts the bytes
  it writes. dcmon is not traced during this run.
* System calls per second and per refresh. These are measured with
  `tests/sccount.c`, a small `ptrace(PTRACE_SYSCALL)` counter that needs no
  root and no strace. A 5 s run is subtracted from a 25 s run, so startup
  cost is excluded.

The script finishes with a per-syscall breakdown for the 1000 ms run.

### Measured results

These numbers come from one machine. They are measured, not estimated.

**Test setup:**

* WSL2 kernel 6.18.54, 12 logical CPUs, gcc 15.2, glibc.
* 120×40 pseudo terminal, 256 colours, two-column layout.
* Mostly idle system.
* 6 filesystems, 4 of them 9p (Windows drives). WSL2 has no hwmon
  sensors, so temperature showed `N/A`.

Each run lasted 60 s, except the syscall counts, which use the 25 s − 5 s
difference described above.

| Interval | Frames | CPU time | CPU (% of one core) | Peak RSS | Output | Bytes per frame | Syscalls/s | Syscalls per refresh |
|---|---|---|---|---|---|---|---|---|
| 250 ms  | 247 | 0.231 s | 0.386 % | 3652 KiB | 4259 B/s | 1035 | 37.5 | 9.4 |
| 500 ms  | 129 | 0.093 s | 0.156 % | 3624 KiB | 2667 B/s | 1240 | 19.4 | 9.7 |
| 1000 ms |  73 | 0.080 s | 0.134 % | 3708 KiB | 1864 B/s | 1532 | 12.1 | 12.1 |
| 2000 ms |  43 | 0.052 s | 0.086 % | 3704 KiB |  968 B/s | 1351 |  7.7 | 15.3 |

**Executable:** 63,616 bytes stripped (text 50,146, data 3,188). The only
dynamic dependency is libc. Most of the ~3.6 MiB peak RSS is glibc and the
dynamic loader. dcmon's own heap use is a few tens of KiB: the `/proc` read
buffers, the line buffer and the per-row hashes.

**Syscall mix:** a 25 s run at 1000 ms made these calls:

* 137 `pread64`: `/proc/stat`, `/proc/loadavg`, `/proc/meminfo`,
  `/proc/net/dev`, operstate.
* 51 `read`: the timerfd and the probe pipe.
* 42 `poll`, 38 `timerfd_settime`, 29 `write` (one per frame).
* 6 `statfs`.
* 14 `openat` and 22 `close`: these come from the 15 s default-route check
  and the disk-probe pipe.

**Notes:**

* Frames above one per refresh come from the disk and probe updates, which
  arrive every 10 s.
* Bytes per frame are higher at slower intervals because each frame then
  carries more changed rows.
* Output on an idle, unchanged screen is 0 bytes.
* The 9p probe forks a child every 10 s. On a host with only local
  filesystems there is no fork at all.

On a real host with temperature sensors, add one `pread` per package sensor
every 2 s.

## Project layout

```
src/main.c          event loop, signals, keys
src/cli.c           argument parsing / validation
src/cpu.c           /proc/stat, /proc/loadavg
src/memory.c        /proc/meminfo
src/network.c       /proc/net/dev, routes, operstate
src/disk.c          mountinfo, statvfs, async probe for network/FUSE mounts
src/temperature.c   hwmon / thermal-zone discovery and reading
src/histogram.c     ring buffers and scaling
src/renderer.c      layout, glyphs, colours, diffed output
src/terminal.c      termios, alternate screen, output buffer, crash restore
src/util.c          pread helpers, integer parsing, formatting
tests/test.c        test-suite
tests/bench.sh      benchmark helper (not used at runtime)
```

## License

MIT.
