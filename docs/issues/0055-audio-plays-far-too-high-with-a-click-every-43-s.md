---
id: 55
title: Audio plays far too high with a click every 43 samples: the render-driver frame is planar, not interleaved
status: resolved
symptom: Audio is badly pitched and crackling/buzzing everywhere, including the title screen where the pump keeps up perfectly
tags: audio,xaudio,layout,port,blocker
created: 2026-08-04
updated: 2026-08-04
---

## Symptom

Wrong pitch and constant crackling, on every screen -- reported first at the
title screen, which matters because the audio pump is HEALTHY there (0 late
slots, backlog 0, 2.3 ms of work per 5.33 ms slot). So this was never the pump
falling behind (#43); the samples themselves were wrong.

## Evidence that named it

Measured on the mix the title submits (`GEARS_AUDIO_WAV`), last 25 s of the title
screen:

- 3.1% of samples were discontinuities (|step| > 0.15),
- and they clustered at offsets **0, 43, 86, 128, 171** within each 256-sample
  frame -- multiples of 256/6 = 42.67,
- 7.1% of the spectrum's energy sat above 20 kHz, which no music has,
- and channels 1..5 correlated 0.99, 0.96, 0.93, 0.90, 0.89 with channel 0 --
  decreasing with channel distance, which is what one signal read at six
  different one-sample offsets looks like, not a 5.1 mix.

The XMA decode was ruled out first, with the project's own instrument: our decode
of the title screen's stream against a golden ffmpeg decode of the same packets
is correlation **1.000000**, difference rms 0.000022. The bitstream side is fine.

## Cause

`XAudioSubmitRenderDriverFrame` hands over one frame as SIX PLANES of 256
big-endian floats -- all of channel 0, then all of channel 1, and so on.
`runtime/xaudio_null.cpp` read it as INTERLEAVED and handed it to SDL that way.

Xenia has the layout in `extern/xenia/src/xenia/apu/conversion.h`, whose
converter indexes `input[channel * ch_sample_count + sample]` and is named
`sequential_6_BE_to_interleaved_6_LE`. We had the byte swap and not the
transpose.

Read planar-as-interleaved, each output channel walks one plane at six times the
rate and falls into the next plane every 256/6 samples: the audio comes out far
too high with a click at every plane crossing. That is the pitch and the
crackling, and it explains the 42.67-sample periodicity exactly.

The `GEARS_AUDIO_WAV` dump had the same bug and so could not reveal it: it wrote
the guest's planes byte for byte into a container that is BY DEFINITION
interleaved, and called that "verbatim". A dump that lies about its own format
looks like evidence.

## Fix

`runtime/audio_frame.h` -- a header-only pure function doing the transpose plus
the byte swap, used by both the device path and the WAV writer.

Written test-first (`tests/test_audio_frame.cpp`): a frame whose plane for
channel c holds nothing but the value c, which the two readings cannot both
satisfy. Against the old reading it fails 4 checks; against the new one it
passes.

## Verified on the same screen, before vs after

| | before | after |
|---|---|---|
| discontinuities (\|step\| > 0.15) | 3.138% | 0.361% |
| energy above 20 kHz | 7.09% | 0.08% |
| busiest offset in the 256-sample frame | 128 (1575 hits), 86, 43, 0, 171 | 39 (30 hits), uniform |
| ch0 vs ch1..5 correlation | 0.99 0.96 0.93 0.90 0.89 | 0.74 0.03 0.15 -0.01 -0.00 |

The 256/6 comb is gone, the ultrasonic energy is gone, and the surround channels
are independent of the front-left one, which is what a real 5.1 mix looks like.
