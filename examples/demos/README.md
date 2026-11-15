# LavaRnd demos

Rebuilds of the demos from the original lavarand.org site (2003), as one
small Flask app. Every random number comes from a running `lavapool` through
the [`lavarnd`](../../python/) Python library. If `lavapool` can't be
reached the demos say so (HTTP 503) rather than quietly using another source
of randomness.

| Page     | Demo                                                                      |
|----------|---------------------------------------------------------------------------|
| `/`      | Index of demos                                                            |
| `/lotto` | **Lotto numbers**: picks for Powerball, Mega Millions, SuperLotto Plus, State Pick 5 (Minnesota's Gopher 5), EuroMillions, UK Lotto or a custom "pick K of N" |
| `/dots`  | **Dot plot**: byte pairs as points on a 256 × 256 grid, next to the low byte of a C-style LCG, which only ever hits 128 cells |
| `/walk`  | **Random walk**: two bits per step on a grid, with distance against √n    |
| `/dist`  | **Distribution**: live histograms of 1 byte, or the sum of 2 or 8 bytes, against the exact expected shape, with χ² |
| `/tally` | **Tally statistics**: the original `randstatd` demo; see below            |

The originals were CGI scripts that drew GIFs. The Wayback Machine kept the
pages describing them but not their output, so these are rebuilt from those
descriptions rather than copied.

## How they get random data

**Lotto** picks on the server with `LavaRandom.sample`, which draws random
bits and rejects out-of-range values, so every combination is equally
likely. The same picks are available as JSON:

```
$ curl -s 'http://localhost:8098/api/lotto?game=powerball&tickets=1'
{"drums":["white balls","Powerball"],"game":"powerball","name":"Powerball","rules":"5 of 1\u201369 + 1 of 1\u201326","source":"lavarnd","tickets":[[[4,19,33,51,62],[17]]]}
```

**Pages that draw in the browser** fetch raw bytes from `/api/bytes?n=N`
(at most 4096 a request), about 2–4 KB a second per open page. Each page
stops after a few hundred KB and pauses while its tab is hidden. All
viewers share one budget,
`LAVARND_DEMOS_BYTES_PER_SEC` (default 8192 bytes a second, with bursts up
to twice that). Past it, `/api/bytes` answers **429** with `Retry-After`
and the pages wait, so the demos never take more than that from `lavapool`
(and from anything else using it, such as `trickle`), however many people
are watching.

**Tally statistics** keeps a pool of 4,000,000 bytes. Every 864 seconds it
replaces the oldest 40,000 bytes with new data, so the whole pool turns
over about once a day, and records a frame: how often each 4-bit and 8-bit
value occurs in the pool and how many of its bits are 1. These are the
original demo's numbers. The page shows the last 100 frames (24 hours) as
a movie, like the original's animated GIFs, magnified around the expected
value with ±1σ and ±2σ bands, or unmagnified (`/tally#unmagnified`),
where it's almost completely flat. Its data is at `/api/frames`. All
viewers share the one pool, so `lavapool` sees 4,000,000 bytes at start
(about 4½ minutes at a QuickCam's ~15 KB/s) and then about 46 bytes a
second. The pool lives in memory, so a restart refills it and frame
numbers start again at 0.

A day is a long time to wait on a web page, so the systemd unit sets
`LAVARND_TALLY_INTERVAL=10`: the movie then fills in about 17 minutes,
which is one full turnover of the pool, for about 4 KB a second from
`lavapool`. Set it back to 864 for the original timing.

## Settings

| Variable                      | Default   | Meaning                                    |
|-------------------------------|-----------|--------------------------------------------|
| `LAVARND_DEMOS_BYTES_PER_SEC` | `8192`    | `/api/bytes` budget shared by all viewers  |
| `LAVARND_DEMOS_PROXIED`       | unset     | `1` to trust one proxy's `X-Forwarded-*`   |
| `LAVARND_TALLY_INTERVAL`      | `864`     | seconds between tally frames               |
| `LAVARND_TALLY_POOL`          | `4000000` | tally pool size in bytes                   |
| `LAVARND_TALLY_CHUNK`         | `40000`   | bytes replaced per tally frame             |

The tally pool size must be a multiple of the chunk size.

## Try it

With `lavapool` running on this machine:

    pip install flask                       # or: apt install python3-flask
    cd examples/demos
    PYTHONPATH=../../python/src LAVARND_TALLY_INTERVAL=5 flask --app app run

and open <http://localhost:5000/>. Tests (no `lavapool` needed):

    PYTHONPATH=../../python/src python3 -m unittest -v test_app

## Run it as a service

After `make install` (which installs the `lavarnd` package under
`/usr/local/lib/lavarnd/python`), as root, from the top of the source tree:

    apt install python3-flask python3-gunicorn
    install -d /opt/lavarnd-demos/templates /opt/lavarnd-demos/static
    install -m 0644 examples/demos/*.py /opt/lavarnd-demos/
    install -m 0644 examples/demos/templates/* /opt/lavarnd-demos/templates/
    install -m 0644 examples/demos/static/* /opt/lavarnd-demos/static/
    rm -f /opt/lavarnd-demos/test_app.py
    install -m 0644 examples/demos/lavarnd-demos.service /etc/systemd/system/
    systemctl daemon-reload
    systemctl enable --now lavarnd-demos.service

It runs gunicorn with **one** worker (and 4 threads) as an unprivileged
dynamic user, listening on `127.0.0.1:8098` only. Keep it to one worker:
each process would keep its own tally pool and byte budget.

Put a reverse proxy in front of it. For Apache on a path prefix (needs
`mod_proxy`, `mod_proxy_http` and `mod_headers`):

    RedirectMatch 301 ^/demos$ /demos/
    <Location /demos/>
        ProxyPass        http://127.0.0.1:8098/
        ProxyPassReverse http://127.0.0.1:8098/
        RequestHeader set X-Forwarded-Prefix "/demos"
        RequestHeader set X-Forwarded-Proto "https"
    </Location>

The unit sets `LAVARND_DEMOS_PROXIED=1`, so the app trusts these
`X-Forwarded-*` headers and puts the prefix in its links. That's only safe
because nothing but the proxy can reach port 8098.
