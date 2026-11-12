# Porting notes

This is a running log of what happens when the code is built and run on a
current system. Newest entries go at the top.

## 2026-11-12 — Running on a Raspberry Pi

Built and installed natively on a Raspberry Pi (aarch64, gcc 14, `dwc_otg`
USB) with the QuickCam Pro 4000 moved over from the x86 box:

- `make all` builds cleanly once the shared libraries are linked with `cc`
  (entry below). `chk_lavarnd` passes.
- The kernel's `pwc` driver picks up the camera as `/dev/video0`, and `camget
  qcpro4000` opens it through the V4L2 driver with the calibrated limits.
- **The calibration carries over.** 900 frames of `camsanity` with `-L`:

  | check               | Pi       | x86 box  | limit    |
  |---------------------|----------|----------|----------|
  | bitdiff fraction    | 0.414785 | 0.414486 | 0.405155 |
  | half level          | 27       | 27       | 24       |
  | top_17 uncom fract  | 0.675417 | 0.674531 | 0.661101 |

  So the limits belong to the camera, not the host, and the Pi's USB
  controller isn't corrupting frames.
- `make install CAMTYPE=qcpro4000 VIDEODEV=/dev/video0` as root, then
  `systemctl enable --now lavapool.service lavarnd-trickle.service`: both
  are active with a clean journal, and the Python client gets data from the
  Pi's `lavapool`.

## 2026-11-12 — Shared libraries linked with cc, for aarch64

The first build on a Raspberry Pi (aarch64, gcc 14) failed linking
`daemon/lavaurl`:

    hidden symbol `__floatunditf' in .../libgcc.a(floatunditf.o) is referenced by DSO

`LD` was never set, so `lib/shared/Makefile` linked the `.so` files with
make's default, bare `ld`, which doesn't add `libgcc`. On x86-64 nothing
needed it. On aarch64 `long double` is 128-bit and done in software, so
`ldrandom()` (`(long double)random64()`) calls libgcc's `__floatunditf`,
and the executables can't supply that hidden symbol to a library. The
libraries are now linked through the compiler driver (`LD= ${CC}`), which
adds `libgcc`.

## 2026-11-11 — Clean root install from a fresh clone

The development (user-level) install was removed with
`scripts/user-uninstall.sh --purge`, leaving no files, units or enable links.
Then, from a fresh `git clone` of the GitHub repo (`24bce32`):

- `make all`, as a user with the default `-Wall -Werror`: no warnings.
  `chk_lavarnd` and the Python tests pass.
- `make install CAMTYPE=qcpro4000 VIDEODEV=/dev/video0` as root, then
  `systemctl enable --now lavapool.service lavarnd-trickle.service`.

Result on Rocky 9 (systemd 252, SELinux enforcing):

- Both services are active with no restarts. `lavapool` runs as dynamic
  user `lavapool` with supplementary group `video` and no capabilities
  (`CapEff` 0). `trickle` runs as `lavarnd-trickle` with only
  `cap_sys_admin`.
- Clients get data from `127.0.0.1:23209`. `trickle` credits 16 bytes
  through `RNDADDENTROPY` with no errors. Its startup check would have exited
  without the capability.
- Installed files are owned by root, and SELinux labels `lavapool` `bin_t`.
- `systemd-analyze security` rates the units 3.7 (`lavapool`) and 4.0
  (`lavarnd-trickle`), both "OK".

This kernel already reports a full pool (`entropy_avail` 256 of 256), as the
newer random driver does once it's initialised, so the credit doesn't show
there.

## 2026-11-11 — Builds with no warnings; -Werror works again

`make all` with the default `CC_WARN= -Wall -Werror` now builds cleanly
on Rocky 9 (gcc 11), so `CC_WARN=-Wall` is no longer needed. Most of the
roughly 140 warnings were cosmetic, but some were real bugs, fixed in their
own commits:

- **Callbacks compared as truncated ints.** The predefined `LAVACALL_*`
  callbacks are small integers cast to function pointers, and the library
  switched on `(int)callback`, dropping the upper 32 bits on 64-bit systems.
  Now `LAVACALL_CASE()` compares the full `intptr_t` value.
- **`pselect()` given a `struct timeval`.** It takes a `struct timespec`, so
  the "tiny" waits in `rawio.c` were 1000 times shorter than intended.
- **Salt hashed a pointer.** `system_stuff()` hashed `&sdata` (8 bytes of
  pointer) instead of the struct, weakening the salt mixed into the output.
- **Small ones:** an `int` overflow in the bit-difference fraction for huge
  frames, a possibly unterminated `sun_path`, filename buffers too small for
  large values, a missing `stdlib.h` in the endian probe, and `tryrnd`
  passing `NULL` to `initstate()`.

The rest were `%lld` used for `u_int64_t` (that's `long` on 64-bit Linux, so
the counters are now cast where printed) and `char`/`unsigned char` pointer
casts. Checked afterwards: `chk_lavarnd`, the `tryrnd` variants, `lavaop`
with each callback, `camget`, `camsanity`, and `lavadump` (64 KB at 7.997
bits/byte) on the QuickCam.

## 2026-11-11 — ov511 driver removed

The `ov511` driver (camera type `dsbc100`, the D-Link DSB-C100) used the V4L1
API, which the kernel removed long ago, and the kernel's own `ov511` module
is gone too (replaced by `gspca_ov519`). It could not open a camera on any
current system. Removed `lib/ov511_drvr.c`, its two headers, the generated
`have/ov511_cam.h`, the `/etc/hotplug` module script `daemon/ov511`, its
`cam_switch` entry, and its Makefile and manifest lines. An OV511 camera on
`gspca_ov519` would need a new V4L2 driver like `uvc_drvr.c`.

## 2026-11-11 — Python client retries are spaced out

After a restart, `lavapool` takes about 4.6 s (camera warm-up) before it
listens. `trickle`, started right after it, got "connection refused" and
skipped a top-up, because the Python client made all its attempts back to
back: its timeouts only bound slow connections, and a refused one fails at
once. The C library (`raw_random()` in `lib/fetchlava.c`) sleeps before each
retry. The client now does the same: an attempt that fails early waits out the
rest of its deadline before the next one. With the default config (3 retries,
2–6 s) that is about 10 s of retrying, enough to cover the start-up.

## 2026-11-11 — QuickCam retest after the V4L2 refactor

With the QuickCam plugged back in, the `pwc` driver (now using
`lib/v4l2cam.c`) was retested:

- `camset qcpro4000 /dev/video0 -L` applies the preset, and `camget` reads it
  back with the calibrated sanity limits.
- `camget c270` on the QuickCam fails with "wrong camera type".
- 900 frames of `camsanity` all passed the calibrated limits by a wide
  margin: top_17 uncommon fraction min 0.674531 (limit 0.661101), half
  level min 27 (limit 24), bitdiff min 0.414486 (limit 0.405155). These
  match the calibration run.
- `lavapool` on the QuickCam served 4 MB through the Python client in 4m42s
  (about 15 KB/s) with no fallback bytes and no insane frame warnings.

## 2026-11-11 — C270 calibration

A full `camsanity` run on the C270 with the `c270` preset, using the fixed
statistics:

    camsanity c270 /dev/video0 DIR 131072 64 -L -T 10 -x 0 -d 0

It ran all 131072 frames (about 73 minutes at 30 fps) as a transient user
unit. Limits are 5 standard deviations below the observed minimums:

| check               | min      | ave      | sd       | limit    |
|---------------------|----------|----------|----------|----------|
| top_2 uncommon fract| 0.605443 | 0.637015 | 0.012940 | 0.540744 |
| half level          | 3        | 4.07     | 0.3006   | 1        |
| bitdiff fraction    | 0.238734 | 0.262382 | 0.009072 | 0.193374 |

`top_x` is 2, not the 3 the short run suggested: ignoring the 2 most common
octets leaves an average uncommon fraction (0.637) closest to 0.6826895;
ignoring 1 leaves 0.735. The half level limit of 1 only rejects a frame
where one octet value fills half of it, and such a frame would already fail
the uncommon fraction check, so for this camera that check adds nothing.

With the limits in place, `lavapool` on the C270 served 26 MB through the
Python client in under 10 minutes (about 45 KB/s, the camera's rate), with
no fallback bytes and no insane frame warnings in the journal.

## 2026-11-11 — UVC webcam driver (Logitech C270)

Most current webcams are USB Video Class (UVC) cameras run by `uvcvideo`, not
`pwc`. This adds a LavaRnd driver for them and a preset for a Logitech
Webcam C270 (USB `046d:0825`).

**Shared V4L2 code.** The generic parts of the `pwc` port moved verbatim from
`lib/pwc_drvr.c` into `lib/v4l2cam.c` (`LavaRnd/v4l2cam.h`), renamed
`v4l2cam_*`: control access, scaling, mmap streaming, frame get/release,
waiting for a frame, and releasing buffers. `pwc_get_frame`, `pwc_msync` and
`pwc_wait_frame` are now thin wrappers around them. `pwc_open()` also now
checks it is talking to the `pwc` driver before changing the format, so the
wrong camera type gets "wrong camera type" instead of a misleading palette
error. The `pwc` driver was retested on the QuickCam afterwards (see the
next entry up).

**`lib/uvc_drvr.c`** (`LavaRnd/uvc_state.h`, `LavaRnd/uvc_drvr.h`):

- Captures uncompressed YUYV. If the camera is in another format (such as
  MJPEG), `uvc_open()` switches it to YUYV. Upstream's `palette.c` already
  knows YUYV, and as upstream decided, the whole buffer (luma and chroma) is
  used as LavaRnd input.
- Settings are kept in each V4L2 control's own units. There is no legacy scale
  to stay compatible with, unlike `pwc`. Values are clamped to the camera's
  range, and controls a camera doesn't offer are ignored. Supported:
  brightness, contrast, saturation, sharpness, gain, exposure (100 µs units;
  `-1` auto, `-2` one frame period), the "auto exposure may lower the frame
  rate" option, white balance mode and temperature, backlight compensation,
  power-line filter, size and frame rate.
- `uvc_identify()` accepts only V4L2 capture devices run by `uvcvideo`, so
  the separate metadata node UVC cameras create (`/dev/video1` here) is
  rejected as "not a camera".
- Most UVC cameras, including the C270, can't do `read()`, so `-M` fails with
  "cannot read, try mmap instead".
- No EEPROM save/restore, which UVC doesn't have.

Two camera types: **`c270`** (model 270) and **`uvcgeneric`** (model 0). The
generic one maximises gain, sharpness and saturation, exposes for a full frame
and turns off automatic processing, but leaves brightness and contrast for
the user to tune. It has sanity checks off, as a starting point for
calibrating another camera.

### Choosing the C270 preset (lens covered)

At default settings a covered C270 gives nothing useful: auto exposure raises
gain and exposure, and every pixel still sits at video black (16). Sweeping
controls in manual exposure, and measuring frame-to-frame (temporal) noise:

- After streaming starts, the C270 sends about **23 all-black frames
  (0.8 s)**. The warm-up discards them, but measurements must skip them; an
  early sweep that didn't gave nonsense.
- **Fixed-pattern noise dominates.** Without sharpening, each pixel changes by
  only 1–4 levels between frames (about 2–4 bits), while the spatial pattern
  has an SD of 6–24, so consecutive frames are 98% correlated. Maximum
  sharpening amplifies the temporal noise much more than the pattern.
- Brightness lifts the noise off black. Contrast above about 128 or
  brightness below about 224 clips it again.
- Saturation 255 adds about 4.9 bits of temporal noise per chroma sample.

Chosen preset: YUYV 160×120 at 30 fps, manual exposure 333 (one frame period,
33.3 ms), dynamic frame rate off, gain 255, brightness 240, contrast 128,
sharpness 255, saturation 255, auto white balance, backlight compensation and
power-line filter off. Result: luma centred near 123, no clipping, about
5.7 bits/pixel of temporal noise in luma and 4.9 in chroma. Verified that
`-L` applies all of it starting from factory defaults and from MJPEG
640×480.

The frame statistics LavaRnd's sanity checks use differ a lot from the
QuickCam's, mostly because the chroma bytes cluster: a short run put the
half level at 6–7 (the QuickCam's is about 27; the full calibration below
measured an average of 4), and about 26% of bits change between frames
(QuickCam: 42%). Calibration sets the limits for this.

## 2026-11-11 — camsanity statistics fixed

`tool/camsanity.c`:

- `stddev()` now uses signed deviations. Upstream's own comment above the
  function gives the right formula (the corrected two-pass
  `Σd² − (Σd)²/n`), but the code took `fabs()` of each deviation, which
  makes it the spread of the absolute deviations, about 0.6× the true value.
- The statistics now cover only the measured frames (array elements
  `1..n`). Before, they also included element 0, which frame 0 never fills:
  zero for most arrays, and uninitialised memory for `bitdiff_fract`, which
  is `malloc`ed.
- SIGINT and SIGTERM now stop the capture loop. `camsanity` then computes
  statistics and recommendations for the frames it has and writes
  `final.stat`, instead of dying with nothing. That would have saved the
  calibration run that was cut short.

Checked on the QuickCam (`qcpro4000`): a 600-frame run's averages and
standard deviations match an independent Python calculation from the
per-frame files to all six printed digits. A run stopped with SIGTERM after
230 frames exited 0 and wrote its recommendations.

## 2026-11-11 — Sanity limits calibrated for the QuickCam Pro 4000

Until now this camera ran with the `pwc740` profile, whose sanity limits were
measured in 2003 on a Philips 740. These limits decide which frames `lavapool`
rejects as not noisy enough, for example because light is getting in. I
measured new ones for this camera with the case sealed:

    camsanity pwc740 /dev/video0 DIR 131072 64 -L -T 10 -x 0 -d 0 -v 2

These are upstream's own calibration settings, with the sanity filter turned
off so every frame is measured. The run was planned for 131,072 frames
(73 min) but stopped after **80,459 frames (44.7 min)** when the session that
started it ended. `camsanity` writes one line per frame as it goes, so the
data for those frames is complete. There was no drift: each third of the run
has the same averages to within 0.0003, so the run wasn't repeated.

**Two bugs in upstream's `camsanity` statistics** (fixed afterwards; see
the next entry):

1. Averages and spreads include an unused slot for frame 0 with value 0.
2. `stddev()` computes `(Σd² − (Σ|d|)²/n) / n` with `d = x − mean`. That is
   the variance of the *absolute* deviations, about 0.6× the true standard
   deviation for normal data. So upstream's "5-sigma" margins were really
   about 3 sigma.

I recomputed the limits from the per-frame files both ways. They came out
close, and the new profile uses the corrected statistics (true sample standard
deviation, frame-0 slot excluded):

| Measure | Min observed | Mean | Std dev | Limit (min − 5σ) | Old `pwc740` limit |
|---|---|---|---|---|---|
| uncommon-value fraction, top 17 values | 0.676562 | 0.690829 | 0.003092 | **0.661101** | 0.520855 |
| half level | 27 | 28.42 | 0.4934 | **24** | 20 |
| fraction of bits changing between frames | 0.411445 | 0.416514 | 0.001258 | **0.405155** | 0.318406 |

`top_x` stays at 17: that is where the average uncommon fraction is closest to
0.6827, upstream's 1-sigma rule.

This camera is noisier and steadier than the 2003 one, so its limits are much
tighter, and abnormal frames such as light leaks are caught much sooner.

**New camera type `qcpro4000`** (`lib/camop.c`, model 4000 on the `pwc`
module). It uses the same preset camera settings as `pwc730`/`pwc740` and the
limits above; the 2003 entries are unchanged. `lavapool`'s config on this
machine now uses `chaos=:driver qcpro4000 /dev/video0 -L`.

Check after switching: `lavapool` ran 2 minutes (about 3,600 frames) under
the new limits with no rejected frames logged. A 256 KiB read through the
Python client had entropy 7.99933 bits/byte and chi-square 242.9.

Not done: the case-open vs. sealed light test (skipped by choice).

## 2026-11-11 — trickle rewritten in Python

`lavarnd.trickle` (`python/src/lavarnd/trickle.py`) replaces the Perl
`daemon/trickle`. It keeps the original's arguments
(`[-v lvl] [-n count] chunk_len period file`) and limits (chunk 1..32768,
period ≥ 0). Like the original, it skips a cycle when `lavapool` can't be
reached after the configured retries, and it doesn't credit entropy.

Changes from the original:

- The positional arguments default to the init script's values: 16 bytes,
  300 seconds, `/dev/urandom`.
- `-b` (fork into the background) is gone; systemd handles that.
- Losing and regaining `lavapool` is logged once each at the default level.
  The original only printed these at higher debug levels.
- SIGTERM, SIGINT and SIGHUP stop it cleanly, including during the sleep
  between cycles.
- The init script's "preload `/var/lib/random-seed`" step isn't ported;
  `systemd-random-seed` does that job now.

`scripts/user-install.sh` now copies the Python package to
`~/.local/lib/lavarnd/python` and installs `trickle.service`, which wants and
starts after `lavapool.service` and runs `/usr/bin/python3 -s -m
lavarnd.trickle 16 300 /dev/urandom` with `PYTHONPATH` pointing there. No pip
install is needed. The wheel also gets a `lavarnd-trickle` command.

Tests: 5 new unit tests (28 in total), a foreground run writing 3 chunks to
the real `/dev/urandom`, and a systemd test with a 2-second period. That test
stopped `lavapool` (one warning, then quiet retries), started it again (one
"back" message, and writing resumed), and checked that SIGTERM stops trickle
cleanly. `trickle.service` is now enabled on this machine.

Still to do: an optional root mode that credits entropy with the
`RNDADDENTROPY` ioctl, after the development stage.

## 2026-11-11 — Python client library

Added `python/`, a pure-Python package `lavarnd` that replaces the removed
Perl API. It talks to `lavapool` directly over the daemon's TCP protocol
(`daemon/client.c`): send a decimal count and a newline, read back exactly
that many bytes, one request per connection, from 1 to `maxrequest` bytes.
It doesn't wrap the C libraries, so nothing has to be compiled or installed to
use it.

- Reads the same `cfg.random` as the C library, with the same defaults when
  no file exists, plus a `$LAVARND_CFG` override.
- Requests larger than `maxrequest` are split across connections.
- Error handling mirrors the C library's policies in Python terms: a fixed
  number of retries and then raise ("return"), `retries=None` to keep trying
  ("retry"), or a `fallback` callable such as `os.urandom` (in place of the
  "try"/"s100" policies; the fork doesn't port the s100 generator). Each
  attempt's timeout grows from `return_min_wait` to `return_max_wait`, like the
  C client's.
- `LavaRandom` is a `random.Random` subclass in the style of
  `random.SystemRandom`. It buffers 4 KiB at a time, never reuses a byte, and
  drops its buffer in a child process after `fork()`.
- `python -m lavarnd N` writes N bytes to stdout, like `tool/poolout`.

Supports Python 3.9 (the system Python on Rocky 9) and later, with no
dependencies. 23 `unittest` tests run against a fake `lavapool`, plus one
live test (`LAVARND_LIVE=1`) that pulled 70,000 bytes from the real daemon.
A wheel builds with `pip wheel`.

## 2026-11-11 — lavapool as a systemd user service

`lavapool` needs no root. It only chroots or switches user if given `-c` /
`-u`, it reads its config from `-p`, and clients fall back to the built-in
default `127.0.0.1:23209` when `/etc/LavaRnd/cfg.random` doesn't exist. Run
from the build tree as an ordinary user, it opened the camera, filled its pool
at about 32 KB/s, and served bytes to `tool/poolout`.

Added `scripts/user-install.sh` and `systemd/lavapool.service.in`. The script
copies the daemon to `~/.local/bin` and the libraries to `~/.local/lib/lavarnd`,
writes `~/.config/LavaRnd/cfg.lavapool` (an existing one is never overwritten)
and installs a user unit. The unit sets `LD_LIBRARY_PATH`, logs to the journal
and restarts on failure.

**Pitfall: the systemd user manager keeps its old groups.** On this machine
the service failed with `failed to open the initial chaos channel` (exit 3)
while the same command worked from a shell. The user manager (`user@1002`)
had been running since before the account joined `video`. With lingering on
it never restarts, and it can't pick up new groups by itself:

    shell groups:        1003 39 190 983 1001 1046
    user manager groups: 1003 190 983 1001 1046        (no 39 = video)

Fix, needs root once: `sudo systemctl restart user@$(id -u).service`. This
restarts every user service. The install script now checks for this with
`systemd-run --user` and prints the fix.

## 2026-11-11 — pwc camera driver ported to V4L2

`lib/pwc_drvr.c` now uses V4L2 only. Every V4L1 call and every old
Philips-specific `VIDIOCPWC*` command has been replaced. The driver's
interface to the rest of LavaRnd is unchanged, as are argument parsing,
range checks, the LavaRnd preset values and the sanity thresholds.

**How settings map.** `struct pwc_state` keeps the old 16-bit V4L1 value
ranges, so command-line flags and the 2003 preset table still mean the same
thing. Each value is scaled linearly onto the range the V4L2 control reports.
For example, LavaRnd's "maximum gain" (65535) becomes `gain=63` on this
camera. The full table is in the comment at the top of `lib/pwc_drvr.c`.
Settings the modern driver no longer offers are ignored: hue, the compression
preference and the LED on/off times. The driver can now read back gain and
exposure, which the V4L1 driver could not.

**Capture.** In mmap mode the driver requests 4 V4L2 buffers and starts
streaming in `pwc_open()`. `pwc_get_frame()` takes the next filled buffer
from the driver and points `siz->image` / `siz->chaos` at it, and
`pwc_msync()` hands it back. This matches how every caller already used these
functions. Four fields were added to `struct opsize` (`lib/LavaRnd/lavacam.h`)
to track the buffers. Read mode (`-M`) uses plain `read()`, which the V4L2
driver still supports.

**Other changes in the same file:**

- Warm-up now takes each frame from the driver and hands it back in mmap mode
  too. Otherwise all 4 buffers fill during warm-up and the first frames
  LavaRnd uses afterwards would be several seconds old.
- Fixed an upstream bug in `pwc_wait_frame()`. It passed a `struct timeval` to
  `pselect()`, which takes a `struct timespec`, so microseconds were read as
  nanoseconds and every timeout was about 1000× too short. It also now resets
  the descriptor set before retrying after a signal.
- Error paths in `pwc_open()` now close the device and free buffers.
- The V4L2 driver doesn't report the Philips type number (730, 740, …), so
  `camget` prints the driver and card name instead.

**Unchanged on purpose:** like upstream, applying settings (for example with
`-L`) also saves them to the camera's EEPROM as its stored user settings. To
put the camera back to factory defaults:

    v4l2-ctl -d /dev/video0 --set-ctrl=restore_factory_settings=1

### Test results (QuickCam Pro 4000, `pwc740` profile, whatever the lens was pointed at)

- `tool/camget pwc740 /dev/video0` reads all settings over V4L2.
- `tool/camdump … -L` applies the LavaRnd preset: 160×120, 30 fps, gain
  63/63, exposure 255/255, auto gain, auto contour, backlight compensation
  and flicker filter all off. `v4l2-ctl --get-ctrl` confirms these values.
- `tool/camsanity pwc740 /dev/video0 DIR 300 17 -L`: all 300 frames pass the
  2003 `pwc740` sanity thresholds with room to spare. Worst uncommon-value
  fraction (top 17) was 0.676 against a minimum of 0.479. Fewest bits differing
  between frames was 0.413 against a minimum of 0.318. Half-level was 27
  against a minimum of 20.
- `tool/lavadump pwc740 /dev/video0 1048576 1.0 -L` produced 1 MiB in 107
  seconds (about 9.8 KB/s). Entropy 7.99983 bits/byte, chi-square 245.5 on 255
  degrees of freedom, mean 127.47, serial correlation −0.0008, Monte Carlo π
  3.1435, and zlib can't compress it.
- Read mode (`-M`) works for both `lavadump` and `camdump`.
- `tool/chk_lavarnd` still passes.

These statistics only show the output isn't obviously broken. LavaRnd's
whitening step makes even weak input look random, so they say nothing about
how much real entropy the camera supplies. The 2003 sanity thresholds were
measured with the lens covered, and they still need re-measuring for this
camera that way, with a long `camsanity` run.

Not yet tried: the `lavapool` daemon and `make install`. The `ov511` driver is
unchanged and still V4L1-only; the kernel dropped that driver too and replaced
it with `gspca_ov519`.

## 2026-11-11 — Perl removed

Deleted `perllib/` (the Perl API), `daemon/trickle`, `daemon/trickle.rc` and
`tool/test_perllib`, and removed them from the Makefiles and
`manifest-LavaRnd`. A Python library and a Python `trickle` will replace them
(see the README's Goals).

- `make all CC_WARN=-Wall` now finishes cleanly, with no `POD2MAN` workaround.
  That problem only affected the Perl modules.
- `make -n install` (a dry run that only shows what install would do)
  completes without errors.
- `tool/chk_lavarnd`, upstream's self-test of the core algorithm, passes:
  `all tests are OK`.
- Still left behind: the per-Makefile `PERL=` / `POD2MAN=` variables (now
  unused) and Perl references in the upstream docs under `doc/`.

## 2026-11-11 — First build of unmodified 0.1.3

**System:** Rocky Linux 9.8, kernel 5.14.0, gcc 11.5.0, Perl 5.32,
libv4l 1.20.0.
**Camera:** Logitech QuickCam Pro 4000 (USB `046d:08b2`), which uses the `pwc`
driver and shows up as `/dev/video0`.

### Build

`make all` with no options does not finish:

1. **Every compiler warning is treated as an error.** The build passes
   `-Werror` by default. Running `make all CC_WARN=-Wall` turns that off; the
   build then reports 54 warnings, nearly all harmless.
2. **The Perl man-page step fails.** The top-level Makefile hands its own
   `POD2MAN=pod2man` setting down to the Perl module builds. That replaces
   Perl's man-page generator wrapper, which modern Perl calls with a
   `--perm_rw` option that plain `pod2man` rejects (`Unknown option: perm_rw`).
   To get past it:

       make all CC_WARN=-Wall 'POD2MAN=$(POD2MAN_EXE)'

   With that, the whole build finishes with exit code 0, though the
   man-page step still fails and the build skips past it. Proper fix:
   stop passing `POD2MAN` down to the Perl builds.

The C code compiles even though the kernel no longer has V4L1, because
upstream ships its own copies of the 2003 headers in
`lib/LavaRnd/have/` (`videodev_2.4.h` and `pwc-ioctl-8.6.h`). So it builds,
but those calls fail when the program runs.

### Runtime

    LD_LIBRARY_PATH=lib/shared tool/camget pwc740 /dev/video0

- **Without help:** it fails on the first V4L1 call:
  `cannot open pwc740 camera at /dev/video0: failed to get/set video channel`.
- **With libv4l's V4L1 compatibility library**
  (`LD_PRELOAD=/usr/lib64/libv4l/v4l1compat.so`): every basic capture call
  is translated to its V4L2 equivalent and succeeds. The camera reports
  640×480 YUV420. The program then fails on the first Philips-specific
  command:

      ioctl(3, _IOC(_IOC_NONE, 0x76, 0xc0, 0), 0x1) = -1 ENOTTY

  `0x76 0xc0` is `VIDIOCPWCRUSER`, the "restore user settings" command. The
  current `pwc` driver no longer supports any of these commands; it exposes
  the same settings (gain, shutter, white balance, contour, noise reduction
  and so on) as standard V4L2 controls instead.

### Conclusion

The camera layer is the only thing blocking a basic run. The work is to
rewrite `lib/pwc_drvr.c` (and the V4L1 parts of `lib/palette.c`) to use V4L2
directly, with each Philips-specific command mapped to the matching standard
V4L2 control. To list the controls the current driver offers:

    v4l2-ctl -d /dev/video0 --list-ctrls-menus
