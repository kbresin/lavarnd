# SPDX-License-Identifier: LGPL-2.1-only
"""Tests for the dice app; no lavapool needed.

    PYTHONPATH=../../python/src python3 -m unittest -v test_app
"""

import random
import unittest

import lavarnd
from app import DiceError, create_app, parse, roll


class Unavailable(random.Random):
    """A random source that fails the way LavaRandom does when lavapool is down."""

    def randint(self, a, b):
        raise lavarnd.LavaRndError("connection refused")


class ParseTests(unittest.TestCase):
    def test_forms(self):
        for text, want in (("d20", (1, 20, 0)), ("3d6", (3, 6, 0)),
                           ("2d8+3", (2, 8, 3)), (" 4 D 10 - 2 ", (4, 10, -2))):
            r = parse(text)
            self.assertEqual((r.count, r.sides, r.modifier), want, text)

    def test_rejects(self):
        for text in ("", "6", "d", "3d", "d1", "0d6", "101d6", "1d1001",
                     "1d6+1001", "2d6*2", "1d6+", "abc"):
            with self.assertRaises(DiceError, msg=text):
                parse(text)

    def test_notation(self):
        self.assertEqual(parse("d6").notation, "1d6")
        self.assertEqual(parse("2d6-1").notation, "2d6-1")


class RollTests(unittest.TestCase):
    def test_rolls_in_range_and_total(self):
        r = roll(random.Random(1), "50d6+3")
        self.assertEqual(len(r.rolls), 50)
        self.assertTrue(all(1 <= x <= 6 for x in r.rolls))
        self.assertEqual(r.total, sum(r.rolls) + 3)

    def test_every_face_appears(self):
        r = roll(random.Random(2), "100d4")
        self.assertEqual(set(r.rolls), {1, 2, 3, 4})


class AppTests(unittest.TestCase):
    def setUp(self):
        self.client = create_app(random.Random(3)).test_client()

    def test_index(self):
        resp = self.client.get("/")
        self.assertEqual(resp.status_code, 200)
        self.assertIn(b"LavaRnd Dice", resp.data)

    def test_index_roll(self):
        resp = self.client.get("/?dice=2d6")
        self.assertEqual(resp.status_code, 200)
        self.assertIn(b'class="total"', resp.data)

    def test_index_bad_notation_is_escaped(self):
        resp = self.client.get("/?dice=<script>")
        self.assertEqual(resp.status_code, 400)
        self.assertNotIn(b"<script>", resp.data)

    def test_api(self):
        data = self.client.get("/api/roll?dice=3d6+2").get_json()
        self.assertEqual((data["count"], data["sides"], data["modifier"]), (3, 6, 2))
        self.assertEqual(data["total"], sum(data["rolls"]) + 2)
        self.assertEqual(data["source"], "lavarnd")

    def test_api_plus_forms(self):
        for q in ("3d6+2", "3d6%2B2"):
            self.assertEqual(self.client.get("/api/roll?dice=" + q).get_json()["modifier"], 2, q)
        self.assertEqual(self.client.get("/api/roll?dice=3d6-2").get_json()["modifier"], -2)

    def test_form_encoded_plus(self):
        # what a browser sends for "2d6+1" typed into the form
        resp = self.client.get("/?dice=2d6%2B1")
        self.assertIn(b"2d6+1", resp.data)

    def test_api_default_and_errors(self):
        self.assertEqual(self.client.get("/api/roll").get_json()["sides"], 6)
        resp = self.client.get("/api/roll?dice=9d0")
        self.assertEqual(resp.status_code, 400)
        self.assertIn("error", resp.get_json())

    def test_lavapool_down_is_503_not_a_fallback(self):
        client = create_app(Unavailable()).test_client()
        self.assertEqual(client.get("/api/roll?dice=1d6").status_code, 503)
        resp = client.get("/?dice=1d6")
        self.assertEqual(resp.status_code, 503)
        self.assertIn(b"LavaRnd is unavailable", resp.data)


if __name__ == "__main__":
    unittest.main()
