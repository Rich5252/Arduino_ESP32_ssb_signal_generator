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

## 2026-09-11: first two `Q` bench results — averaged bias insensitive to
## dither amplitude, real-time spread looks threshold-like, not graded

Two captures on the 700/1900Hz (wide legacy) pair, both against this file's
own confirmed baseline for that pair above (`weighted_bias` -20.72Hz, SDR
~8Hz, dither off):

- **`Q` on, +/-0.5Hz**: board read as a noisy peak +/- ~6Hz around zero.
  `weighted_bias` -16.4Hz. Not a clean isolated A/B — this capture came right
  after reflashing the `Q` build, so settle time wasn't independently
  controlled for.
- **`Q` on, +/-0.05Hz** (`TWOTONE_DITHER_MAX_HZ` reduced 10x): board read as
  "continuous" shifting, roughly -10Hz to +3Hz. `weighted_bias` -17.4Hz.

Two findings, taken together:

1. **`weighted_bias` barely moved** (-16.4 -> -17.4Hz) despite the dither
   amplitude shrinking 10x. If dither worked by smoothly smearing the bias in
   proportion to how far off-grid it pushes tone2, the averaged bias should
   have shrunk back toward the -20.72Hz undithered baseline (or trended
   toward 0) as amplitude fell. It didn't move in either direction by much.
2. **The real-time spread stayed roughly the same overall size at both
   amplitudes** (~12Hz peak-to-peak at 0.5Hz dither vs. ~12-13Hz at 0.05Hz
   dither). A swing of similar magnitude at dither depths 10x apart is the
   signature of a threshold/discontinuity effect, not a graded one: once
   dither is nonzero at all, it appears sufficient to occasionally knock a
   null onto the other side of whatever discrete sample-grid boundary drives
   the coherent bias described in "Root mechanism identified" above,
   producing a close-to-full-scale jump regardless of how small the nudge
   actually is.

**Open discrepancy, not yet resolved**: the -17.4Hz `weighted_bias` average
from the +/-0.05Hz run sits well outside the user's reported real-time range
(-10 to +3Hz, midpoint ~-3.5Hz). Two live hypotheses, not yet distinguished:

- `weighted_bias` and whatever instrument produces the board's reading are
  different statistics of the same underlying signal (different
  integration/settling behavior) and simply aren't required to agree
  numerically even if both are "correct" in their own terms.
- The continuous re-dithering (a fresh random target every 250ms at
  `TWOTONE_DITHER_UPDATE_HZ=4.0`) may be preventing that instrument from ever
  reaching steady state — i.e. some or all of the "continuous shifting" could
  be a measurement-methodology artifact of the test itself never settling,
  not a property of the transmitted signal.

Proposed follow-up, not yet run: slow `TWOTONE_DITHER_UPDATE_HZ` down a lot
(e.g. to 0.2Hz — a new target every 5s) while keeping amplitude small, and
see whether the board's reading can settle between updates. If the swings
shrink at a slower update rate, that points at the settling-artifact
hypothesis; if they don't, that favors either the two-different-statistics
explanation or the null-boundary-crossing read above being real and
independent of update rate. Also still unconfirmed: exactly what "the board"
is and how it derives a frequency reading (SDR waterfall peak vs. a
frequency counter vs. a tracking/PLL demod would each have very different
settling behavior).

## 2026-09-12: "the board" identified — an FFT panadapter, not a tracking
## loop, which resolves most of the open discrepancy above via basic
## integration-time math, no settling-artifact needed

User confirmed "the board" is SDRUno's "Aux SP" high-resolution FFT display,
currently set to 0.18Hz/bin, with the receiver's absolute frequency
reference (a 10MHz OCXO/ref) independently estimated accurate to +/-1Hz.
That +/-1Hz figure rules out RX calibration drift as an explanation for a
~12Hz-wide swing, but it also means every individual Aux SP reading can be
taken at face value as a real measurement, not an artifact of the receiver's
own frequency accuracy — so the -10 to +3Hz swing is genuinely showing
something about the signal (or about how a short window samples it), not
about the SDR being mistuned.

**This is an FFT panadapter, not a PLL/tracking demodulator** — worth
correcting the framing from the previous entry above, which floated "the
instrument not settling" as if a tracking loop were involved. There's no
loop here to settle; each Aux SP redraw is a fresh (or overlapped) spectral
snapshot integrated over however many samples its resolution setting
requires. That integration TIME is the number that matters, and it can be
worked out directly from the stated resolution:

- Basic FFT resolution identity: bin spacing = 1 / (window duration in
  seconds), for a rectangular window. 0.18Hz/bin → window duration ~= 5.56s.
  SDRUno's Aux SP most likely applies a lower-sidelobe window (Hann/
  Blackman-Harris-family, standard for panadapter displays) rather than a
  bare rectangular one, which widens the effective mainlobe for the same
  window length by roughly 1.5-2x — so achieving 0.18Hz of DISPLAYED
  resolution probably means an actual capture length somewhat longer than
  5.56s, plausibly out to 8-11s. Call it "on the order of 5-10 seconds"
  per spectrum snapshot.
- `Q`'s dither redraws an independent random target every
  `1/TWOTONE_DITHER_UPDATE_HZ` = 0.25s (`config.h`,
  `TWOTONE_DITHER_UPDATE_HZ=4.0`) and ramps linearly toward it in between —
  so within one ~5-10s Aux SP snapshot, only about **20-40 independent
  dither realizations** get folded into that single spectral picture.
- `weighted_bias` (`env2_sum`/`env2_dphi_sum` in `ssb_dsp.c`) is a plain
  lifetime accumulator that only resets on `'r'` — never a decaying/moving
  average. By the time it's read at, say, t=706s into a run, it has
  integrated across ~706s / 0.25s = **~2800 independent dither
  realizations** — roughly 70-130x more than a single Aux SP snapshot sees.

Given the two bench results directly above already established that this
null-bias effect behaves like a threshold/discontinuity rather than a
smoothly-graded one (each dither realization can swing the LOCAL average by
close to a full-scale amount, not a small increment), an estimator built
from only ~20-40 such realizations (one Aux SP snapshot) is expected to have
much higher variance than one built from ~2800 of them (`weighted_bias` at
the point of reading) — even though both are, in principle, converging
toward the same true long-run mean as more realizations accumulate. That is
a completely ordinary small-N-vs-large-N sampling-variance effect once the
underlying per-realization distribution is skewed/bimodal-ish rather than
tightly clustered, and needs no receiver-settling mechanism to explain it.
This makes the "two different statistics of the same signal" hypothesis
from the entry above the better-supported one specifically because of *how*
different their integration times are (~10x per snapshot vs. potentially
hundreds-to-thousands of accumulated seconds for `weighted_bias`), not just
that they're different in some general sense.

**Cleaner next experiment than varying the dither update rate**: toggle `Q`
OFF and watch Aux SP on the same 700/1900Hz pair for a comparable stretch.
The undithered baseline (this file's confirmed-measurement table) already
gives a single steady `weighted_bias` (-20.72Hz for this pair) — the open
question is whether Aux SP *also* shows several-Hz snapshot-to-snapshot
scatter with dither off (which would mean much of the ~12Hz spread seen with
`Q` on isn't new — the receiver/signal already had comparable short-window
noise for other reasons, e.g. residual phase noise or ADC-referred jitter
unrelated to the coherent null-bias mechanism), or whether it's rock-steady
without dither (which would confirm the scatter is specifically
dither-induced, via the small-N sampling-variance mechanism above). This is
a single-variable toggle on hardware already in hand, cheaper to run than
the slow-update-rate experiment proposed above, and worth doing first — not
yet run.

## 2026-09-12, later same day: CORRECTION to the entry above (Aux SP is
## exponential-averaging, not a fixed-window FFT snapshot) — plus two new
## hardware facts (visual read resolution, AD9851 XO thermal drift) — and a
## direct answer to "shouldn't dither have centered this on zero?"

User corrected the entry above: Aux SP uses **exponential averaging**, not
a fixed-length rectangular/Hann window — so the "~5-10s snapshot, ~20-40
independent dither draws per snapshot" arithmetic above is the wrong model
and is superseded by this entry (left in place rather than deleted, per this
file's convention, since the underlying "short effective memory sees more
variance than a long one" conclusion still holds, just via a different
mechanism — see below). Two more relevant facts: (1) the user's own visual
read of the moving trace is reliable only to about +/-5Hz (cursor-based
static measurements are much better) — some of the apparent -10..+3Hz
spread is eyeballing uncertainty on top of whatever the display is actually
doing; (2) the AD9851's reference is a plain 30MHz XO (not an OCXO) —
confirmed stable once warmed up, but a physical disturbance (touching it to
cool it) shifts the synthesized output by **~20Hz**, i.e. an effect of
comparable magnitude to the whole swing being investigated here.

**Why exponential averaging still predicts persistent, never-settling
scatter (arguably better than the fixed-window model did):** an EWMA has a
FIXED asymptotic variance that does not keep shrinking the longer you watch
it — it continuously "forgets" older samples at a constant rate, so its
noise floor is set by its time constant, not by total elapsed observation
time. `weighted_bias` (`env2_sum`/`env2_dphi_sum`, plain lifetime
accumulator, never resets except on `'r'`) is the opposite: its variance
keeps shrinking like 1/(elapsed time) for as long as you let it run. So Aux
SP is expected to go on fluctuating around whatever its true underlying mean
is indefinitely, while `weighted_bias` keeps getting quieter — which matches
"continuous shifting" being observed as an ongoing, non-settling behavior
rather than something that would eventually stop if watched longer.

**The AD9851 XO point matters because it's a confound `weighted_bias` is
immune to and Aux SP is not.** `weighted_bias` is computed entirely in the
digital/audio domain from the DSP's own I/Q (`ssb_dsp.c`) — it has no
dependency on the AD9851's actual reference clock accuracy. Whatever Aux SP
reads on-air, by contrast, is the ACTUAL transmitted RF frequency, which
rides on top of the real DDS output and therefore includes any real thermal
drift of the 30MHz reference. A confirmed ~20Hz/disturbance sensitivity on
that XO means ordinary ambient thermal movement over the course of a
multi-minute test could plausibly contribute several Hz of genuine,
real-world frequency drift — layered on top of, and NOT distinguishable
from, whatever the null-bias/dither mechanism itself is doing, when reading
Aux SP alone. **This makes `weighted_bias` the cleaner of the two signals
for judging whether `Q` is working**, precisely because it can't see XO
drift at all — Aux SP is the necessary real-world validation channel, but
it's reading two superimposed effects, not one.

**Direct answer to "I expected dither to center the frequency on zero — is
that not happening, and if so why?"** Judged on `weighted_bias` (the
XO-drift-immune channel), the answer is: partially, and the shortfall looks
structural, not a tuning problem. Undithered on this pair: -20.72Hz. With
`Q` on: -16.4Hz (0.5Hz dither) and -17.4Hz (0.05Hz dither) — a real
improvement over the undithered case, but plateaued around -17Hz rather than
continuing toward 0, and unmoved by a 10x change in dither amplitude. The
mechanism `Q` targets (per "Root mechanism identified" above) is
specifically the *coherent amplification* that comes from every null
recurring at the identical sample-grid alignment — breaking that coherence
should convert "the single worst-case alignment's bias, repeated forever"
into "the bias averaged over MANY different alignments." That is a
different claim from "the per-null bias averages to zero across alignments"
— it only nets out to ~0 if the underlying bias-vs-alignment function is
itself roughly zero-mean, which nothing in this investigation has actually
established. The amplitude-insensitivity of the ~-17Hz plateau (same result
at 0.5Hz and 0.05Hz dither, i.e. across two very different samplings of the
alignment space) is consistent with `Q` already fully achieving its
designed job — decorrelating alignment — and what's left (-17Hz) being the
genuine alignment-averaged mean of the near-null bias mechanism itself,
which this evidence suggests is NOT zero-mean. If so, no amount of further
dither tuning (amplitude or update rate) would be expected to close that
remaining ~17Hz gap — a different fix (one of the two "Targeted"/
"Principled" directions in "Open, un-actioned next steps" below, which
change how the ±π resolution is computed at a null rather than just
scrambling which alignment gets hit) would be needed for that part.

**Two concrete follow-ups, not yet run:**
- **Repeat the `Q` on/off comparison on a pair that starts near zero
  undithered** — e.g. 1500/1700 (`+0.11Hz` undithered per the confirmed
  table above) rather than 700/1900 (`-20.72Hz`, and separately already
  flagged elsewhere in this codebase as a group-delay outlier vs. the
  tighter-spaced bands). If `Q` leaves a near-zero pair still near zero,
  that's reassuring — dither isn't introducing a new artifact of its own
  (e.g. via its own 4Hz update periodicity). If it PUSHES a near-zero pair
  away from zero, that would suggest the alignment-averaged mean of the
  bias function varies by pair in a way that isn't simply "coherent bias
  good, dithered bias better," and would need its own explanation.
- **For any future real-world (not just `weighted_bias`) comparison, use
  Aux SP's cursor-based static measurement (confirmed by the user to be far
  more precise than eyeballing the moving trace) rather than reading the
  live display**, and take several such readings over a run rather than one,
  to separate genuine signal movement from both the +/-5Hz visual-read
  limit and the EWMA's own non-settling noise floor.

## 2026-09-12, later still: mechanism found for "changing relative_delay
## (or switching presets) shifts the measured tone frequency" — and a real
## gap identified in every weighted_bias reading taken so far

User reports (back on 700/1700 now, and reporting Aux SP readings as offsets
from a 1000Hz nominal center from here on) that adjusting relative delay
(`'['`/`']'`) or switching between presets with different delay values
(e.g. 1 & 3) repeatedly shifts the measured tone frequency - sometimes by a
few Hz, sometimes by as much as 25Hz - and that the shift often behaves like
it's "locked in" to one of two discrete values rather than moving smoothly.
Traced to a real, previously-unflagged interaction between `relative_delay`
(`relative_delay.cpp`/`.h`) and the near-null bias mechanism this whole file
is about.

**Mechanism.** `relative_delay_apply()` runs in `ssb_mic_test.ino` AFTER
`ssb_dsp_process_sample()` has already computed `freq_dev_hz`/`envelope` for
the tick - it doesn't touch the analytic-signal math, it only holds one of
the two signals back relative to the other (positive delay holds
`freq_dev_hz` back; every two-tone preset uses positive delay, so
`envelope` passes through untouched while `freq_dev_hz` gets time-shifted).
`weighted_bias`'s env^2-weighting exists specifically because it's supposed
to match the real transmitted power spectrum's centroid — that only works
if the near-null `freq_dev` spikes (the thousand-Hz-plus, single-tick
excursions already characterized via `max_freq_dev_step`/the post-step
trace) land on samples where `envelope` is genuinely near zero, so their
contribution to what's actually radiated stays suppressed. `relative_delay`
directly controls whether that alignment holds: shift `freq_dev_hz` far
enough relative to the untouched `envelope`, and a spike that used to land
on envelope~0 now lands on non-negligible envelope - i.e. non-negligible
transmitted power - at the exact instant the instantaneous frequency is
badly wrong. Since these spikes recur at the identical coherent sample-grid
alignment every cycle (the same rational-tone/`SAMPLE_RATE_HZ` mechanism as
"Root mechanism identified" above), a FIXED delay value produces a FIXED,
repeatable amount of this contamination - matching the "locks in" behavior
directly. And because the spike itself is narrow/near-discontinuous (1-2
samples), how much of it gets exposed is a steep, non-linear function of
delay - small delay changes near a good alignment barely matter, crossing
in or out of the spike's window can swing the bias by a lot - matching both
the "sometimes a few Hz, sometimes 25Hz" and the snap-between-two-values
observations. Confirmed directly relevant to presets: preset 1 ("TwoTone
Base") has `relative_delay_samples=0.00`; preset 3 ("Shelf2 Baseline gdeq
adj#4") has `2.00` (`settings.h`) - a full 2-sample/125us swing baked
straight into the preset switch, easily enough to move a spike from
aligned-with-zero to fully exposed.

**Real diagnostic gap, not just a theoretical point.** `ssb_mic_test.ino`
already carries a comment at the `relative_delay_apply()` call site (added
some earlier session) noting the null-bias diagnostics are "PRE-delay
values ... and have always been blind to whatever the delay line does,"
reasoning "it shouldn't [alter frequency content] - a pure sample delay
can't change frequency content." That's correct about `freq_dev_hz`'s own
spectrum in isolation, but it's the wrong question - the effect is in the
CROSS-alignment between two different signals (`freq_dev_hz` vs the
untouched `envelope`), not in `freq_dev_hz`'s own frequency content. Net
effect: **every `weighted_bias` reading taken anywhere in this file,
including both `Q`-dither bench results above, was computed pre-delay and
is structurally blind to whatever `relative_delay_samples` (hence '['/']'
and preset choice) does to the actually-transmitted spectrum.** Aux SP has
been the only instrument able to see this effect at all so far. This is
also very plausibly the same underlying event behind this whole codebase's
extensive delay-tuning-for-two-tone-IMD history (`relative_delay.h`'s doc
comment) - IMD splatter and this center-frequency shift read as two symptoms
of the same "spike exposed to nonzero transmitted power" event, not
separate phenomena.

**Proposed, not yet implemented**: add a post-delay variant of the
null-bias accumulation, fed from `delayed_freq_dev_hz`/`delayed_envelope`
right after the `relative_delay_apply()` call in `ssb_mic_test.ino`, so
there's a firmware-side, AD9851-XO-drift-immune number that actually tracks
what `'['`/`']'` and preset switches do to the transmitted spectrum, instead
of relying on Aux SP alone (which per the entry above has its own
integration-time and XO-drift complications). Asked the user whether to
implement this - not yet actioned.

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
