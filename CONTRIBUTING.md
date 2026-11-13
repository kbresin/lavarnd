# Working on this fork

## `main` is for finished work

Everything on `main` should be something we'd be happy to release:

- It builds with `make all` (which uses `-Wall -Werror`) and the Python tests
  pass: `cd python && PYTHONPATH=src python3 -m unittest discover -s tests`.
- Changes that touch the camera, `lavapool` or the install have been run on
  real hardware, and the result is recorded in [`PORTING.md`](PORTING.md).
- Docs (`README.md`, `PORTING.md`, `doc/CHANGES` at release time) match the
  code.

Don't push experiments, half-done work or "fix the last commit" commits to
`main`.

## Develop on a branch

Start each piece of work on its own branch off `main`:

    git switch -c fix/camera-unplug main

Name branches `fix/…`, `feature/…`, `docs/…` or `chore/…`. Commit as often as
you like there; push the branch to GitHub if you want a backup or a pull
request.

When it's done and tested, tidy it into small commits that each do one thing,
then bring it into `main` without a merge commit, so history stays linear:

    git switch fix/camera-unplug && git rebase main
    git switch main && git merge --ff-only fix/camera-unplug
    git push && git branch -d fix/camera-unplug

## Versions and releases

Versions continue upstream's numbering (upstream's last release was 0.1.3),
following [Semantic Versioning](https://semver.org/). While we're at 0.x, a
minor bump (0.2 → 0.3) can break things and a patch bump (0.2.0 → 0.2.1) is
for fixes only. 1.0 will mean the C library, the `lavapool` protocol and the
Python API are stable.

The version is in two places, and both change only in a release commit:

- `version` (the C side; upstream's `make release` and man pages read it)
- `__version__` in `python/src/lavarnd/__init__.py` (`pyproject.toml` reads
  it from there)

To release:

1. Add a `LavaRnd version X.Y.Z` entry at the top of `doc/CHANGES`.
2. Bump both version strings and commit.
3. Tag the commit and push the tag:
   `git tag -a vX.Y.Z -m "LavaRnd X.Y.Z" && git push origin vX.Y.Z`
4. Make a GitHub release from the tag, using the CHANGES entry as its notes.

Only make a `release-X.Y` branch if an older release needs a fix after `main`
has moved on to incompatible changes.
