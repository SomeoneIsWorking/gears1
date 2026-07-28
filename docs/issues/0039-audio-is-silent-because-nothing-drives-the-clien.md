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
