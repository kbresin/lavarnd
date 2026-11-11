# SPDX-License-Identifier: LGPL-2.1-only
"""A :class:`random.Random` whose randomness comes from lavapool."""

from __future__ import annotations

import os
import random
import threading
from typing import Optional

from .client import LavaPool

_RECIP_BPF = 2.0 ** -53  # same scaling as random.SystemRandom


class LavaRandom(random.Random):
    """Drop-in :class:`random.Random` backed by LavaRnd, like SystemRandom.

    All the usual methods (``randint``, ``choice``, ``shuffle``, ``sample``,
    ``gauss``...) work. As with :class:`random.SystemRandom`, there is no
    seed: :meth:`seed` does nothing and :meth:`getstate` / :meth:`setstate`
    raise :class:`NotImplementedError`.

    To avoid one TCP connection per call, bytes are fetched ``bufsize`` at a
    time and handed out in order; each byte is used once. The buffer is
    discarded in a child process after ``os.fork()`` so parent and child never
    share random data.
    """

    def __init__(self, pool: Optional[LavaPool] = None, bufsize: int = 4096) -> None:
        if bufsize <= 0:
            raise ValueError("bufsize must be > 0")
        self._pool = pool if pool is not None else LavaPool()
        self._bufsize = bufsize
        self._buf = b""
        self._pos = 0
        self._lock = threading.Lock()
        self._pid = os.getpid()
        super().__init__()

    def _take(self, n: int) -> bytes:
        with self._lock:
            if self._pid != os.getpid():
                self._buf, self._pos, self._pid = b"", 0, os.getpid()
            avail = len(self._buf) - self._pos
            if n > avail:
                need = n - avail
                fresh = self._pool.read(max(need, self._bufsize))
                self._buf = self._buf[self._pos:] + fresh
                self._pos = 0
            out = self._buf[self._pos:self._pos + n]
            self._pos += n
            return out

    def random(self) -> float:
        """Return a float in [0.0, 1.0) with 53 random bits."""
        return (int.from_bytes(self._take(7), "big") >> 3) * _RECIP_BPF

    def getrandbits(self, k: int) -> int:
        """Return an int with ``k`` random bits."""
        if k < 0:
            raise ValueError("number of bits must be non-negative")
        if k == 0:
            return 0
        nbytes = (k + 7) // 8
        value = int.from_bytes(self._take(nbytes), "big")
        return value >> (nbytes * 8 - k)

    def randbytes(self, n: int) -> bytes:
        """Return ``n`` random bytes."""
        if n < 0:
            raise ValueError("n must be >= 0")
        return self._take(n)

    def seed(self, *args, **kwds) -> None:
        """No-op: LavaRnd is not a pseudo-random generator and has no seed."""
        return None

    def _notimplemented(self, *args, **kwds):
        raise NotImplementedError("LavaRandom has no state to save or restore")

    getstate = setstate = _notimplemented
