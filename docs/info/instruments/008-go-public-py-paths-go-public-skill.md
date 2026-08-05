---
id: I008
kind: instrument
status: trusted
created: 2026-08-05
---

## Instrument

go_public.py paths (go-public skill)

## Validated by

Scans git HISTORY for machine-specific paths. WAS WRONG IN BOTH DIRECTIONS when first run on gears1 (2026-08-05): it MISSED a real /home/<user> leak in a tracked tools/__pycache__/*.pyc that had been committed for 340 commits -- a .pyc records the absolute path of the source it was compiled from, and it was skipped twice over (not in TEXT_EXTS, and NUL bytes in the first 8 KiB) -- while reporting 55 BLOCKING findings that were all false positives in one doc: 'D:\WarGame\Checkpoints\chapter37.sav' is the TITLE's own save path and '~/.local/share/gears1/' carries no username. FIXED in the skill: binaries are now scanned by extracting printable runs (>=6 chars) instead of being skipped. Validated against BOTH classes -- a synthetic repo with a home path in a .pyc reports it, one with a relative path in a .pyc reports clean. The false positives remain: treat D:\ and bare ~/ hits in this repo's docs as noise.

## Known failure modes

(none recorded yet)
