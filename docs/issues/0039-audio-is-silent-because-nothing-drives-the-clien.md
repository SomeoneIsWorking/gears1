---
id: 39
title: Audio is silent because nothing drives the client callback, so the title never submits a frame
status: open
symptom: no sound; xaudio_null reports a client registered but the submitted-frame counter never advances -- zero frames across entire runs
tags: audio,xaudio,kernel,blocker
created: 2026-07-28
updated: 2026-07-28
---

MEASURED across two full runs (live-cclear, live-depthres): the title registers
an audio client and submits ZERO frames. The "N frames submitted" line never
appears once. Audio is not silent because we discard sound -- the title never
produces any.

The chain, and where it breaks:

  1. XAudioRegisterRenderDriverClient hands us a guest CALLBACK and its context.
     We record both (g_callback, g_callbackContext) and never call them. The stub
     says so: "callback {:#x} will never fire".
  2. On hardware that callback is invoked from an audio thread to ask for the
     next buffer. The title is waiting to be asked.
  3. Because it is never asked, XAudioSubmitRenderDriverFrame is never called.

So THE BLOCKER IS STEP 1, and everything downstream is untestable until it moves.
This is worth stating because "implement audio output" sounds like the task and
is not: there is nothing to output yet.

A SECOND defect sits behind it, harmless only because step 1 blocks first:
XAudioSubmitRenderDriverFrame ignores r4. Xenia's contract is
XAudioSubmitRenderDriverFrame(driver_ptr, samples_ptr) with samples as a float*
(xboxkrnl_audio.cc). We count the call and discard the pointer, so even once the
callback fires we would be dropping the samples.

THE FRAME FORMAT, from Xenia's AudioDriver: 48000 Hz, 6 channels, 256 samples per
channel, float32 -- 6144 bytes per frame, 187.5 frames per second.

WHAT STEP 1 NEEDS: a host thread that can call a guest function, which this
runtime already does once -- vd_null_gpu.cpp dispatches the graphics ISR by
setting up a PPCContext (r13 = KPCR, r1 = stack, r3/r4 = args) and calling
PPC_LOOKUP_FUNC(base, addr)(ctx, base). The audio pump wants the same thing at
187.5 Hz with r3 = the client's callback context.

STAGED SO EACH STEP IS VERIFIABLE:
  (a) Drive the callback. Measurable: the submitted-frame counter starts
      advancing, and the sample pointer is non-null.
  (b) Capture a frame's samples and check they are plausible PCM (not silence,
      not garbage) -- dump to a .wav and listen, the same way the texture decode
      was verified by turning a blob into a PNG.
  (c) Only then wire an output device.

Do not skip to (c). A silent output device and a working one are
indistinguishable until (b) says there is signal to play.

### Note (2026-07-28)
STEP (a) IS DONE: the title now submits audio frames.

The chain that had to be built for it, in order: drive the registered callback
(the pump), implement KeWaitForMultipleObjects, and take the guest thread's
PROCESSOR NUMBER from the title instead of inventing one -- the last of these
was the actual blocker and is written up in catalog #40.

Measured over 60 s: 11250 callback invocations, 11250 frames submitted by the
title, samples at 0x40165380, none submitted without a buffer. Rendering is
unaffected at 29.96 fps.

STEP (b) IS NOW THE OPEN QUESTION AND IT IS NOT A FORMALITY. The submitted
buffers have not been looked at. XMACreateContext still hands out contexts with
no decoder behind them, so the plausible outcome is 11250 buffers of silence
that look exactly like 11250 buffers of music from the driver's side. Dump the
PCM to scratch/wav/ and look at it before wiring any output device: an output
device fed silence and an output device fed nothing sound identical, and only
one of them is a bug you can find afterwards.

### Note (2026-07-28)
STEP (b) IS DONE, AND THE ANSWER IS: THE TITLE IS PRODUCING REAL AUDIO.

Two instruments, both in xaudio_null.cpp:

- every submitted frame's peak is measured and the count of digitally silent
  frames is reported, unconditionally. This is the check that distinguishes
  "11250 frames of music" from "11250 frames of nothing", which look identical
  from the driver's side.
- GEARS_AUDIO_WAV=<path> writes the frames verbatim -- 6 channels, 48 kHz,
  32-bit float, no downmix and no conversion, because a transform here would be
  a second thing that can be wrong. The header is refreshed once a second, since
  runs end by SIGKILL far more often than they end cleanly and a dump that is
  only readable on a graceful exit is unreadable when it matters.

Measured on a run to gameplay (11000 frames submitted, none without a buffer):

    2763 frames are non-silent, 8237 are digital zero
    peak 0.518, mean peak of the non-silent frames 0.136
    ffmpeg volumedetect over the dump: mean -27.4 dB, max -5.7 dB

The non-silent frames are not scattered -- they are TWO contiguous runs,
4.4 s to 10.4 s and 10.5 s to 19.3 s. So the title produces genuine sound
through boot and the intro, and then goes silent from about 19 s onward, which
is around when it settles on the title screen.

That shape is the next question and it is a good one: audio is not broken in
general, it stops. The obvious suspect remains XMA -- contexts are still handed
out with no decoder, so anything streamed rather than mixed from resident PCM
would fall silent exactly like this once the intro's own samples are exhausted.
Suspect, not conclusion; nothing has yet tied the two runs to a source.

STEP (c), an output device, is now unblocked and is deliberately still last: it
would add nothing this measurement has not already established, and a device fed
silence sounds exactly like a device fed nothing.

### Note (2026-07-28)
THE SILENCE AFTER 19 s IS XMA, AND IT IS NOW MEASURED RATHER THAN SUSPECTED.

The previous note named XMA as the suspect and refused to call it the cause.
Dumping the live contexts settles it.

The title creates a handful of XMA contexts and PROGRAMS THEM FULLY. Decoding
one against Xenia's XMA_CONTEXT_DATA (extern/xenia/src/xenia/apu/xma_context.h):

    input_buffer_0_valid  = 1        input_buffer_0_ptr = 0xaba10000
    input_buffer_0_packet_count = 55 (55 x 2 KB of XMA queued)
    output_buffer_valid   = 1        output_buffer_ptr  = 0xaa012880
    output_buffer_block_count = 30   work_buffer_ptr    = 0xaa014680
    is_stereo = 1   subframe_decode_count = 8
    error_set = 0   parser_error_set = 0
    input_buffer_read_offset = 32 bits   output_buffer_write_offset = 0

A second context is armed with 1780 packets and BOTH input buffers valid, which
is the streaming double-buffer pattern -- music.

Sampled twice, 12 s apart, both contexts byte-for-byte IDENTICAL. Nothing
consumes the input and nothing produces output: the read offset never leaves the
start of the stream and the write offset never leaves zero. No error bit is set,
because from the title's side nothing has gone wrong -- it queued data for the
hardware decoder and is waiting, exactly as it would on a console where the
decode is late.

So the shape of the earlier measurement is explained. The sound from 4.4 s to
19.3 s is the voices the title mixes in software; everything streamed is XMA,
and it has been silently pending since boot.

RECORDED ON THE FRONTIER as `xma-decode`, status HACK rather than missing:
XMACreateContext handing out a context record LOOKS like support, and the title
behaves as though decode were coming. The real step is an XMA bitstream parser
plus a WMA-Pro-class decoder over the packet stream, writing decoded blocks to
output_buffer_ptr and advancing the offsets the title polls.

DO NOT fake this by writing zeros or noise into the output buffer to make the
offsets move. It would advance the title past its wait and produce audible
garbage that looks like progress.

### Note (2026-07-28)
THE XMA INTERFACE IS NOW REACHABLE. THE DECODER IS STILL ABSENT.

The title imports only XMACreateContext and XMAReleaseContext -- confirmed
against the whole recompiled image, which declares no other XMA import. So
everything else is its own statically linked library driving the hardware
registers at 0x7FEA0000 directly, and the register block is not decoration
around a decoder: it IS the interface.

The contract that was missing: register 0x600 publishes the address of the
CONTEXT ARRAY, and the title reads it with lwbrx at sub_825E8FE0 to learn where
contexts live. Contexts are then named by their INDEX in that array, and the
kick/lock/clear registers are bitmaps over those indices. The runtime published
nothing, so the title computed every index against a base of zero, and
XMACreateContext handed out unrelated heap blocks besides.

The damage was visible in the device window and is the reason this was worth
chasing: dumping 0x7FC00000..0x80000000 showed the title's register writes
landing at 0x7FEA3460, 0x7FEA3478, 0x7FEA3560 and similar -- addresses that
correspond to no register in Xenia's table, because they were computed from
garbage. Every kick the title made was being written into inert memory nobody
would ever read.

FIXED (runtime/xma.cpp): a 320 x 64-byte context array is allocated at startup
and published in register 0x600, NextContextIndex is seeded to 1, and
XMACreateContext allocates slots from that array instead of arbitrary blocks.

VERIFIED by re-dumping the same window. Every non-zero word is now a named
register and nothing else is written:

    0x7fea1800  0x600 ContextArrayAddress  0xa0000000
    0x7fea181c  0x607 NextContextIndex     0x00000001
    0x7fea1940  0x650 Context0Kick         0x00000002
    0x7fea1a40  0x690 Context0Lock         0x00000002
    0x7fea1a80  0x6a0 Context0Clear        0x00000002

and the runtime observes contexts 0 through 7 kicked in order over a run to
gameplay -- one per context the title creates. Rendering is unaffected at
29.63 fps and both test suites pass.

AUDIO IS UNCHANGED, exactly as expected: 2763 non-silent frames, same as before.
Nothing decodes. This commit makes the decoder REACHABLE, which it was not.

ONE HONEST LIMITATION, recorded in the code as well: the kick watcher POLLS, and
a kick is a register write the title overwrites with a fresh bitmap each time.
Polling can therefore report which contexts were ever seen set, not every kick.
That is enough to answer "is the title asking", and not enough to drive a
decoder. The real mechanism is to trap the write -- the recompiler already
routes device stores through PPC_MM_STORE_U32, which is #ifndef-guarded
precisely so a runtime can define it. Decoder work starts there, not with
ffmpeg.

### Note (2026-07-28)
THE KICK POLLER WAS WRONG BY THREE ORDERS OF MAGNITUDE, AND NOW IT IS A WRITE HOOK.

The previous note said polling the kick register was "an instrument, not a
design" and that driving a decoder would need the write trapped. That was the
right call and the size of the error is worth recording, because it is the
difference between a plausible instrument and a correct one.

The recompiler emits PPC_MM_STORE_* instead of a plain store wherever it decided
a store is memory-mapped I/O, and ppc_context.h leaves those macros
#ifndef-guarded so a runtime can take them over. Taking over PPC_MM_STORE_U32
(runtime/ppc_mmio.h, force-included into gears_ppc ahead of ppc_context.h)
delivers every device write as it happens.

The cost is nothing. The ENTIRE recompiled image contains eleven
PPC_MM_STORE_U32 sites and no PPC_MM_LOAD sites at all -- five of them are the
XMA library's register writes in ppc_recomp.95.cpp, the rest are the GPU's.
Reads need no hook because the registers live in real guest memory and an
ordinary load already sees them.

MEASURED, same 50 s headless run:

    polling every 2 ms  ->    8 "distinct contexts kicked"
    the write hook      -> 8000+ context kicks

The title kicks context 0 in a tight kick/lock/kick/lock cycle. Sampling saw one
event per context per run; the truth is thousands. A decoder driven off the
poller would have decoded a handful of blocks, produced almost nothing, and
looked like a decoder bug rather than a delivery bug -- which is exactly the
kind of silent-wrong plumbing this project keeps paying for.

Rendering is unaffected at 29.88 fps, audio is unchanged at 2763 non-silent
frames, both suites pass. Still nothing decodes; what is now true is that the
decoder, when it exists, will be asked at the right moments.

ALSO FIXED, same lesson as the WAV header: the kick total was originally
reported from atexit, which never runs because these runs end by SIGKILL. It is
reported as it accumulates instead. An exit summary in a process that is always
killed is a summary nobody reads.

### Note (2026-07-28)
THE PREMISE IS CONFIRMED: THE QUEUED BITSTREAM IS REAL XMA2, AND IT DECODES.

This was done OFFLINE, deliberately, before writing any decoder in the runtime.
An in-runtime decoder that produced silence could be failing at the bitstream,
at the context bookkeeping, at the output ring, or at the premise that this is
XMA at all -- and from the audio pump those are indistinguishable. Decoding the
dump with a known-good tool separates the premise from the plumbing.

GEARS_XMA_DUMP=<dir> writes, on the first kick of each context, its 64 bytes and
the raw packets its input buffer points at. tools/xma_wrap.py wraps those
packets in a WAVEFORMATEX of tag 0x0166 carrying a 34-byte XMA2WAVEFORMATEX.

RESULT, on the title's own streams:

    ctx1: 1780 packets -> 2 min 21.8 s, 44100 Hz stereo, mean -22.2 dB, peak -2.9 dB
    ctx0:   55 packets -> 1.17 s,       24000 Hz stereo, mean -15.4 dB, peak -1.1 dB

No decoder errors on either. This is the music the title has been queueing since
boot and never getting back.

ONE THING I HAD WRONG, and it would have cost days: the plan was to "convert the
XMA bitstream into something libavcodec's WMA Pro decoder accepts". No shipping
implementation does that. XMA framing (15-bit length-prefixed frame chains in
2048-byte packets) is not WMA Pro framing (superframes, ASF extradata). What
exists is upstream ffmpeg's packet-level xma1/xma2 decoders -- which is what
this milestone uses -- and Xenia's own patched fork, which adds a per-FRAME
codec (AV_CODEC_ID_XMAFRAMES) that upstream does not have. Confirmed: the system
libavcodec 62 headers contain no XMAFRAMES, and `ffmpeg -decoders` lists xma1,
xma2 and wmapro only.

THAT DISTINCTION DECIDES THE RUNTIME ARCHITECTURE, so it is recorded here rather
than left as trivia. Upstream's xma2 decoder consumes whole 2048-byte packets
and withholds up to 4096 samples until EOF, which is fine for decoding a file
and hostile to emulating a hardware context, where the title paces consumption
per 128-sample subframe, polls input_buffer_read_offset in BITS, and can
loop-jump mid-stream. Matching those would mean inventing a latency/trim
compensation layer that exists in no implementation, exactly where verification
is hardest. The per-frame codec has no FIFO, no priming trim, and is
re-seedable at any frame boundary.

A HEADER DETAIL WORTH KEEPING: the decoder takes its channel count from
XMA2WAVEFORMATEX's ChannelMask, NOT from the WAVEFORMATEX. Filling in only the
latter gives "0 channels" and a refusal to open -- which is how the layout got
pinned down rather than guessed.

This also produces the GOLDEN REFERENCE the runtime decoder must later match:
the runtime's PCM for the same context and stream offset has to agree with these
files, modulo the priming samples ffmpeg trims and a hardware context does not.

### Note (2026-07-28)
THE DECODER IS LINKED AND OPENS. THE CONTEXT PROTOCOL IS WHAT IS LEFT.

Following the architecture decision recorded above -- upstream's packet-level
xma2 is the wrong shape for a hardware context -- Xenia's FFmpeg fork is now
vendored at extern/ffmpeg-xmaframes and built by the build itself
(cmake/ffmpeg_xma.cmake). --disable-everything with two decoders enabled gives
libavcodec.a + libavutil.a totalling under 2 MB; x86 assembly is off because it
needs nasm and buys nothing for audio.

VERIFIED, and it is a real cross-check rather than a smoke test: on the first
kick of each context the runtime opens AV_CODEC_ID_XMAFRAMES using the sample
rate and channel count read out of the GUEST's context, and reports

    [xma] decoder open: stereo at 24000 Hz
    [xma] decoder open: stereo at 44100 Hz

which is exactly what the offline dumps decoded to. Two independent paths --
the runtime's own field decoding and ffmpeg's reading of the wrapped file --
agree on both streams. Rendering unaffected at 29.86 fps, both suites pass.

The failure mode this closes is worth naming: "the wrong libavcodec was linked"
and "the bitstream is bad" produce identical silence downstream, so the codec
lookup is checked at the point where the answer is cheap, and says which one it
is.

TWO BUILD TRAPS, recorded because both cost a cycle and both were silent:
- include() shares the caller's scope, so `set(... PARENT_SCOPE)` in an included
  .cmake writes PAST the top-level list file. The feature flag was set and never
  arrived, and the runtime reported "this build has no XMA decoder" while the
  libraries sat built on disk.
- FFmpeg refuses an out-of-tree build if a config.h exists in the source tree.
  An earlier in-tree configure poisons every later CMake build until the
  submodule is cleaned.

STILL NOT DECODING. What remains is the context protocol: reassembling frames
out of the 2 KB packet stream (frames span packets, and a frame's own 15-bit
length header can straddle a packet boundary -- undetectable for XMA1),
pacing output per 128-sample subframe into the 256-byte block ring, byte-
swapping the PCM to big-endian, and advancing input_buffer_read_offset in BITS
and output_buffer_write_offset in blocks. Xenia's xma_context_new.cc is the
reference; its rest value for the read offset is 32 bits, never 0.

### Note (2026-07-28)
XMA DECODES. The title's audio now runs continuously instead of dying at 19 s.

runtime/xma_context.cpp is a port of Xenia's current decoder
(extern/xenia/src/xenia/apu/xma_context_new.cc): frame-chain walking, frames
spanning packets, 0xFF skip packets, subframe pacing into the 256-byte block
ring, big-endian int16 output, StoreContextMerged, and the Clear register.
Decode is synchronous on the kick, which is what this Xenia does too, and it
means the title's kick/lock cycle needs no cross-thread choreography.

VERIFIED BY THE OPERATOR, from a clean build, not taken on report:

    ctx1 (1780 packets): correlation 1.000000 over the full 141.84 s
                         rms diff 0.000023, peak 0.000061, constant 576-sample lag
    ctx0 (55 packets):   correlation 1.000000 over 8.34 s

The 576-sample lag is the encoder priming ffmpeg's file decoder trims and the
hardware does not. The residual rms is 1-2 LSB of int16 -- float rounding
between the two pipelines, no structural difference anywhere in the stream.

THE COMPARATOR WAS VALIDATED BEFORE ITS VERDICT WAS BELIEVED: fed the golden
against a reversed copy of itself it reports correlation 0.081 and exits 1;
fed the real match it reports 1.000000 and exits 0. A comparator that cannot
say "different" is not evidence.

LIVE, on a run to gameplay: 81.8% of submitted frames are non-silent, against
25% before, and the audio now spans 4.4 s to 60.7 s and resumes at 69.6 s after
the level load rather than stopping dead at 19.3 s. Zero xma warnings or errors.

TWO FINDINGS FROM THE PORT, both root causes rather than symptoms:

1. GEARS SHIPS XMA1-TYPE PACKETS, and the reference decoder silently loses
   frames on them. Packet metadata is 0, so the byte the reference reads as a
   frame count is an XMA1 sequence number. When a packet's last frame ends
   0 < n < 15 bits before the boundary, its 15-bit header straddles the edge,
   and the reference's only detection is the XMA2 frame count -- its own
   comment (xma_context_new.cc:487-489) admits those frames "will be silently
   lost". Four frames vanished in the 142 s music stream. The fix detects the
   split from the CONTINUATION BIT, which both variants carry, and only when
   more than zero bits remain: a frame ending exactly on the boundary means the
   next frame starts cleanly in the next packet, and counting it sends the
   next-offset arithmetic wrapping back to the packet's own start, decoding it
   forever.

2. AV_CODEC_FLAG2_SKIP_MANUAL was missing. Without it libavcodec discards
   priming samples through skip side data -- the first frame returns 384
   samples instead of 512, and mid-stream discards happen too. That plus the
   lost frames held correlation at 0.678.

PERFORMANCE, measured rather than assumed: decode costs 1.59 s across 71000
kicks over a ~140 s run (22 us mean, 2.4 ms worst). Frame rate in gameplay is
15.9 fps WITH decode and 16.6 fps in a control run with no audio pump at all,
so the drop is gameplay being CPU-bound and not this code. The audio pump does
fall behind under that load -- 16875 invocations where 187.5 Hz wants 26250 --
but removing 1.59 s from 140 s cannot account for it, so the pump's rate under
a CPU-bound guest is a separate open question, not an XMA regression.

WHAT IS NOT VERIFIED, and the code says so where it matters: loop playback and
true double-buffer streaming are ported from the reference and exercised by no
stream here. Mono DOES decode live (contexts 23-25, 24 kHz and 44.1 kHz) but has
not been compared against a golden. interrupt_when_done and stop_when_done are
not implemented, as in every Xenia variant, and warn once if a context asks.
NOBODY HAS HEARD THIS ON SPEAKERS: there is still no output device, which is
step (c) and now the only thing between this and audible sound.
