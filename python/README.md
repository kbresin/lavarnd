# lavarnd (Python)

A Python client for LavaRnd™: random numbers from the noise of a webcam
sensor. It replaces upstream's Perl API, and is part of the
[LavaRnd revival fork](https://github.com/kbresin/lavarnd).

The library talks directly to a running `lavapool` daemon over its simple TCP
protocol, so you don't need the C libraries or a compiler. It is pure Python
with no dependencies and supports Python 3.9 and later.

## Usage

```python
import lavarnd

key = lavarnd.random_bytes(32)        # bytes straight from lavapool

rng = lavarnd.LavaRandom()            # a random.Random backed by LavaRnd
rng.randint(1, 6)
rng.shuffle(deck)
```

`LavaRandom` works like `random.SystemRandom`: every `random.Random` method is
available, and there is no seed or saved state.

You can control what happens when the daemon can't be reached:

```python
import os
import lavarnd

pool = lavarnd.LavaPool(retries=None)                  # keep trying forever
pool = lavarnd.LavaPool(retries=0)                     # fail fast
pool = lavarnd.LavaPool(fallback=os.urandom)           # use the kernel instead
data = pool.read(64)
pool.fallback_bytes                                    # bytes that came from the fallback
```

Without a fallback, failures raise `lavarnd.LavaRndError`.

From the shell:

    python -m lavarnd --hex 32
    python -m lavarnd 1048576 > random.bin

## trickle

`lavarnd.trickle` is a Python port of upstream's Perl `trickle` daemon. Every
`period` seconds it writes `chunk_len` bytes from `lavapool` into a file,
normally `/dev/urandom`:

    python -m lavarnd.trickle [-v LVL] [-n COUNT] [-e BITS] [chunk_len [period [file]]]

It defaults to 16 bytes every 300 seconds into `/dev/urandom`, the values
upstream's init script used. If `lavapool` can't be reached, it skips that
cycle and keeps going, logging once when `lavapool` goes away and once when it
comes back. `scripts/user-install.sh` installs it as `trickle.service`.

Any user can write to `/dev/urandom`. Doing so mixes the data into the
kernel's pool but doesn't raise the kernel's entropy estimate. The original
only did that.

`-e BITS` (`--credit`) adds each chunk with the `RNDADDENTROPY` ioctl
instead, crediting BITS bits of entropy per byte (1 to 8). That needs
`CAP_SYS_ADMIN`. Without it, trickle exits at startup with an error rather
than failing every cycle. Crediting is a claim about how unpredictable the
data is, so choose BITS deliberately. On current kernels, once the pool is
initialised the credit only affects the reported estimate, not what readers
get.

## Configuration

The client reads the same `cfg.random` file as the C library. It looks in:

1. the path you pass to `lavarnd.load_config(path)` or `-c FILE`,
2. the file named by `$LAVARND_CFG`,
3. `/etc/LavaRnd/cfg.random`.

If none of these exists it uses the built-in defaults: `lavapool` on
`127.0.0.1:23209`, at most 65536 bytes per request, 3 retries, and per-attempt
timeouts that grow from 2 to 6 seconds. A `lavapool` set up with
`scripts/user-install.sh` uses those defaults, so no config file is needed.

## Tests

    cd python
    PYTHONPATH=src python3 -m unittest discover -s tests

The tests run against a fake `lavapool`. Set `LAVARND_LIVE=1` to also test
against a real daemon on the default port.

## Authorship and license

The protocol and defaults come from the original LavaRnd by Landon Curt Noll
and Simon Cooper. This Python client was written for the fork with AI
assistance (Claude), reviewed by the maintainer. LGPL-2.1, like the rest of
LavaRnd. LavaRnd™ is a trademark of Landon Curt Noll and Simon Cooper.
