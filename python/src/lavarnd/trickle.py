# SPDX-License-Identifier: LGPL-2.1-only
"""trickle: feed LavaRnd data into the kernel's random pool.

A Python port of upstream's Perl ``daemon/trickle``. Every ``period``
seconds it fetches ``chunk_len`` bytes from lavapool and writes them to
``file`` (normally ``/dev/urandom``)::

    python -m lavarnd.trickle [-v LVL] [-n COUNT] [-e BITS] [chunk_len [period [file]]]

The defaults (16 bytes every 300 seconds into /dev/urandom) are the ones
upstream's init script used.

Writing to /dev/urandom mixes the data into the kernel's pool but does not
raise the kernel's entropy estimate; any user can do it, and the original
only did that. With ``-e BITS`` (``--credit``) the data is instead added
with the RNDADDENTROPY ioctl, crediting BITS bits of entropy per byte (1 to
8). That needs CAP_SYS_ADMIN; trickle checks for it at startup.

If lavapool cannot be reached (after the configured retries), that cycle is
skipped and trickle carries on, as the original did.
"""

from __future__ import annotations

import argparse
import fcntl
import os
import signal
import struct
import sys
import threading
from typing import Callable, Optional

from . import config as _config
from .client import LavaPool

#: Largest chunk the original accepted ("of that size is a waste").
MAX_CHUNK = 32768

DEFAULT_CHUNK = 16
DEFAULT_PERIOD = 300.0
DEFAULT_FILE = "/dev/urandom"

#: RNDADDENTROPY is _IOW('R', 0x03, int[2]) in <linux/random.h>. This is the
#: generic ioctl encoding used by x86, ARM (including the Raspberry Pi) and
#: most other Linux architectures; Alpha, MIPS, PowerPC and SPARC differ.
RNDADDENTROPY = (1 << 30) | (8 << 16) | (ord("R") << 8) | 0x03


def add_entropy(fd: int, data: bytes, bits: int) -> None:
    """Mix ``data`` into the kernel pool, crediting ``bits`` of entropy.

    ``struct rand_pool_info`` is ``int entropy_count; int buf_size;`` followed
    by the bytes. Raises :class:`OSError` (EPERM without CAP_SYS_ADMIN).
    """
    fcntl.ioctl(fd, RNDADDENTROPY, struct.pack("ii", bits, len(data)) + data)


def write_chunk(fd: int, data: bytes) -> int:
    """Plain write; returns the number of bytes written."""
    return os.write(fd, data)


def trickle(
    pool: LavaPool,
    fd: int,
    chunk_len: int,
    period: float,
    count: int = 0,
    log: Callable[[int, str], None] = lambda lvl, msg: None,
    stop: Optional[threading.Event] = None,
    credit: int = 0,
) -> int:
    """Run the trickle loop; return the number of chunks written.

    ``count`` of 0 means run until ``stop`` is set. ``log(level, msg)``
    receives level 0 for warnings that should always be shown and higher
    levels for debug detail. ``credit`` > 0 adds each chunk with
    :func:`add_entropy`, crediting that many bits per byte.
    """
    stop = stop if stop is not None else threading.Event()
    cycle = 0
    written = 0
    failing = False
    while not stop.is_set():
        try:
            data = pool.read(chunk_len)
        except OSError as err:
            if not failing:
                log(0, f"cannot get data from lavapool, skipping until it is back: {err}")
                failing = True
            else:
                log(2, f"still cannot get data from lavapool: {err}")
        else:
            if failing:
                log(0, "lavapool is back, trickling again")
                failing = False
            try:
                if credit:
                    add_entropy(fd, data, credit * len(data))
                    n = len(data)
                else:
                    n = write_chunk(fd, data)
            except OSError as err:
                log(0, f"write failed: {err}")
            else:
                if n == len(data):
                    written += 1
                    extra = f", credited {credit * n} bits" if credit else ""
                    log(3, f"wrote {n} octets{extra}")
                else:
                    log(0, f"short write: {n} of {len(data)} octets")
        cycle += 1
        if count and cycle >= count:
            break
        if period > 0:
            log(3, f"trickle cycle {cycle}, sleeping {period} seconds")
            stop.wait(period)
    return written


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        prog="python -m lavarnd.trickle",
        description="Trickle LavaRnd data from lavapool into the kernel random pool.",
    )
    parser.add_argument("chunk_len", nargs="?", type=int, default=DEFAULT_CHUNK,
                        help=f"octets per trickle (default {DEFAULT_CHUNK})")
    parser.add_argument("period", nargs="?", type=float, default=DEFAULT_PERIOD,
                        help=f"seconds between trickles, 0 for no pause (default {DEFAULT_PERIOD:g})")
    parser.add_argument("file", nargs="?", default=DEFAULT_FILE,
                        help=f"where to write (default {DEFAULT_FILE})")
    parser.add_argument("-n", "--count", type=int, default=0,
                        help="number of trickles, 0 for forever (default 0)")
    parser.add_argument("-v", "--verbose", type=int, default=0, metavar="LVL",
                        help="debug level, 0 for warnings only (default 0)")
    parser.add_argument("-c", "--config", metavar="FILE", help="cfg.random file to use")
    parser.add_argument("-e", "--credit", type=int, default=0, metavar="BITS",
                        help="credit BITS of entropy per byte (1-8) via RNDADDENTROPY; "
                             "needs CAP_SYS_ADMIN (default 0: plain write, no credit)")
    args = parser.parse_args(argv)

    if args.chunk_len <= 0:
        parser.error(f"chunk_len: {args.chunk_len} must be an integer > 0")
    if args.chunk_len > MAX_CHUNK:
        parser.error(f"chunk_len: {args.chunk_len} of that size is a waste (max {MAX_CHUNK})")
    if args.period < 0:
        parser.error(f"period: {args.period} must be >= 0")
    if args.count < 0:
        parser.error(f"count: {args.count} must be >= 0")
    if not 0 <= args.credit <= 8:
        parser.error(f"credit: {args.credit} must be between 0 and 8 bits per byte")
    if not os.path.exists(args.file):
        parser.error(f"file: {args.file} does not exist")

    def log(level: int, msg: str) -> None:
        if level <= args.verbose:
            prefix = "DEBUG: " if level > 0 else ""
            print(f"trickle: {prefix}{msg}", file=sys.stderr, flush=True)

    try:
        pool = LavaPool(_config.load(args.config))
    except (OSError, ValueError) as err:
        print(f"trickle: bad config: {err}", file=sys.stderr)
        return 1
    try:
        fd = os.open(args.file, os.O_WRONLY | os.O_APPEND | os.O_CLOEXEC)
    except OSError as err:
        print(f"trickle: cannot open for writing: {args.file}: {err}", file=sys.stderr)
        return 1
    if args.credit:
        # An empty add credits nothing but goes through the same permission
        # check, so a missing capability is reported now rather than later.
        try:
            add_entropy(fd, b"", 0)
        except OSError as err:
            os.close(fd)
            print(f"trickle: cannot credit entropy via {args.file}: {err} "
                  "(needs CAP_SYS_ADMIN; run without -e to write without credit)",
                  file=sys.stderr)
            return 1

    stop = threading.Event()
    for sig in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(sig, lambda *_: stop.set())

    every = f"every {args.period:g} second(s)" if args.period > 0 else "every cycle"
    log(1, f"will trickle {args.chunk_len} octets {every} into {args.file}")
    if args.credit:
        log(1, f"crediting {args.credit} bit(s) of entropy per octet")
    log(1, "will trickle forever" if args.count == 0 else f"will trickle {args.count} time(s)")
    try:
        written = trickle(pool, fd, args.chunk_len, args.period, args.count, log, stop,
                          credit=args.credit)
    finally:
        os.close(fd)
    log(1, f"finished: wrote {written} chunk(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
