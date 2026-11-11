# SPDX-License-Identifier: LGPL-2.1-only
"""trickle: feed LavaRnd data into the kernel's random pool.

A Python port of upstream's Perl ``daemon/trickle``. Every ``period``
seconds it fetches ``chunk_len`` bytes from lavapool and writes them to
``file`` (normally ``/dev/urandom``)::

    python -m lavarnd.trickle [-v LVL] [-n COUNT] [chunk_len [period [file]]]

The defaults (16 bytes every 300 seconds into /dev/urandom) are the ones
upstream's init script used.

Writing to /dev/urandom mixes the data into the kernel's pool but does not
raise the kernel's entropy estimate; any user can do it. Crediting entropy
needs root and the RNDADDENTROPY ioctl. Like the original, this does not do
that.

If lavapool cannot be reached (after the configured retries), that cycle is
skipped and trickle carries on, as the original did.
"""

from __future__ import annotations

import argparse
import os
import signal
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


def trickle(
    pool: LavaPool,
    fd: int,
    chunk_len: int,
    period: float,
    count: int = 0,
    log: Callable[[int, str], None] = lambda lvl, msg: None,
    stop: Optional[threading.Event] = None,
) -> int:
    """Run the trickle loop; return the number of chunks written.

    ``count`` of 0 means run until ``stop`` is set. ``log(level, msg)``
    receives level 0 for warnings that should always be shown and higher
    levels for debug detail.
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
                n = os.write(fd, data)
            except OSError as err:
                log(0, f"write failed: {err}")
            else:
                if n == len(data):
                    written += 1
                    log(3, f"wrote {n} octets")
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
    args = parser.parse_args(argv)

    if args.chunk_len <= 0:
        parser.error(f"chunk_len: {args.chunk_len} must be an integer > 0")
    if args.chunk_len > MAX_CHUNK:
        parser.error(f"chunk_len: {args.chunk_len} of that size is a waste (max {MAX_CHUNK})")
    if args.period < 0:
        parser.error(f"period: {args.period} must be >= 0")
    if args.count < 0:
        parser.error(f"count: {args.count} must be >= 0")
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

    stop = threading.Event()
    for sig in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(sig, lambda *_: stop.set())

    every = f"every {args.period:g} second(s)" if args.period > 0 else "every cycle"
    log(1, f"will trickle {args.chunk_len} octets {every} into {args.file}")
    log(1, "will trickle forever" if args.count == 0 else f"will trickle {args.count} time(s)")
    try:
        written = trickle(pool, fd, args.chunk_len, args.period, args.count, log, stop)
    finally:
        os.close(fd)
    log(1, f"finished: wrote {written} chunk(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
