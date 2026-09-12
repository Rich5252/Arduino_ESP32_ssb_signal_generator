# Two-tone null-crossing frequency bias — investigation notes

**Status: characterized, NOT fixed. Parked 2026-08-31** — picking up the TF
(transfer function) measurement system and group-delay re-tuning first. See
"Where to resume" at the bottom. **Late addition, same day: user reports the
current `I` (x4 envelope interpolation) algorithm makes two-tone stability
audibly worse, not just subtly worse — see item 5 under "Open, un-actioned
next steps" below. Treat that as higher priority than the null-bias fix
itself when this is picked back up.**

**UPDATE 2026-09-09 — important caveat on `weighted_bias`, discovered while
chasing an unrelated random-frequency-jump symptom (see
`moving_forward_notes.md`):** `weighted_bias` was assumed to be a fast,
stable, per-tone-pair constant (reset with `'r'`, read a second or two
later). Live-watching it continuously (a new firmware change exempts the
`null_bias*` lines from the diagnostics mute, so they stream at 1Hz
regardless of `'v'`) on 700/1700Hz showed instead that after an `'r'` reset
it keeps drifting for TENS OF SECONDS TO MINUTES — not settling quickly —
and can wander by 10-30Hz over that time even with the tone pair, presets,
and hardware completely unchanged (one capture went from -34Hz shortly
after reset, up toward -0.2Hz, back down past -19Hz, with no external event
at all). Loading a different settings preset (which changes
gdeq/ampeq/predistort, and therefore the actual envelope shape through each
null) also visibly shifted where it was heading, which makes physical sense
given the root mechanism below - but the sheer slowness/magnitude of the
drift even with NOTHING changed was not expected or previously
characterized. **This means the "Confirmed measurement table" below,
captured "freshly reset, read ~1-2s later" per its own reproduction recipe,
may not represent settled/steady-state values** - a longer, fixed dwell
time (and ideally logging the full time-series to see whether/when it
actually plateaus, rather than a single point read shortly after reset)
would be needed before trusting those numbers as precise per-tone-pair
constants. It also directly explains the previously-unresolved puzzle in
item 4 under "Open, un-actioned next steps" below (the ~15Hz drift observed
"on the timescale of typing a sentence" that confounded the `I` on/off
comparison) - that's most likely this same slow-convergence behavior, not
noise or a separate mechanism. Real-world takeaway confirmed on the bench
the same session: this metric's wandering does NOT track the actual
transmitted frequency - the SDR showed a stable carrier while
`weighted_bias` swung by tens of Hz, so it is unrelated to (and not usable
as a live detector for) the separate random ~40Hz two-tone frequency-jump
symptom it was being tested against. Not yet root-caused *why* the
convergence is so slow - worth understanding before trusting this
diagnostic for anything time-sensitive again.

**Cross-reference, 2026-09-01:** the new PNP BC327+attn filter's group-delay
equalizer testing (`group_delay_fit_notes.md`, "2026-09-01 refit" Status
section) raised the same null-region-fidelity question from the other end —
that filter's own HF roll-off can't be the whole story for the disappointing
real-hardware IMD result, since `envelope_interp.h`'s Catmull-Rom stage
(item 5 below) independently rounds off the exact same two-tone null fold,
upstream of the analog filter. The two issues may be entangled in any
comp-on/off data taken with `'I'` enabled — worth checking `'I'`'s state
before trusting either investigation's real-hardware numbers in isolation.

## Symptom that started this

Poor perceived audio quality on two-tone tests: pitch wandering ~10Hz on a
slow timescale, plus a faster few-Hz FM noise riding on top. Both worse with
envelope interpolation (`I`) on, present at a lower level with `I` off.
Single-tone always tested spot-on to 1-2Hz.

## Ruled out, in order

1. **Digital scheduling/wakeup jitter.** `[timing] max_gap_us` essentially
   unchanged between `I` off/on across three separate measurement pairs
   (75→80us, `late_ticks_total=0` both ways). Not the mechanism.
2. **Analog AM-to-PM via envelope activity alone.** `AMTEST` (envelope swept
   through a full sine sweep, `freq_dev_hz` pinned to 0, bypasses
   `ssb_dsp_process_sample()` entirely) showed tone stability "close to
   perfect" regardless of `I`. Not the mechanism.
3. **`fast_atan2`/`fast_sqrt` approximation error.** Already ruled out in
   earlier project history (see `ssb_dsp.c`'s `SSB_DSP_FAST_TRIG` comment) —
   a direct A/B against real `atan2f`/`sqrtf` on real hardware showed no
   change to a similar historical two-tone offset.
4. **`max_freq_dev_hz` clamp asymmetry.** `clip_count` reads 0 at every
   tone-pair/spacing tested in this session at the current 8000Hz clamp —
   the clamp never engages, so it cannot be biasing anything right now. This
   *closes* the long-open thread in `config.h`'s `MAX_FREQ_DEV_HZ` comment
   ("whether that clamping was actually the cause... was never confirmed
   either way") for the current configuration.

## Root mechanism identified

At each two-tone destructive-interference null (envelope → 0, i.e. I≈Q≈0 in
`ssb_dsp_process_sample()`), the analytic-signal phase is mathematically
required to jump by exactly ±π — this is genuine signal content, not noise
(see `envelope_floor.cpp`'s own historical postmortem making the same point
about the phase side). The discrete-time `dphi = wrap_pi(phase -
prev_phase)` computation (`ssb_dsp.c`) resolves that jump with a small bias
that is **deterministic and specific to the exact tone pair**, not random —
because these test tones are generated as exact phase-accumulator multiples
of the sample rate, every null in a given test recurs at an *identical*
alignment to the sample grid, so whatever bias one null produces, every null
in that run produces identically. It accumulates coherently instead of
averaging out.

## Two different averages — only one of them is physically real

- **Plain (unweighted) time-average** of `dphi * Fs / 2π` ("`plain_mean`" /
  "`plain_bias`" in the diagnostic): dominated by the tiny fraction of
  samples sitting at a null, where `|dphi|` swings hugely for one sample.
  Measured 95-600Hz "bias" across the sweep below.
  **CONFIRMED WRONG as a predictor of on-air effect** — direct real-hardware
  check (700/1900Hz pair) showed the transmitted tones sitting close to
  their nominal frequencies, not shifted by the ~600Hz this predicted.
- **Envelope²-weighted average** instantaneous frequency ("`weighted_mean`"
  / "`weighted_bias`"): the physically correct quantity. For an analytic
  signal `A(t)e^{jφ(t)}`, the power spectrum's centroid equals the
  energy-weighted (`A(t)²`-weighted) average instantaneous frequency — a
  standard identity, not the plain time-average. The near-null samples that
  dominate the plain average sit exactly where envelope (and so envelope²)
  is smallest, so this weighting suppresses almost all of their
  contribution — which is exactly why the plain average overstated things
  so badly.

## Confirmed measurement table

All six `TWOTONE_BAND_PRESETS` pairs (`test_signals.cpp`), EQ and compressor
off, `null_bias_threshold` fixed at 0.050, freshly reset (`r`) before each
reading:

| pair | spacing (Δf) | plain_bias | weighted_bias | SDR-observed |
|---|---|---|---|---|
| 300/500 | 200Hz | +100.36Hz | **+4.03Hz** | ~3Hz |
| 700/900 | 200Hz | −101.11Hz | **−0.70Hz** | ~2Hz |
| 1500/1700 | 200Hz | −99.60Hz | **+0.11Hz** | <1Hz |
| 2500/2700 | 200Hz | −95.05Hz | **+0.33Hz** | <1Hz |
| 3500/3700 | 200Hz | −99.75Hz | **+0.16Hz** | <1Hz |
| 700/1900 | 1200Hz | −607.69Hz | **−20.72Hz** | ~8Hz |

`weighted_bias` matches the real, SDR-observed deviation to within a few Hz
every time (6/6) — this is the confirmed, working predictor.
`plain_bias` is wrong by 2-3 orders of magnitude and must not be used to
reason about on-air/perceptual effects — kept only so `near_null_contrib`
(see instrumentation below) can localize the mechanism.

## Key finding: NOT simply proportional to tone spacing

The very first data point taken at each spacing (300/500 at 200Hz, 700/1900
at 1200Hz) fit a clean "proportional to Δf" story (≈3Hz and ≈18Hz, ratio 6,
matching the spacing ratio) — but that was coincidental. 700/900 has the
*same* 200Hz spacing as 300/500 yet gives a `weighted_bias` of opposite sign
and ~6x smaller magnitude. **The bias is a function of the specific absolute
tone pair, not spacing (Δf) alone.** Likely contributors: the Hilbert FIR's
amplitude/phase response isn't perfectly flat across the audio band, and/or
the exact sample-grid alignment through each null depends on the specific
frequencies involved, not just their difference.

## Likely real-world significance (reasoned, not yet verified)

Two-tone test tones are perfectly periodic, so every null lands at the
identical relative sample-grid position every cycle — the per-event bias
accumulates coherently rather than averaging out. Real voice content's
envelope nulls occur at essentially unpredictable times relative to the
sample clock, so the same per-event bias would likely land with effectively
random sign from one occurrence to the next and mostly cancel over time.
**Working assessment: this is probably substantially a two-tone-test
artifact rather than a significant real-voice-quality problem** — still
worth fixing since it undermines trusting the IMD/linearity test signal
itself, but likely lower priority than it looked at the start of this
thread. Not verified against real speech/mic input.

## Instrumentation added this session (live in the tree)

- **`ssb_dsp.c`/`.h`**: `ssb_dsp_null_bias_stats_t` — `dphi_sum` /
  `dphi_sample_count` (plain), `near_null_dphi_sum` / `near_null_sample_count`
  (plain, restricted to `envelope < null_bias_threshold`), `env2_dphi_sum` /
  `env2_sum` (the weighted numerator/denominator). Accumulated in
  `ssb_dsp_process_sample()` right where `dphi` is computed, BEFORE
  slew-limiting or the `max_freq_dev_hz` clamp. `ssb_dsp_get_null_bias_stats()`
  reads it; `ssb_dsp_set/get_null_bias_threshold()` tune the near-null
  envelope cutoff (default 0.05). Everything resets via the existing
  `ssb_dsp_reset_freq_dev_stats()` (the `r` command already calls this).
- **`diagnostics.cpp`**: three print lines per ~1s cycle —
  `[dsp] null_bias` (f1/f2/expected_center/plain_mean/plain_bias),
  `[dsp] null_bias2` (weighted_mean/weighted_bias — **the one to trust**),
  `[dsp] null_bias3` (near_null_samples%/near_null_contrib/rest/threshold).
  Each is its own short `Serial.printf()` behind its own `diag_room_for()`
  call — a single combined line was tried first and silently never printed
  (needed ~200 bytes against this board's ~162-byte typical free buffer);
  splitting fixed it. Worth remembering if a future diagnostic line "goes
  missing" for no apparent reason — check its `diag_room_for()` size first.
- **`serial_commands.cpp`**: `n` cycles `null_bias_threshold` through
  `{0.02, 0.05, 0.10, 0.20}`.
- **Reproduce a reading**: `T` to a preset → confirm `e`/`c` off → confirm
  threshold=0.050 (`n` to cycle back if not) → `r` → wait ~1-2s → read the
  three `null_bias*` lines. See `ssb_mic_test_commands.md`'s "Null-crossing
  frequency bias diagnostic" section for the quick-reference version of all
  of this.

## 2026-09-11: null-uncertainty dither ('Q') added — untested

A third candidate fix, added this session in response to a direct "what about
dithering?" question and NOT yet bench-tested. Unlike the two "Targeted"/
"Principled" directions in item 1 below (which change how the ±π resolution
is computed at a null), this one leaves that computation alone and instead
attacks the *coherence* called out in "Root mechanism identified" above: the
test tones are exact phase-accumulator multiples of `SAMPLE_RATE_HZ`, so every
null lands at an identical sample-grid position every cycle, and whatever
tiny bias one null produces, every null produces identically — that's what
lets it accumulate instead of averaging out. `Q` (`test_signals.cpp`/`.h`)
adds a small (+/-0.5Hz), slowly and continuously varying frequency offset to
tone2 only (tone1 stays exactly at nominal as an undithered reference),
specifically to make successive nulls drift across the sample grid instead of
recurring at the same spot — the same role this file's own "likely real-world
significance" section says real voice's unpredictable null timing already
plays for free.

Validation plan, not yet run: compare `[dsp] null_bias2` (`weighted_bias`)
across several `r` resets on the SAME tone-pair preset, `Q` off vs. on, with a
LONG dwell before reading (see the 2026-09-09 update above — `weighted_bias`
drifts for tens of seconds to minutes after reset even with nothing changed,
so a quick point-read would just compare noise to noise). Looking for the
off-case's tightly-repeatable, tone-pair-specific constant to turn into
something that scatters and/or trends toward a smaller magnitude on-case,
given enough integration time. Full details/rationale: `test_signals.h`'s
doc comment on `test_signals_get_twotone_dither_enabled()`.

Note this targets the null-bias *measurement* artifact specifically, not the
separate "theoretically infinite phase bandwidth at the origin" EER/polar
problem `group_delay_fit_notes.md` researched (2026-09-03) — that entry found
NO literature precedent for dithering as a fix for that other problem, and
recommended an upstream I/Q trajectory reshape instead. The two shouldn't be
conflated even though both start with "there's a problem at the null."

## Open, un-actioned next steps

1. **Two candidate fix directions identified, neither implemented.** This
   codebase has a documented history of two abandoned null-handling
   attempts (`envelope_floor.cpp`'s NOTE 1/NOTE 2) — be deliberate here,
   validate incrementally on real hardware.
   - **Targeted**: for samples flagged near-null (same test used above),
     replace the raw `atan2`/`wrap_pi` resolution of the ±π direction with
     something less numerically fragile than the instantaneous Q/I ratio
     at a near-zero point.
   - **Principled** (matches the forward note already left in
     `envelope_floor.cpp`: "track/pre-warp the phase trajectory through the
     null, not freeze it"): extrapolate the phase trajectory through the
     null from the clean, well-defined phase rate on either side, rather
     than trusting `atan2`'s read for the 1-2 samples actually at the
     crossing.
2. Map the bias-vs-absolute-frequency relationship more finely than the 6
   fixed `TWOTONE_BAND_PRESETS` points (would help confirm/deny the
   Hilbert-filter-response explanation above).
3. Verify (or refute) the "mostly a test-artifact, not a real voice problem"
   assessment against actual speech/mic input — not done this session.
4. The still-separately-unexplained puzzle from earlier in this
   investigation: why toggling `I` (envelope interpolation, which provably
   never touches `freq_dev_hz`) appeared to shift the *measured* center
   frequency in some earlier observations. Leading candidate explanation
   reached at the time: the `I`-on/`I`-off shifts observed were smaller
   (~couple Hz) than a separately-observed ~15Hz baseline drift on the
   timescale of typing a sentence, so much of what looked like an `I`-caused
   shift may simply have been that independent drift being sampled at
   different moments — never isolated with a controlled dwell test.
5. **UPDATE 2026-08-31, end of session**: user's direct assessment is that
   the current x4 envelope-interpolation algorithm (`I`, `envelope_interp.h`/
   `.cpp`) makes two-tone stability *much worse* — a clearly audible
   instability, not just the subtle few-Hz effect item 4 above was framed
   as. This reads as stronger/more definitive than item 4's framing and
   should take priority over it when this is picked back up: before
   spending more time on the null-crossing bias mechanism above (which is
   independent of `I` — provably doesn't touch `freq_dev_hz`), first
   characterize what `I` itself is doing to produce an audible instability.
   `envelope_interp.h`'s own header comment documents four earlier
   real-hardware failure modes (v1-v4, all fixed) before landing on the
   current implementation — worth re-reading that history first in case
   this is a fifth, not-yet-identified failure mode of the same kind,
   rather than assuming it's connected to the null-bias work above. Not
   investigated further this session — parked here as the more urgent of
   the two open `I`-related threads.

## Where to resume

The null-crossing bias itself is well-characterized and, per the "likely
real-world significance" assessment above, may matter more for trusting
two-tone/IMD test data than for actual on-air voice quality — nothing there
is urgent. **Higher priority: item 5 above (`I` making two-tone stability
audibly worse)** — that's a clear, user-confirmed regression, not just a
measurement subtlety, and should be looked at before returning to the
null-bias fix directions. When picking this back up: start with item 5
(re-read `envelope_interp.h`'s v1-v4 failure history, then characterize what
changes with `I` on vs off on a two-tone signal specifically), then, once
that's resolved or understood, come back to "Confirmed measurement table"
above to refresh context on the null-bias mechanism and decide between the
two fix directions in "Open, un-actioned next steps" #1. The instrumentation
(`null_bias`/`null_bias2`/
`null_bias3`, the `n` command) is already in place and doesn't need to be
rebuilt — just `r` and read.
