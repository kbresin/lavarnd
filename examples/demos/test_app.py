# SPDX-License-Identifier: LGPL-2.1-only
"""Tests for the demos app; no lavapool needed.

    PYTHONPATH=../../python/src python3 -m unittest -v test_app
"""

import random
import threading
import unittest

import lavarnd
import lotto
from app import Budget, create_app
from tally import Collector, TallyPool, byte_counts, nibble_counts, ones


# Each demo page: its path, its heading, and its script (if any).
PAGES = (
    ("/lotto", b"LavaRnd lotto numbers", None),
    ("/dots", b"LavaRnd dot plot", "dots.js"),
    ("/tally", b"LavaRnd tally statistics", "tally.js"),
)


def source(seed=1):
    rng = random.Random(seed)
    return lambda n: rng.randbytes(n)


def unavailable(n):
    raise lavarnd.LavaRndError("connection refused")


def recount(pool):
    """Tallies computed from scratch, to check the incremental ones."""
    counts = byte_counts(bytes(pool.pool))
    return counts, nibble_counts(counts), ones(counts)


class CountTests(unittest.TestCase):
    def test_byte_counts(self):
        counts = byte_counts(b"\x00\x00\xff\x10")
        self.assertEqual((counts[0], counts[255], counts[16], sum(counts)), (2, 1, 1, 4))

    def test_nibbles_and_ones(self):
        counts = byte_counts(b"\x00\xff\x1f")
        n = nibble_counts(counts)
        self.assertEqual((n[0], n[15], n[1], sum(n)), (2, 3, 1, 6))
        self.assertEqual(ones(counts), 0 + 8 + 5)


class PoolTests(unittest.TestCase):
    def make(self, **kw):
        kw.setdefault("pool_size", 1000)
        kw.setdefault("chunk_size", 100)
        return TallyPool(source(), **kw)

    def test_rejects_bad_sizes(self):
        for pool_size, chunk in ((1000, 300), (0, 100), (1000, 0)):
            with self.assertRaises(ValueError):
                TallyPool(source(), pool_size, chunk)

    def test_fill_then_frame_zero(self):
        pool = self.make()
        self.assertFalse(pool.fill_step(400))
        self.assertEqual((pool.filled, len(pool.frames)), (400, 0))
        with self.assertRaises(RuntimeError):
            pool.advance()
        while not pool.fill_step(400):
            pass
        self.assertEqual(pool.filled, 1000)
        self.assertEqual([f["frame"] for f in pool.frames], [0])
        self.assertTrue(pool.fill_step(400))  # already full: no new frame
        self.assertEqual(len(pool.frames), 1)

    def test_advance_keeps_tallies_right(self):
        pool = self.make()
        while not pool.fill_step():
            pass
        for _ in range(25):  # wraps the pool more than twice
            frame = pool.advance()
            self.assertEqual(len(pool.pool), 1000)
            counts, nibbles, n1 = recount(pool)
            self.assertEqual(frame["tally256"], counts)
            self.assertEqual(frame["tally16"], nibbles)
            self.assertEqual(frame["ones"], n1)
        self.assertEqual(frame["frame"], 25)

    def test_advance_replaces_oldest_chunk(self):
        pool = TallyPool(lambda n: b"\x00" * n, 300, 100)
        while not pool.fill_step():
            pass
        pool.source = lambda n: b"\xff" * n
        pool.advance()
        self.assertEqual(bytes(pool.pool), b"\xff" * 100 + b"\x00" * 200)
        self.assertEqual(pool.frames[-1]["ones"], 800)

    def test_keeps_last_frames(self):
        pool = self.make(frames=5)
        while not pool.fill_step():
            pass
        for _ in range(9):
            pool.advance()
        self.assertEqual([f["frame"] for f in pool.frames], [5, 6, 7, 8, 9])


class CollectorTests(unittest.TestCase):
    def test_collects_frames(self):
        pool = TallyPool(source(), 1000, 100)
        got = threading.Event()
        real = pool._record

        def record():
            frame = real()
            if frame["frame"] >= 3:
                got.set()
            return frame

        pool._record = record
        c = Collector(pool, 0.01)
        c.start()
        self.assertTrue(got.wait(5))
        c.stop.set()
        c.join(5)
        self.assertIsNone(pool.error)

    def test_lavapool_down_is_reported(self):
        pool = TallyPool(unavailable, 1000, 100)
        c = Collector(pool, 0.01)
        c.start()
        for _ in range(500):
            if pool.error:
                break
            threading.Event().wait(0.01)
        c.stop.set()
        c.join(5)
        self.assertIn("LavaRnd is unavailable", pool.error)
        self.assertEqual(pool.filled, 0)


class LottoTests(unittest.TestCase):
    def test_picks_are_distinct_sorted_and_in_range(self):
        rng = random.Random(5)
        for game in lotto.GAMES.values():
            for ticket in lotto.pick(rng, game, 3):
                self.assertEqual(len(ticket), len(game.drums))
                for drum, nums in zip(game.drums, ticket):
                    self.assertEqual(len(nums), drum.count)
                    self.assertEqual(len(set(nums)), drum.count)
                    self.assertEqual(nums, sorted(nums))
                    self.assertTrue(all(1 <= n <= drum.high for n in nums))

    def test_every_number_can_come_up(self):
        game = lotto.custom_game(2, 10)
        seen = {n for t in lotto.pick(random.Random(6), game, 10) for n in t[0]}
        seen |= {n for t in lotto.pick(random.Random(7), game, 10) for n in t[0]}
        self.assertEqual(seen, set(range(1, 11)))

    def test_custom_limits(self):
        self.assertEqual(lotto.custom_game(6, 6).rules, "6 of 1–6")
        for count, high in ((0, 10), (11, 10), (21, 100), (1, 0), (1, 1001)):
            with self.assertRaises(lotto.LottoError, msg=(count, high)):
                lotto.custom_game(count, high)
        for tickets in (0, 11):
            with self.assertRaises(lotto.LottoError):
                lotto.pick(random.Random(1), lotto.GAMES["uklotto"], tickets)


class BudgetTests(unittest.TestCase):
    def test_refills_at_rate_up_to_burst(self):
        now = [0.0]
        b = Budget(1000, burst=2000, clock=lambda: now[0])
        self.assertEqual(b.take(1500), 0)
        self.assertAlmostEqual(b.take(1000), 0.5)   # 500 left, needs 500 more
        now[0] = 0.5
        self.assertEqual(b.take(1000), 0)
        now[0] = 100
        self.assertEqual(b.take(2000), 0)            # capped at the burst
        self.assertGreater(b.take(1), 0)


class AppTests(unittest.TestCase):
    def make(self, src=None, **kw):
        kw.setdefault("bytes_per_sec", 100_000)
        app = create_app(src or source(), tally_pool=1000, tally_chunk=100,
                         tally_interval=5, start=False, **kw)
        return app, app.config["POOL"], app.test_client()

    def test_pages(self):
        _, _, client = self.make()
        index = client.get("/").data
        self.assertIn(b"LavaRnd demos", index)
        self.assertIn(b'"/api/bytes"', index)
        for path, heading, script in PAGES:
            resp = client.get(path)
            self.assertEqual(resp.status_code, 200, path)
            self.assertIn(heading, resp.data, path)
            self.assertIn(b'href="' + path.encode() + b'"', index)
            if script:
                self.assertIn(b"/static/" + script.encode(), resp.data, path)
        scripts = tuple(script for _, _, script in PAGES if script)
        for name in ("style.css", "common.js") + scripts:
            with client.get("/static/" + name) as resp:
                self.assertEqual(resp.status_code, 200, name)

    def test_lotto_page(self):
        _, _, client = self.make()
        self.assertNotIn(b'class="ball', client.get("/lotto").data)
        resp = client.get("/lotto?game=powerball&tickets=2")
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.data.count(b'class="ball extra"'), 2)
        self.assertEqual(resp.data.count(b'class="ball"'), 10)
        resp = client.get("/lotto?game=custom&count=3&high=9")
        self.assertIn(b"Pick 3 of 9", resp.data)

    def test_lotto_errors_are_escaped(self):
        _, _, client = self.make()
        resp = client.get("/lotto?game=<b>x</b>")
        self.assertEqual(resp.status_code, 400)
        self.assertNotIn(b"<b>x</b>", resp.data)
        self.assertIn(b"&lt;b&gt;x&lt;/b&gt;", resp.data)
        self.assertEqual(client.get("/lotto?game=uklotto&tickets=x").status_code, 400)

    def test_lotto_api(self):
        _, _, client = self.make()
        data = client.get("/api/lotto?game=euromillions&tickets=3").get_json()
        self.assertEqual((data["game"], data["drums"], data["source"]),
                         ("euromillions", ["main numbers", "Lucky Stars"], "lavarnd"))
        self.assertEqual([len(t[0]) for t in data["tickets"]], [5, 5, 5])
        self.assertEqual(client.get("/api/lotto?game=nope").status_code, 400)

    def test_bytes_api(self):
        _, _, client = self.make()
        resp = client.get("/api/bytes?n=4096")
        self.assertEqual((resp.status_code, len(resp.data)), (200, 4096))
        self.assertEqual(resp.mimetype, "application/octet-stream")
        self.assertEqual(resp.headers["Cache-Control"], "no-store")
        self.assertEqual(len(client.get("/api/bytes").data), 1024)
        for n in ("0", "4097", "x"):
            self.assertEqual(client.get("/api/bytes?n=" + n).status_code, 400, n)

    def test_bytes_budget_is_shared(self):
        _, _, client = self.make(bytes_per_sec=1000)  # burst: 8192 bytes
        self.assertEqual(client.get("/api/bytes?n=4096").status_code, 200)
        self.assertEqual(client.get("/api/bytes?n=4096").status_code, 200)
        resp = client.get("/api/bytes?n=4096")
        self.assertEqual(resp.status_code, 429)
        self.assertGreaterEqual(int(resp.headers["Retry-After"]), 1)

    def test_lavapool_down_is_503_not_a_fallback(self):
        _, _, client = self.make(unavailable)
        self.assertEqual(client.get("/api/bytes?n=16").status_code, 503)
        self.assertEqual(client.get("/api/lotto").status_code, 503)
        resp = client.get("/lotto?game=powerball")
        self.assertEqual(resp.status_code, 503)
        self.assertIn(b"LavaRnd is unavailable", resp.data)

    def test_frames_api(self):
        _, pool, client = self.make()
        data = client.get("/api/frames").get_json()
        self.assertEqual((data["pool_size"], data["filled"], data["frames"]), (1000, 0, []))
        while not pool.fill_step():
            pass
        pool.advance()
        resp = client.get("/api/frames")
        self.assertEqual(resp.headers["Cache-Control"], "no-store")
        data = resp.get_json()
        self.assertEqual(data["interval"], 5)
        self.assertEqual([f["frame"] for f in data["frames"]], [0, 1])
        self.assertEqual(sum(data["frames"][1]["tally256"]), 1000)
        self.assertEqual(sum(data["frames"][1]["tally16"]), 2000)

    def test_links_follow_proxy_prefix(self):
        _, _, client = self.make(proxied=True)
        resp = client.get("/tally", headers={"X-Forwarded-Prefix": "/demos"})
        self.assertIn(b'"/demos/api/frames"', resp.data)
        self.assertIn(b"/demos/static/tally.js", resp.data)
        self.assertIn(b'href="/demos/"', resp.data)
        _, _, client = self.make()
        resp = client.get("/tally", headers={"X-Forwarded-Prefix": "/demos"})
        self.assertIn(b'"/api/frames"', resp.data)

    def test_rejects_bad_interval(self):
        with self.assertRaises(ValueError):
            create_app(source(), tally_interval=-1, start=False)


if __name__ == "__main__":
    unittest.main()
