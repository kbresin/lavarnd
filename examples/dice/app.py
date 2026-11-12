# SPDX-License-Identifier: LGPL-2.1-only
"""A small Flask app that rolls dice with LavaRnd.

Every roll comes from a running ``lavapool`` daemon through the ``lavarnd``
Python library. If lavapool can't be reached the app says so (HTTP 503)
instead of quietly using another random source.

Run it (lavapool must be running on this machine)::

    PYTHONPATH=../../python/src flask --app app run

Pages:

    /                       roll from a form: d4..d100 buttons or dice notation
    /api/roll?dice=3d6+2    JSON: {"dice", "count", "sides", "modifier",
                            "rolls", "total", "source"}

Behind a reverse proxy on a path prefix, have the proxy send
``X-Forwarded-Prefix`` (e.g. ``/dice``) and set ``LAVARND_DICE_PROXIED=1``;
links then include the prefix. Only do that when the app listens where the
proxy alone can reach it, since the X-Forwarded headers are then trusted.
"""

from __future__ import annotations

import os
import random
import re
from urllib.parse import parse_qs
from dataclasses import dataclass, field
from typing import List, Optional

from flask import Flask, jsonify, render_template_string, request
from werkzeug.middleware.proxy_fix import ProxyFix

import lavarnd

MAX_DICE = 100
MAX_SIDES = 1000
MAX_MODIFIER = 1000
QUICK_DICE = (4, 6, 8, 10, 12, 20, 100)

_NOTATION = re.compile(r"^\s*(\d*)\s*d\s*(\d+)\s*(?:([+-])\s*(\d+))?\s*$", re.I)


class DiceError(ValueError):
    """The dice notation is malformed or out of range."""


@dataclass
class Roll:
    count: int
    sides: int
    modifier: int = 0
    rolls: List[int] = field(default_factory=list)

    @property
    def notation(self) -> str:
        mod = f"{self.modifier:+d}" if self.modifier else ""
        return f"{self.count}d{self.sides}{mod}"

    @property
    def total(self) -> int:
        return sum(self.rolls) + self.modifier


def parse(notation: str) -> Roll:
    """Parse ``NdS``, ``dS``, ``NdS+M`` or ``NdS-M``."""
    m = _NOTATION.match(notation or "")
    if not m:
        raise DiceError(f"can't read {notation!r}: use dice notation like 3d6 or 1d20+5")
    count = int(m.group(1)) if m.group(1) else 1
    sides = int(m.group(2))
    modifier = int(m.group(4) or 0) * (-1 if m.group(3) == "-" else 1)
    if not 1 <= count <= MAX_DICE:
        raise DiceError(f"number of dice must be 1 to {MAX_DICE}")
    if not 2 <= sides <= MAX_SIDES:
        raise DiceError(f"dice must have 2 to {MAX_SIDES} sides")
    if abs(modifier) > MAX_MODIFIER:
        raise DiceError(f"modifier must be within ±{MAX_MODIFIER}")
    return Roll(count, sides, modifier)


def roll(rng: random.Random, notation: str) -> Roll:
    """Parse ``notation`` and roll it with ``rng``.

    ``randint`` on :class:`lavarnd.LavaRandom` draws random bits and rejects
    out-of-range values, so every face is equally likely (no modulo bias).
    """
    r = parse(notation)
    r.rolls = [rng.randint(1, r.sides) for _ in range(r.count)]
    return r


PAGE = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>LavaRnd Dice</title>
<style>
  :root { --bg: #fdfaf6; --fg: #1d1b19; --muted: #6b645c; --accent: #c2410c;
          --card: #ffffff; --line: #e7e0d8; --bad: #b91c1c; }
  @media (prefers-color-scheme: dark) {
    :root { --bg: #1a1714; --fg: #f3eee8; --muted: #a59d93; --accent: #fb923c;
            --card: #24201c; --line: #3a342e; --bad: #f87171; }
  }
  body { margin: 0; background: var(--bg); color: var(--fg);
         font: 16px/1.5 system-ui, sans-serif; }
  main { max-width: 34rem; margin: 0 auto; padding: 2rem 1rem; }
  h1 { margin: 0 0 .25rem; font-size: 1.6rem; }
  p.sub { margin: 0 0 1.5rem; color: var(--muted); }
  .quick { display: flex; flex-wrap: wrap; gap: .5rem; margin-bottom: 1rem; }
  button, input { font: inherit; border-radius: .5rem; border: 1px solid var(--line);
                  background: var(--card); color: var(--fg); padding: .5rem .8rem; }
  button { cursor: pointer; }
  button:hover { border-color: var(--accent); }
  form.custom { display: flex; gap: .5rem; }
  form.custom input { flex: 1; min-width: 0; }
  .result { margin-top: 1.5rem; padding: 1.25rem; background: var(--card);
            border: 1px solid var(--line); border-radius: .75rem; }
  .total { font-size: 3rem; font-weight: 700; color: var(--accent); line-height: 1.1; }
  .detail { color: var(--muted); word-wrap: break-word; }
  .error { color: var(--bad); }
</style>
</head>
<body>
<main>
  <h1>LavaRnd Dice</h1>
  <p class="sub">Rolled with random numbers from webcam sensor noise.</p>

  <form class="quick" action="{{ url_for('index') }}" method="get">
    {% for s in quick %}<button name="dice" value="1d{{ s }}">d{{ s }}</button>{% endfor %}
  </form>
  <form class="custom" action="{{ url_for('index') }}" method="get">
    <input name="dice" value="{{ dice }}" placeholder="3d6+2" aria-label="dice notation">
    <button>Roll</button>
  </form>

  {% if error %}
    <div class="result error">{{ error }}</div>
  {% elif result %}
    <div class="result">
      <div class="detail">{{ result.notation }}</div>
      <div class="total">{{ result.total }}</div>
      <div class="detail">
        {{ result.rolls | join(" + ") }}{% if result.modifier %}
        {{ "+" if result.modifier > 0 else "−" }} {{ result.modifier | abs }}{% endif %}
      </div>
    </div>
  {% endif %}
</main>
</body>
</html>
"""


def create_app(rng: Optional[random.Random] = None, proxied: Optional[bool] = None) -> Flask:
    """Build the app.

    ``rng`` defaults to a :class:`lavarnd.LavaRandom`. ``proxied`` (default:
    the ``LAVARND_DICE_PROXIED`` environment variable) trusts one reverse
    proxy's X-Forwarded-For/Proto/Host/Prefix headers.
    """
    app = Flask(__name__)
    app.config["RNG"] = rng if rng is not None else lavarnd.LavaRandom()
    if proxied is None:
        proxied = os.environ.get("LAVARND_DICE_PROXIED") == "1"
    if proxied:
        app.wsgi_app = ProxyFix(app.wsgi_app, x_for=1, x_proto=1, x_host=1, x_prefix=1)

    @app.get("/")
    def index():
        dice = request.args.get("dice", "")
        result = error = None
        status = 200
        if dice:
            try:
                result = roll(app.config["RNG"], dice)
            except DiceError as err:
                error, status = str(err), 400
            except lavarnd.LavaRndError as err:
                error, status = f"LavaRnd is unavailable: {err}", 503
        page = render_template_string(PAGE, dice=dice, result=result, error=error,
                                      quick=QUICK_DICE)
        return page, status

    @app.get("/api/roll")
    def api_roll():
        # In a query string "+" means a space, so /api/roll?dice=3d6+2 would
        # arrive as "3d6 2". People type this URL by hand (or with curl), so
        # treat a literal "+" as a plus; "%2B" works too.
        raw = request.query_string.decode("latin-1").replace("+", "%2B")
        dice = parse_qs(raw).get("dice", ["1d6"])[0]
        try:
            r = roll(app.config["RNG"], dice)
        except DiceError as err:
            return jsonify(error=str(err)), 400
        except lavarnd.LavaRndError as err:
            return jsonify(error=f"LavaRnd is unavailable: {err}"), 503
        return jsonify(dice=r.notation, count=r.count, sides=r.sides,
                       modifier=r.modifier, rolls=r.rolls, total=r.total,
                       source="lavarnd")

    return app


app = create_app()
