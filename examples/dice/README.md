# LavaRnd Dice

A small Flask app that rolls dice with LavaRnd. Every roll comes from a
running `lavapool` through the [`lavarnd`](../../python/) Python library.

- `/`: buttons for d4 to d100, and a box for dice notation such as `3d6+2`
- `/api/roll?dice=3d6+2`: the same as JSON

```
$ curl 'http://localhost:8099/api/roll?dice=3d6+2'
{"count":3,"dice":"3d6+2","modifier":2,"rolls":[4,1,6],"sides":6,"source":"lavarnd","total":13}
```

Notation is `NdS`, `dS`, `NdS+M` or `NdS-M`, with 1 to 100 dice of 2 to 1000
sides and a modifier up to ±1000. A literal `+` in the API URL is a plus,
not a space.

Rolls use `LavaRandom.randint`, which draws random bits and rejects
out-of-range values, so every face is equally likely. If `lavapool` can't be
reached, the app answers **503** rather than quietly using another source of
randomness.

## Try it

With `lavapool` running on this machine:

    pip install flask                       # or: apt install python3-flask
    cd examples/dice
    PYTHONPATH=../../python/src flask --app app run

Tests (no `lavapool` needed):

    PYTHONPATH=../../python/src python3 -m unittest -v test_app

## Run it as a service

After `make install` (which installs the `lavarnd` package under
`/usr/local/lib/lavarnd/python`), as root:

    apt install python3-flask python3-gunicorn
    install -d /opt/lavarnd-dice
    install -m 0644 examples/dice/app.py /opt/lavarnd-dice/
    install -m 0644 examples/dice/lavarnd-dice.service /etc/systemd/system/
    systemctl daemon-reload
    systemctl enable --now lavarnd-dice.service

It runs gunicorn with 2 workers as an unprivileged dynamic user, listening
on `127.0.0.1:8099` only. Put a reverse proxy in front of it. For Apache
on a path prefix (needs `mod_proxy`, `mod_proxy_http` and `mod_headers`):

    RedirectMatch 301 ^/dice$ /dice/
    <Location /dice/>
        ProxyPass        http://127.0.0.1:8099/
        ProxyPassReverse http://127.0.0.1:8099/
        RequestHeader set X-Forwarded-Prefix "/dice"
        RequestHeader set X-Forwarded-Proto "https"
    </Location>

The unit sets `LAVARND_DICE_PROXIED=1`, so the app trusts these
`X-Forwarded-*` headers and puts the prefix in its links. That's only safe
because nothing but the proxy can reach port 8099.

Every roll draws from `lavapool`, which refills at the camera's rate
(roughly 15 KB/s for a QuickCam). Heavy public use could drain the pool and
leave `trickle` short, so rate-limit at the proxy if that matters.
