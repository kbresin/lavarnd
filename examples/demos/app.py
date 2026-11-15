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
    /lotto              lottery numbers (server-side picks)
    /dots               dot plot (draws in the browser)
    /tally              tally statistics (shared rolling pool)

    /api/lotto?game=powerball&tickets=2     JSON picks
    /api/bytes?n=1024                       raw bytes (n <= 4096) for the
                                            pages that draw in the browser
    /api/frames                             tally frames as JSON

Settings (environment variables):

    LAVARND_DEMOS_PROXIED        1 to trust one reverse proxy's X-Forwarded-*
                                 headers, including X-Forwarded-Prefix
    LAVARND_DEMOS_BYTES_PER_SEC  /api/bytes budget shared by all viewers
                                 (default 8192)
    LAVARND_TALLY_INTERVAL       seconds between tally frames (default 864)
    LAVARND_TALLY_POOL           tally pool size in bytes (default 4000000)
    LAVARND_TALLY_CHUNK          bytes replaced per frame (default 40000)
"""

from __future__ import annotations

import math
import os
import threading
import time
from typing import Callable, Optional

from flask import Flask, Response, jsonify, render_template, request
from werkzeug.middleware.proxy_fix import ProxyFix

import lavarnd
import lotto
import tally

MAX_BYTES = 4096
BYTES_PER_SEC = 8192

DEMOS = (
    ("lotto", "Lotto numbers", "Lottery picks drawn with LavaRnd."),
    ("dots", "Dot plot", "Random points filling a square, and what a weak generator looks like."),
    ("tally", "Tally statistics", "A day of 4-bit, 8-bit and 1-bit tallies over a rolling 4 MB pool."),
)


class Budget:
    """A token bucket: ``rate`` bytes per second, bursting to ``burst``.

    Shared by every viewer, so the canvas demos together can't take more
    than ``rate`` bytes a second from lavapool however many people watch.
    """

    def __init__(self, rate: float, burst: Optional[float] = None,
                 clock: Callable[[], float] = time.monotonic) -> None:
        if rate <= 0:
            raise ValueError("rate must be > 0")
        self.rate = rate
        self.burst = burst if burst is not None else max(2 * rate, 2 * MAX_BYTES)
        self.clock = clock
        self.tokens = self.burst
        self.last = clock()
        self.lock = threading.Lock()

    def take(self, n: int) -> float:
        """Spend ``n`` bytes. Returns 0 if allowed, else seconds to wait."""
        with self.lock:
            now = self.clock()
            self.tokens = min(self.burst, self.tokens + (now - self.last) * self.rate)
            self.last = now
            if self.tokens >= n:
                self.tokens -= n
                return 0.0
            return (n - self.tokens) / self.rate


class _SourcePool:
    """Adapts a ``source(n)`` function to what LavaRandom expects."""

    def __init__(self, source: Callable[[int], bytes]) -> None:
        self.read = source


def _env(name: str, default, kind=float):
    value = os.environ.get(name)
    return kind(value) if value else default


def create_app(source: Optional[Callable[[int], bytes]] = None, *,
               tally_interval: Optional[float] = None,
               tally_pool: Optional[int] = None,
               tally_chunk: Optional[int] = None,
               bytes_per_sec: Optional[float] = None,
               start: bool = True, proxied: Optional[bool] = None) -> Flask:
    """Build the app and (unless ``start`` is False) start the tally collector.

    ``source(n)`` returns ``n`` random bytes; it defaults to
    :func:`lavarnd.random_bytes` and must raise :class:`lavarnd.LavaRndError`
    when it can't. The other settings default to the environment variables
    above, then to the original demos' values.
    """
    app = Flask(__name__)
    source = source or lavarnd.random_bytes
    rng = lavarnd.LavaRandom(_SourcePool(source), bufsize=256)
    budget = Budget(bytes_per_sec or _env("LAVARND_DEMOS_BYTES_PER_SEC", BYTES_PER_SEC))

    pool = tally.TallyPool(
        source,
        tally_pool or _env("LAVARND_TALLY_POOL", tally.POOL_SIZE, int),
        tally_chunk or _env("LAVARND_TALLY_CHUNK", tally.CHUNK_SIZE, int),
    )
    interval = tally_interval or _env("LAVARND_TALLY_INTERVAL", tally.INTERVAL)
    if interval <= 0:
        raise ValueError("tally interval must be > 0")
    app.config.update(POOL=pool, BUDGET=budget, RNG=rng)
    if start:
        app.config["COLLECTOR"] = tally.Collector(pool, interval)
        app.config["COLLECTOR"].start()

    if proxied is None:
        proxied = os.environ.get("LAVARND_DEMOS_PROXIED") == "1"
    if proxied:
        app.wsgi_app = ProxyFix(app.wsgi_app, x_for=1, x_proto=1, x_host=1, x_prefix=1)

    def unavailable(err):
        return f"LavaRnd is unavailable: {err}"

    @app.get("/")
    def index():
        return render_template("index.html", demos=DEMOS)

    # ---- lotto ----------------------------------------------------------

    def lotto_request():
        """Read game settings from the query string; returns (game, tickets)."""
        key = request.args.get("game", "powerball")
        try:
            tickets = int(request.args.get("tickets", "1"))
            count = int(request.args.get("count", "6"))
            high = int(request.args.get("high", "49"))
        except ValueError:
            raise lotto.LottoError("tickets, count and high must be whole numbers") from None
        if key == "custom":
            return lotto.custom_game(count, high), tickets
        if key not in lotto.GAMES:
            raise lotto.LottoError(f"unknown game {key!r}")
        return lotto.GAMES[key], tickets

    @app.get("/lotto", endpoint="lotto")
    def lotto_page():
        result = error = game = None
        status = 200
        if request.args:
            try:
                game, tickets = lotto_request()
                result = lotto.pick(rng, game, tickets)
            except lotto.LottoError as err:
                error, status = str(err), 400
            except lavarnd.LavaRndError as err:
                error, status = unavailable(err), 503
        page = render_template("lotto.html", games=lotto.GAMES.values(), game=game,
                               result=result, error=error, args=request.args,
                               max_tickets=lotto.MAX_TICKETS)
        return page, status

    @app.get("/api/lotto")
    def api_lotto():
        try:
            game, tickets = lotto_request()
            picks = lotto.pick(rng, game, tickets)
        except lotto.LottoError as err:
            return jsonify(error=str(err)), 400
        except lavarnd.LavaRndError as err:
            return jsonify(error=unavailable(err)), 503
        return jsonify(game=game.key, name=game.name, rules=game.rules,
                       drums=[d.name for d in game.drums], tickets=picks,
                       source="lavarnd")

    # ---- canvas demos -----------------------------------------------------

    for key, title, _ in DEMOS:
        if key not in ("lotto", "tally"):  # these have their own routes
            app.add_url_rule(f"/{key}", key,
                             lambda key=key, title=title: render_template(f"{key}.html", title=title))

    @app.get("/api/bytes")
    def api_bytes():
        try:
            n = int(request.args.get("n", "1024"))
        except ValueError:
            n = 0
        if not 1 <= n <= MAX_BYTES:
            return Response(f"n must be 1 to {MAX_BYTES}\n", 400, mimetype="text/plain")
        wait = budget.take(n)
        if wait:
            return Response("busy: too many random bytes requested; try again shortly\n",
                            429, {"Retry-After": str(math.ceil(wait))}, mimetype="text/plain")
        try:
            data = source(n)
        except lavarnd.LavaRndError as err:
            return Response(unavailable(err) + "\n", 503, mimetype="text/plain")
        return Response(data, mimetype="application/octet-stream",
                        headers={"Cache-Control": "no-store"})

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
