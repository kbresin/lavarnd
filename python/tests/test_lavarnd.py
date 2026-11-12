# SPDX-License-Identifier: LGPL-2.1-only
"""Tests for the lavarnd Python client, against a fake lavapool server.

Run from the python/ directory:

    PYTHONPATH=src python3 -m unittest discover -s tests

Set LAVARND_LIVE=1 to also test against a real lavapool on its default port.
"""

from __future__ import annotations

import collections
import io
import math
import os
import random
import socket
import socketserver
import struct
import tempfile
import threading
import time
import unittest
from contextlib import redirect_stderr, redirect_stdout
from unittest import mock

import lavarnd
from lavarnd import config as cfgmod
from lavarnd import trickle
from lavarnd.__main__ import main as cli_main


class FakeLavapool:
    """A lavapool stand-in that follows daemon/client.c's protocol.

    ``mode`` controls the reply: "ok" sends the requested bytes, "short"
    sends half and closes, "close" closes without replying, "stall" never
    replies. ``fail_first`` makes that many connections use "close" first.
    """

    def __init__(self, maxrequest=65536, mode="ok", fail_first=0, port=0):
        self.maxrequest = maxrequest
        self.mode = mode
        self.fail_first = fail_first
        self.requests = []
        self.connections = 0
        self.lock = threading.Lock()
        outer = self

        class Handler(socketserver.StreamRequestHandler):
            def handle(self):
                with outer.lock:
                    outer.connections += 1
                    failing = outer.connections <= outer.fail_first
                line = self.rfile.readline(22)
                if not line.endswith(b"\n"):
                    return
                n = int(line.strip() or b"0", 0)
                with outer.lock:
                    outer.requests.append(n)
                mode = "close" if failing else outer.mode
                if n <= 0 or n > outer.maxrequest or mode == "close":
                    return
                if mode == "stall":
                    time.sleep(1.0)
                    return
                data = os.urandom(n)
                if mode == "short":
                    data = data[: n // 2]
                self.wfile.write(data)

        class Server(socketserver.ThreadingTCPServer):
            allow_reuse_address = True
            daemon_threads = True

        self.server = Server(("127.0.0.1", port), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)

    @property
    def config(self):
        host, port = self.server.server_address
        return lavarnd.Config(
            lavapool=f"{host}:{port}",
            maxrequest=self.maxrequest,
            return_retries=2,
            return_min_wait=0.2,
            return_max_wait=0.4,
        )

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *exc):
        self.server.shutdown()
        self.server.server_close()


def unused_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class ConfigTests(unittest.TestCase):
    def test_defaults_match_c_library(self):
        c = lavarnd.Config()
        self.assertEqual(c.address, ("127.0.0.1", 23209))
        self.assertEqual(c.maxrequest, 65536)

    def test_parse_upstream_default_file(self):
        here = os.path.dirname(os.path.abspath(__file__))
        path = os.path.join(here, "..", "..", "lib", "cfg.random-def")
        if not os.path.exists(path):
            self.skipTest("lib/cfg.random-def not found")
        with open(path) as f:
            c = cfgmod.parse(f.read(), path)
        self.assertEqual(c.lavapool, "127.0.0.1:23209")
        self.assertEqual(c.return_retries, 3)
        self.assertEqual(c.return_max_wait, 6.0)

    def test_parse_rules(self):
        c = cfgmod.parse(
            "# comment\n\n   lavapool = 10.0.0.1:999 \nmaxrequest=0x100\n"
            "s100_retries=7\n"  # known to C, ignored here
        )
        self.assertEqual(c.address, ("10.0.0.1", 999))
        self.assertEqual(c.maxrequest, 256)
        with self.assertRaises(ValueError):
            cfgmod.parse("no equals sign\n")
        with self.assertRaises(ValueError):
            cfgmod.parse("lavapool=nonsense\n")
        with self.assertRaises(ValueError):
            cfgmod.parse("maxrequest=0\n")

    def test_load_order(self):
        with tempfile.NamedTemporaryFile("w", suffix=".cfg", delete=False) as f:
            f.write("lavapool=127.0.0.1:1234\n")
        try:
            self.assertEqual(cfgmod.load(f.name).address[1], 1234)
            with mock.patch.dict(os.environ, {cfgmod.CFG_ENV: f.name}):
                self.assertEqual(cfgmod.load().address[1], 1234)
            with mock.patch.dict(os.environ, {cfgmod.CFG_ENV: ""}), \
                    mock.patch.object(cfgmod, "SYSTEM_CFG", "/nonexistent/cfg.random"):
                self.assertEqual(cfgmod.load(), lavarnd.Config())
        finally:
            os.unlink(f.name)


class PoolTests(unittest.TestCase):
    def test_read_sends_decimal_request(self):
        with FakeLavapool() as srv:
            data = lavarnd.LavaPool(srv.config).read(100)
        self.assertEqual(len(data), 100)
        self.assertEqual(srv.requests, [100])

    def test_large_read_is_chunked_by_maxrequest(self):
        with FakeLavapool(maxrequest=1000) as srv:
            data = lavarnd.LavaPool(srv.config).read(2500)
        self.assertEqual(len(data), 2500)
        self.assertEqual(srv.requests, [1000, 1000, 500])

    def test_zero_and_negative(self):
        with FakeLavapool() as srv:
            pool = lavarnd.LavaPool(srv.config)
            self.assertEqual(pool.read(0), b"")
            with self.assertRaises(ValueError):
                pool.read(-1)
        self.assertEqual(srv.requests, [])

    def test_retries_then_succeeds(self):
        with FakeLavapool(fail_first=2) as srv:
            data = lavarnd.LavaPool(srv.config).read(16)
        self.assertEqual(len(data), 16)
        self.assertEqual(srv.connections, 3)

    def test_gives_up_after_retries(self):
        with FakeLavapool(mode="close") as srv:
            pool = lavarnd.LavaPool(srv.config, retries=1)
            with self.assertRaises(lavarnd.LavaRndError):
                pool.read(16)
        self.assertEqual(srv.connections, 2)

    def test_short_reply_is_an_error(self):
        with FakeLavapool(mode="short") as srv:
            with self.assertRaises(lavarnd.LavaRndError):
                lavarnd.LavaPool(srv.config, retries=0).read(64)

    def test_timeout(self):
        with FakeLavapool(mode="stall") as srv:
            start = time.monotonic()
            with self.assertRaises(lavarnd.LavaRndError):
                lavarnd.LavaPool(srv.config, retries=0).read(8)
            self.assertLess(time.monotonic() - start, 0.9)

    def test_connection_refused_uses_fallback(self):
        c = lavarnd.Config(lavapool=f"127.0.0.1:{unused_port()}",
                           return_min_wait=0.1, return_max_wait=0.1)
        pool = lavarnd.LavaPool(c, retries=0, fallback=lambda n: b"\x00" * n)
        self.assertEqual(pool.read(10), b"\x00" * 10)
        self.assertEqual(pool.fallback_bytes, 10)

    def test_bad_fallback_length(self):
        c = lavarnd.Config(lavapool=f"127.0.0.1:{unused_port()}",
                           return_min_wait=0.1, return_max_wait=0.1)
        pool = lavarnd.LavaPool(c, retries=0, fallback=lambda n: b"")
        with self.assertRaises(lavarnd.LavaRndError):
            pool.read(4)

    def test_refused_attempts_are_spaced_out(self):
        # A refused connection fails at once; each retry must still wait out
        # the failed attempt's deadline (lavapool may be starting up), and
        # there is no wait after the last attempt.
        c = lavarnd.Config(lavapool=f"127.0.0.1:{unused_port()}",
                           return_min_wait=2.0, return_max_wait=6.0)
        pool = lavarnd.LavaPool(c, retries=2, fallback=lambda n: b"\x00" * n)
        sleeps = []
        pool._clock = lambda: 100.0
        pool._sleep = sleeps.append
        self.assertEqual(pool.read(4), b"\x00" * 4)
        self.assertEqual(sleeps, [2.0, 4.0])

    def test_reaches_lavapool_that_starts_late(self):
        port = unused_port()
        c = lavarnd.Config(lavapool=f"127.0.0.1:{port}", return_retries=3,
                           return_min_wait=0.2, return_max_wait=0.4)
        late = {}

        def start():
            late["srv"] = FakeLavapool(port=port).__enter__()

        timer = threading.Timer(0.3, start)
        timer.start()
        try:
            data = lavarnd.LavaPool(c).read(8)
        finally:
            timer.join()
            if "srv" in late:
                late["srv"].__exit__(None, None, None)
        self.assertEqual(len(data), 8)

    def test_timeouts_grow_from_min_to_max(self):
        pool = lavarnd.LavaPool(lavarnd.Config(return_min_wait=2.0, return_max_wait=6.0),
                                retries=2)
        self.assertEqual([pool._timeout(i) for i in range(4)], [2.0, 4.0, 6.0, 6.0])


class LavaRandomTests(unittest.TestCase):
    def setUp(self):
        self.srv = FakeLavapool().__enter__()
        self.rng = lavarnd.LavaRandom(lavarnd.LavaPool(self.srv.config), bufsize=1024)

    def tearDown(self):
        self.srv.__exit__(None, None, None)

    def test_is_a_random_random(self):
        self.assertIsInstance(self.rng, random.Random)
        for _ in range(200):
            self.assertTrue(1 <= self.rng.randint(1, 6) <= 6)
            x = self.rng.random()
            self.assertTrue(0.0 <= x < 1.0)
        items = list(range(20))
        self.rng.shuffle(items)
        self.assertEqual(sorted(items), list(range(20)))
        self.assertEqual(len(self.rng.sample(range(100), 10)), 10)
        self.assertTrue(math.isfinite(self.rng.gauss(0, 1)))

    def test_getrandbits_range(self):
        for k in (1, 7, 8, 9, 63, 64, 65, 200):
            for _ in range(50):
                self.assertLess(self.rng.getrandbits(k), 1 << k)
        self.assertEqual(self.rng.getrandbits(0), 0)
        with self.assertRaises(ValueError):
            self.rng.getrandbits(-1)

    def test_randbelow_is_roughly_uniform(self):
        counts = collections.Counter(self.rng.randrange(6) for _ in range(6000))
        self.assertEqual(set(counts), set(range(6)))
        for c in counts.values():
            self.assertTrue(800 < c < 1200, counts)

    def test_buffering_uses_few_connections(self):
        self.rng.randbytes(100)
        self.rng.randbytes(100)
        self.assertEqual(len(self.srv.requests), 1)
        self.assertEqual(len(self.rng.randbytes(5000)), 5000)

    def test_bytes_are_not_reused(self):
        a = self.rng.randbytes(512)
        b = self.rng.randbytes(512)
        self.assertNotEqual(a, b)

    def test_no_seed_or_state(self):
        self.assertIsNone(self.rng.seed(42))
        with self.assertRaises(NotImplementedError):
            self.rng.getstate()
        with self.assertRaises(NotImplementedError):
            self.rng.setstate(None)

    @unittest.skipUnless(hasattr(os, "fork"), "needs fork")
    def test_child_discards_parent_buffer(self):
        self.rng.randbytes(1)  # fill the buffer
        r, w = os.pipe()
        pid = os.fork()
        if pid == 0:
            os.close(r)
            os.write(w, self.rng.randbytes(16))
            os._exit(0)
        os.close(w)
        child = os.read(r, 16)
        os.close(r)
        os.waitpid(pid, 0)
        parent = self.rng.randbytes(16)
        self.assertNotEqual(child, parent)


class CliTests(unittest.TestCase):
    def test_hex_output(self):
        with FakeLavapool() as srv, tempfile.NamedTemporaryFile("w", delete=False) as f:
            f.write(f"lavapool={srv.config.lavapool}\n")
            f.close()
            out = io.StringIO()
            with redirect_stdout(out):
                rc = cli_main(["--hex", "-c", f.name, "8"])
            os.unlink(f.name)
        self.assertEqual(rc, 0)
        self.assertEqual(len(bytes.fromhex(out.getvalue().strip())), 8)


class TrickleTests(unittest.TestCase):
    def setUp(self):
        fd, self.path = tempfile.mkstemp()
        os.close(fd)
        self.fd = os.open(self.path, os.O_WRONLY | os.O_APPEND)

    def tearDown(self):
        os.close(self.fd)
        os.unlink(self.path)

    def test_writes_count_chunks(self):
        with FakeLavapool() as srv:
            n = trickle.trickle(lavarnd.LavaPool(srv.config), self.fd, 16, 0, count=5)
        self.assertEqual(n, 5)
        self.assertEqual(os.path.getsize(self.path), 80)
        self.assertEqual(srv.requests, [16] * 5)

    def test_skips_when_lavapool_is_down_and_recovers(self):
        logs = []
        with FakeLavapool(fail_first=2) as srv:
            pool = lavarnd.LavaPool(srv.config, retries=0)
            n = trickle.trickle(pool, self.fd, 8, 0, count=4,
                                log=lambda lvl, msg: logs.append((lvl, msg)))
        self.assertEqual(n, 2)
        self.assertEqual(os.path.getsize(self.path), 16)
        warnings = [m for lvl, m in logs if lvl == 0]
        self.assertEqual(len(warnings), 2, warnings)  # one "down", one "back"
        self.assertIn("cannot get data", warnings[0])
        self.assertIn("back", warnings[1])

    def test_stop_event_ends_the_loop(self):
        stop = threading.Event()
        with FakeLavapool() as srv:
            t = threading.Thread(target=trickle.trickle,
                                 args=(lavarnd.LavaPool(srv.config), self.fd, 4, 10.0),
                                 kwargs={"stop": stop})
            t.start()
            time.sleep(0.3)
            stop.set()
            t.join(2)
        self.assertFalse(t.is_alive())
        self.assertEqual(os.path.getsize(self.path), 4)

    def test_cli(self):
        with FakeLavapool() as srv, tempfile.NamedTemporaryFile("w", delete=False) as cfg:
            cfg.write(f"lavapool={srv.config.lavapool}\n")
            cfg.close()
            rc = trickle.main(["-c", cfg.name, "-n", "3", "32", "0", self.path])
            os.unlink(cfg.name)
        self.assertEqual(rc, 0)
        self.assertEqual(os.path.getsize(self.path), 96)

    def test_rndaddentropy_number(self):
        # _IOW('R', 0x03, int[2]) from <linux/random.h> on x86 and ARM
        self.assertEqual(trickle.RNDADDENTROPY, 0x40085203)

    def test_credit_uses_rndaddentropy(self):
        calls = []
        with FakeLavapool() as srv, \
                mock.patch.object(trickle.fcntl, "ioctl",
                                  lambda fd, req, arg: calls.append((fd, req, arg))):
            n = trickle.trickle(lavarnd.LavaPool(srv.config), self.fd, 16, 0,
                                count=2, credit=8)
        self.assertEqual(n, 2)
        self.assertEqual(os.path.getsize(self.path), 0)  # nothing write()n
        for fd, req, arg in calls:
            self.assertEqual((fd, req), (self.fd, trickle.RNDADDENTROPY))
            bits, size = struct.unpack("ii", arg[:8])
            self.assertEqual((bits, size, len(arg)), (128, 16, 24))

    @unittest.skipIf(os.geteuid() == 0, "root may hold CAP_SYS_ADMIN")
    def test_credit_without_permission_fails_at_startup(self):
        err = io.StringIO()
        with redirect_stderr(err):
            rc = trickle.main(["-e", "8", "-n", "1", "16", "0", "/dev/urandom"])
        self.assertEqual(rc, 1)
        self.assertIn("CAP_SYS_ADMIN", err.getvalue())

    def test_cli_validation(self):
        for args in (["0"], ["40000"], ["16", "-1"], ["16", "1", "/nonexistent/x"],
                     ["-e", "9"], ["-e", "-1"]):
            with self.assertRaises(SystemExit), redirect_stderr(io.StringIO()):
                trickle.main(args)


@unittest.skipUnless(os.environ.get("LAVARND_LIVE") == "1", "set LAVARND_LIVE=1 to use a real lavapool")
class LiveTests(unittest.TestCase):
    def test_live_daemon(self):
        pool = lavarnd.LavaPool(lavarnd.Config(), retries=0)
        data = pool.read(70000)  # crosses maxrequest, so two connections
        self.assertEqual(len(data), 70000)
        self.assertGreater(len(set(data)), 250)
        self.assertEqual(pool.fallback_bytes, 0)
        self.assertTrue(1 <= lavarnd.LavaRandom(pool).randint(1, 6) <= 6)


if __name__ == "__main__":
    unittest.main()
