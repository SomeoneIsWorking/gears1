# Interactive control of a run

An offscreen product run, or the player's windowed product, can serve a loopback-only HTTP control channel, so a
maintainer or agent can press buttons, read status and look at the guest's
output while the title plays. It is off unless asked for:

```sh
uv run --locked python tools/run_offscreen.py --walk gameplay --seconds 900 \
    --control-port 32125
```

`./run.sh` always serves the channel on loopback port 32125 (or
`GEARS_CONTROL_PORT` from the environment or `.env`), so the game can be
measured while the player plays it; the launcher passes the port to the
product as `--control-port`, and the channel starts once the title has
launched. In the window,
`POST /api/stop` is refused (409): the game ends when its window closes.
`runtime/product/control_channel.*`
owns the routes, and Lucent owns the listener, bounded parsing, concurrent
dispatch and shutdown.

## A walk first, then the channel

A scripted walk owns the pad until its last step has fired; until then every
input write is refused with HTTP 409, so a measurement cannot be disturbed.
Once the walk has finished, the channel may take the pad and continue from
where the walk left the game. `--walk none` hands the pad over from the start.

## The client

`tools/product_control.py` drives the routes:

```sh
C="uv run --locked python tools/product_control.py --port 32125"
$C status                              # presents and the pad's source and state
$C perf                                # fps and frame times since the last perf call
$C perf --every 1                      # one live line per second until interrupted
$C pad --ly 32767 --hold 2             # walk forward for 2 s, then release
$C pad --buttons A,START               # replace the whole pad, held until changed
$C pad --lt 255 --hold 0.3             # pull the left trigger
$C release                             # neutral pad, still connected
$C frame scratch/control/now.png       # the latest guest output
$C player                              # the local player's position, yaw and weapon
$C memory 82BED138 64                  # guest words and big-endian floats
```

Every pad write replaces the whole pad; omitted fields are neutral. Buttons use
the names scripted input uses: `UP`, `DOWN`, `LEFT`, `RIGHT`, `START`, `BACK`,
`LTHUMB`, `RTHUMB`, `LB`, `RB`, `A`, `B`, `X`, `Y`. Sticks `lx`, `ly`, `rx`,
`ry` take `-32767..32767` and triggers `lt`, `rt` take `0..255`; anything else
is refused with HTTP 400 naming the field.

## Routes

| Route | Result |
|---|---|
| `GET /api/status` | JSON: presents so far, and the pad's source, packet and state |
| `GET /api/perf` | JSON: presents per second, frame-time p50/p95/p99/max (0.1 ms buckets; `at_least` in the open-ended ≥100 ms bucket), and translation and override counts, over the interval since the previous `/api/perf` (since launch on the first call) |
| `POST /api/input` | form fields as above; 409 while a walk owns the pad |
| `POST /api/input/release` | a neutral pad, still connected |
| `DELETE /api/input` | disconnect the remote controller |
| `GET /api/frame.ppm` | the latest guest output as binary PPM; 503 before the first present |
| `GET /api/memory?address=HEX&length=N` | `N` (1..4096) bytes of guest virtual memory; 422 when any of the range is unmapped |
| `GET /api/player` | JSON: control and camera yaw, the world's game time in seconds, and the pawn's location and weapon (`null` while dead); 409 before gameplay |
