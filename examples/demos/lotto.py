# SPDX-License-Identifier: LGPL-2.1-only
"""Lottery number picks for the lotto demo.

Each game draws one or more sets of distinct numbers, like balls out of a
drum. ``random.Random.sample`` uses ``randbelow``, which on
:class:`lavarnd.LavaRandom` draws random bits and rejects out-of-range
values, so every combination is equally likely.
"""

from __future__ import annotations

import random
from dataclasses import dataclass
from typing import Dict, List, Tuple

MAX_TICKETS = 10
MAX_PICK = 20
MAX_RANGE = 1000


class LottoError(ValueError):
    """The game or its settings are out of range."""


@dataclass(frozen=True)
class Drum:
    """Pick ``count`` distinct numbers from 1..``high``."""
    name: str
    count: int
    high: int


@dataclass(frozen=True)
class Game:
    key: str
    name: str
    drums: Tuple[Drum, ...]

    @property
    def rules(self) -> str:
        return " + ".join(f"{d.count} of 1–{d.high}" for d in self.drums)


# Rules as of 2025; lotteries change them now and then.
GAMES: Dict[str, Game] = {g.key: g for g in (
    Game("powerball", "Powerball", (Drum("white balls", 5, 69), Drum("Powerball", 1, 26))),
    Game("megamillions", "Mega Millions", (Drum("white balls", 5, 70), Drum("Mega Ball", 1, 24))),
    Game("superlotto", "SuperLotto Plus", (Drum("numbers", 5, 47), Drum("Mega", 1, 27))),
    # Minnesota's Gopher 5
    Game("statepick5", "State Pick 5", (Drum("numbers", 5, 47),)),
    Game("euromillions", "EuroMillions", (Drum("main numbers", 5, 50), Drum("Lucky Stars", 2, 12))),
    Game("uklotto", "UK Lotto", (Drum("balls", 6, 59),)),
)}


def custom_game(count: int, high: int) -> Game:
    if not 1 <= high <= MAX_RANGE:
        raise LottoError(f"the highest number must be 1 to {MAX_RANGE}")
    if not 1 <= count <= min(MAX_PICK, high):
        raise LottoError(f"pick 1 to {min(MAX_PICK, high)} numbers from 1–{high}")
    return Game("custom", f"Pick {count} of {high}", (Drum("numbers", count, high),))


def pick(rng: random.Random, game: Game, tickets: int = 1) -> List[List[List[int]]]:
    """Return ``tickets`` tickets; each is one sorted list per drum."""
    if not 1 <= tickets <= MAX_TICKETS:
        raise LottoError(f"tickets must be 1 to {MAX_TICKETS}")
    return [[sorted(rng.sample(range(1, d.high + 1), d.count)) for d in game.drums]
            for _ in range(tickets)]
