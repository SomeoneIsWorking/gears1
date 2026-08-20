# Interactive debug control

The live port exposes a loopback-only HTTP API on `127.0.0.1:32123`. It is on
for the default `./run.sh` target and for the executable itself. Set
`GEARS_DEBUG_HTTP_PORT=0` to disable it or `./run.sh --http-port N` when several
instances need distinct ports.

This is a runtime control plane, not a second emulator path. Controller writes
replace the same `PadState` snapshot read by `XamInputGetState`, and frame probes
arm the renderer's existing report/readback path for the next accepted frame.
The normal shared-device path therefore keeps pixels on the GPU until a probe is
requested; a probe causes one deliberate hitch rather than a permanent readback
tax.

Lucent owns the reusable loopback listener, bounded HTTP parsing, concurrent
dispatch, response framing, and shutdown. This port owns only the routes and
their controller/renderer semantics, so a probe waiting for a frame does not
block a second connection from delivering input or reading status.

## Drive the controller

Every `POST /api/input` replaces the whole pad atomically. Omitted values are
neutral. Button names are the same names accepted by `GEARS_INPUT_SCRIPT`:
`UP`, `DOWN`, `LEFT`, `RIGHT`, `START`, `BACK`, `LTHUMB`, `RTHUMB`, `LB`, `RB`,
`A`, `B`, `X`, and `Y`.

```sh
# Hold A and push the left stick forward.
curl -sS -X POST http://127.0.0.1:32123/api/input \
  -d 'buttons=A&ly=32767'

# Neutral state, still connected as the remote controller.
curl -sS -X POST http://127.0.0.1:32123/api/input/release

# Disconnect remote input and hand the pad back to SDL.
curl -sS -X DELETE http://127.0.0.1:32123/api/input
```

Stick fields `lx`, `ly`, `rx`, and `ry` accept `-32767..32767`; triggers `lt`
and `rt` accept `0..255`. A startup `GEARS_INPUT_SCRIPT` deliberately rejects
remote writes with HTTP 409: mixing an interactive source into a scripted
measurement would make that measurement irreproducible.

## Probe graphics

```sh
curl -sS http://127.0.0.1:32123/api/status | jq
curl -sS http://127.0.0.1:32123/api/frame.ppm \
  -o scratch/screenshots/interactive.ppm
```

`/api/status` reports the guest's own present counter, exact controller packet,
render-thread submitted/dropped/rendered counters, and metadata for the latest
probe: guest frame, draw and shader-pair counts, non-black pixels, mean RGB, and
an FNV-1a identity hash of the exact RGBA bytes.

`/api/frame.ppm` waits up to ten seconds for the next frame the renderer accepts.
It returns that renderer readback as binary PPM. A timeout is HTTP 504; a frame
that could not render or did not yield correctly sized RGBA is HTTP 503. Neither
case is returned as an empty or black image, because absence and black output are
different graphics findings.
