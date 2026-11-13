# SPDX-License-Identifier: LGPL-2.1-only
"""Python client for LavaRnd, a random number generator driven by webcam noise.

Talks to a running ``lavapool`` daemon over its TCP protocol; no C library
is needed. Quick start::

    import lavarnd

    key = lavarnd.random_bytes(32)

    rng = lavarnd.LavaRandom()          # a random.Random backed by LavaRnd
    rng.randint(1, 6)

Use :class:`LavaPool` directly to control retries or add a fallback source.
"""

from __future__ import annotations

import threading
from typing import Optional

from .client import LavaPool, LavaRndError, fetch
from .config import Config, load as load_config
from .rng import LavaRandom

__all__ = [
    "Config",
    "LavaPool",
    "LavaRandom",
    "LavaRndError",
    "fetch",
    "load_config",
    "random_bytes",
]

__version__ = "0.2.1"

_default_pool: Optional[LavaPool] = None
_default_lock = threading.Lock()


def _pool() -> LavaPool:
    global _default_pool
    with _default_lock:
        if _default_pool is None:
            _default_pool = LavaPool()
        return _default_pool


def random_bytes(n: int) -> bytes:
    """Return ``n`` random bytes from lavapool using the default settings.

    Raises :class:`LavaRndError` if lavapool can't be reached after the
    configured retries.
    """
    return _pool().read(n)
