# SPDX-License-Identifier: LGPL-2.1-only
"""Command line: write random bytes from lavapool to stdout.

    python -m lavarnd 32          # 32 raw bytes
    python -m lavarnd --hex 32    # as hex
"""

from __future__ import annotations

import argparse
import sys

from . import config as _config
from .client import LavaPool, LavaRndError


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        prog="python -m lavarnd",
        description="Write random bytes from a lavapool daemon to stdout.",
    )
    parser.add_argument("count", type=int, help="number of bytes to output")
    parser.add_argument("-x", "--hex", action="store_true", help="output hex text")
    parser.add_argument("-c", "--config", metavar="FILE", help="cfg.random file to use")
    parser.add_argument(
        "-r", "--retries", type=int, default=None, metavar="N",
        help="extra attempts before giving up (default: from config)",
    )
    args = parser.parse_args(argv)
    if args.count < 0:
        parser.error("count must be >= 0")

    try:
        cfg = _config.load(args.config)
        pool = LavaPool(cfg, retries=args.retries if args.retries is not None else cfg.return_retries)
        data = pool.read(args.count)
    except (LavaRndError, OSError, ValueError) as err:
        print(f"lavarnd: {err}", file=sys.stderr)
        return 1

    if args.hex:
        sys.stdout.write(data.hex() + "\n")
    else:
        sys.stdout.buffer.write(data)
    return 0


if __name__ == "__main__":
    sys.exit(main())
