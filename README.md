# LavaRnd™ — revival fork for modern Linux

This is an unofficial fork of **LavaRnd™ 0.1.3**, a random number generator by
Landon Curt Noll and Simon Cooper. It makes random numbers from the noise a
webcam's sensor produces when the lens is covered. Upstream stopped in 2003,
and its code only worked with a handful of webcams of that era on Linux 2.4.

This fork brings it up to date:

- **It runs on current Linux.** The camera code was rewritten for V4L2, and
  the build is clean with current gcc.
- **It works with today's webcams.** A new driver supports standard USB Video
  Class (UVC) cameras, which is almost every USB webcam sold now, alongside
  the original Philips `pwc` cameras. See [Cameras](#cameras).
- **It runs on x86-64 and on a Raspberry Pi** (aarch64).
- **It installs as systemd services** that run unprivileged, with an
  entropy-crediting feed into the kernel's random pool.
- **Python replaces Perl:** a pure-Python client library and `trickle`
  daemon.

- Original project: <https://www.lavarand.org/> (also <http://www.LavaRnd.org/>)
- Original source: <https://sourceforge.net/projects/lavarnd/>
- Fork status: **working.** See [Status](#status).

This fork is not affiliated with or endorsed by the original authors.

## Why fork?

Upstream's last release was 0.1.3 in November 2003, labelled "alpha". It was
written for Linux 2.4 and USB webcams of that era. The design still holds up:
[Digital Blender™](https://www.lavarand.org/what/digital-blender.html)
whitening of chaotic sensor noise is a sound way to get random numbers. But the
code no longer builds or runs on a current Linux system:

1. **It is written for Video4Linux version 1, which the kernel no longer has.**
   The capture code includes `<linux/videodev.h>` and calls `VIDIOCGCAP`,
   `VIDIOCMCAPTURE`, `VIDIOCSYNC` and similar. Linux removed that interface in
   2.6.38 (2011). Current kernels only offer V4L2.
2. **It depends on Philips-specific webcam commands that the modern driver
   dropped.** LavaRnd uses about 20 of them (`VIDIOCPWCSSHUTTER`,
   `VIDIOCPWCSAGC`, `VIDIOCPWCPROBE` and others) to lock gain, shutter and white
   balance, which is central to how it generates randomness. The current `pwc`
   driver handles these settings through standard V4L2 controls instead. The
   build also looks for a `pwc-ioctl.h` header under
   `/usr/src/linux-2.4/drivers/usb/`.
3. **It only supports cameras from 2003.** The supported cameras are a few
   models using the `pwc` and `ov511` drivers. Almost every webcam made today is
   a standard USB video class (UVC) camera.
4. **It needs build and packaging updates.** The build treats every compiler
   warning as an error, and current gcc flags a lot of 2003-era C. Install
   relies on SysV `/etc/init.d` scripts plus `chkconfig`, and on unloading and
   reloading kernel modules by hand. A modern system expects systemd units and
   udev rules.

The generator core, the `lavapool` daemon and the C client library are mostly
portable C. This fork keeps them and replaces the camera layer.

## Cameras

LavaRnd needs a camera whose sensor noise it can read raw: gain and exposure
locked at maximum, all automatic processing off, and the lens covered. Each
camera model gets a preset (the settings that produce the most noise) and
sanity-check limits measured from that camera, which reject frames that look
wrong, such as a camera returning blank or repeated frames.

| Camera type  | Camera                         | Driver     | Status |
|--------------|--------------------------------|------------|--------|
| `qcpro4000`  | Logitech QuickCam Pro 4000     | `pwc`      | Calibrated by this fork; tested on x86-64 and Raspberry Pi |
| `c270`       | Logitech Webcam C270           | `uvcvideo` | **New in this fork.** Preset and limits measured by this fork |
| `uvcgeneric` | Any UVC webcam                 | `uvcvideo` | **New in this fork.** Generic preset; sanity checks off until you calibrate it |
| `pwc730`     | Logitech QuickCam 3000 Pro     | `pwc`      | Upstream's limits; untested on V4L2 |
| `pwc740`     | Philips 740                    | `pwc`      | Upstream's limits; untested on V4L2 |

`LD_LIBRARY_PATH=lib/shared tool/camset list all` lists them. Upstream's
`dsbc100` (D-Link DSB-C100, `ov511` driver) was dropped: that kernel driver
no longer exists.

**Adding another UVC camera.** Start with `uvcgeneric`. Then use the C270's
work as a model: find the settings that give the most frame-to-frame noise,
run `tool/camsanity` for a long calibration, and add a preset and limits to
`lib/uvc_drvr.c`. [`PORTING.md`](PORTING.md) records how the C270 and the
QuickCam were done. Calibration belongs to the camera model, not the
computer: the QuickCam's limits held unchanged on the Raspberry Pi.

## Goals

These were the fork's goals at the start. All are done; see [Status](#status).

- Build without errors on a current Linux distribution (first target: Rocky
  Linux 9, kernel 5.14, gcc 11).
- Replace the V4L1 capture code with V4L2, setting the camera through standard
  V4L2 controls.
- Keep working with `pwc` cameras (tested on a Logitech QuickCam Pro 4000), and
  add support for ordinary UVC webcams (done: `uvcvideo` driver, Logitech
  C270 preset).
- Replace the SysV init scripts with systemd units.
- **Remove Perl entirely.** Upstream's only scripting interface is a Perl API
  (`perllib/`), and its `trickle` daemon is a Perl script. Far fewer people use
  Perl today, so this fork drops both. This is a deliberate break from
  upstream.
- Add a Python library to replace the Perl API. It would sit on the same C
  client API and give access to the random data from `lavapool`.
- Rewrite the `trickle` daemon in Python on top of that library. `trickle`
  feeds LavaRnd data into the kernel's `/dev/random` pool.
- Stay as close to upstream as practical, and record every deviation and its
  reason.

Build and runtime findings are recorded in [`PORTING.md`](PORTING.md).
How we branch, test and release is in [`CONTRIBUTING.md`](CONTRIBUTING.md).

## Status

| Commit / tag    | What it is                                     |
|-----------------|------------------------------------------------|
| `upstream-0.1.3`| The unmodified 0.1.3 release, as downloaded (see below) |
| `v0.2.1`        | Latest release (November 2026). See [`doc/CHANGES`](doc/CHANGES) |
| `main`          | Builds on Rocky 9 with `make all`, with no compiler warnings. Perl has been removed. The `pwc` camera driver now uses V4L2, and a new `uvc` driver supports ordinary USB webcams. Tested with a Logitech QuickCam Pro 4000 (`pwc`) and a Logitech C270 (UVC), on x86-64 and on a Raspberry Pi (aarch64). `make install` sets up `lavapool` and an entropy-crediting `trickle` as unprivileged systemd system services, and the user-level install still works without root. A pure-Python client library is in [`python/`](python/), and `trickle` has been rewritten in Python |

The upstream release was imported from
`LavaRnd-0.1.3.tar.gz`
(SHA-256 `59723f6b5a5c0f5feb31aab5985869380e6a971e59491467d4d4d9198fda2e3d`).
To see everything this fork has changed, run:

    git diff upstream-0.1.3

The upstream documentation is still in [`README-first`](README-first) and
[`doc/`](doc/). It describes the 2003 behaviour, not this fork. In particular, it still
documents the Perl API and `trickle`, which this fork has removed.

## To do

Nothing is outstanding. Known limits: only the cameras above have been
tested, and there is no `.deb` or `.rpm` package; install from source.

## Installing system-wide (root)

Build as an ordinary user, then install as root:

    make all
    make install CAMTYPE=qcpro4000 VIDEODEV=/dev/video0    # as root
    systemctl enable --now lavapool.service lavarnd-trickle.service

`make install` doesn't compile anything. It runs `scripts/system-install.sh`,
which installs:

- `lavapool` in `/usr/local/sbin`
- the LavaRnd libraries and the Python package under `/usr/local/lib/lavarnd`
- a config file, `/etc/LavaRnd/cfg.lavapool`. An existing one is kept, so
  `CAMTYPE` is only needed the first time.
- the `lavapool.service` and `lavarnd-trickle.service` units in
  `/etc/systemd/system`

Both services run as a throwaway unprivileged user (systemd `DynamicUser`)
with a read-only view of the system. `lavapool` gets the `video` group to
open the camera. `lavarnd-trickle` gets only `CAP_SYS_ADMIN`, which it needs to
add 16 bytes to `/dev/random` every 5 minutes with `RNDADDENTROPY`, crediting
8 bits of entropy per byte (`trickle -e 8`). Edit the unit to credit less.
Logs: `journalctl -u lavapool -u lavarnd-trickle`.

If the camera is unplugged or stops working, `lavapool` exits with an error
within about 5 seconds and systemd restarts it every 10 seconds until the
camera is back. Clients get nothing while it's gone.

`make uninstall` removes it all except `/etc/LavaRnd`; `make uninstall
PURGE=1` removes that too. Don't run the system and user installs at the same
time, because both need the camera and port 23209.

## Running it as a user (no root needed)

Build, then install `lavapool` as a systemd user service for your account:

    make all
    scripts/user-install.sh qcpro4000 /dev/video0
    systemctl --user enable --now lavapool.service
    systemctl --user enable --now trickle.service     # optional

Use the camera type that matches your camera
(`LD_LIBRARY_PATH=lib/shared tool/camset list all`). `qcpro4000` is the
Logitech QuickCam Pro 4000 and `c270` the Logitech Webcam C270, both
calibrated for this fork; see `PORTING.md`. Other UVC webcams can start from
`uvcgeneric`, which has its sanity checks off until calibrated.

This installs `lavapool` and its libraries under `~/.local` and writes a
config file to `~/.config/LavaRnd/cfg.lavapool`. The daemon listens on
`127.0.0.1:23209`, which is where LavaRnd clients look by default, so no
`/etc` config is needed. Check that it is serving random data with:

    LD_LIBRARY_PATH=lib/shared tool/poolout 32 1 0 | od -An -tx1

`trickle` writes 16 bytes from `lavapool` into `/dev/urandom` every 5 minutes,
as upstream's did. Run as a normal user, this mixes the data into the kernel's
pool without raising the kernel's entropy estimate. Crediting entropy needs
`CAP_SYS_ADMIN`; the system install does that.

Both services log to the journal:
`journalctl _SYSTEMD_USER_UNIT=lavapool.service` (warnings such as frames
rejected by the sanity check) and `journalctl _SYSTEMD_USER_UNIT=trickle.service`. That works even where
journald keeps logs only in memory, which makes `journalctl --user` find
nothing.

Your account must be able to open the camera, which usually means being in
the `video` group. To keep the service running while you're logged out, run
`loginctl enable-linger`.

To use it from Python, see [`python/README.md`](python/README.md):

    cd python && PYTHONPATH=src python3 -m lavarnd --hex 32

`scripts/user-uninstall.sh` removes the user install (add `--purge` to also
remove `~/.config/LavaRnd`).

## Authorship

- **Original LavaRnd code and documentation:** Copyright © 2000–2003 Landon
  Curt Noll and Simon Cooper. Everything at tag `upstream-0.1.3` is their work,
  unchanged.
- **Fork changes:** maintained by Kyle Bresin. **The fork is AI-assisted.**
  Much of the analysis, code and documentation added after `upstream-0.1.3` is
  written with Anthropic's Claude (via Claude Code), then reviewed and directed
  by the maintainer. Commits made with AI help carry a `Co-Authored-By: Claude …`
  trailer, so `git log` shows which ones they are.

## License

The license is unchanged from upstream: LavaRnd is under the GNU Lesser General
Public License v2.1, and some parts are public domain. See [`COPYING`](COPYING)
and [`COPYING-LGPL`](COPYING-LGPL). Fork changes are released under the same
terms.

## Trademarks

LavaRnd™, Digital Blender™ and LavaCan™ are trademarks of Landon Curt Noll and
Simon Cooper; Mel Pleasant is a co-owner of LavaCan™. "lavarand" is a trademark
of SGI. These names are used here only to identify the original software this
fork is based on.
