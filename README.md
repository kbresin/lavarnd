# LavaRnd™ — revival fork for modern Linux

This is an unofficial fork of **LavaRnd™ 0.1.3**, a random number generator by
Landon Curt Noll and Simon Cooper. It makes random numbers from the noise a
webcam's sensor produces when the lens is covered. The goal of this fork is to
get it building and running on a current Linux system.

- Original project: <https://www.lavarand.org/> (also <http://www.LavaRnd.org/>)
- Original source: <https://sourceforge.net/projects/lavarnd/>
- Fork status: **not working yet.** The tree currently holds the unmodified
  upstream release. See [Status](#status).

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
portable C. Our current plan is to keep these and replace the camera layer.

## Goals

- Build without errors on a current Linux distribution (first target: Rocky
  Linux 9, kernel 5.14, gcc 11).
- Replace the V4L1 capture code with V4L2, setting the camera through standard
  V4L2 controls.
- Keep working with `pwc` cameras (tested on a Logitech QuickCam Pro 4000), and
  if practical add support for ordinary UVC webcams.
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

## Status

| Commit / tag    | What it is                                     |
|-----------------|------------------------------------------------|
| `upstream-0.1.3`| The unmodified 0.1.3 release, as downloaded (see below) |
| `main`          | Builds on Rocky 9 with `make all CC_WARN=-Wall`. Perl has been removed. The `pwc` camera driver now uses V4L2, and a new `uvc` driver supports ordinary USB webcams. Tested with a Logitech QuickCam Pro 4000 (`pwc`) and a Logitech C270 (UVC). The `lavapool` daemon runs unprivileged as a systemd user service. A pure-Python client library is in [`python/`](python/), and `trickle` has been rewritten in Python |

The upstream release was imported from
`LavaRnd-0.1.3.tar.gz`
(SHA-256 `59723f6b5a5c0f5feb31aab5985869380e6a971e59491467d4d4d9198fda2e3d`).
To see everything this fork has changed, run:

    git diff upstream-0.1.3

The upstream documentation is still in [`README-first`](README-first) and
[`doc/`](doc/). It describes the 2003 behaviour, not this fork. In particular, it still
documents the Perl API and `trickle`, which this fork has removed.

## To do

- **Credit entropy as root.** An optional `trickle` mode using the
  `RNDADDENTROPY` ioctl, plus a system-wide install. Planned for after the
  development stage.
- **Clean install from scratch.** Remove the existing development install
  (the `~/.local` files, user units and config), then build from a fresh
  clone of the repo and install as root, to confirm it compiles and installs
  cleanly with nothing left over from development.
- **Housekeeping.** Remove the unused `PERL=`/`POD2MAN=` Makefile variables
  and the dead V4L1 `ov511` driver, fix the remaining upstream compiler
  warnings, and then turn warnings back into errors.

## Running it (no root needed)

Build, then install `lavapool` as a systemd user service for your account:

    make all CC_WARN=-Wall
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
root and is planned for after the development stage.

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

The upstream root install (`make install`, SysV init
scripts) is still there but hasn't been ported.

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
