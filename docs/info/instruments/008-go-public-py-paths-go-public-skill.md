---
id: I008
kind: instrument
status: trusted
created: 2026-08-05
---

## Instrument

go_public.py paths (go-public skill)

## Validated by

Scans Git history for machine-specific paths. It initially missed an absolute
host-home leak embedded in tracked Python bytecode while classifying guest
drive-letter paths and generic home-relative examples as blocking. Binary
scanning was validated with a synthetic bytecode file that must report an
absolute host path and a relative-path control that must remain clean. The
guest-path false positives remain and require provenance review rather than
automatic replacement.

## Known failure modes

The `rules` subcommand currently raises a `NameError` before producing its
replacement file, and the path classifier cannot distinguish a guest filesystem
path from a host Windows path. Only the Unix host-path and binary printable-run
checks are trusted until those controls are repaired.
