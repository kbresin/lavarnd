# SPDX-License-Identifier: LGPL-2.1-only
"""Fetch random bytes from a lavapool daemon.

The protocol (``daemon/client.c``) is one request per TCP connection: the
client sends a decimal byte count followed by a newline, lavapool writes
exactly that many bytes and closes the connection. A request must be between
1 and ``maxrequest`` bytes; anything else makes lavapool close the connection
without sending data.
"""

from __future__ import annotations

import socket
import threading
import time
from typing import Callable, Optional

from . import config as _config


class LavaRndError(OSError):
    """lavapool could not supply the requested random data."""


class LavaPool:
    """A connection-per-request client for lavapool.

    ``config`` defaults to :func:`lavarnd.load_config`.

    ``retries`` is how many extra attempts to make after a failed one before
    giving up. The default comes from the config's ``return_retries`` (3);
    ``None`` means keep trying forever (the C library's "retry" policy).
    Each attempt has an overall deadline that grows linearly from
    the config's ``return_min_wait`` to ``return_max_wait``.

    ``fallback``, if given, is called as ``fallback(n)`` to supply bytes when
    every attempt has failed, for example ``os.urandom``. Without it a
    :class:`LavaRndError` is raised. :attr:`fallback_bytes` counts how many
    bytes came from the fallback, so callers can tell.
    """

    def __init__(
        self,
        config: Optional[_config.Config] = None,
        *,
        retries: Optional[int] = -1,
        fallback: Optional[Callable[[int], bytes]] = None,
    ) -> None:
        self.config = config if config is not None else _config.load()
        self.retries = self.config.return_retries if retries == -1 else retries
        if self.retries is not None and self.retries < 0:
            raise ValueError("retries must be >= 0 or None")
        self.fallback = fallback
        self.fallback_bytes = 0
        self._lock = threading.Lock()

    def read(self, n: int) -> bytes:
        """Return ``n`` random bytes.

        Requests larger than ``maxrequest`` are split across connections.
        """
        if n < 0:
            raise ValueError("n must be >= 0")
        chunks = []
        remaining = n
        while remaining > 0:
            size = min(remaining, self.config.maxrequest)
            chunks.append(self._read_chunk(size))
            remaining -= size
        return b"".join(chunks)

    def _timeout(self, attempt: int) -> float:
        lo, hi = self.config.return_min_wait, self.config.return_max_wait
        if self.retries is None or self.retries == 0:
            return lo if attempt == 0 else hi
        step = min(attempt, self.retries) / self.retries
        return lo + (hi - lo) * step

    def _read_chunk(self, n: int) -> bytes:
        attempt = 0
        while True:
            try:
                return fetch(self.config.address, n, self._timeout(attempt))
            except OSError as err:
                last_error = err
            attempt += 1
            if self.retries is not None and attempt > self.retries:
                break
        if self.fallback is not None:
            data = self.fallback(n)
            if len(data) != n:
                raise LavaRndError(f"fallback returned {len(data)} of {n} bytes")
            with self._lock:
                self.fallback_bytes += n
            return data
        raise LavaRndError(
            f"lavapool at {self.config.lavapool} failed after "
            f"{attempt} attempt(s): {last_error}"
        ) from last_error


def fetch(address, n: int, timeout: float) -> bytes:
    """Make a single lavapool request for ``n`` bytes.

    ``timeout`` bounds the whole exchange, not each socket call. Raises
    :class:`LavaRndError` on a short or empty reply and :class:`OSError`
    (including :class:`socket.timeout`) on connection problems.
    """
    if n <= 0:
        raise ValueError("n must be > 0")
    deadline = time.monotonic() + timeout
    with socket.create_connection(address, timeout=timeout) as sock:
        sock.sendall(b"%d\n" % n)
        buf = bytearray(n)
        view = memoryview(buf)
        got = 0
        while got < n:
            left = deadline - time.monotonic()
            if left <= 0:
                raise socket.timeout(f"timed out after {got} of {n} bytes")
            sock.settimeout(left)
            count = sock.recv_into(view[got:])
            if count == 0:
                raise LavaRndError(f"lavapool closed the connection after {got} of {n} bytes")
            got += count
    return bytes(buf)
