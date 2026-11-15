# SPDX-License-Identifier: LGPL-2.1-only
"""LavaRnd demos: rebuilds of the demos from the original lavarand.org site.

Every random number comes from a running ``lavapool`` daemon through the
``lavarnd`` Python library. If lavapool can't be reached the demos say so
(HTTP 503) instead of quietly using another random source.

Run it (lavapool must be running on this machine)::

    PYTHONPATH=../../python/src flask --app app run

``flask`` and gunicorn (``'app:create_app()'``) both build the app with
:func:`create_app`, which also starts the tally demo's collector thread.
Run it as a single process (gunicorn ``--workers 1 --threads N``): each
process would keep its own tally pool and read from lavapool separately.

Pages:

    /                   index of demos
    /tally              tally statistics (shared rolling pool)

    /api/frames                             tally frames as JSON

Settings (environment variables):

    LAVARND_DEMOS_PROXIED        1 to trust one reverse proxy's X-Forwarded-*
                                 headers, including X-Forwarded-Prefix
    LAVARND_TALLY_INTERVAL       seconds between tally frames (default 864)
    LAVARND_TALLY_POOL           tally pool size in bytes (default 4000000)
    LAVARND_TALLY_CHUNK          bytes replaced per frame (default 40000)
"""

from __future__ import annotations

import os
from typing import Callable, Optional

from flask import Flask, jsonify, render_template
from werkzeug.middleware.proxy_fix import ProxyFix

import lavarnd
import tally

DEMOS = (
    ("tally", "Tally statistics", "A day of 4-bit, 8-bit and 1-bit tallies over a rolling 4 MB pool."),
)


def _env(name: str, default, kind=float):
    value = os.environ.get(name)
    return kind(value) if value else default


def create_app(source: Optional[Callable[[int], bytes]] = None, *,
               tally_interval: Optional[float] = None,
               tally_pool: Optional[int] = None,
               tally_chunk: Optional[int] = None,
               start: bool = True, proxied: Optional[bool] = None) -> Flask:
    """Build the app and (unless ``start`` is False) start the tally collector.

    ``source(n)`` returns ``n`` random bytes; it defaults to
    :func:`lavarnd.random_bytes` and must raise :class:`lavarnd.LavaRndError`
    when it can't. The other settings default to the environment variables
    above, then to the original demos' values.
    """
    app = Flask(__name__)
    source = source or lavarnd.random_bytes

    pool = tally.TallyPool(
        source,
        tally_pool or _env("LAVARND_TALLY_POOL", tally.POOL_SIZE, int),
        tally_chunk or _env("LAVARND_TALLY_CHUNK", tally.CHUNK_SIZE, int),
    )
    interval = tally_interval or _env("LAVARND_TALLY_INTERVAL", tally.INTERVAL)
    if interval <= 0:
        raise ValueError("tally interval must be > 0")
    app.config.update(POOL=pool)
    if start:
        app.config["COLLECTOR"] = tally.Collector(pool, interval)
        app.config["COLLECTOR"].start()

    if proxied is None:
        proxied = os.environ.get("LAVARND_DEMOS_PROXIED") == "1"
    if proxied:
        app.wsgi_app = ProxyFix(app.wsgi_app, x_for=1, x_proto=1, x_host=1, x_prefix=1)

    @app.get("/")
    def index():
        return render_template("index.html", demos=DEMOS)

    # ---- tally statistics ---------------------------------------------------

    @app.get("/tally", endpoint="tally")
    def tally_page():
        return render_template("tally.html", pool_size=pool.pool_size,
                               chunk_size=pool.chunk_size, interval=interval,
                               frames=tally.FRAMES)

    @app.get("/api/frames")
    def api_frames():
        snap = pool.snapshot()
        snap["interval"] = interval
        resp = jsonify(snap)
        resp.headers["Cache-Control"] = "no-store"
        return resp

    return app
