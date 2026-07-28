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
