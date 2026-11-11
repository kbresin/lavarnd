# Porting notes

This is a running log of what happens when the code is built and run on a
current system. Newest entries go at the top.

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
