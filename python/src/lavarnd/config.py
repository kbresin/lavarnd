# SPDX-License-Identifier: LGPL-2.1-only
"""Client configuration: where lavapool listens and how to retry.

This reads the same ``cfg.random`` file as the C client library
(``lib/fetchlava.c``), with the same built-in defaults when no file exists.
"""

from __future__ import annotations

import os
from dataclasses import dataclass, fields, replace
from typing import Optional, Tuple

#: Path the C client library reads by default.
SYSTEM_CFG = "/etc/LavaRnd/cfg.random"

#: Environment variable that names a cfg.random file to use instead.
CFG_ENV = "LAVARND_CFG"


@dataclass(frozen=True)
class Config:
    """Settings from cfg.random that matter to a client.

    Defaults match ``lib/LavaRnd/cfg.h`` in the C library.
    """

    #: ``host:port`` where lavapool accepts requests.
    lavapool: str = "127.0.0.1:23209"
    #: Largest request lavapool accepts in one connection.
    maxrequest: int = 65536
    #: Extra attempts after the first one fails before giving up.
    return_retries: int = 3
    #: Timeout for the first attempt, in seconds.
    return_min_wait: float = 2.0
    #: Timeout for the last attempt; timeouts grow linearly up to this.
    return_max_wait: float = 6.0

    @property
    def address(self) -> Tuple[str, int]:
        """The lavapool address as a ``(host, port)`` tuple."""
        host, sep, port = self.lavapool.rpartition(":")
        if not sep or not host or not port.isdigit():
            raise ValueError(f"lavapool address must be host:port, got {self.lavapool!r}")
        return host, int(port)


_FIELD_TYPES = {f.name: f.type for f in fields(Config)}


def parse(text: str, source: str = "<string>") -> Config:
    """Parse the contents of a cfg.random file.

    Keys this client doesn't use (the C library's other retry policies, the
    s100 generator settings) are accepted and ignored.
    """
    values = {}
    for lineno, raw in enumerate(text.splitlines(), 1):
        if raw.startswith("#"):
            continue
        line = raw.strip()
        if not line:
            continue
        key, sep, value = line.partition("=")
        if not sep:
            raise ValueError(f"{source}:{lineno}: expected key=value")
        key, value = key.strip(), value.strip()
        kind = _FIELD_TYPES.get(key)
        if kind is None:
            continue
        try:
            if kind == "int":
                values[key] = int(value, 0)
            elif kind == "float":
                values[key] = float(value)
            else:
                values[key] = value
        except ValueError:
            raise ValueError(f"{source}:{lineno}: bad value for {key}: {value!r}") from None
    config = replace(Config(), **values)
    config.address  # validate early
    if config.maxrequest <= 0:
        raise ValueError(f"{source}: maxrequest must be > 0")
    return config


def load(path: Optional[str] = None) -> Config:
    """Load client settings.

    Looks in, in order: ``path`` if given (it must exist), the file named by
    ``$LAVARND_CFG`` (it must exist), then ``/etc/LavaRnd/cfg.random`` if it
    exists. With none of those, the built-in defaults are used, the same as
    the C library.
    """
    if path is None:
        path = os.environ.get(CFG_ENV) or None
    if path is None:
        if not os.path.exists(SYSTEM_CFG):
            return Config()
        path = SYSTEM_CFG
    with open(path, encoding="ascii") as f:
        return parse(f.read(), path)
