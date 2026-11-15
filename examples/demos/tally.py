# SPDX-License-Identifier: LGPL-2.1-only
"""The rolling pool behind the tally statistics demo.

The original lavarand.org demo (``randstatd``) kept a pool of 4,000,000
bytes of LavaRnd data. Every 864 seconds it replaced 40,000 bytes of the
pool (oldest first) with fresh data, so the whole pool turned over about
once a day, and after each change it counted how often each 4-bit and 8-bit
value appeared and how many bits were 1. The last 100 frames were shown as
an animated GIF covering 24 hours.

:class:`TallyPool` keeps the pool and its tallies; :class:`Collector` is the
background thread that fills it from lavapool and advances it on a timer.
All viewers share the one pool, so the load on lavapool is fixed.
"""

from __future__ import annotations

import logging
import os
import threading
import time
from collections import deque
from typing import Callable, Deque, Dict, List, Optional

import lavarnd

POOL_SIZE = 4_000_000
CHUNK_SIZE = 40_000
INTERVAL = 864.0
FRAMES = 100
# How much of the pool to read per lavapool request while filling at start,
# so fill progress shows up on the page.
FILL_STEP = 100_000

log = logging.getLogger("lavarnd.tally")

POPCOUNT = [bin(b).count("1") for b in range(256)]


def byte_counts(data: bytes) -> List[int]:
    """Return how many times each byte value 0..255 occurs in ``data``."""
    # 256 passes of bytes.count (a C loop) take a few ms for 40 KB and need
    # no numpy.
    return [data.count(b) for b in range(256)]


def nibble_counts(counts256: List[int]) -> List[int]:
    """4-bit tallies (high and low nibble of every byte) from byte tallies."""
    out = [0] * 16
    for b, n in enumerate(counts256):
        out[b >> 4] += n
        out[b & 15] += n
    return out


def ones(counts256: List[int]) -> int:
    """Number of 1 bits in data whose byte tallies are ``counts256``."""
    return sum(n * POPCOUNT[b] for b, n in enumerate(counts256))


class TallyPool:
    """A rolling pool of random bytes and its running tallies.

    ``source(n)`` returns ``n`` random bytes. Tallies are kept incrementally:
    replacing a chunk subtracts the old bytes' counts and adds the new ones.
    """

    def __init__(self, source: Callable[[int], bytes], pool_size: int = POOL_SIZE,
                 chunk_size: int = CHUNK_SIZE, frames: int = FRAMES) -> None:
        if chunk_size <= 0 or pool_size <= 0 or pool_size % chunk_size:
            raise ValueError("pool size must be a positive multiple of the chunk size")
        self.source = source
        self.pool_size = pool_size
        self.chunk_size = chunk_size
        self.pool = bytearray()
        self.counts = [0] * 256
        self.pos = 0  # start of the oldest chunk, replaced next
        self.next_frame = 0
        self.frames: Deque[Dict] = deque(maxlen=frames)
        self.error: Optional[str] = None
        self.lock = threading.Lock()

    @property
    def filled(self) -> int:
        return len(self.pool)

    def fill_step(self, step: int = FILL_STEP) -> bool:
        """Add up to ``step`` bytes to a pool that isn't full yet.

        Returns True once the pool is full; the first full pool becomes
        frame 0.
        """
        need = min(step, self.pool_size - len(self.pool))
        if need > 0:
            data = self.source(need)
            add = byte_counts(data)
            with self.lock:
                self.pool += data
                self.counts = [a + b for a, b in zip(self.counts, add)]
        if len(self.pool) < self.pool_size:
            return False
        if not self.frames:
            self._record()
        return True

    def advance(self) -> Dict:
        """Replace the oldest chunk with new data and record a frame."""
        if len(self.pool) < self.pool_size:
            raise RuntimeError("pool is not full yet")
        data = self.source(self.chunk_size)
        end = self.pos + self.chunk_size
        old = byte_counts(bytes(self.pool[self.pos:end]))
        new = byte_counts(data)
        with self.lock:
            self.pool[self.pos:end] = data
            self.counts = [c - o + n for c, o, n in zip(self.counts, old, new)]
            self.pos = end % self.pool_size
            return self._record()

    def _record(self) -> Dict:
        # caller holds the lock, or nothing else can see the pool yet
        frame = {
            "frame": self.next_frame,
            "time": time.time(),
            "tally256": list(self.counts),
            "tally16": nibble_counts(self.counts),
            "ones": ones(self.counts),
        }
        self.next_frame += 1
        self.frames.append(frame)
        return frame

    def snapshot(self) -> Dict:
        with self.lock:
            return {
                "pool_size": self.pool_size,
                "chunk_size": self.chunk_size,
                "filled": len(self.pool),
                "error": self.error,
                "frames": list(self.frames),
            }


class Collector(threading.Thread):
    """Fills the pool, then advances it every ``interval`` seconds."""

    def __init__(self, pool: TallyPool, interval: float) -> None:
        super().__init__(name="tally-collector", daemon=True)
        self.pool = pool
        self.interval = interval
        self.stop = threading.Event()

    def _try(self, fn):
        """Call ``fn``; on a lavapool error record it and return None."""
        try:
            result = fn()
        except lavarnd.LavaRndError as err:
            self.pool.error = f"LavaRnd is unavailable: {err}"
            log.warning("%s", self.pool.error)
            return None
        self.pool.error = None
        return result

    def run(self) -> None:
        while not self.stop.is_set():
            full = self._try(self.pool.fill_step)
            if full:
                break
            if full is None:
                self.stop.wait(min(self.interval, 30))
        next_at = time.monotonic() + self.interval
        while not self.stop.wait(max(0.0, next_at - time.monotonic())):
            next_at += self.interval
            if self._try(self.pool.advance) is None:
                # skip the missed frame rather than bursting to catch up
                next_at = max(next_at, time.monotonic() + self.interval)
