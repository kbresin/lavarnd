# Porting notes

This is a running log of what happens when the code is built and run on a
current system. Newest entries go at the top.

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
