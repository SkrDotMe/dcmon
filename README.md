# dcmon

A very small, fast terminal system monitor for Linux, written in pure C17.
It shows CPU usage and frequency per core, CPU temperature, RAM, GPU
(NVIDIA and AMD), network throughput, disk I/O and disk space, with coloured
bars and scrolling Unicode graphs. Two extra views show a summary of the
machine (`i`, like fastfetch) and everything the kernel reports about the
batteries (`b`). `--once` and `--json` print a single snapshot for scripts
and status bars. It is built to run all day over SSH with almost no CPU,
memory or bandwidth.

Example layout (illustrative values, shortened):

```
┌─ dcmon ─ build-box ─────────────────────────── up 3d 4h, 412 tasks, 1000 ms ─┐
│ CPU  12 threads               load 0.34 0.11 0.04  3.42 GHz  47.2°C    8.7% │
│ ████████▎░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░ │
│                                                              ▂▃▅▇█▆▄▂▂▂▂▂▂ │
│  0 ░░░░░░░░░░   0% 4.2G   1 ██████████ 100% 4.6G   2 ▏░░░░░░░░░   2% 800M │
│ RAM                         avail 22.2 GiB  661 MiB / 22.9 GiB         2.8% │
│ ██▋░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░ │
│ GPU  NVIDIA GeForce RTX 3060 Ti      210 MHz  fan 31%  21 / 176 W  37.0°C  0.0% │
│ VRAM ███████▌░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░  1.34 GiB / 8.00 GiB │
│ NETWORK  eth0  up  10 Gb/s                                   default route │
│ ↓ DOWNLOAD                                      scale 2.00 MiB/s  1.21 MiB/s │
│ ↑ UPLOAD                                        scale 10.0 KiB/s  3.40 KiB/s │
│ DISK I/O  nvme0n1                   scale 64.0 MiB/s  R 12.4 MiB/s  W 1.20 MiB/s │
│                                                              ▁▁▃▇█▅▂▁ │
│ DISK USAGE                                                 3 filesystems │
│ /  ext4                       free 949 GiB  6.44 GiB / 1006 GiB          1% │
│ ▋░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░ │
└─ q quit +/- speed h help i info b battery n net c cores d disks t temp g gpu ┘
```

* **No dependencies:** only libc, `/proc`, `/sys` and system calls. No
  ncurses, no libsensors, and no helper processes (`top`, `df`, `ip`,
  `sensors`, `nvidia-smi`, …). NVIDIA's NVML library is loaded at run time
  only when it is installed. It is not needed to build or run dcmon.
* **Small:** the stripped release binary is about 98 KiB (x86-64, gcc 15).
  Its only shared library is libc. Without the GPU section, peak memory is
  about 3.6 MiB. Loading NVIDIA's NVML raises it to about 27 MiB; see the
  benchmark section.
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
dcmon --no-gpu                # do not load NVML or scan for GPUs
dcmon --temp-warn 70 --temp-hot 85
dcmon --ascii                 # plain ASCII drawing
dcmon --once                  # one plain-text snapshot, then exit
dcmon --json                  # one JSON snapshot, then exit
dcmon --config ~/dcmon.conf   # another configuration file (--no-config: none)
dcmon --duration 60 --stats   # exit after 60 s and print resource usage
dcmon --help | --version
```

Bad arguments and a bad configuration file print a clear message and exit
with status 2.

### Configuration file

dcmon reads `$XDG_CONFIG_HOME/dcmon/config`, by default
`~/.config/dcmon/config`, if it exists. Each line is one option, written as
its long name without the dashes; `#` starts a comment line:

```ini
# ~/.config/dcmon/config
interval = 500
interface = enp3s0
mount = /
mount = "/mnt/my data"
no-gpu                 # same as "no-gpu = yes"
temp-hot = 90

# start-up state of the toggles (yes/no, on/off, true/false, 1/0)
cores = off
disk-details = on
temp-details = off
gpu-section = on
```

* **Precedence:** an option given on the command line wins over the same
  option in the file. The mount selection (`mount`, `all-mounts`) and the
  temperature thresholds (`temp-warn`, `temp-hot`) are taken as a group from
  one source.
* **Errors** name the file and the line, for example
  `dcmon: /home/me/.config/dcmon/config:3: unknown option 'colour'`.
* `once`, `json`, `duration`, `stats`, `config` and `help` are command-line
  only. `cores`, `disk-details`, `temp-details` and `gpu-section` are
  file-only (the keys `c`, `d`, `t` and `g` change them while running).
* `--config FILE` reads another file, which must exist. `--no-config`
  ignores the file.

### Snapshots: `--once` and `--json`

Both take two samples one refresh interval apart (default 1 s), print the
result to standard output and exit. They need no terminal, so they work in
scripts, cron jobs and status bars such as i3blocks or waybar:

```bash
dcmon --once
dcmon --json | jq '.cpu.usage_pct, .memory.usage_pct'
dcmon --json --interval 250 --no-gpu
```

`--once` prints aligned plain text with no escape sequences. `--json` prints
one object on one line. Sizes are bytes, rates are bytes per second,
percentages have one decimal and temperatures are degrees Celsius. A value
that is not available is `null`. Top-level keys: `version`, `host`,
`uptime_s`, `tasks`, `cpu` (usage, load, threads, frequency, temperature,
per-core list), `memory`, `gpus`, `network`, `disk_io` (total and per disk),
`filesystems`, `batteries` and `ac_online`. Network filesystems that do not
answer within the interval are reported with `"error": "pending"`.

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
| `g` | GPU section on/off. While hidden, the GPU is not queried at all. |
| `i` | system information view (press again or `Esc` to go back) |
| `b` | battery view (press again or `Esc` to go back) |
| `Esc` | back to the monitor from any view |
| `↑` `↓` `PgUp` `PgDn` `j` `k` | scroll when the window is too small |

Ctrl-Z suspends dcmon and restores the terminal. `fg` resumes it.

## What is shown and where it comes from

| Section | Source | Notes |
|---|---|---|
| CPU | `/proc/stat` | busy = Δ(user+nice+system+irq+softirq+steal) / Δtotal. idle includes iowait. Guest time is already counted inside user/nice. Handles iowait going backwards, counter resets, and CPU hotplug (a CPU that comes back starts from a fresh baseline). |
| CPU frequency | `/sys/devices/system/cpu/cpuN/cpufreq/scaling_cur_freq` | Average in the CPU header, per core in the core grid when it fits. Read once a second. Not shown where cpufreq does not exist (WSL2, many VMs). |
| Load, tasks | `/proc/loadavg` | The 4th field gives runnable and total scheduling entities (processes plus threads), shown as "tasks" in the top border. |
| Uptime | `clock_gettime(CLOCK_BOOTTIME)` | Same value as `/proc/uptime`, read through the vDSO without a system call. |
| Temperature | `/sys/class/hwmon/*`, fallback `/sys/class/thermal/thermal_zone*` | See below. |
| RAM | `/proc/meminfo` | used = MemTotal − MemAvailable, so page cache counts as available. Kernels without `MemAvailable` use Free+Buffers+Cached+SReclaimable. Swap is shown when present. |
| Network | `/proc/net/dev`, `/proc/net/route`, `/proc/net/ipv6_route`, `/sys/class/net/<if>/{operstate,speed}` | One read of `/proc/net/dev` gives the counters for every interface. |
| Disk I/O | `/proc/diskstats` | Read and write rates and busy time of whole disks. See below. |
| Disk space | `/proc/self/mountinfo` + `statvfs()` | |
| GPU (NVIDIA) | NVML, `libnvidia-ml.so.1`, loaded with `dlopen()` | Utilisation, VRAM, temperature, power and limit, fan, graphics clock. |
| GPU (AMD) | `/sys/class/drm/cardN/device/` (amdgpu) | Utilisation, VRAM, temperature, power and cap. |
| Battery, AC | `/sys/class/power_supply/*/uevent` | Only while the `i` or `b` view is open, or for `--once`/`--json`. |
| System facts | `/etc/os-release`, `uname()`, `/proc/cpuinfo`, `/sys/class/dmi/id/*`, `getifaddrs()` | Only for the `i` view, read the first time it opens. |

### GPU

**NVIDIA (GeForce, RTX, Quadro, data-centre cards):** the proprietary
driver publishes no utilisation or VRAM data in sysfs. The supported
interface is NVML, the library that ships with the driver (and with WSL2,
in `/usr/lib/wsl/lib`). `nvidia-smi` itself is only a front end for NVML.

How dcmon uses NVML:

* **Loading:** it calls `dlopen("libnvidia-ml.so.1")` at startup. It needs
  no NVIDIA header and has no link-time dependency. dcmon declares the dozen
  entry points it uses itself. If the library or driver is missing, the GPU
  section is simply not shown.
* **Missing values:** each query is optional. A value the GPU does not
  report (`NOT_SUPPORTED`, for example the fan on many laptops) is hidden
  instead of shown as zero.
* **VRAM:** read with `nvmlDeviceGetMemoryInfo_v2`, which excludes the
  memory the driver reserves for itself. This matches `nvidia-smi`. The old
  v1 call counted about 160 MiB of that reservation as "used" on the test
  card. Older drivers fall back to v1.
* **Temperature colours:** they come from the card's own slowdown threshold
  (warm = slowdown − 20 °C, hot = slowdown − 10 °C). Without it, the
  defaults are 75 and 85 °C.
* **Threads:** NVML starts a worker thread. dcmon blocks its signals
  *before* any initialisation, so Ctrl-C or SIGTERM can never land in that
  thread and kill dcmon without restoring the terminal. The terminal tests
  caught this bug while the GPU support was being written.

**AMD (amdgpu):** dcmon reads `gpu_busy_percent`, `mem_info_vram_used`,
`mem_info_vram_total` and the card's hwmon `temp1_input`,
`power1_average`/`power1_input` and `power1_cap`. These files are kept open
and read with `pread()`. Connector entries (`card0-DP-1`) and non-AMD cards
are skipped.

**Intel iGPUs** are not shown, because i915/xe have no simple utilisation
file.

GPUs are sampled at most once per second: every `max(interval, 1000 ms)`.
Each NVML query costs ioctls, so a shorter interval speeds up CPU, RAM and
network but not the GPU. One graph column is therefore one GPU sample, which
can span more time than a column of the CPU graph. Up to 8 GPUs are supported, and each gets a header line, a utilisation bar,
a history graph and a VRAM bar.

### Disk I/O

One `pread()` of `/proc/diskstats` per refresh gives the counters of every
block device. dcmon counts only **whole disks**: a device is a whole disk
when `/sys/block/<name>` exists, and the answer is cached per name, so the
check costs nothing after the first sample. Partitions, `dm-*` (LVM,
dm-crypt), `md*` (software RAID), loop, ram, zram and optical drives are
left out, because their I/O also appears on the disks below them and would
be counted twice. Disks that have never done any I/O are hidden.

* **Rates:** sectors in `/proc/diskstats` are always 512 bytes. Rates use
  the same overflow-safe calculation as the network; a counter that goes
  backwards (driver reload) gives a gap instead of a spike.
* **Busy:** the share of time with I/O in flight, from the `io_ticks` field.
  100 % means the device always had a request outstanding, not that it is
  at its throughput limit (SSDs and NVMe serve many requests in parallel).
* **Display:** a `DISK I/O` header with total read and write rates, a graph
  of read + write (scale at least 1 MiB/s, so idle journal writes stay
  flat), and one row per disk when there is more than one.

### Views: system information and battery

**`i`, system information:** a fastfetch-style summary: user and host,
distribution, hardware model, kernel, virtualisation (WSL, Docker or Podman
container, or the hypervisor vendor), uptime, tasks, shell, terminal,
locale, CPU model with core and thread count and maximum frequency, the
current average CPU frequency (where cpufreq exists) and temperature, GPUs
with VRAM size, the sizes of memory, swap and the root filesystem, the
network interface with its address, and battery and AC state. Usage is left
to the monitor itself. The static facts are read once, the first time the
view opens.

* CPU cores are counted from the distinct `physical id`/`core id` pairs in
  `/proc/cpuinfo`, so hyper-threads are not counted as cores.
* Placeholder strings from the firmware (`To be filled by O.E.M.`,
  `Default string`, ...) are dropped from the hardware model.

**`b`, battery:** for every battery: status, charge (bar), level, time to
empty or to full, power draw, energy now / full, design capacity, health
(full ÷ design), cycle count, voltage, current, temperature, charge limits,
technology and serial number. Below that, **every property the driver
exports** is listed raw from the battery's `uevent` file, in the kernel's
units (µWh, µAh, µW, µA, µV, tenths of °C). The AC adapter state comes from
supplies of type `Mains` or `USB`.

* Charge-based batteries (`charge_*` in µAh) are converted to energy with
  the design voltage, or the current voltage if there is none.
* Drivers that report current instead of power: power = |current| ×
  voltage.
* The time estimates are energy ÷ power, so they jump when the load
  changes. They are marked "estimate".

Both views refresh with the monitor. While they are open the battery files
are re-read once a second; while they are closed, neither view costs
anything.

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
| Kernel files opened once and re-read with `pread(fd, …, 0)` | One syscall per file per refresh, with no open/close. A short read under 2 KiB counts as EOF, which avoids an extra zero-length read. (Multi-record procfs files return about one page per read, so a longer short read is followed by one more read. Files whose records can be longer, `mountinfo` and `cpuinfo`, are always read until EOF.) |
| Separate schedules: CPU/RAM/net/disk I/O at the refresh interval; GPU and CPU frequency every max(1 s, interval); temperature every max(2 s, interval); disk space every max(10 s, interval) | Each source is polled only as often as needed. Timers due within 20 ms are merged into one wake-up. |
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

* 110 columns or wider: two columns (CPU, RAM and GPU on the left;
  network, disk I/O and disk space on the right).
* Narrower: one column.
* Spare rows make the graphs taller.
* When rows run short, dcmon first drops separators, then disk bars, then
  network totals, then the per-disk I/O rows, and finally enables
  scrolling.
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

`tests/test.c` contains 763 checks and runs under AddressSanitizer,
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
* GPU, NVIDIA: tested against `tests/fake_nvml.c`, a stand-in
  `libnvidia-ml.so` with two GPUs. One supports every query; the other
  returns `NOT_SUPPORTED` for most. The tests cover:
  * the library missing, and `nvmlInit` failing;
  * preferring the `_v2` memory call, and checking its struct version;
  * clamping out-of-range driver values;
  * every `nvmlInit` being matched by `nvmlShutdown`.
* GPU, AMD: amdgpu sysfs fixtures with a connector entry and an Intel card
  that must both be ignored.
* Disk I/O: whole-disk detection (partitions, loop, dm and never-used
  disks left out, `cciss/c0d0` mapped to `/sys/block/cciss!c0d0`), rates
  and busy time, an unplugged disk, a counter reset, a missing file.
* CPU frequency (average, maximum, cores without cpufreq not retried) and
  the task counts from `/proc/loadavg`.
* Batteries: energy- and charge-based batteries, power from current ×
  voltage, time estimates, health, raw property list, AC adapters, no
  battery at all.
* System information: `os-release` variants, `/proc/cpuinfo` topology for
  x86 (two sockets, hyper-threading) and ARM, DMI placeholders, VM and WSL
  detection. A regression test reads the real `/proc/cpuinfo`, which
  returns about one page per read, and checks that every CPU is counted.
* The command-line parser, including 18 invalid inputs, and the
  configuration file: precedence, groups, booleans, quoting, errors with
  file and line, the default path.
* `--json` output checked by a JSON syntax checker (escaped mount names,
  `null` for missing values), `--once` free of control characters, and the
  info and battery views at several terminal sizes.
* Layout: 16 terminal sizes from 1×1 to 2000×30, in 6 display modes. The
  test checks that every emitted row is exactly the terminal width, that no
  raw control byte reaches the output, and that an unchanged frame emits 0
  bytes. It also covers scrolling, history kept across resizes, and
  escape-sequence injection through mount names.
* Under heavy `SIGALRM` interruption (`EINTR`): 20,000 reads.
* A 3000-iteration stress run that changes data and terminal sizes, with
  leak checking.
* The real binary in a pseudo terminal, quitting through `q`, Ctrl-C,
  SIGTERM, SIGHUP, every key, and switching between all views. The test verifies exit status 0, a
  byte-identical termios afterwards, the alternate screen left, and no
  sanitizer reports. On a machine with an NVIDIA GPU this also exercises
  the real NVML.

NVIDIA's own libraries leak a few hundred bytes once inside
`pthread_once`. `tests/lsan.supp` suppresses exactly those two libraries,
so dcmon's own code stays fully leak-checked.
* Exit codes for invalid arguments, a broken configuration file and a
  non-terminal stdout; `--json` and `--once` from the real binary without a
  terminal.

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

* GPU: NVIDIA GeForce RTX 3060 Ti, driver 610.47, through WSL2's NVML
  (`/usr/lib/wsl/lib`), idle at the time.

Each run lasted 60 s, except the syscall counts, which use the 25 s − 5 s
difference described above. All threads are counted.

**Without GPU** (`--no-gpu`, or a machine with no GPU):

| Interval | Frames | CPU time | CPU (% of one core) | Peak RSS | Output | Bytes per frame | Syscalls/s | Syscalls per refresh |
|---|---|---|---|---|---|---|---|---|
| 250 ms  | 247 | 0.120 s | 0.201 % | 3648 KiB | 6291 B/s | 1528 | 41.4 | 10.3 |
| 1000 ms |  73 | 0.068 s | 0.114 % | 3592 KiB | 2798 B/s | 2300 | 13.5 | 13.5 |
| 2000 ms |  43 | 0.020 s | 0.034 % | 3612 KiB | 1231 B/s | 1718 |  8.0 | 16.0 |

**With the NVIDIA GPU** (default when NVML is present):

| Interval | Frames | CPU time | CPU (% of one core) | Peak RSS | Output | Bytes per frame | Syscalls/s | Syscalls per refresh |
|---|---|---|---|---|---|---|---|---|
| 250 ms  | 246 | 0.216 s | 0.360 % | 27988 KiB | 7250 B/s | 1768 | 53.9 | 13.5 |
| 1000 ms |  72 | 0.103 s | 0.171 % | 27724 KiB | 2905 B/s | 2421 | 25.6 | 25.6 |
| 2000 ms |  42 | 0.059 s | 0.099 % | 27768 KiB | 1907 B/s | 2724 | 14.3 | 28.5 |

These are version 1.1 (disk I/O, uptime and tasks on screen). Compared with
1.0 on the same machine, the 1.1 run made about 1.3 more syscalls per
second (one `pread` of `/proc/diskstats` per refresh) and sent more output
per frame, because the disk I/O rows and the task count in the top border
change often. The CPU times varied by up to ±30 % between otherwise equal
runs of the same binary, so they show the order of magnitude, not small
differences. The GPU is sampled once per second even at 250 ms; before
that limit a 250 ms run measured 0.798 % CPU and 86.1 syscalls/s.

**What the GPU costs:** NVML is by far the most expensive part of dcmon.

* **Memory:** loading it (plus `libcuda.so` under WSL2) raises peak RSS
  from about 3.6 MiB to about 27 MiB.
* **CPU:** roughly 1.5–2× the CPU time of `--no-gpu`.
* **Syscalls:** about 12 per GPU sample, i.e. about 12 per second. In the
  25 s run at 1000 ms that was
  228 `ioctl`, 165 `getpid` and about 40 `openat`/`mmap`, issued by NVML
  itself. NVML also starts one worker thread.

The numbers come from WSL2's NVML, which talks to `/dev/dxg`. The native
Linux driver uses a different path, so measure it with
`sh tests/bench.sh ./dcmon 60 120x40` on that machine.

If you don't need the GPU, run `--no-gpu`. The library is then not loaded
at all. The `g` key only stops the queries; the library stays loaded.

**Executable:** 100,480 bytes stripped (text 89,628, data 3,844); 1.0 was
67,712 bytes. Most of the growth is the system information and battery
views, the snapshot output and the configuration file parser. The only
dynamic dependency is libc; NVML is loaded with `dlopen()` only when
present. Without the GPU, most of the ~3.6 MiB peak RSS is glibc and the
dynamic loader. dcmon's own heap use is a few tens of KiB: the `/proc` read
buffers, the line buffer and the per-row hashes.

**Syscall mix without GPU:** a 25 s run at 1000 ms, start-up included,
made these calls:

* 167 `pread64`: `/proc/stat`, `/proc/loadavg`, `/proc/meminfo`,
  `/proc/net/dev`, `/proc/diskstats`, operstate.
* 56 `read`: the timerfd and the probe pipe.
* 45 `poll`, 38 `timerfd_settime`, 29 `write` (one per frame).
* 6 `statfs`.
* 27 `openat` and 23 `close`: the start-up opens (including one attempt
  per CPU to find cpufreq, which WSL2 lacks), the 15 s default-route check
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
src/cli.c           argument and configuration file parsing / validation
src/cpu.c           /proc/stat, /proc/loadavg, cpufreq
src/memory.c        /proc/meminfo
src/network.c       /proc/net/dev, routes, operstate
src/disk.c          mountinfo, statvfs, async probe for network/FUSE mounts
src/diskio.c        /proc/diskstats: whole-disk read/write rates and busy time
src/battery.c       /sys/class/power_supply: batteries and AC adapters
src/sysinfo.c       static machine facts; rows of the info and battery views
src/report.c        --once (plain text) and --json snapshots
src/temperature.c   hwmon / thermal-zone discovery and reading
src/gpu.c           NVIDIA via NVML (dlopen), AMD via amdgpu sysfs
src/histogram.c     ring buffers and scaling
src/renderer.c      layout, glyphs, colours, diffed output
src/terminal.c      termios, alternate screen, output buffer, crash restore
src/util.c          pread helpers, integer parsing, formatting
tests/test.c        test-suite
tests/bench.sh      benchmark helper (not used at runtime)
tests/sccount.c     ptrace-based syscall counter for the benchmark (all threads)
tests/fake_nvml.c   stand-in libnvidia-ml.so for the GPU tests
tests/lsan.supp     LeakSanitizer suppressions for NVIDIA's libraries
```

## License

MIT.
