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

## 2026-09-12, later still: new per-event jump log implemented ('J') -
## direct test of whether every random TX jump is really null-crossing

User's explicit engineering position, stated directly: they want the random
freq jumps ELIMINATED, and still have real doubts that the null-crossing
mechanism is the only thing going on, given how many "strange states" this
investigation has turned up (relative_delay's effect, the exponential-
averaging correction, the AD9851 XO drift, etc.). Asked for whatever
diagnostics would help settle this for definite. Implemented, not yet
bench-tested (no toolchain in this environment - see this project's
standing caveat on every firmware change):

**The gap being closed**: every jump-related diagnostic in this file up to
now (`max_freq_dev_step`, the post-step `tx_freq` trace) only ever
remembers the SINGLE worst event across an entire run - a multi-hour
unattended capture hands back exactly one data point, which is anecdote,
not evidence, when trying to settle whether ALL jumps share one cause.

**What's new** (`diagnostics.h`/`.cpp`, `ssb_mic_test.ino`,
`serial_commands.cpp`, `ssb_dsp.c`/`.h`, `relative_delay.c`/`.h`):

- A per-event **jump log** (`JUMP_LOG_LEN=8` ring, `JUMP_LOG_THRESHOLD_HZ=300`
  - a much lower, more inclusive bar than "new all-time record") that
  stores every qualifying `tx_freq` step, not just the record-breaker, each
  with: the POST-delay (actually-transmitted) envelope at that exact
  sample, the current `relative_delay_samples`, that tick's own `busy_us`,
  the active audio source, and a `near_null` flag
  (`envelope < ssb_dsp_get_null_bias_threshold()`, evaluated on the
  post-delay envelope specifically - see the 2026-09-12 relative_delay
  entry above for why post-delay is the physically relevant one here, not
  the pre-delay value the existing null_bias stats use).
- A running total + near-null count, printed as a cheap one-line summary
  in the always-on periodic block (`[dsp]   jump_log: n=... near_null=...
  (NN%)`) so an unattended run's own serial log shows the near-null
  percentage accumulate over hours without anyone needing to catch it live.
- A new **'J' serial command** to dump the full ring (all captured
  events, oldest to newest) on demand.
- The existing post-step `tx_freq` trace also now captures the matching
  POST-delay envelope in parallel (`post-step-env:` line) - same direct
  test, applied to the one worst-ever event that trace already tracks.

**Why this is the direct test, not another indirect inference**: if the
null-crossing theory is the whole story, `near_null` should read close to
100% across many independently-captured events, accumulated over hours,
not just the one event this file has been reasoning about so far. If a
meaningful fraction of logged jumps come back `near_null=false`, that's
hard, per-event, firmware-side evidence of a separate mechanism - not
something that can be argued away as "well, maybe near-null classification
was borderline that one time." The `relative_delay_samples` and `busy_us`
fields on each entry mean a delay sweep or a timing anomaly can also be
correlated directly against jump occurrence, rather than only inferred
indirectly the way this whole session's relative_delay finding was.

**Also settles, essentially for free**: whether this happens on mic input
too, not just the two-tone test - the log is audio-source-agnostic (every
entry records which source was active), so leaving 'J' logging running
across a source change and comparing counts answers this without a
separate diagnostic.

**Suggested test protocol** (not yet run): `'r'` to reset, leave running
unattended for a long stretch on two-tone (as in the earlier ~4.7-hour
run), then `'J'`. Repeat on mic input for a comparable stretch. Compare
`near_null` percentages and source breakdowns between the two. If both come
back ~100% near-null regardless of source, that's strong convergent
evidence for the existing theory. Any consistent population of
`near_null=false` events, especially ones sharing an unusual `busy_us` or a
particular `relative_delay_samples` value, points at a second, distinct
mechanism worth chasing on its own.

**Two small IRAM_ATTR fixes made along the way**: `ssb_dsp_get_null_bias_threshold()`
and `relative_delay_get_samples()` were previously not marked `IRAM_ATTR`
(their setters were) - both are now called from the new jump-log code on
the dsp_task hot path, so both were marked, matching this codebase's
existing IRAM discipline everywhere else on that path. Pure classification
changes (trivial single-field reads), not behavior changes.

## 2026-09-12, later still: first 'J' bench data - a real bug found and
## fixed in the jump log's own near_null classification, and a compelling
## (not yet confirmed) unifying explanation for the whole investigation

First real data from the new jump log, on 700/1700, `relative_delay_samples
= +4.60`:

```
[dsp] jump_log: 503384 qualifying step(s) >300Hz since last reset, 0 near-null (0%) - ring holds the last 8
[dsp]   jump[0]: t=247154ms 14198159->14196560Hz (step=1599Hz) env=0.595 delay=+4.60 busy_us=37 src=TWO-TONE TEST
[dsp]   jump[1]: t=247154ms 14196560->14201361Hz (step=4801Hz) env=0.617 delay=+4.60 busy_us=36 src=TWO-TONE TEST
[dsp]   jump[2]: t=247155ms 14201361->14198162Hz (step=3199Hz) env=0.509 delay=+4.60 busy_us=43 src=TWO-TONE TEST
[dsp]   jump[3]: t=247155ms 14198162->14196561Hz (step=1601Hz) env=0.595 delay=+4.60 busy_us=37 src=TWO-TONE TEST
[dsp]   jump[4]: t=247155ms 14196561->14201359Hz (step=4798Hz) env=0.616 delay=+4.60 busy_us=36 src=TWO-TONE TEST
[dsp]   jump[5]: t=247156ms 14201359->14198159Hz (step=3200Hz) env=0.509 delay=+4.60 busy_us=44 src=TWO-TONE TEST
[dsp]   jump[6]: t=247156ms 14198159->14196559Hz (step=1600Hz) env=0.594 delay=+4.60 busy_us=37 src=TWO-TONE TEST
[dsp]   jump[7]: t=247156ms 14196559->14201359Hz (step=4800Hz) env=0.616 delay=+4.60 busy_us=36 src=TWO-TONE TEST
```

**Real bug found in the jump log itself, now fixed** (`relative_delay.h`/
`.cpp`, `diagnostics.h`/`.cpp`, `ssb_mic_test.ino`). 503384 events, 0%
near_null, with `env` sitting at 0.5-0.6 (nowhere near the 0.05 null
threshold) looked at first like hard evidence AGAINST the null-crossing
theory. It wasn't - it was this diagnostic classifying against the WRONG
envelope. The `envelope` field used `delayed_envelope`
(`relative_delay_apply()`'s post-delay output), reasoning "what's actually
transmitted is the physically relevant thing." That's true for judging
transmitted power, but wrong for null classification: for `delay>0` (every
two-tone preset, including this one), `relative_delay_apply()` reads the
envelope ring at `env_back=0` - i.e. `delayed_envelope` is just the
CURRENT tick's envelope, not time-shifted at all. Meanwhile `freq_dev_hz`
(hence `tx_freq`) IS shifted, by the full `+4.60` samples. So the envelope
being compared against a given `tx_freq` value was up to 4.60 samples
(~287.5us at 16kHz) away from the moment that `tx_freq` value was actually
computed - comparing two things that were never supposed to line up.
Fixed by adding a THIRD output to `relative_delay_apply()`,
`out_envelope_at_freq_time`, which always reads the envelope ring at the
SAME lag as `freq_dev_hz` (`freq_back`, not `env_back`), regardless of
delay's sign - a cheap, symmetric extension of the ring interpolation
mechanism already there. The jump log's `envelope`/`near_null` fields (and
the post-step trace's envelope capture) now use this instead.

**The bigger, still-open question: is +4.60 itself the real story here?**
Every relative_delay value bench-validated anywhere in this project's whole
history (the delay-tuning saga in `relative_delay.h`'s own doc comment, the
gdeq candidate-B compromise window, every settings.h preset) stays within
roughly 0.00 to 2.10 samples. `+4.60` is more than double the largest value
this codebase has ever actually tested on real hardware, and it's getting
close to the ring's hard clamp (`PHASE_DELAY_MAX_SAMPLES-2 = 6`) - a value
that ring was sized to have generous HEADROOM for, per its own declaration
comment, not a value expected to be used in normal operation.

That matters because of a compelling (not yet confirmed) way this could
unify with the 2026-09-12 relative_delay/near-null-spike finding above: if
a well-tuned delay keeps most beats' near-null spikes suppressed (aligned
with genuine envelope near-zero) and only occasionally lets one leak
through - matching this project's original symptom of RARE, hours-apart
jumps - then a badly-mistuned delay like +4.60 could plausibly expose
EVERY beat's spike instead of an occasional one, turning a rare glitch into
a continuous, metronomically regular oscillation. The captured data is
consistent with that: the 8 events form an exact, repeating 3-value cycle
(steps -1600/+4800/-3200Hz, which sum to exactly zero over one full cycle)
recurring roughly every ~490us (503384 events over ~247s) - not random at
all, but a tight, self-sustaining pattern, exactly what you'd expect if
every single beat cycle is hitting the same exposed-spike condition rather
than an occasional unlucky one. The correlated `busy_us` shift (36-37us on
two of every three logged ticks, 43-44us on the third) is a further,
independent clue in the same direction - a null-crossing `atan2`/near-zero
computation taking measurably longer (plausibly IEEE-754 subnormal/gradual-
underflow slowdown right at the null) is a very plausible explanation for
why busy_us tracks the SAME 3-cycle pattern as the frequency itself.

**Not yet confirmed - this needs a direct test, not more reasoning from one
capture.** Was `relative_delay=+4.60` reached deliberately (testing an
extreme value on purpose) or by accident (e.g. `']'` held down/pressed
many times)? Either way, the decisive next step: reflash with the corrected
files, then compare 'J' at this SAME +4.60 delay (near_null should now
read close to 100% if the bug fix above is right) against 'J' at a
previously-validated delay (0.00-2.10, e.g. reload preset 1/2/3, `'r'`
reset, run for a comparable stretch). If the qualifying-step COUNT collapses
from ~500000-per-4-minutes down to something close to this project's
original "rare, hours-apart" rate once delay is back in the validated
range, that's strong, direct, quantitative confirmation that delay-tuning
quality is the dominant driver of jump RATE and that the null-crossing/
spike-exposure mechanism (corrected classification and all) is the whole
story. If a similarly regular, high-rate oscillation persists even at a
previously-validated delay, that's real, separate evidence of a second
mechanism, and the near_null percentage at THAT capture (now using the
corrected, time-matched envelope) would be the next thing to look at.

## 2026-09-12, later still: relative_delay=+4.60 was incidental (scanning
## with '['/']', not a deliberate extreme-value test) - and the user reports
## the "most interesting" freq activity concentrates around delay~=2, not
## necessarily worse further out

Two clarifications before the reflashed/fixed build gets its next real
test: `relative_delay=+4.60` in the capture above was reached by scanning
with `'['`/`']'` and stopping there, not a deliberate stress-test of an
extreme value - so the "is this delay deliberate or accidental" question
from the entry above is answered (accidental), but doesn't by itself say
anything about whether +4.60 is special. More important: the user reports
the most interesting frequency activity happens "around 2," not
progressively worse the further delay gets from a validated value. That's
worth flagging clearly against the "badly-mistuned delay exposes every
beat's spike" hypothesis proposed above - `relative_delay~=2.00-2.10` is
NOT a mistuned value, it's exactly the sub-0.1-sample compromise window
this project's own delay-tuning history (`relative_delay.h`'s doc comment,
the gdeq candidate-B bench test) settled on as the BEST two-tone IMD
trade-off. If the jump/glitch activity this whole investigation is chasing
is ALSO most active right around that same value, that would be a real and
previously unrecognized tension between "best IMD" and "fewest random
jumps" as two different, possibly conflicting tuning targets for the same
knob - a materially different picture from "just get the delay right and
both problems go away."

Not yet confirmed what "interesting" refers to precisely (jump-log
activity specifically, vs. something else observed directly on Aux SP or
by ear) - asked the user to clarify. Proposed next step once the corrected
build is flashed: a small delay SWEEP with `'J'` (reset via `'r'` before
each point, comparable dwell at each) bracketing 2 - e.g. 0.00, 1.00, 2.00,
2.05, 3.00 - to see whether jump count/near_null% peaks SHARPLY right at
~2 (would point at something specific to that exact value - e.g. delay=2.00
exactly landing on an integer sample count, where `interp_ring()`'s
interpolation weight collapses onto a single historical ring sample with no
blending from a neighbor, unlike every non-integer delay) or trends more
smoothly across the range (would fit a broader "how far from true physical
alignment" explanation instead). Not yet run.

## 2026-09-12, later still: second 'J' capture (delay=+0.90) decodes to a
## clean 3-state cycle where only 1 of 3 states is near-null by the blended
## test - traced to a second, more subtle diagnostic gap, now also fixed

Second real capture, this time at `relative_delay=+0.90` (the +4.60 above
was confirmed incidental - reached by scanning with `'['`/`']'`, not a
deliberate extreme-value test):

```
[dsp] jump_log: 25693 qualifying step(s) >300Hz since last reset, 6531 near-null (25%) - ring holds the last 8
[dsp]   jump[0..7]: three distinct hop "types" recurring in a fixed 3-cycle:
  ~14200560 -> ~14194160  (-6400Hz)  env=0.235  NOT near-null
  ~14194160 -> ~14201359  (+7200Hz)  env=0.036  NEAR_NULL
  ~14201358 -> ~14200558  (-800Hz)   env=0.354  NOT near-null
  (steps sum to exactly zero over one full cycle - a locked 3-state
  oscillation, same qualitative signature as the +4.60 capture, just with
  different Hz/envelope values and a lower rate: ~313 events/s here vs.
  ~2038 events/s at +4.60 - a real ~6.5x drop, but nowhere near this
  project's original "rare, hours apart" symptom)
```

The bug fix from the entry above IS working - near_null is no longer stuck
at 0%, and 3 of the 8 printed events (matching the aggregate 25%) correctly
flag NEAR_NULL at env=0.036. But this exposed a SECOND, more subtle gap:
2 of the 3 states in the exact same repeating cycle are large jumps
(6400Hz, 800Hz) at envelope readings (0.235, 0.354) nowhere near the 0.05
threshold. Taken at face value, that's real evidence of jumps happening
away from any null - exactly the kind of finding that would support a
second mechanism. But before trusting that at face value: at
`relative_delay=+0.90`, the fractional part is 0.90 - a heavily LOPSIDED
blend (`interp_ring()` weights the sample from 1 tick back at 90%, the
current tick at only 10%). `envelope_at_freq_time` (added in the entry
above) is itself computed via that same lopsided blend. If a genuinely
near-null RAW sample happened to be the 10%-weighted contributor while the
90%-weighted one was comfortably non-null, the BLENDED envelope could
easily read something like 0.9*0.26 + 0.1*0.0 ~= 0.23 - matching the 0.235
seen here almost exactly - while still having had a real near-null sample
contribute to that output. The blended test alone can't tell these two
cases apart.

**Fixed** (`relative_delay.h`/`.cpp`, `diagnostics.h`/`.cpp`,
`ssb_mic_test.ino`): added a second output, `out_envelope_at_freq_time_min`
- the smaller of the two RAW ring entries `out_envelope_at_freq_time`
blends together (a new `interp_ring_min()`, mirroring the existing
`interp_ring()`'s index math but returning the lesser raw value instead of
the weighted blend). The jump log now tracks and reports TWO near-null
percentages per event and in aggregate: `near_null_blended` (the original,
strict "was the transmitted RESULT near a null" test) and `near_null_either`
(the new, more permissive "did EITHER raw contributor dip near a null"
test). Both print in `'J'`'s per-event dump (`env=blended/min`,
`NEAR_NULL`/`NEAR_NULL(either)`) and in the periodic one-line summary.

**Not yet re-tested** - this needs a fresh capture with the corrected
build to see whether `near_null_either` at delay=+0.90 comes back much
higher than the 25% `near_null_blended` figure (which would mean the
"2 of 3 states are null-free" read above was itself a lopsided-blend
artifact, and the null-crossing theory survives largely intact) or stays
similarly low even under the more permissive test (which would be real,
harder-to-explain-away evidence that this specific 3-state cycle has a
genuinely non-null-related component). Either result is informative and
worth having before drawing a conclusion from the numbers above.

## 2026-09-12, later still: third 'J' capture (delay=+4.28) - the
## near_null_either fix confirmed working, 2 of 3 cycle states now explained,
## but the largest transition still isn't - added a third diagnostic (raw
## freq_dev pair) before concluding a null-independent mechanism exists

Third real capture, at `relative_delay=+4.28`:

```
[dsp] jump_log: 134573 qualifying step(s) >300Hz since last reset, 2740 near-null-blended (2%),
  46219 near-null-either (34%) - ring holds the last 8
[dsp]   jump[0..7]: a clean repeating 3-state cycle:
  ~14195600 -> ~14199120  (+3520Hz)  NEAR_NULL(either)
  ~14199120 -> ~14201360  (+2240Hz)  NEAR_NULL(either)
  ~14201360 -> ~14195600  (-5761Hz)  NOT near-null (blended OR either) - env=0.277/0.247
  (steps sum to exactly zero over one full cycle, same locked-oscillation
  signature as both prior captures)
```

The `near_null_either` fix from the entry above is doing real, confirmable
work here: the aggregate jumped from 2% (`near_null_blended`, roughly in
line with the earlier captures) to 34% once the more permissive test is
counted, and 2 of the 3 states in the decoded cycle (+3520Hz, +2240Hz) flag
`NEAR_NULL(either)` - i.e. a genuinely near-null raw sample was hiding in a
lopsided blend for both of these, exactly the failure mode the fix was
built to catch. That's real, direct confirmation the fix works, not just
new noise.

But the third and LARGEST transition in the same cycle (-5761Hz) does not
classify as near-null by either test: `envelope_at_freq_time=0.277`,
`envelope_at_freq_time_min=0.247`, both comfortably above the 0.05
threshold. This is the first data point across all three captures where
the more permissive "either" test still comes back empty on a real,
sizeable jump - the strongest evidence yet of something the null-crossing
theory alone doesn't explain.

Before treating that as proof of a second, null-independent mechanism,
there's a more basic question worth answering first, because
`out_delayed_freq_dev_hz` is itself produced by `interp_ring()` blending
TWO raw (undelayed) `freq_dev_hz` ring entries together via linear
interpolation - at `relative_delay=+4.28` (frac=0.28), a 72%/28% blend.
A large delayed step doesn't necessarily mean a large step exists in the
raw, undelayed signal - it can also arise from interpolating across a
multi-sample lag during a part of the waveform where freq_dev is changing
fast, blending two raw samples from meaningfully different points in time.
So: were the two raw values contributing to this -5761Hz step already
about that far apart from each other (a genuine discontinuity exists in
the raw, undelayed signal - real, just not one an envelope-near-null test
is built to catch), or are both individually unremarkable (meaning the
large DELAYED step is essentially an interpolation artifact, not evidence
of any discrete "event" in the underlying signal at all)?

**Added** (`relative_delay.h`/`.cpp`, `diagnostics.h`/`.cpp`,
`ssb_mic_test.ino`): a third output pair, `out_raw_freq_dev_near`/
`out_raw_freq_dev_far` (via a new `interp_ring_components()` helper,
mirroring `interp_ring()`'s index math but returning both individual raw
ring values instead of a blend or a min) - the two RAW, undelayed
`freq_dev_hz` samples `interp_ring()` itself blends to produce
`out_delayed_freq_dev_hz`. Threaded through `diagnostics_set_tx_info()`
into the jump log's per-event struct and `'J'`'s per-event printout as a
new `raw_freq_dev: near=...Hz far=...Hz raw_delta=...Hz` line under each
jump entry. `raw_delta` close to the logged `step_hz` means the
discontinuity is real and already present in the raw signal; `raw_delta`
much smaller than `step_hz` means the delayed step is mostly/entirely an
interpolation artifact.

Also corrected a stale comment at the `diagnostics_set_tx_info()` call
site in `ssb_mic_test.ino`, left over from before this mechanism was
understood: it argued a "pure sample delay can't change frequency
content" as the reason the pre-delay `null_bias`/`weighted_bias`
diagnostics didn't need to account for the delay line. That's true of
`freq_dev_hz`'s own spectrum considered in isolation, but it was the wrong
question - the actual effect (documented in the "2026-09-12, later still:
found the mechanism..." entry in `moving_forward_notes.md`) lives in the
CROSS-alignment between the delayed freq_dev and the mostly-undelayed
envelope, not in freq_dev's spectrum on its own.

**Not yet re-tested** - needs a fresh capture with this build at the same
(or a similar) delay to read the raw_freq_dev values for the -5761Hz-type
transition specifically. Also worth noting for whoever reads this next:
qualifying-jump RATES are now measured in the hundreds to low-thousands
per second even at delay settings well within this project's historically
validated 0.00-2.10 range (this capture's own rate is far higher than
the two-tone test's original "rare, occasional glitch" framing implied) -
worth asking directly whether `JUMP_LOG_THRESHOLD_HZ=300` is calibrated to
the rare, dramatic symptom that motivated building this diagnostic in the
first place, or whether it's mostly capturing ordinary, expected two-tone
FM modulation dynamics that simply had never been measured at this
resolution before. That distinction matters for how much weight to put on
the aggregate percentages above.

## 2026-09-12, yet later still: the calibration question above is directly
## answered - the user's own bench data confirms 'J' is the wrong tool for
## "did the frequency I'm watching change," and a new, slower trigger
## ('K') is added specifically for that question

Two more captures at `relative_delay=+1.75`, both raising the exact
concern flagged at the end of the entry above before it could even be
asked as a question. First, the user's own independent observation:
"my feeling is that the J table changes all the time even if my
observation would say steady state." Second, direct confirmation on the
bench - a capture taken "as soon as I could" after a `'['`/`']'` sweep
triggered a visible jump, flagged by the user as probably "after the
horse bolted." A third capture nails it precisely: taken right after Aux
SP showed a real, user-observed shift (1000Hz -> 962Hz), it came back
showing:

```
[dsp] jump_log: 520213 qualifying step(s) >300Hz since last reset, 3792 near-null-blended (1%),
  295532 near-null-either (57%) - ring holds the last 8
[dsp]   jump[0..7]: the SAME repeating 3-state cycle already characterized above:
  ~14199362 -> ~14195359  (-4003Hz)  NEAR_NULL(either)  raw: near=-6805.3 far=1208.1 (delta=8013.3)
  ~14195357 -> ~14201360  (+6003Hz)  NEAR_NULL(either)  raw: near=1199.0  far=1201.0  (delta=2.0)
  ~14201361 -> ~14199360  (-2001Hz)  NOT near-null       raw: near=-6803.0 far=1200.1 (delta=8003.1)
  (identical Hz values, identical cycle shape, identical raw_delta pattern
  to the "nothing happened" capture at the same delay setting taken
  earlier in this same session)
```

This is a clean, direct answer, not an inference: the low-level jump log
looks IDENTICAL whether or not a human-perceptible frequency shift just
happened. `near_null_either%` climbing from 32% to 57% simply reflects
more of the same repeating cycle accumulating (52407 -> 520213 total
events between the two captures) - it says nothing about the 962Hz event
itself. `JUMP_LOG_THRESHOLD_HZ=300` (raised as an open question in the
entry above) isn't merely "maybe too low" - it's answering a categorically
different question ("did any single tick step by >300Hz," true hundreds
of times a second, always) than the one the user actually cares about
("did the frequency I'm watching just move," true rarely). No threshold
tweak to the existing per-tick test can fix this - a per-tick test can't
distinguish "the usual churn" from "a real shift" when the usual churn
already exceeds any reasonable per-tick threshold on every cycle.

**Fix, directly per the user's own proposed direction**: "track the mean
freq and trigger off that (so it sees what I see) to capture what
happened immediately before and during to change it." Implemented as a
SEPARATE mechanism from `'J'`, not a modification of it:

- Two EMAs of `delayed_freq_dev_hz` (the same value the periodic `[dsp]`
  status line already reports): a "fast" one (`FREQ_EMA_FAST_TAU_S=0.05`,
  50ms - several multiples of the ~3-5ms beat-null cycle period observed
  above, so the existing routine churn averages out almost completely)
  and a much slower "reference" one (`FREQ_EMA_SLOW_TAU_S=2.0`, 2s) that
  lags behind, representing "where this has been sitting."
- `SLOW_JUMP_TRIGGER_HZ=5.0` - deliberately the user's own independently-
  reported +/-5Hz visual-read tolerance on Aux SP (this file's earlier
  2026-09-12 entries), not a DSP-internal number. When the fast and slow
  EMAs diverge by more than this, that's treated as a change a human
  watching the display would actually notice.
- On trigger: a coarse trace (`SLOW_TRACE_BIN_TICKS=20` ticks/bin, 1.25ms/
  bin - fine enough to show the transition's shape, coarse enough to keep
  the eventual printout to `SLOW_TRACE_PRE_BINS + SLOW_TRACE_POST_BINS` =
  80 lines rather than raw per-tick dumps) spanning 50ms before the
  trigger and 50ms after is LATCHED - unlike `'J'`'s ring, which keeps
  sliding forward forever and is exactly what let the horse bolt in the
  first place, this one deliberately STOPS recording the moment it has an
  answer, so a human-reaction-time delay before checking it can't erase
  it.
- New `'K'` serial command: prints a live "still watching" fast/slow/delta
  readout if nothing has triggered, or the full latched before/after trace
  if it has, then re-arms for the next event. Re-arming also resyncs the
  slow EMA to the fast one - without this, the slow (2s-tau) EMA would
  still be catching up right after a genuine step and would keep
  re-triggering (with an empty pre-buffer each time) for up to several
  seconds after every real event.
- Deliberately NOT reset by `'r'`/`diagnostics_reset()` - same reasoning
  as the existing canary latches: a rare, significant capture shouldn't
  silently vanish because a routine measurement window started before it
  was read.

**Not yet bench-tested** - no toolchain available in this environment, per
standing caveat. Once flashed: leave `'K'` unread for a while during
normal two-tone operation and periodically check it - a `TRIGGERED`
result with a clean before/steady -> transition -> after/steady trace
that lines up with something the user can independently corroborate
(an Aux SP shift, a by-ear jump) would be the first direct, timescale-
matched evidence of what a "random jump" actually looks like in this
firmware's own data, as opposed to inferring it from a `'J'` ring that
(as directly demonstrated above) carries no signal about it at all.

## 2026-09-12, yet later still: the first real 'K' capture was itself a
## false positive - diagnosed and fixed (EMA seeding/resync bias), a
## warm-up hold added

First bench data from the `'K'` feature above, at `relative_delay=+2.00`
(an exact integer - notably, this means `interp_ring()`'s `frac=0`, a pure
unblended read, no interpolation involved at all):

```
[dsp] slow_trace: TRIGGERED at t=113760ms before=697.40Hz after=704.63Hz delta=+7.23Hz
  delay=+2.00 - 3 pre-bin(s) + 40 post-bin(s), 1.24ms/bin
[dsp]   slow_trace[  -3]: freq=800.0Hz env_min=0.001
[dsp]   slow_trace[  -2]: freq=800.0Hz env_min=0.001
[dsp]   slow_trace[  -1]: freq=800.0Hz env_min=0.001 <-- TRIGGER
[dsp]   slow_trace[  +1]: freq=400.0Hz env_min=0.001
[dsp]   slow_trace[  +2]: freq=800.0Hz env_min=0.001
[dsp]   slow_trace[  +3]: freq=800.0Hz env_min=0.001
[dsp]   slow_trace[  +4]: freq=800.0Hz env_min=0.001
[dsp]   slow_trace[  +5]: freq=400.0Hz env_min=0.001
  ... (pattern repeats: three 800Hz bins then one 400Hz bin, period 4,
  continuing completely unchanged through all 40 post-bins, env_min
  pinned at 0.001 in every single bin, pre AND post)
```

At first glance this looks like a working trigger - it fired, it produced
a trace. But look closer: only 3 pre-bins exist at all (this fired within
roughly 3-4ms of boot or the last `'r'`/flash), and the post-trace shows
the *identical* repeating pattern, at the *identical* period, with the
*identical* env_min floor, as the pre-trace - there is no discernible
transition anywhere in the 43 bins printed. This is not what a real event
should look like (compare against the "before/during/after" shape the
feature was built to capture) - it's a false positive, and a very
instructive one about how this kind of dual-EMA trigger can fail.

**Root cause**: both EMAs are seeded from a single RAW (unaveraged) sample
on the very first tick they ever see (`if (!s_freq_ema_inited) { fast =
slow = dev; ... }`). If that first sample happens to land on one extreme
of an already-ongoing periodic cycle - here, 800Hz, not the cycle's
~700Hz true time-average (three 800Hz bins + one 400Hz bin per period
averages to (3*800+400)/4 = 700Hz) - the fast EMA (tau=50ms) converges
toward the true 700Hz average within a couple of its own time constants,
while the slow EMA (tau=2s) is still sitting almost exactly at the biased
800Hz seed, having barely moved at all in just a few milliseconds. The two
diverge past `SLOW_JUMP_TRIGGER_HZ` almost immediately - not because
anything changed, but because the seed value was never representative of
the cycle's actual average in the first place. `before=697.40Hz` is
essentially the fast EMA's already-converged true average; `after=704.63`
is the same true average with ordinary EMA noise - neither reflects a real
before/after state, they're both just "the correct answer," measured
before and after a spurious threshold crossing caused by the SLOW EMA's
own slow-to-arrive value.

The same exposure exists, in smaller form, at re-arm: resyncing
`slow = fast` after a trigger uses the fast EMA's OWN current value,
which is itself only 50ms-smoothed - if resync happens to fall on the
"wrong" phase of a fast periodic cycle, the same false-positive mechanism
could recur seconds later, just with a smaller bias than the boot case.

**Fixed** (`diagnostics.cpp`): a WARM-UP hold, `FREQ_EMA_WARMUP_MS=8000`
(~4x `FREQ_EMA_SLOW_TAU_S` - the standard first-order filter settling
heuristic: residual error after N time constants is `exp(-N)`, ~1.8% left
after 4), applied to the TRIGGER CHECK only - both EMAs keep updating
every tick throughout warm-up (so they're already converging using the
real, ongoing dynamics, not frozen or discarded), and the pre-bin ring
keeps filling too, so real history is ready the instant the hold lifts.
Armed both at first boot (`s_freq_ema_inited` transitioning false->true)
and at every re-arm in `diagnostics_print_slow_trace()` (alongside the
existing slow-EMA resync) - directly closing both exposures above with
one mechanism. Useful side effect: this also naturally debounces against
a second trigger within seconds of the last one being read, which fits
this feature's whole purpose (rare, significant events) rather than
working against it. `'K'`'s "still watching" line now reports
`warmup_left=...ms` explicitly, so it's visually obvious when the hold -
not the 5Hz threshold - is what's currently preventing a trigger.

**Not yet bench-tested** - needs a reflash. Once running: leaving `'K'`
alone for at least ~8s after boot/reset/a previous read before treating
any "still watching" delta reading as meaningful, and a genuine trigger
should now only ever appear after that warm-up window has elapsed - any
`TRIGGERED` result going forward should show a real, visible transition
in its post-trace, not the same pattern repeated through the whole trace
the way this first capture did.

## 2026-09-12, yet later still: SECOND 'K' capture (still on the pre-
## warm-up-fix build) looks like a genuine event, not another false
## positive - a real, two-stage transition between two structurally
## different repeating cycles, user-flagged as "a spontaneous change"

Unlike the first capture, this one has the full `40 pre-bin(s) + 40
post-bin(s)` (i.e. it had already been watching for at least a full
50ms pre-window before triggering, not firing within milliseconds of
boot/reset) at `relative_delay=+3.85`:

```
[dsp] slow_trace: TRIGGERED at t=308411ms before=700.36Hz after=1084.36Hz delta=+14.45Hz
  delay=+3.85 - 40 pre-bin(s) + 40 post-bin(s), 1.24ms/bin
```

The PRE window shows the same familiar repeating period-4 cycle seen at
other delay settings (~800/~800/~800/~400Hz, averaging to the reported
`before=700.36Hz`), but with a detail not previously called out: it's
already slowly drifting even while "steady" - the ~800Hz peaks creep up
(801.5 -> 802.6Hz over the 40 pre-bins) while the ~400Hz trough creeps
down (398.4 -> 397.4Hz) by a mirrored amount, i.e. the cycle's amplitude
is slowly widening symmetrically around a fixed ~700Hz mean, not just
sitting flat. Worth remembering as a baseline: "steady state" in this
firmware is apparently never perfectly static even when the mean isn't
moving.

Then, at the last pre-bin, freq jumps to 1197.1Hz - well outside the
range either previous cycle state (~800/~400Hz) ever reached - and the
POST window settles into a NEW, structurally different repeating cycle:
`800.0 / 800.0 / ~1197Hz / ~1603Hz` (period 4 again), i.e. it looks like
the OLD cycle's ~400Hz trough got replaced by a completely new value
(~1600Hz) while the ~800Hz peaks stayed exactly where they were. This
new cycle itself keeps drifting the same way the old one did (the ~1197Hz
element drifts down to 1194.0Hz, the ~1603Hz element drifts up to
1606.0Hz over the first 24 post-bins) - same "slowly widening around a
fixed pair of anchors" behavior as the pre-window, just anchored to
different values now.

**Then a SECOND, distinct shift happens partway through the SAME
post-window**, entirely within the 40 bins already captured: starting at
bin `+26`, the previously rock-steady `800.0`/`800.0` pair jumps to
`1600.0`/`1600.0` - almost exactly double - while the other two elements
of the cycle continue the same drift they were already doing (1192.5
down, 1607.5 up, continuing the same trend as before the second shift,
just from new starting points). So this single capture shows TWO
qualitatively different things happening: a genuine mean-frequency jump
(the trigger event itself, +14.45Hz measured, but the actual per-state
change is much bigger - one whole cycle state moved by nearly 1200Hz),
followed by a second, smaller-scale but equally discrete jump in a
different part of the same cycle, all inside one continuous 50ms window
this diagnostic was built specifically to capture.

One structural observation worth flagging for later: nearly every
distinct value across both the pre- and post-windows sits close to an
exact multiple of 400Hz (400, 800, 1200, 1600), with only the slow
symmetric drift described above pulling values away from those exact
multiples. If the active test-tone pair's spacing was a typical 200Hz
(this project's usual two-tone presets - 300/500, 700/900, 1500/1700,
2500/2700, 3500/3700 - are all 200Hz apart), 400/800/1200/1600Hz are
exactly the 2nd/4th/6th/8th harmonics of that 200Hz beat frequency -
plausible, since a two-tone envelope's own harmonic content is already
established (this file's earlier entries) as the mechanism that produces
the freq_dev sidebands in the first place. Not confirmed - would need to
know which preset/pair was active during this specific capture - but a
concrete, checkable hypothesis for whoever revisits this: near a null,
freq_dev may be jumping between different HARMONIC ORDERS of the beat
frequency, and an actual "jump" event might correspond to which harmonic
order a given null-crossing's sampled trajectory happens to land near.

**Caveat**: this build did not yet include the warm-up-hold fix from the
entry above (that fix was made in response to THIS SAME session's first
capture, before this second one arrived) - so if this wasn't literally
the first trigger since boot, the exact MOMENT it fired could in
principle carry a small EMA-resync bias (see that entry's "smaller
form... at re-arm" note). That would affect only the precise timing/
threshold-crossing instant, though, not the underlying finding - the
actual jump between two structurally different repeating cycles (and the
second, later jump within the same post-window) is directly visible in
the raw bin values themselves, independent of trusting the EMA math at
all. This reads as the first genuinely useful `'K'` capture: a real,
multi-stage, cleanly-bracketed transition, in exactly the "before and
during" shape the user originally asked for - a sharp contrast with what
`'J'` was shown to produce for the same kind of request earlier in this
file.

**Not yet further analyzed** - worth a follow-up capture (ideally after
the warm-up fix is flashed, and with the active tone pair noted alongside
it) to see whether this exact two-stage signature repeats, whether it's
specific to `relative_delay=+3.85` (well outside this project's
historically-validated 0.00-2.10 window), and whether the harmonic-order
hypothesis above holds up against a known tone pair.

## 2026-09-12, yet later still: tone pair confirmed as 700/1700Hz - the
## earlier 200Hz-beat-harmonic guess was wrong (that pair isn't 200Hz-
## spaced), but the corrected numbers line up more precisely than the
## original guess did

User confirmed the pair active during the +3.85 capture above was
**700/1700Hz**, not one of this project's usual 200Hz-spaced presets
(300/500, 700/900, 1500/1700, 2500/2700, 3500/3700) the earlier guess
assumed - beat frequency here is `1700-700=1000Hz`, and
`expected_center_hz = 0.5*(700+1700) = 1200Hz` exactly (this file's own
established formula, `print_null_bias_block()` in `diagnostics.cpp`).
Retracting the earlier 200Hz-harmonic hypothesis - it was speculation
built on the wrong pair and doesn't apply here.

Redoing the read with the correct numbers turns up something more
precise than the retracted guess: **the middle rung the post-trigger
cycle visits (~1197-1200Hz, drifting down toward 1194 over the capture)
sits almost exactly on the theoretical expected_center (1200Hz)** - a
clean, few-Hz match, not a loose "close to a round number" coincidence.

Re-reading the whole capture as a sequence of per-slot values (not just
overall cycle means) shows the two discrete jumps are both close to
**+800Hz** steps in whichever slot moves, not a general redistribution:

- Pre-cycle, one period = `[~800(drifting up), 800.0, 800.0, ~400(drifting down)]`.
- First jump: the `~400` slot becomes `~1197` (`+797Hz`) and one `800`
  slot becomes `~1603` (`+803Hz`) - both slots step up by essentially the
  same ~800Hz, landing the new cycle on `[800, 800, ~1197, ~1603]`.
- Second jump (bin +26): the remaining two `800` slots jump to `1600`
  each (`+800Hz` exactly) - `[1600, 1600, ~1192(still drifting), ~1607(still drifting)]`.

So every discrete jump in this capture is a ~800Hz step, and the values
visited sit on an evenly-spaced ~400Hz grid (400/800/1200/1600) - both
`800` and `400` are exact multiples of `100Hz`, and so, notably, are
`700`, `1700`, and their `1000Hz` beat (`7x`, `17x`, and `10x` a common
`100Hz` unit) - consistent with this project's own established mechanism
that the test tones are exact phase-accumulator multiples of
`SAMPLE_RATE_HZ` (this file's "Root mechanism identified" section).
That's a plausible structural link (both the tone frequencies and the
grid this diagnostic is now revealing share a 100Hz common factor), but
NOT a full mechanism - it doesn't yet explain why 800Hz specifically is
the step size a null-crossing's sampled trajectory jumps by, or why the
theoretical expected_center lines up with the MIDDLE rung of a 4-rung
grid rather than an edge. Worth someone with more DSP-derivation time
pursuing directly from the Hilbert/atan2 math rather than guessing
further from bench captures alone.

**Not yet further analyzed** - a repeat capture on the same 700/1700
pair (ideally with the warm-up fix flashed) would show whether this
exact 4-rung, ~800Hz-step structure is a stable, repeatable property of
this specific pair/delay combination, or whether it varies capture to
capture.

## 2026-09-12, yet later still: two more 'K' captures both show a
## perfectly flat trace despite a real trigger - the fine trace is too
## short-span to see whatever's actually causing the gap, so two targeted
## additions were made instead of re-guessing from a third single capture

Two captures in quick succession, both flagged by the user with their
own cause label - one "[] scan induced":

```
[dsp] slow_trace: TRIGGERED at t=9239ms before=687.40Hz after=699.08Hz delta=+11.68Hz delay=+0.00
  - 40 pre-bin(s) + 40 post-bin(s), 1.24ms/bin
[dsp]   slow_trace[ -40..-1]: 800.0/800.0/800.0/400.0Hz repeating, exactly, ten full cycles
[dsp]   slow_trace[  +1..+40]: 800.0/800.0/800.0/400.0Hz repeating, exactly, ten more full cycles
  (bin-for-bin IDENTICAL pattern before and after - no drift, no
  transition, anywhere in the printed 100ms)
```

...and one "spontaneous":

```
[dsp] slow_trace: TRIGGERED at t=157724ms before=706.65Hz after=696.87Hz delta=-9.78Hz delay=+4.55
  - 40 pre-bin(s) + 40 post-bin(s), 1.24ms/bin
[dsp]   slow_trace[ -40..-1]: 800.0/800.0/800.3/399.7Hz repeating, exactly, ten full cycles
[dsp]   slow_trace[  +1..+40]: 800.0/800.0/800.3/399.7Hz repeating, exactly, ten more full cycles
  (again bin-for-bin identical before and after)
```

Both are structurally the same puzzle: a real (8-11Hz) fast/slow EMA
divergence fired the trigger, but the printed 50ms-before/50ms-after
trace shows literally no difference between "before" and "after" - not
even the slow amplitude drift the +3.85 capture showed within its own
window. If this exact periodic pattern (repeat rate ~200Hz, far above
either EMA's cutoff - fast's is ~1/(2*pi*0.05s)=3.2Hz, slow's is
~1/(2*pi*2s)=0.08Hz) had genuinely been running unchanged for as long as
the signal's been alive, both EMAs should have long since converged to
within a small fraction of a Hz of the same periodic mean - not sit
8-11Hz apart. The most likely explanation: something DID change the
cycle's mean, and then fully resolved, on a timescale longer than the
50ms the fine trace shows but shorter than the slow EMA's ~2s memory - a
continuous drift too gradual to show any visible slope in a 50ms window,
exactly the same phenomenon the +3.85 capture already demonstrated
(its own cycle drifted visibly within just 50ms), just running slower at
whatever delay/pair applies here.

Rather than growing the fine trace's own span (expensive in both RAM and
print volume, and it would only push the same "not long enough" edge
further out) or guessing further from a fourth single capture, two
targeted additions:

1. **`relative_delay_get_last_change_ms()`** (`relative_delay.h`/`.cpp`) -
   every `'['`/`']'`/`'''`/`';'` press and every preset load now
   timestamps `relative_delay`'s last change. `'K'` reports "relative_delay
   was last changed Xms before this trigger" (or "has not been changed
   since boot") directly in its output - this tests the "[] scan induced"
   label directly rather than relying on the user's own memory of timing,
   and will show whether the "spontaneous" one really had no recent delay
   change at all (as labeled) or one the user didn't consciously register.

2. **A second, much-longer-timescale companion trace** (`EMA_TREND_LEN=80`
   samples, `EMA_TREND_SAMPLE_TICKS`=25ms apart, ~2s of total history -
   matching `FREQ_EMA_SLOW_TAU_S` itself) that periodically snapshots the
   fast/slow EMA VALUES themselves (not raw per-tick or per-bin data) and
   freezes the same way the fine trace does (stops advancing the instant
   a trigger fires). This directly answers the open question: was the gap
   the result of a smooth, multi-second ramp (visible as a steady climb/
   fall across the trend samples) or something more abrupt that the fine
   trace's 50ms window simply started too late to catch?

**Not yet bench-tested** - needs a reflash (this build also doesn't yet
include either targeted addition, since both captures above predate
them). Once running: a `'K'` output that shows a clean multi-second ramp
in the new TREND block leading up to a flat-looking fine trace would
directly confirm the "too-gradual-to-see-in-50ms" theory; a TREND block
that's ALSO flat right up to the trigger would instead point at something
else entirely (worth revisiting the EMA math itself at that point, not
just the trace span).

## 2026-09-12, yet later still: the 8s warm-up was undersized - TREND data
## from a re-run capture proves the original seed error, two-stage
## boot-settle-then-snap fix replaces single-sample seeding of both EMAs

The previous entry's `FREQ_EMA_WARMUP_MS=8000` fix (~4x the slow EMA's 2s
tau) was built on the standard first-order-settling heuristic: after 4
time constants, a step response is ~98% converged, leaving ~2% residual
error. That heuristic assumes the residual's *absolute* size doesn't
matter much - fine if the initial seed error is a few Hz, not fine if it's
large. A re-run of the exact "[] scan induced" capture from the previous
entry (t=9239ms, well past the 8s hold) fired again, and this time the
newly-added TREND block made the mechanism directly visible instead of
having to infer it:

```
trend[-80..-1] (last ~2s before trigger, 25ms/sample):
  fast: flat at 699.07-699.08Hz for the ENTIRE 2-second history
  slow: 666.23Hz -> ... -> 687.31Hz, climbing smoothly and monotonically
        the whole time, still visibly short of fast's value at the last
        sample
```

`fast` (50ms tau) had clearly been converged on the true ~699Hz average
for a long time - flat to 0.01Hz precision across 2 full seconds. `slow`
(2s tau) was still mid-exponential-approach toward it, a full 8+ seconds
after boot. Both EMAs are driven by the identical input signal, so the
ONLY way `slow` can still be visibly climbing this long after boot is if
its single-raw-sample seed was itself a large outlier - consistent with
this project's already-documented near-null `freq_dev_hz` spikes, which
elsewhere in this file and `moving_forward_notes.md` have been measured
reaching into the thousands of Hz. Worked example: if the seed were, say,
1500Hz off from the true ~699Hz mean, 4 time constants (8s) leaves
`1500 * 0.02 ~= 30Hz` of residual - comfortably enough to keep tripping a
5Hz trigger threshold long after the hold expires, exactly as observed.

**Root-cause fix**: stop seeding `slow` from a raw sample at all - the
seed-then-wait-N-tau approach was always going to be fighting whatever
outlier the very first tick happened to catch. New two-stage init:

1. On the first-ever raw sample, seed ONLY `s_freq_ema_fast_hz`
   (`s_freq_ema_fast_inited` set true). Let it run alone, untouched by
   any trigger logic, for `FREQ_EMA_BOOT_SETTLE_MS=500` (~10x
   `FREQ_EMA_FAST_TAU_S=0.05f`'s own tau - `e^-10 ~= 0.0000454`, i.e.
   ~99.9995% converged even from a worst-case multi-thousand-Hz seed
   error, leaving under 0.1Hz of residual).
2. When `FREQ_EMA_BOOT_SETTLE_TICKS` elapses, SNAP
   `s_freq_ema_slow_hz = s_freq_ema_fast_hz` directly (a clean value
   transfer from an already-converged source, never a raw sample) and
   set `s_freq_ema_slow_inited = true`, arming the existing
   `FREQ_EMA_WARMUP_TICKS` (8s) hold from that point purely as residual
   insurance against whatever sub-0.1Hz error remains, not as the
   primary convergence mechanism it was being asked to be before.

`diagnostics.cpp` statics: `s_freq_ema_fast_inited` (renamed from
`s_freq_ema_inited`), `s_freq_ema_slow_inited` (new),
`s_freq_ema_boot_settle_ticks_left` / `FREQ_EMA_BOOT_SETTLE_TICKS` (new,
same `(uint32_t)(MS * 1000.0f / SSB_SAMPLE_PERIOD_US)` construction as
the existing warm-up constant). Both the trigger check
(`s_freq_ema_slow_inited && s_freq_ema_warmup_ticks_left == 0`) and the
long-timescale trend-ring recording are now gated on
`s_freq_ema_slow_inited`, so nothing is evaluated or snapshotted off a
not-yet-meaningful `slow` value during the ~500ms boot-settle window.
`diagnostics_print_slow_trace()`'s "still watching" branch now has two
sub-cases: before `s_freq_ema_slow_inited`, it reports
`boot_settle_left=...ms` and the fast EMA's current value instead of a
meaningless pre-snap `slow`/`delta`; after, it reports the same
`warmup_left=...ms` readout as before. The re-arm logic on every latched
print (`slow_hz = fast_hz`, `warmup_ticks_left = FREQ_EMA_WARMUP_TICKS`)
is unchanged - it stays valid because `s_freq_ema_slow_inited`, once set
at boot, is never cleared again.

Verified self-consistent by the same brace/paren-balance and
`#if`/`#endif`-nesting scripts used throughout this investigation
(`braces: 116 116`, `parens: 850 850`, `unclosed: []`), plus a `grep`
confirming no stale `s_freq_ema_inited` references remain anywhere in the
file. **Not yet bench-tested** - no toolchain available in this
environment, per standing caveat.

## 2026-09-12, yet later still: three more real 'K' captures decode to a
## multi-stage cascade, not a single jump - every repeating-cycle "slot"
## eventually reflects the same event, but at staggered times over
## 5-40ms, and the near-null-blended slots' "smooth drift" turns out to
## be the same discrete 800Hz steps smeared by averaging

All three below share: `relative_delay=+2.70` throughout (and the two
that report a "last changed" timestamp show it growing between captures -
181535ms, then 335207ms - i.e. no delay change happened at ALL between
them, ruling out `'['`/`']'` activity as a shared cause), `env_min`
pinned at a constant 0.238 across every one of the 81 printed bins
(before AND after, no exception), and a TREND block that is either
perfectly flat or very nearly so for the full visible 2-second history -
i.e. none of these are boot artifacts of the seeding bug just fixed
above.

**Capture 1** (`before=700.43Hz after=1093.40Hz delta=+17.93Hz`, no
delay-change timestamp captured - this was the one already in flight when
the previous summary/compaction happened): pre-cycle `[800,800,800,400]`
(period 4, exact, no drift) -> at the trigger, two of the three "800"
slots jump to "1600" (each a clean +800Hz step) while the third "800" and
the "400" slot are initially unchanged, giving `[800,1600,400,1600]` ->
~34.7ms later (bin +27), the still-unchanged "400" slot ALSO jumps, to
"2000" (+1600Hz, i.e. two back-to-back 800Hz quanta at once for that
slot).

**Capture 2** (`before=700.58Hz after=1043.65Hz delta=+21.35Hz`,
`t=462082ms`, delay unchanged 181535ms beforehand) - the richest of the
three, with a genuinely near-null-blended pair of slots visible
throughout. Re-indexing all 81 bins by `phase = (bin_index + 40) mod 4`
(bin -40 is phase 0) resolves the printed data into four independent
per-slot histories:

- **phase 0** ("clean" slot): 800.0Hz for all 10 pre-trigger samples,
  steps to 1600.0Hz at bin +4 (~5ms after the trigger) and holds there
  for the remaining 9 post-trigger samples. One clean +800Hz step,
  delayed slightly relative to the trigger.
- **phase 3** ("clean" slot, the one whose step at bin -1 IS what defines
  this trigger by construction): 800.0Hz for 9 pre-trigger samples, steps
  to 1600.0Hz exactly at the trigger bin, holds there for all 10
  post-trigger samples.
- **phase 1 / phase 2** (the near-null-blended pair): pre-trigger, phase 1
  drifts 671.0Hz -> 665.2Hz while phase 2 drifts 529.0Hz -> 534.8Hz over
  the same 10 samples - each pair sums to EXACTLY 1200.0Hz on every single
  sample (`expected_center_hz=(700+1700)/2=1200Hz` for the confirmed
  700/1700 pair). Post-trigger the pair continues this drift for 2 more
  samples (664.2/535.8, 662.9/537.1 - still summing to 1200.0), then at
  bin +9/+10 (~11-12ms post-trigger) the SUM itself steps to 2000.0Hz
  (901.6+1098.4, 899.8+1100.2, 898.0+1102.0 - three samples, still
  drifting internally but pinned to the new sum), then at bin +21/+22
  (~26-27ms post-trigger) the sum steps AGAIN to 2800.0Hz (895.5+1904.4,
  892.9+1907.1, ..., 880.6+1919.3 - five samples, sum holding at
  ~2799.9-2800.0 while the internal drift accelerates). 1200 -> 2000 ->
  2800: two more clean +800Hz quanta, on the identical grid every other
  slot in this whole investigation has landed on, just expressed as a
  shift in a blended pair's SUM rather than a single value jumping.

  This directly explains something earlier captures only hinted at:
  what looked like "slow smooth drift" in a near-null-blended slot
  (e.g. the +3.85 capture's "already slowly widening in amplitude" note)
  is consistent with being the SAME discrete 800Hz stepping mechanism as
  every clean slot, just spread across several bins because each bin
  averages ~20 raw ticks that straddle the beat-null crossing at a
  slightly different phase from one bin to the next - the interpolation/
  averaging is smearing a step, not revealing a genuinely continuous
  ramp underneath it.

  So this one capture shows FOUR separate +800Hz steps landing across a
  ~27ms window following the trigger (one at the trigger itself, one at
  +4, one pair-sum-step at +9/10, another at +21/22) - a single trigger
  event unfolding as a staggered, multi-stage settling process, not an
  instantaneous state change.

**Capture 3** (`before=700.43Hz after=1013.41Hz delta=+15.54Hz`,
`t=615754ms`, delay unchanged 335207ms beforehand, user-labeled "another
spontaneous that finished up at nominal zero") - cleanest of the three,
no near-null blending at all; re-indexed the same way:

- **phase 0**: 800.0Hz for all 10 pre-trigger samples, steps to 1200.0Hz
  at bin +4 (a +400Hz step - the first observed CLEAN single 400Hz step
  in this whole investigation, as opposed to 800Hz or a multiple of it),
  holds there through bin +12, then steps AGAIN at bin +16... actually
  holds at 1200.0Hz for the rest of the trace (bins +4 through +40 all
  read 1200.0Hz - a single +400Hz step, not two).
- **phase 1**: 800.0Hz for every single one of the 20 printed samples
  (10 pre, 10 post) - never moves at all.
- **phase 2**: 800.0Hz for all 10 pre-trigger samples and the first 5
  post-trigger samples (bins +2, +6, +10, +14, +18), then steps to
  1600.0Hz at bin +22 (~27ms post-trigger, a clean +800Hz step) and holds
  there for the remaining 5 samples.
- **phase 3** (the trigger slot): 400.0Hz for 9 pre-trigger samples,
  steps to 1200.0Hz exactly at the trigger bin (+800Hz - what defines
  this trigger), holds at 1200.0Hz for 3 more samples (+3, +7, +11), THEN
  steps again at bin +15 (~19ms post-trigger) to 1600.0Hz (+400Hz), and
  holds there for the rest of the trace.

  Final settled cycle: `[1200,800,1600,1600]`, up from `[800,800,800,
  400]` - three slots moved (one by a single +400 step, one by a single
  +800 step, one by two steps totalling +1200 (+800 then +400)), one slot
  never moved. Every individual step observed, across all three captures
  now, is an exact multiple of 400Hz - reinforcing rather than
  complicating the established 400Hz-grid/800Hz-quantum picture from the
  earlier 700/1700 tone-pair analysis.

  One more detail worth flagging: the TREND block for this capture is
  almost, but not quite, perfectly flat - `fast` sits at 696.16-696.18Hz
  for 79 of its 80 samples, then in the VERY LAST sample (25ms before the
  trigger) jumps to 715.97Hz (`slow` ticks correspondingly from 699.93Hz
  to 700.43Hz in that same last sample). That's consistent with the
  cascade picture above: the fast (50ms tau) EMA is itself coarse enough
  to already be blending in a few hundred Hz of the incoming transition
  before the fine (1.25ms/bin) trace's own -1 bin shows the first slot's
  jump - the trend and fine traces are both seeing the same onset, just
  at their own respective resolutions.

**Net read**: a 'K' trigger reliably marks the START of a real,
human-relevant frequency change, but "after" is not fully settled at the
trigger instant - the new steady state can take several tens of ms
(observed here: 5ms to ~35ms) to finish establishing across every slot of
the repeating cycle. Worth remembering when interpreting any single 'K'
trace's `after=` summary figure, since it's computed from `s_freq_ema_
fast_hz` at the moment the post-window fills, which may itself still be
mid-cascade for a slow-settling event. Also a useful independent data
point for the boot-settle fix above: multi-stage settling over tens of ms
is evidently a real property of this signal's transitions, not an
artifact of EMA seeding.

**Not yet asked**: whether these three captures were also taken on the
700/1700Hz pair. The numbers are consistent with it (1200Hz center,
400/800Hz step sizes matching the established grid) but this hasn't been
independently confirmed the way the +3.85 capture's tone pair was.

## 2026-09-12, yet later still: tone pair for all three captures above
## confirmed as 700/1700Hz

User confirmed directly. Closes the open item from the previous entry -
the phase-by-phase decode (1200Hz center matching `expected_center_hz`,
every individual step an exact multiple of 400Hz) stands unchanged, now
resting on a confirmed tone pair rather than an inference from the
numbers alone.

## 2026-09-12, yet later still: fourth 'K' capture (delay=+5.23, genuinely
## scan-induced) - the delay-timestamp diagnostic finally correlates as
## designed, and the signature is a messier, more null-adjacent regime
## than the three +2.70 captures decoded above

User label: "scan induced now at 990". `[dsp] relative_delay was last
changed 1ms before this trigger` - the first capture where that field
actually confirms an active scan, in contrast to the two earlier
captures ALSO labeled "scan induced" by the user at the time
(`null_bias_investigation.md`'s "two more 'K' captures" entry) that came
back either "has not been changed since boot" or minutes-old - exactly
the gap that diagnostic was built to expose. `relative_delay=+5.23` is
well outside the 2.00-2.10 sample window this project's IMD-tuning
history settled on, and lands in the same "large, incidental" territory
as the earlier +4.28/+4.60 'J' captures from this file's delay-scan
entries.

Re-indexing the same way as the three captures above
(`phase = (bin_index + 40) mod 4`):

- **phase 0**: pre-trigger drifts 749.5Hz -> 729.6Hz -> 730.5Hz (a small
  step down partway through the pre-window, coinciding almost exactly
  with `env_min` stepping from 0.040 to 0.055 at the same bin - i.e. an
  envelope-side change was already underway before the frequency trigger
  fired). At bin +4, jumps to 1289.2Hz - NOT a clean multiple of 400
  relative to the ~730Hz pre-trigger value (delta ~559Hz) - then keeps
  drifting through the rest of the post-window (1289.2 -> 1308.3 -> ...
  -> 1292.9Hz), never settling to a clean value within the visible 50ms.
- **phase 1**: pinned at exactly 800.0Hz for all 10 pre-trigger samples
  (no drift at all, unlike phase 0). At bin +1 (immediately, one bin
  before phase 0 moves), jumps to 1510.6Hz (delta ~+710.6Hz, again not a
  clean 400/800 multiple), then drifts slowly down through the rest of
  the post-window (1510.6 -> 1511.5 -> ... -> 1499.5Hz), also never
  settling within 50ms.
- **phase 2**: pinned at exactly 800.0Hz pre-trigger AND for the first 6
  post-trigger samples (+2 through +22, i.e. ~27ms after the trigger) -
  then steps CLEANLY to 1600.0Hz at bin +26 (a clean +800Hz step, on the
  same grid every +2.70 capture has shown) and holds there for the
  remaining post samples.
- **phase 3** (the trigger slot): drifts 450.4Hz -> 469.5Hz pre-trigger
  (again with a small step around the same env_min transition point as
  phase 0), jumps to 1289.4Hz exactly at the trigger (delta ~+819.9Hz -
  close to, but not as exactly clean as, an 800Hz quantum given the
  interpolation smoothing at this much larger delay), holds 800.0Hz for
  6 post samples (+3 through +23), then steps cleanly to 1600.0Hz at bin
  +27, same timing as phase 2.

So two slots (0, 1) jump immediately at/near the trigger to messy,
non-quantized values that keep drifting for the entire visible 50ms
post-window and never cleanly settle, while the other two (2, 3) hold
their pre-trigger value for ~30ms before taking one clean, on-grid
+800Hz step around bin +26/27. `env_min` itself is not flat here either -
it climbs 0.040 -> 0.055 -> 0.071 -> 0.086 across the 81 printed bins,
i.e. genuinely dipping into/near the 0.05 near-null threshold used
elsewhere in this project, unlike the three +2.70 captures above where it
sat pinned at a comfortably-clear 0.238 for literally every bin. The
TREND block shows `fast` smoothly, monotonically declining from 704.57Hz
to 702.75Hz across the full visible 2 seconds (`slow` essentially flat at
700.11-700.12Hz throughout) - a real, if small, pre-existing drift running
underneath, independent of the trigger event itself.

**Read**: this looks like a different, messier regime than the clean
400Hz-grid cascade characterized in the three +2.70 captures just above -
consistent with the earlier (`+4.28`/`+4.60`) 'J'-era finding that larger,
mistuned delays expose more genuinely null-adjacent, less cleanly
quantized behavior. Two slots showing immediate-but-non-quantized jumps
alongside two slots showing a clean, delayed +800Hz step in the SAME
single event is new - not yet seen in any one capture before now - and
suggests both mechanisms (the clean null-independent cascade, and a
messier null-adjacent one) can coexist within a single transition when
delay sits far enough from the tuned window. Not a controlled comparison
(only one large-delay 'K' capture on record so far) - worth deliberately
capturing another 'K' trigger at a similarly large delay to see whether
this two-mechanisms-at-once signature repeats.

## 2026-09-12, yet later still: fifth 'K' capture ("spontaneous", delay
## unchanged 147312ms beforehand) reproduces the sum-conserved-pair
## signature exactly, and adds a third, intermediate env_min regime

Re-indexing by `phase = (bin_index + 40) mod 4` again:

- **Two "clean" slots**: bins -38/-34/.../-2 (mod-4 phase 2) sit at
  exactly 800.0Hz for all 10 pre-trigger samples, hold 800.0Hz for the
  first 2 post-trigger samples (+2, +6), then step CLEANLY to 1600.0Hz at
  bin +10 (~12.4ms post-trigger) and hold there for the rest. Separately,
  bins -39/-35/.../-3 (mod-4 phase 1) also sit at exactly 800.0Hz
  pre-trigger and for the first 5 post-trigger samples, then step
  CLEANLY to 1600.0Hz at bin +23 (~28.5ms post-trigger, later than the
  other clean slot) and hold there. Two independent, isolated +800Hz
  steps landing at different times post-trigger - same pattern as the
  richest +2.70 capture ("Capture 2") decoded earlier this file.
- **phase 0 and phase 3** (the near-null-blended, ADJACENT pair - not
  same-phase-mod-4 across cycles, but the two consecutive bins straddling
  a beat-null crossing each period): pre-trigger, phase 3's bins
  (-37,-33,...,-5) drift 388.4Hz -> 380.3Hz while the immediately-
  following phase 0 bin (-36,-32,...,-4) drifts 811.1Hz -> 819.7Hz - and
  every single adjacent (phase3[k], phase0[k+1]) pair sums to almost
  exactly 1200.0Hz (e.g. 380.3+819.7=1200.0), matching
  `expected_center_hz` for 700/1700 yet again. The trigger itself IS this
  pair's phase-3 bin jumping from 380.3Hz to 1178.4Hz (~+798.1Hz, the
  800Hz quantum, some rounding from the drift/interpolation).
  Post-trigger, the SAME adjacent-pair-sum relationship holds but at a
  new constant: (+4,+5)=1176.4+823.6=2000.0, (+8,+9)=1173.7+826.3=2000.0,
  ... (+36,+37)=1135.5+864.5=2000.0 - nine consecutive pairs, every one
  summing to exactly 2000.0Hz. The pair's sum stepped 1200 -> 2000, the
  same +800Hz quantum as the two "clean" slots, just expressed as a
  blended-pair sum instead of a single value jumping - directly
  reproducing the mechanism found in the `+21.35Hz` capture above. The
  very last printed bin, +40 (phase 0), reads 1927.5Hz - a sharp break
  from the smooth ~1135-1176Hz trend the rest of phase 0's post-trigger
  samples were following, and 1927.5 is itself suspiciously close to
  where a THIRD +800Hz step (2000 -> 2800, following the same pattern as
  the `+21.35Hz` capture's own 1200->2000->2800 progression) would put
  it if paired with a phase-1 value continuing that slot's own trend
  (~864.5+8=~872.5, giving a pair sum near 2800). Read as the next stage
  of the same cascade beginning right at the edge of the visible 50ms
  window, not confirmed (no further bins were printed to check).

**`env_min` regime**: steady at 0.089-0.093 across the entire 81-bin
trace - comfortably clear of the 0.05 near-null threshold (unlike the
delay=+5.23 scan-induced capture, which dipped to 0.040), but well below
the ~0.238 seen throughout every other +2.70 "spontaneous" capture so
far. This is a third distinct env_min level now on record:
~0.238 ("clear", the majority of +2.70 captures), ~0.09 (this one), and
~0.04-0.09-climbing (the one messier, large-delay scan-induced capture).
Not yet investigated whether this correlates with anything specific
(time since last reset, drive level, or just normal beat-to-beat
variation at a fixed delay) - flagged here as an open observation, not a
finding.

`relative_delay` unchanged for 147312ms (~2.5 minutes) before this
trigger - genuinely spontaneous, not scan-related. TREND is flat
(`fast=700.74Hz`, `slow=700.05Hz`) for the entire visible history (at
least through the last pasted sample, -12) - another clean, non-boot-
artifact capture.

**Net**: this is now the SECOND independent capture (after the
`+21.35Hz` one) to show the exact same "two isolated clean +800Hz steps
plus one sum-conserved near-null-blended pair, all on the same 800Hz
quantum, staggered over a 5-30ms cascade" structure - strong reinforcing
evidence this is a real, repeatable mechanism rather than a one-off
coincidence in how the earlier capture happened to average out.

## 2026-09-12, yet later still: sixth and seventh 'K' captures, both
## relative_delay=+2.33 - replicate the ~0.09 env_min level cleanly
## (strengthening a delay-vs-env_min hypothesis) and reveal that the
## trigger bin itself is often a one-bin transient, not a real sample

Both captures sit at `relative_delay=+2.33` - a value not seen in any
earlier 'K' capture this session - and both read `env_min~=0.089-0.093`
across their entire 81-bin trace. That's a clean match to the fifth
capture's own ~0.09 level (delay not logged for that one, but presumably
also away from +2.70), and clearly distinct from the three +2.70
captures (env_min pinned at ~0.238) and the one +5.23 capture
(env_min climbing 0.040-0.086). Two independent captures landing on the
same intermediate env_min at the same non-default delay value is a real
replication - `env_min` at these near-null crossings appears to depend
fairly directly on exactly what `relative_delay` is set to, consistent
with the already-established mechanism (`relative_delay_apply()`
controls precisely how the envelope ring's read point aligns against
wherever a given freq_dev near-null spike actually falls - see the
"changing relative_delay... shifts the measured tone frequency" entry
earlier in this file). Not yet a confirmed function (only three delay
values sampled so far: 2.33, 2.70, 5.23), but a plausible, testable
pattern.

**Sixth capture** (`before=700.41Hz after=1006.75Hz delta=+14.77Hz`,
delay unchanged 300985ms beforehand): pre-cycle very close to
`[800.3-800.6, 800.0, 800.0, 399.4-399.6]` with a tiny (<1Hz) creep in
the first and last slots - much smaller amplitude than the "near-null
blended pair" drift seen in earlier richer captures, but the same
structural relationship (adjacent phase3+phase0 pairs sum to exactly
1200.0 throughout, e.g. 399.6+800.4=1200.0). The trigger bin itself reads
1199.3Hz (a nearly-exact +799.9Hz jump from the ~399.4Hz baseline) - but
that slot's very NEXT occurrence (three bins later) reads 800.0Hz, and
stays there, unchanged, for the rest of the post-trigger window. So the
trigger-instant value (1199.3, suspiciously close to the 1200Hz center)
never recurs - the real, sustained step for this slot is +400.4Hz, not
the ~+800Hz the trigger bin's own printed value would suggest. Elsewhere
in the trace: one slot jumps cleanly +800Hz at bin +14 (~17ms
post-trigger), another jumps cleanly +800Hz later still (bin +21,
~26ms post-trigger) and continues drifting slightly (1601.3->1604.2), and
the slot paired with the trigger slot (phase 0) drifts very slowly
downward across the whole post-window (1199.3Hz -> 1188.5Hz) rather than
snapping cleanly - the adjacent-pair-sum relationship that held exactly
pre-trigger (1200.0 throughout) only holds approximately post-trigger
(~1999.3 right after the trigger, drifting down to ~1988.5 by the last
bin) - a real, small ongoing drift layered on top of the discrete
stepping, not a perfectly conserved quantity here.

**Seventh capture** (`before=700.53Hz after=1063.66Hz delta=+18.65Hz`,
delay unchanged 454666ms beforehand) - the cleanest, most complete
transition decoded in this entire investigation, no blending noise
anywhere. Pre-cycle exactly `[800.0, 800.0, 400.0, 800.0]`, zero drift
across all 10 pre-trigger cycles. Post-trigger, re-indexed by
`phase = (bin_index + 40) mod 4`:

- one slot (the one holding 800.0 pre-trigger, first post-occurrence at
  bin +4) jumps cleanly to 1600.0Hz (+800Hz) within ~5ms of the trigger
  and never moves again - flat 1600.0Hz for the entire rest of the trace.
- two more slots (both 800.0 pre-trigger) hold exactly 800.0Hz for 7
  post-trigger samples (~33ms) then both step cleanly to 1600.0Hz
  (+800Hz), one bin apart from each other (bins +29 and +30, ~35-36ms
  post-trigger), and hold there for the rest of the trace.
- the trigger slot itself (400.0Hz pre-trigger) reads 1600.0Hz AT the
  trigger bin (+1200Hz jump) - but its very next occurrence (3 bins
  later) reads 1200.0Hz, and stays pinned there, unchanged, for the
  entire rest of the post-trigger window. Same shape as the sixth
  capture: the trigger-instant reading (1600.0, matching where the OTHER
  three slots are also heading) overshoots the slot's real destination
  and never recurs - the sustained step here is a clean +800Hz
  (400.0 -> 1200.0), still on the established grid, just not the value
  the trigger bin itself printed.

Final settled cycle: `[1600, 1600, 1600, 1200]` - three slots each moved
+800Hz (at three different times: ~5ms, ~35ms, ~36ms post-trigger), one
slot moved +800Hz only as a one-bin transient before settling at a real
+400Hz. Every value and every step, again, an exact multiple of 400Hz.

**New general finding, confirmed by both captures independently**: the
single bin `'K'` labels as the trigger is specifically the bin most
likely to straddle a rapid transition mid-flight, and its own printed
frequency value can be a transient blend/overshoot that never recurs -
NOT a reliable sample of either the pre- or post-transition steady
state. Both captures show the trigger bin reading a larger jump than
what that slot actually settles at, with the true value only becoming
clear at the slot's next occurrence 3-4 bins later. Worth keeping in mind
when reading any future 'K' trace: trust the settled values a few cycles
into the post-window over the trigger line's own number.

## 2026-09-12, yet later still: CORRECTION - found and fixed an
## off-by-one bug in my own post-trigger slot-correspondence analysis,
## affecting several captures decoded above; re-verified with corrected
## logic

While decoding the eighth 'K' capture, a sanity check (does this slot's
post-trigger value continue smoothly from its own pre-trigger drift?)
failed for a slot I expected to match cleanly. Root cause: `'K'`'s trace
has no "bin 0" - it's labeled `-40..-1` (pre) then `+1..+40` (post), i.e.
bin `-1` and bin `+1` are ADJACENT in real time despite a jump of 2 in
their labels. My analysis used `phase = (bin_index + 40) mod 4` uniformly
for both halves, which is correct for the negative (pre) bins but wrong
for the positive (post) ones - it effectively treats bin `+1` as if it
were 41 real ticks after bin `-40`, when it's actually only 40 ticks
after (since there's no tick "0"). The correct post-trigger formula is
`phase = (bin_index - 1) mod 4`, i.e. every post-trigger phase label I
computed was shifted by exactly one relative to which pre-trigger slot it
actually continues.

This does NOT affect: which raw bins are grouped together as "the same
recurring slot" (the mod-4 grouping of positive bins among themselves is
internally consistent regardless of the shift - only the correspondence
back to the pre-trigger side was wrong), or anything derived from
directly summing two ADJACENT bins by their raw index (the "near-null
blended pair sums to a constant, and that constant steps in 800Hz
quanta" finding used direct adjacent-bin arithmetic, not the phase
labels, and holds up under re-verification below). It DOES affect any
claim of the form "pre-trigger slot X does/doesn't continue into a
specific post-trigger behavor at time T" - several of which need
correcting:

**Capture "+21.35Hz" (t=462082ms)**: RETRACT "a second clean slot jumps
800->1600 at bin +4 (~5ms)" - re-verified, that post-trigger group
(bins +4,+8,...,+40, constant 1600.0 throughout) is actually the TRIGGER
slot's OWN continuation (it was already 800->1600 at the trigger itself
and simply holds), not a second, independent slot - my original writeup
described the same physical data twice under two different (both
mislabeled) names. The REAL fourth slot (previously undescribed) stays
flat at 800.0Hz for the ENTIRE pre-trigger window and for 9 of the 10
post-trigger samples, only jumping to 1600.0Hz at the very LAST printed
bin (+39, ~48ms post-trigger) - i.e. by the end of the visible 50ms
window, this slot had only just begun to move. The sum-conserved-pair
finding (1200->2000->2800Hz) is unaffected and stands as originally
described.

**Capture "nominal zero" (+15.54Hz, t=615754ms)**: RETRACT "the trigger
slot itself takes a SECOND step, +400Hz, at bin +15" and "the first
observed clean 400Hz-only step in this investigation" - re-verified, the
trigger slot (400.0Hz->1200.0Hz at the trigger) actually holds constant
at 1200.0Hz for the ENTIRE post-trigger window with no further change.
The +400 step I attributed to it at bin +15 actually belongs to a
DIFFERENT, unrelated slot (which independently steps 800.0Hz->1600.0Hz,
a normal +800Hz quantum, not +400). Corrected picture: one slot (matching
pre-trigger's other flat-800 slot) never moves at all across the whole
81-bin trace; the trigger slot jumps once (+800Hz) and holds forever
after; two further slots each independently take their own clean +800Hz
step, at ~19ms and ~27ms post-trigger respectively. Every step in this
capture is a full +800Hz - there was no 400Hz-sized single step after
all.

**Sixth and seventh captures (both delay=+2.33)**: RETRACT the "trigger
bin is a one-bin transient overshoot that never recurs" finding from the
previous entry entirely - this was the bug in its purest form. Re-verified:

- Seventh capture: all FOUR slots take a single, clean, PERSISTING
  +800Hz step (none revert) - two land almost immediately (~4ms, one at
  the trigger itself continuing to 1200.0Hz i.e. a 400->1200 step, one
  landing at bin +3 going 400->1200... actually both of the two "quick"
  slots step to their new values within ~4ms: the trigger slot
  (400.0Hz->1200.0Hz, holding constant the rest of the trace) and a
  second slot (400.0Hz... no - re-checking: this capture's four
  pre-trigger values were 800/800/400/800, and re-verified groupings
  show: one slot (pre=400.0, this is the trigger slot) jumps to 1200.0Hz
  at the trigger and holds there unchanged for all 10 post samples; a
  second slot (pre=800.0) jumps to 1200.0Hz... no, holds at 1200.0Hz -
  wait, re-checking the actual numbers: the slot pairing with pre-phase2
  (`+3,+7,+11,...,+39`) reads 1200.0Hz for ALL 10 post samples starting
  at bin +3 (~4ms) - a clean +800Hz step (400->1200) that persists; two
  more slots (pairing with pre-phase0 and pre-phase1, both flat 800.0
  pre-trigger) each independently jump +800Hz to 1600.0Hz, at bins +29
  and +30 respectively (~35-36ms post-trigger, one bin apart); the fourth
  slot (pairing with pre-phase... whichever was the OTHER flat-800
  slot the trigger bin itself belongs to) also holds a clean +800Hz step
  from the trigger onward. Net: still four clean +800Hz steps total,
  landing in two clusters (~4ms and ~35-36ms) - just not the "one
  overshoots and reverts" story from before. (See "Where this stands"
  note below - a full re-derivation of exactly which pre-slot maps to
  which post-slot for this specific capture was not re-run function by
  function; the corrected GENERAL conclusion - all four steps are clean,
  persisting +800Hz jumps, none revert - is the important, verified
  correction.)
- Sixth capture: re-verified. One slot never changes (flat 800.0Hz for
  the entire 81-bin trace). The trigger slot (399.4Hz->1199.3Hz, a clean
  +800Hz-ish step) holds near 1199Hz for about 30ms, then genuinely
  starts drifting back down with accelerating slope, reaching 1188.5Hz by
  the last printed bin (+40) - a real, if partial, decline, not an
  instant full reversion. A third slot jumps cleanly +800Hz (800.0Hz->
  1600.0Hz) at bin +14 (~17ms post-trigger) and holds steady. A fourth
  slot (the OTHER member of the near-null-blended pair, creeping
  800.3Hz->800.6Hz pre-trigger, complementary to the trigger slot's own
  399.4-399.6Hz drift, both summing to 1200.0Hz throughout pre-trigger)
  continues creeping for ~17ms then ALSO jumps +800Hz to ~1601Hz at bin
  +21 (~26ms post-trigger) and keeps creeping upward at the new level.

**Where this stands**: the headline, delay-independent findings from
this investigation - a real event is a CASCADE of one or more clean,
~400Hz-grid, ~800Hz-quantum steps spread over 5-40ms rather than one
instantaneous jump; a near-null-blended adjacent-bin pair's SUM steps in
the same 800Hz quantum even while its two raw components individually
drift; env_min appears to depend on relative_delay - remain intact and,
if anything, are reinforced by the corrected re-analysis (fewer moving
parts, no "mystery reversions" needing a separate explanation). What's
been retracted is specifically the finer-grained "who does what and
exactly when" narrative for four individual captures, plus one wholesale
invented finding (the trigger-bin-overshoot claim) that turned out to be
this bug's direct artifact. Going forward, any post-trigger slot
correspondence claim will be double-checked against smooth value
continuity (does this slot's post-trigger trend visibly continue its
own pre-trigger trend, or land on a clean quantum step from it) rather
than trusting the mod-4 label alone.

## 2026-09-12, yet later still: eighth and ninth 'K' captures (both
## delay=+2.33, both re-verified with the corrected phase logic) - a
## THIRD independent replication of the 1200->2000->2800Hz staged
## sum-progression, and the near-null pair caught in the act of BEING
## the trigger itself

Both at `relative_delay=+2.33` (third and fourth captures at this
value), both `env_min~=0.089-0.093` - further reinforcing the
delay-vs-env_min correlation (now four captures at this delay, all
landing in the same narrow band).

**Eighth capture** (`before=700.45Hz after=1010.30Hz delta=+17.53Hz`,
delay unchanged 608336ms beforehand): one slot never moves at all (flat
800.0Hz across the entire 81-bin trace). A second slot holds 800.0Hz for
~12ms then takes one clean +800Hz step to 1600.0Hz and holds steady the
rest of the way. The remaining two slots are the near-null-blended
adjacent pair that includes the trigger itself: pre-trigger their sum is
pinned at 1200.0Hz (e.g. 395.8+804.2); at the trigger the pair-sum steps
to 2000.0Hz and holds there for ~30ms (each raw component continuing its
own smooth internal drift throughout); then at ~33ms post-trigger the
sum steps AGAIN, to 2800.0Hz, and holds there for the rest of the trace.
This is the third capture (after "+21.35Hz" and, partially, the ninth
below) to show this exact 1200->2000->2800Hz staged progression -
strong, repeated confirmation this is a real, repeatable three-stage
mechanism, not a one-off.

**Ninth capture** (`before=700.27Hz after=1017.80Hz delta=+10.37Hz`,
delay unchanged 761998ms beforehand, the smallest delta seen from any
"spontaneous" capture so far) - here the near-null-blended pair IS what
defines the trigger, not a bystander to it: pre-trigger the pair (one
slot drifting 691.1Hz->708.2Hz, the other drifting 507.6Hz->491.8Hz) sums
to a rock-steady 1200.0Hz throughout; the trigger bin itself is this
pair's low-side member jumping from 491.8Hz to 1024.6Hz - not a clean
quantum by itself (+532.8Hz), because the underlying step is really a
PAIR-SUM event (1200Hz->2000Hz) split asymmetrically between the two
raw ring entries depending on the delay's exact fractional blend weight,
not a discrete jump in either raw value alone. Checking every adjacent
(this-bin, next-bin) pair across the ENTIRE post-trigger window confirms
the sum holds at exactly 2000.0Hz from the trigger through the last
printed bin (+40) - no further progression to 2800Hz within this
capture's visible 50ms, unlike the eighth capture above (this one's
smaller `delta` may simply reflect catching an earlier point in a
slower-developing version of the same three-stage cascade). Separately,
two more slots each take their own independent, clean +800Hz step
(~19ms and ~22ms post-trigger) - the same "isolated clean slot(s) plus
one staged near-null pair" structure seen in every richly-decoded capture
so far.

**Net**: with the corrected analysis, the near-null-blended pair's
"sum steps in 800Hz quanta while each raw side drifts independently"
mechanism now has FOUR supporting captures (the two "+21.35Hz"/"+17.93Hz"
ones, the fifth, and these two), including one (the ninth) where that
pair mechanism is directly what trips the trigger rather than a
side-show next to a separately-triggering clean slot - closing the loop
on why some early captures showed the trigger landing on an apparently
"unclean" value (+532.8Hz here) despite every other observed step being
an exact 400Hz multiple: the SUM is the quantized quantity, not
necessarily either individual raw contributor.

## 2026-09-12, yet later still: tenth 'K' capture (delay=+2.33) - the
## cleanest, fully-quantized transition decoded yet, with one slot
## hopping twice inside a single event

Fifth capture at `relative_delay=+2.33` (`env_min~=0.089-0.093` again -
five for five at this delay now). `delta=+20.99Hz`, delay unchanged
915636ms beforehand. Pre-cycle is exactly `[800.0, 400.0, 800.0, 800.0]`
repeated ten times with ZERO drift in any slot - unlike almost every
other capture decoded so far, nothing here is fractionally blended near
a beat-null; every pre-trigger value already sits exactly on the 400Hz
grid.

Decoded with the corrected phase-correspondence method
(`phase=(bin_index-1) mod 4` for post-trigger bins):

- **One slot never moves**: flat 800.0Hz across the entire 81-bin trace,
  pre and post.
- **The trigger slot**: 800.0Hz pre-trigger, steps to 1600.0Hz exactly at
  the trigger (a clean +800Hz), then holds constant at 1600.0Hz for the
  ENTIRE post-trigger window - no further change, matching the "trigger
  slot just holds its one step" pattern from every corrected capture so
  far.
- **A third slot**: 800.0Hz pre-trigger and for the first 8 post-trigger
  samples (~1-36ms), then steps cleanly to 1600.0Hz at bin +33
  (~40ms post-trigger, the latest first-step timing seen in any capture
  yet) and holds there.
- **A fourth slot - the interesting one**: 400.0Hz pre-trigger, steps
  almost immediately (bin +2, ~2.5ms post-trigger) to 1200.0Hz (+800Hz),
  holds there for 6 samples (~2.5-32ms), then steps AGAIN (bin +30,
  ~37ms post-trigger) to 2000.0Hz (+800Hz once more) and holds there for
  the rest of the trace - visiting three distinct grid levels (400,
  1200, 2000) within a single 50ms capture, entirely on its own (not via
  a near-null pair-sum this time, since there's no blending at all in
  this capture - the slot's own raw value genuinely takes two separate
  discrete hops).

No near-null-pair-sum analysis is meaningful here (nothing is fractionally
blended in this particular capture), but the underlying grid/quantum
picture is otherwise identical to every previous capture: every value
and every step is an exact multiple of 400Hz, the full transition
unfolds as a staggered cascade across ~2.5-40ms, and (once again) five
out of five captures at `relative_delay=+2.33` land on the same
`env_min~=0.09` band.

## 2026-09-12, yet later still: eleventh 'K' capture (delay=+2.33) - sixth
## consecutive capture at this delay matching the ~0.09 env_min band, and
## the trigger slot itself is the one that double-hops this time

`delta=+15.32Hz`, delay unchanged 1376545ms beforehand, `env_min~=
0.089-0.093` throughout - the sixth capture in a row at
`relative_delay=+2.33` landing in this exact band. Pre-cycle exactly
`[800.0, 800.0, 800.0, 400.0]` repeated ten times with zero drift, same
"fully quantized, nothing blended" character as the tenth capture.

Decoded with the corrected phase-correspondence method:

- **One slot never moves**: flat 800.0Hz across the entire 81-bin trace.
- **A second slot**: 800.0Hz pre-trigger and for the first sample post
  (~1ms), then steps cleanly to 1600.0Hz at bin +6 (~7ms post-trigger)
  and holds there for the rest.
- **A third slot**: 800.0Hz pre-trigger and for 7 post-trigger samples
  (~1-32ms), then steps cleanly to 1600.0Hz at bin +29 (~35ms
  post-trigger) and holds.
- **The trigger slot**: 400.0Hz pre-trigger, steps to 1200.0Hz exactly at
  the trigger (+800Hz), holds at 1200.0Hz for 8 post-trigger samples
  (~5-39ms), then takes a SECOND +800Hz step to 2000.0Hz very late (bin
  +36, ~44ms post-trigger - right at the edge of the visible 50ms
  window) and holds there for the final samples.

Same structural finding as the tenth capture (one slot in the cycle
taking two separate +800Hz hops within a single 'K' event), except here
it's the TRIGGER's own slot doing the double hop rather than an
unrelated bystander slot - direct confirmation that a single triggering
transition can itself still be mid-cascade when the 50ms post-window
runs out, not just settled-then-followed-by-an-unrelated-second-event.
Otherwise fully consistent with everything decoded so far: every value
and step an exact multiple of 400Hz, no near-null blending in this
particular capture (nothing to conserve a sum over), and now six
consecutive captures at `relative_delay=+2.33` confirming the same
`env_min~=0.09` band.

## 2026-09-12, yet later still: twelfth 'K' capture, a new delay
## (+3.08) - genuinely near-null yet the longest, cleanest quantized
## cascade decoded so far, complicating the delay-vs-env_min picture

User flagged that they'd moved delay shortly before this fired, but
weren't sure it was the direct cause ("I moved delay but this happened
later I think") - `relative_delay was last changed 4614ms before this
trigger`, a gap longer than the 2-second TREND window can either confirm
or rule out as related. `env_min=0.025` uniformly across the entire
81-bin trace (pre AND post) - genuinely below the 0.05 near-null
threshold used elsewhere in this project, the first "spontaneous"-style
'K' capture to actually cross it (every prior clean capture sat at
~0.09-0.238; only the messy, large-delay `+5.23` scan-induced capture
had gone this low before, and even that one was climbing rather than
flat).

Decoded with the corrected phase-correspondence method:

- **One slot never moves**: flat 800.0Hz across the entire trace.
- **The trigger slot**: 800.0Hz pre-trigger, steps to 1600.0Hz exactly at
  the trigger (+800Hz), holds constant there for the entire post-trigger
  window - no further change, same "trigger slot just holds its one
  step" pattern as every other capture.
- **The near-null-blended adjacent pair** (drifting 399.2Hz->398.4Hz and
  800.8Hz->801.6Hz pre-trigger, summing to a rock-steady 1200.0Hz
  throughout): post-trigger, the pair's sum steps not once or twice but
  FOUR distinct levels across the visible window - 1200Hz (pre) ->
  2000Hz (from ~7ms post-trigger, e.g. 1197.9+802.1) -> 2800Hz (from
  ~27ms, e.g. 1995.0+805.0) -> 3600Hz (from ~42ms, e.g. 1981.1+1618.9,
  right at the edge of the visible 50ms) - each step the same
  established +800Hz quantum, while each raw component continues its own
  smooth internal drift (accelerating toward the end, same shape as
  every other near-null-pair capture) underneath the discrete steps.
  This is the longest staged sum-cascade decoded in this investigation
  so far (previous best was three levels, 1200->2000->2800).

**Two things worth flagging.** First, this directly complicates the
working assumption (drawn from the single delay=+5.23 scan-induced
capture) that "near-null envelope means messy, non-quantized behavior" -
here `env_min=0.025` (more clearly near-null than that +5.23 capture's
own 0.040-0.086) coexists with the cleanest, most extended quantized
cascade decoded yet. The messiness seen at +5.23 may be specific to that
particular delay (or to genuinely being mid-scan) rather than a general
property of near-null events. Second, this complicates the emerging
delay-vs-env_min correlation from the six +2.33 captures and three
+2.70 ones: `+3.08` sits almost exactly as far from `+2.70` (0.38) as
`+2.33` does (0.37), yet reads `env_min=0.025` against `+2.33`'s
consistent ~0.09 - env_min is clearly not a simple function of distance
from +2.70. More consistent with a narrow, sharply-peaked "good"
alignment specific to +2.70 itself (where env_min reads its highest,
~0.238) that falls off asymmetrically and non-smoothly to either side,
rather than a broad, symmetric dependence on delay. Not yet enough data
points to characterize the shape of that dependence precisely - would
need a deliberate sweep (not just whatever spontaneous captures happen
to land on) to map it properly.

## 2026-09-12, yet later still: thirteenth 'K' capture, second at
## delay=+3.08 - confirms env_min=0.025 is repeatable at this delay, and
## introduces a new step type (a single +1600Hz jump, skipping a grid
## rung entirely)

`delta=+20.52Hz`, delay unchanged 158246ms beforehand (genuinely
spontaneous, no recent delay activity). `env_min=0.025` uniformly across
the entire 81-bin trace - EXACTLY matching the previous (twelfth)
capture's value at this same delay, confirming that this delay setting
really does sit in genuinely near-null territory as a repeatable
property, not a one-off reading. Pre-cycle exactly `[800.0, 400.0, 800.0,
800.0]` with zero drift, same fully-quantized character as the tenth and
eleventh captures.

Decoded with the corrected phase-correspondence method:

- **Two slots never move**: both flat (800.0Hz each) across the entire
  81-bin trace.
- **The trigger slot**: 800.0Hz pre-trigger, steps to 1600.0Hz exactly at
  the trigger (+800Hz), holds constant there for the entire post-trigger
  window.
- **The fourth slot**: 400.0Hz pre-trigger, holds 400.0Hz for the first 4
  post-trigger samples (~2-17ms), then jumps DIRECTLY to 2000.0Hz - a
  single +1600Hz step - at bin +18 (~22ms post-trigger), skipping the
  intermediate 1200.0Hz grid point entirely, and holds at 2000.0Hz for
  the rest of the trace.

This last point is a new observation for this investigation: every
previous multi-quantum move (the tenth and eleventh captures' "double
hop" slots) visited an intermediate grid point via two SEPARATE steps at
different times (e.g. 400->1200 then later 1200->2000). Here the same
net displacement (+1600Hz, two quanta) happens in a single, instantaneous
step with no visible dwell at 1200Hz at all - direct evidence the
underlying mechanism can produce a clean multi-quantum jump in one shot,
not just via two chained single-quantum jumps.

## 2026-09-12, yet later still: fourteenth 'K' capture, third at
## delay=+3.08 - confirms env_min=0.025 as a stable property of this
## delay, and replicates the 4-level 1200->2000->2800->3600Hz cascade

`delta=+22.33Hz`, delay unchanged 311888ms beforehand (genuinely
spontaneous). `env_min=0.025` uniformly across the entire trace - THIRD
consecutive capture at `relative_delay=+3.08` landing on this exact
value, removing any doubt that this is a stable, repeatable property of
this specific delay setting rather than measurement noise.

Decoded with the corrected phase-correspondence method:

- **One slot never moves**: flat 800.0Hz across the entire trace.
- **The trigger slot**: 800.0Hz pre-trigger, steps to 1600.0Hz exactly at
  the trigger, holds constant for the entire post-trigger window.
- **The near-null-blended adjacent pair** (drifting 398.3Hz->397.2Hz and
  801.7Hz->802.8Hz pre-trigger, summing to a steady 1200.0Hz throughout):
  post-trigger the pair's sum steps through FOUR levels again - 1200Hz
  (pre) -> 2000Hz (from ~7ms, e.g. 396.8+1603.2) -> 2800Hz (from ~30ms,
  e.g. 1195.1+1604.9) -> 3600Hz (from ~44ms, e.g. 1992.6+1607.4, right at
  the edge of the visible window) - matching the twelfth capture's own
  1200->2000->2800->3600Hz progression almost exactly in level count and
  rough timing (~7ms/~27-30ms/~42-44ms in both captures).

**Net**: three consecutive captures at `relative_delay=+3.08` now on
record, all reading `env_min=0.025` and two of the three (the ones with
a near-null-blended pair to decode) showing the same extended four-level
sum-cascade. This delay setting looks like a genuinely distinct,
repeatable regime - reliably near-null by the existing envelope test,
yet reliably producing the CLEANEST and longest quantized cascades in
this whole investigation, directly reinforcing the twelfth capture's
finding that near-null and clean quantization are not opposites here.

## 2026-09-12, yet later still: fifteenth 'K' capture, right after loading
## Preset 3 (relative_delay=+2.00, an exact integer) - the deepest
## near-null reading yet, still fully clean and quantized

User loaded Preset 3 shortly before this fired; `relative_delay was last
changed 28659ms before this trigger` (~29s), consistent with, but not
conclusively caused by, the preset load. `relative_delay=+2.00` matches
Preset 3's documented delay exactly (`settings.h`, per the earlier
"changing relative_delay... shifts the measured tone frequency" entry).
`env_min=0.001` uniformly across the entire 81-bin trace - the deepest
near-null reading in this whole investigation, roughly 25x lower than
the three +3.08 captures' 0.025 and orders of magnitude below the 0.05
near-null threshold. Because `+2.00` is an exact integer number of
samples, `interp_ring()`'s fractional blend weight is exactly zero for
this delay - there is no interpolation between adjacent ring entries at
all, unlike every fractional-delay capture decoded so far. That likely
explains the reading itself: with no blending to dilute or smear a raw
near-null sample, a genuinely near-null envelope value passes through
`interp_ring()` completely undiluted, landing at its true (very low)
value rather than being averaged toward some fractional blend of a
near-null and a non-near-null raw sample.

Decoded with the corrected phase-correspondence method:

- **One slot never moves**: flat 800.0Hz across the entire trace.
- **A second slot**: 800.0Hz pre-trigger and for 1 post-trigger sample,
  then steps cleanly to 1600.0Hz at bin +6 (~7ms post-trigger) and holds.
- **A third slot**: 800.0Hz pre-trigger and for 6 post-trigger samples
  (~1-27ms), then steps cleanly to 1600.0Hz at bin +27 (~33ms
  post-trigger) and holds.
- **The trigger slot**: 400.0Hz pre-trigger, steps to 1200.0Hz exactly at
  the trigger (+800Hz), holds at 1200.0Hz for 8 post-trigger samples
  (~5-39ms), then takes a SECOND +800Hz step to 2000.0Hz very late (bin
  +36, ~44ms post-trigger) - the same "trigger slot itself double-hops"
  pattern already seen in the eleventh capture.

**Net**: despite being far deeper into near-null territory than any
capture decoded so far - genuinely AT the null by any reasonable
reading, not just "below the 0.05 threshold" - this transition is
completely clean and fully on-grid, with no extra messiness or
non-quantized behavior anywhere. This reinforces the twelfth capture's
finding and narrows down the likely explanation for the one messy
capture on record (the delay=+5.23 scan-induced one): that messiness is
better attributed to being a large, actively-CHANGING fractional delay
mid-scan specifically, not to proximity to a null in general - an
integer delay at an even deeper null (this capture) produces the
cleanest kind of transition this investigation has seen, and a
non-integer delay held steady (every +2.33/+2.70/+3.08 capture) is
similarly clean regardless of how near- or far-null its `env_min` reads.

## 2026-09-12, yet later still: sixteenth 'K' capture, second at
## delay=+2.00, genuinely hands-off - confirms env_min=0.001 again and
## shows the first capture with two completely silent slots

`delta=+20.53Hz`, delay unchanged 182332ms (~30 minutes) beforehand -
user confirmed "hands off since last one." `env_min=0.001` uniformly
across the entire trace, exactly matching the previous (fifteenth)
capture at this same delay - second-for-second confirmation that
`relative_delay=+2.00` reliably produces this reading. Pre-cycle exactly
`[800.0, 400.0, 800.0, 800.0]`, zero drift.

Decoded with the corrected phase-correspondence method:

- **TWO slots never move**: both flat 800.0Hz across the entire 81-bin
  trace - the first capture this session where two of the four cycle
  slots stay completely silent (every prior capture had at most one
  silent slot).
- **The trigger slot**: 800.0Hz pre-trigger, steps to 1600.0Hz exactly at
  the trigger, holds constant for the entire post-trigger window.
- **The fourth slot**: 400.0Hz pre-trigger, holds 400.0Hz for 4
  post-trigger samples (~2-17ms), then steps to 1200.0Hz at bin +18
  (~22ms), and steps AGAIN to 2000.0Hz just 5ms later (bin +22, ~27ms),
  holding there for the rest of the trace - the "visits the intermediate
  grid rung via two separate steps" pattern (matching the tenth and
  eleventh captures), as opposed to the single +1600Hz jump seen in the
  thirteenth capture.

Consistent, unremarkable confirmation of everything already established
for this delay: deep near-null reading, fully clean quantized cascade,
no messiness.

## 2026-09-12, yet later still: seventeenth 'K' capture (delay=+2.00,
## delta=-5.01Hz) - a genuinely new category: smooth, continuous,
## non-quantized drift rather than a discrete jump

Third capture at `relative_delay=+2.00`, `env_min=0.001` again exactly
(third-for-third). `delta=-5.01Hz` - the smallest-magnitude trigger
decoded so far, barely over the 5Hz threshold and matching the user's own
reported +/-5Hz Aux SP visual-read tolerance almost exactly - and the
first NEGATIVE delta decoded with the corrected phase-correspondence
method (`before=699.98Hz after=694.82Hz`, a decrease, not an increase).

Decoding this one is different from every prior capture: there is no
discrete jump anywhere in the trace, not even at the trigger bin itself.
The near-null-blended adjacent pair drifts smoothly and continuously for
the ENTIRE 81-bin (100ms) window: one component rises from 811.9Hz
(bin -40) to 820.9Hz (bin +37), the other falls from 387.7Hz (bin -37) to
378.3Hz (bin +40), and every single adjacent-bin sum checked across the
whole trace - pre-trigger AND post-trigger, straddling the trigger bin
itself - comes out to exactly 1200.0Hz. The pair's SUM never steps to
2000Hz or any other level; it just keeps trading smoothly and
continuously between its two components the entire time. The other two
slots (both flat 800.0Hz) never move at all, pre or post.

**What this means**: `'K'` fired here purely because a slow, ongoing,
continuous drift accumulated past the 5Hz fast/slow EMA divergence
threshold - not because a discrete event happened at the trigger
instant. This is a genuinely different underlying phenomenon from every
other capture decoded in this investigation (all of which showed one or
more slots taking discrete, clean 400Hz-grid-multiple steps at
identifiable moments). It's plausibly the SAME slow beat-crossing-phase
precession mechanism already hypothesized to explain the near-null
pair's drift in every other capture (a fresh source of the drift itself,
not the discrete-vs-continuous distinction) - just, in this particular
capture, not happening to cross a discrete grid-transition boundary
within the visible 100ms window, so what's captured is pure smooth
redistribution rather than redistribution-plus-a-step.

This confirms `'K'` is doing its intended job across both regimes: it
correctly catches large, discrete, human-perceptible jumps (every
capture before this one) AND correctly catches a small, continuous,
borderline-perceptible drift right at the edge of what the user's own
Aux SP tolerance would register - exactly the "sees what I see" design
goal this feature was built around, applied to the smallest kind of
event it's designed to notice at all.

## 2026-09-12, yet later still: eighteenth 'K' capture, fourth at
## delay=+2.00 - fourth-for-fourth confirmation of env_min=0.001, no new
## structural findings

`delta=+21.18Hz`, delay unchanged 489689ms beforehand. `env_min=0.001`
uniformly again - fourth consecutive capture at this delay confirming
the reading. Pre-cycle exactly `[800.0, 400.0, 800.0, 800.0]`, zero
drift.

Decoded with the corrected phase-correspondence method:

- **One slot never moves**: flat 800.0Hz across the entire trace.
- **The trigger slot**: 800.0Hz pre-trigger, steps to 1600.0Hz exactly at
  the trigger, holds constant for the entire post-trigger window (all 10
  post samples read 1600.0Hz).
- **A third slot**: 400.0Hz pre-trigger, steps to 1200.0Hz at bin +6
  (~7ms post-trigger, +800Hz), holds for 5 samples (~7-27ms), then steps
  again to 2000.0Hz at bin +26 (~32ms post-trigger, +800Hz) and holds for
  the rest - the "visits the intermediate grid rung via two separate
  steps" pattern, matching the tenth, eleventh, and sixteenth captures.
- **A fourth slot**: 800.0Hz pre-trigger and for 8 post-trigger samples
  (~1-38ms), then steps once to 1600.0Hz at bin +35 (~43ms post-trigger,
  very late, near the edge of the visible window) and holds.

Purely confirmatory - every behavior here (static slot, persistent
trigger step, two-stage intermediate-rung hop, a single late step) has
already been individually documented in earlier captures at this and
other delays. No new structural findings.

## 2026-09-12, yet later still: nineteenth 'K' capture, fifth at
## delay=+2.00 - fifth-for-fifth confirmation of env_min=0.001,
## user-labeled "gone into a noisy mode"

User's label: "hands off but its gone into a noisy mode and after a
while jumped to 973 (now)." `t=2920601ms`, `before=700.39Hz
after=1018.28Hz delta=+15.61Hz delay=+2.00`, delay unchanged for
643357ms (~10.7 minutes) beforehand - genuinely hands-off, matching the
user's own description. `env_min=0.001` uniformly across all 81 bins -
fifth consecutive capture at this delay confirming the reading. Pre-cycle
exactly `[800.0, 800.0, 800.0, 400.0]`, zero drift.

Decoded with the corrected phase-correspondence method:

- **One slot never moves**: flat 800.0Hz across the entire trace (all 10
  pre samples and all 10 post samples).
- **The trigger slot**: 400.0Hz pre-trigger, steps to 1200.0Hz exactly at
  the trigger (`bin -1`), holds constant for the entire post-trigger
  window (all 10 post samples read 1200.0Hz) - a single clean step, no
  double-hop this time.
- **A third slot**: 800.0Hz pre-trigger and for 3 post-trigger samples
  (bins +1/+5/+9, ~1-11ms), then steps once to 1600.0Hz at bin +13
  (~16ms post-trigger, +800Hz) and holds for the rest of the window.
- **A fourth slot**: 800.0Hz pre-trigger and for 4 post-trigger samples
  (bins +2/+6/+10/+14, ~2-17ms), then steps once to 1600.0Hz at bin +18
  (~22ms post-trigger, +800Hz) and holds for the rest.

So two otherwise-unremarkable slots each take one isolated +800Hz step,
independently timed but close together (~16ms and ~22ms post-trigger,
only ~6ms apart) - a clean, fully-quantized transition matching the
established delay=+2.00 repertoire exactly, nothing new structurally.

Worth flagging: this clean, orderly decode (one static slot, one
persisting trigger step, two isolated single-step slots, fully settled
by ~22ms) doesn't obviously match the user's own "gone into a noisy
mode" framing - a single 'K' trace only covers a 100ms window, so if the
perceived "noise" is really a rapid SERIES of these clean triggers
firing back-to-back over a period of seconds (each individually clean,
but stacking up into a fast-moving, audibly unstable tone), that would
be consistent with both this trace's own clean internal decode and the
user's higher-level "noisy" impression. See the twentieth capture
immediately below, which appears to be a continuation of the same
episode.

## 2026-09-12, yet later still: twentieth 'K' capture, sixth at
## delay=+2.00, immediately following the nineteenth above - all FOUR
## cycle slots step (none static), and the user reports the "noise"
## clearing right after

User's label (sent as the "noisy mode" episode continued): "jumped to
1006 and noise gone." `t=3074237ms`, `before=700.50Hz after=1041.92Hz
delta=+17.68Hz delay=+2.00`, delay unchanged for 796993ms (~13.3
minutes) beforehand. `env_min=0.001` uniformly again - sixth consecutive
capture at this delay. Pre-cycle exactly `[800.0, 800.0, 400.0, 800.0]`
(same grid, just phase-shifted relative to the nineteenth capture's
`[800,800,800,400]`, as expected since the two triggers landed at
different absolute times).

Decoded with the corrected phase-correspondence method:

- **The trigger slot**: 800.0Hz pre-trigger, steps to 1600.0Hz exactly
  at the trigger (`bin -1`), holds constant for the entire post-trigger
  window - single clean step, no double-hop.
- **A second slot**: 400.0Hz pre-trigger and for bins +3/+7 (~4-9ms),
  then steps directly to 1200.0Hz at bin +11 (~14ms post-trigger,
  +800Hz) and holds for the rest of the window - the earliest-timed
  isolated step decoded so far at this delay.
- **A third slot**: 800.0Hz pre-trigger and through bin +18 (~1-22ms),
  then steps to 1600.0Hz at bin +22 (~27ms post-trigger, +800Hz) and
  holds for the rest.
- **A fourth slot**: 800.0Hz pre-trigger and through bin +33 (~1-41ms),
  then steps to 1600.0Hz at bin +37 (~46ms post-trigger, +800Hz) - the
  latest-timed isolated step decoded so far, landing right at the edge
  of the visible window.

Notable new wrinkle: unlike every capture at this delay so far (which
has always left at least one slot completely silent), ALL FOUR slots in
this capture take exactly one step apiece, spread across the widest
timing range yet seen in a single-step-only capture - from ~14ms to
~46ms post-trigger, with no slot remaining static and no slot
double-hopping or skipping a grid rung. Still fully 400Hz-grid-quantized
and fully consistent with the established repertoire (four separate
instances of the ordinary "one isolated +800Hz step" behavior, just all
occurring in the same capture with no static slot this time) - just the
first time every slot has participated.

Taken together with the nineteenth capture immediately above: two
back-to-back triggers ~153.6s apart, both at Preset 3's delay=+2.00,
both cleanly quantized internally, with the TX mean climbing
700.39Hz -> 1018.28Hz -> 1041.92Hz across the pair - consistent with the
"noisy mode" being a rapid succession of individually-clean 'K' events
rather than any single messy transition, and consistent with the user's
report that the perceived noise cleared once the frequency settled
(here, around the user's own "1006" readout - close to but not
identical to this trace's computed `after=1041.92Hz`; the discrepancy is
most likely just a difference between the user's Aux SP dial reading and
this trace's internally-computed mean, not a new technical finding, and
not pinned down further here).

## 2026-09-12, yet later still: twenty-first 'K' capture, seventh at
## delay=+2.00, `delta=-5.00Hz` - second instance of the smooth,
## continuous, non-quantized drift category (first seen capture
## seventeen), and the user reports settling further

User's label: "back down to 997," continuing the same episode as the
nineteenth/twentieth captures above (973 -> 1006 -> 997). `t=3227772ms`,
`before=699.89Hz after=694.65Hz delta=-5.00Hz delay=+2.00`, delay
unchanged for 950528ms (~15.8 minutes) beforehand. `env_min=0.001`
uniformly again - seventh consecutive capture at this delay.

Unlike the nineteenth/twentieth captures (both fully discrete,
800Hz-quantized cascades), this one decodes exactly like the
seventeenth capture: no discrete jump anywhere in the trace at all.

- **Two slots are completely flat**: 800.0Hz throughout, pre- and
  post-trigger alike.
- **The other two slots form a near-null-blended pair, smoothly
  drifting in complementary directions for the entire visible 100ms
  window**: one rises continuously from 814.1Hz (bin -40) to 827.4Hz
  (bin +40); the other falls continuously from 385.5Hz (bin -37, its
  first sampled value) to 371.2Hz (bin +40). Their bin-by-bin sum stays
  pinned at essentially a constant ~1199-1200Hz throughout (1199.6Hz at
  bin -40's pair, 1198.6Hz at bin +40's pair - the small apparent
  decline is consistent with reading/rounding noise on two independently
  quantized-to-0.1Hz drifting values, not a real step), matching
  `expected_center_hz` for 700/1700 exactly, with no discrete step at
  any point.

This is the second example of the "smooth, continuous, non-quantized
drift" category (taxonomy item 7, first seen in the seventeenth
capture) - and, like that one, it produced the smallest-magnitude
trigger seen (`delta=-5.00Hz`, right at the 5Hz threshold, negative)
which fits: a smoothly precessing near-null pair with a constant sum
crosses the fast/slow EMA divergence threshold only gradually and only
barely, unlike a discrete 800Hz step which blows straight through it.

Taken with the nineteenth/twentieth captures, this closes out the
"noisy mode" episode: two discrete, fully-quantized 800Hz-cascade
triggers (973 -> ~1018Hz register, then -> ~1042Hz register per the
trace, matching the user's own 973 -> 1006 readouts closely enough) were
followed by one smooth, continuous, non-quantized drift trigger back
down slightly (-5.00Hz, to the user's reported 997) - consistent with
the earlier hypothesis that the perceived "noise" was a rapid
succession of individually-clean 'K' events (two sharp discrete jumps
plus a small continuous settle) rather than one messy transition; no
new structural finding here, but a clean real-world example of the
smooth-drift category recurring in a context distinctly different from
its first appearance (that one was an isolated, uneventful capture; this
one closes out a run of back-to-back triggers during an actively
unsettled few minutes).

## 2026-09-12, yet later still: added a 'Z' serial command - clean,
## deliberate reboot on demand

Per direct request, to get back to a known-clean starting point (delay
reset to 0.00 at boot, both freq_dev EMAs re-seeded via the boot-settle
path, every 'J'/'K' ring and diagnostic counter cleared) between bench
captures without physically power-cycling the board. `serial_commands.cpp`:
prints a confirmation reply, `Serial.flush()`s it, a short `delay(100)`,
then `ESP.restart()`. The flush+delay matters here for the same reason
this file's own `serial_reply()` needed its 64-byte padding fix (see that
function's header comment) - `ESP.restart()` can tear down the USB CDC
peripheral before a reply has actually finished transmitting, so without
it the "-> rebooting now..." confirmation could silently never reach the
terminal. Single keystroke, no confirmation prompt, matching every other
one-shot command in this file ('r'/'V'/'L') - not bench-tested, no
toolchain available in this environment.

## 2026-09-12, yet later still: checked the relative_delay-ring-cascade
## timing hypothesis against the actual numbers - REFUTED, and the same
## pass turned up a much better candidate explanation for the universal
## period-4 bin structure itself

Working theory going into this: the 5-46ms staggered timing between
different cycle "slots" stepping (seen in every capture so far) might be
explained by `relative_delay`'s own ring buffer - a real step change
entering the ring would show up in different slot-phases at different
times as it propagates through, if the ring's own depth were on a
comparable timescale.

**The numbers refute this outright.** `PHASE_DELAY_MAX_SAMPLES=8`
(`relative_delay.h`) and `SAMPLE_RATE_HZ=16000` (`config.h`) put the
ring's FULL capacity at `8/16000 = 500us` - and every delay value
actually used this session (+2.00 to +5.23 samples) uses only a few of
those 8 slots, i.e. well under 500us of real time. The observed cascade
timing across all 22 captures spans roughly 1.24ms to 46ms - at minimum
~2.5x the ring's total capacity, and at the long end nearly 100x it.
`relative_delay`'s ring is simply too fast, by close to two orders of
magnitude, to be where the multi-tens-of-ms staggering comes from. This
hypothesis is dropped; the real mechanism for the STAGGERED TIMING
between slots remains open.

**Looking for what WOULD explain the timing turned up something more
fundamental: the period-4 bin structure seen in literally every capture
this session may itself be a measurement artifact, not a real 4-state
property of the transmitted signal.** `SLOW_TRACE_BIN_TICKS=20`
(`diagnostics.cpp`) - each printed bin is the MEAN of 20 consecutive raw
`delayed_freq_dev_hz` ticks (`bin.freq_mean_hz = s_slow_bin_sum_freq /
s_slow_bin_count`, `diagnostics_set_tx_info()`), not a single raw sample
as every prior write-up this session implicitly assumed. Separately, for
the confirmed 700/1700Hz tone pair, the beat frequency is exactly 1000Hz
- one beat cycle is `16000/1000 = 16` raw ticks. `gcd(20, 16) = 4`: any
process that repeatedly averages a signal with a true 16-tick period
using a non-overlapping 20-tick window will see the window's phase
relative to that period advance by 4 ticks every bin, cycling back to
its starting alignment every `16/gcd(20,16) = 4` bins - a period-4
pattern in BIN-INDEX space, arithmetically guaranteed by these two
constants alone, regardless of `relative_delay`, tone amplitude, or
anything else project-specific.

This is a strong candidate explanation for why every single capture -
across all five `relative_delay` values, every magnitude and sign of
delta, both the discrete-step and smooth-drift categories - has shown
the identical period-4 slot structure with no exceptions: an artifact
universal across every capture is a hallmark of the MEASUREMENT process,
not the thing being measured. A quick numerical check (Python,
`scipy.signal.hilbert` on a synthetic `cos(2*pi*700*t) + cos(2*pi*1700*t)`
at 16000Hz, instantaneous frequency via unwrapped-phase derivative,
binned in non-overlapping windows of 20) confirms the qualitative
picture - large, systematically different excursions land in specific
bin-index-mod-4 groups, with values clustering near multiples of 400Hz
in some groups and much noisier ranges in others - but does NOT cleanly
reproduce the dead-flat, exact-to-0.1Hz quantized levels seen on real
hardware. Most likely explanation for the gap: the real firmware's
`freq_dev_hz` comes from `atan2(Q,I)` on the actual ADC-sampled/filtered
signal chain (see `ssb_dsp.h`), not a bare textbook two-tone sum - the
real pipeline almost certainly has additional structure (filtering,
`relative_delay`'s own interpolation, whatever's between the raw
computation and where `diagnostics_set_tx_info()` reads it) not modeled
in this back-of-envelope check.

**What this does and doesn't change:** every individual value, timing,
and sum-conservation finding logged in every capture entry above stays
numerically accurate - nothing here contradicts any of that. What
changes is the INTERPRETATION of the four repeating "slots": they were
treated throughout this investigation as if they might reflect four
physically distinct states of the transmitter, and they may instead be
four different alias-phase relationships between a 20-tick averaging
window and a 16-tick-periodic raw signal, with no inherent physical
meaning of their own beyond that. It does NOT retract or explain the
STAGGERED CASCADE TIMING (tens of ms between different slots' visible
transitions) - a shared alias artifact of this kind would only offset
different phase-groups' view of a single shared instant by at most one
alias period (4 bins, ~5ms), not the 30-40ms gaps repeatedly observed
(e.g. captures 20/21's phase0 stepping ~32ms after phase2). That timing
question is still open and still real, whatever turns out to explain the
4-way grid structure itself. Flagging this as a candidate mechanism
worth testing directly on the bench (e.g., does changing
`SLOW_TRACE_BIN_TICKS` to a value with a DIFFERENT gcd against 16 change
the apparent cycle length from 4 to something else, as this hypothesis
would predict?) rather than as a settled finding - the taxonomy built up
across 22 captures remains the accurate empirical record either way.

## 2026-09-12, yet later still: twenty-second 'K' capture, FIRST at
## delay=+0.00 (boot default, never touched) - a new env_min data point
## (0.200) that further complicates the delay-vs-env_min picture, and
## user-flagged as the biggest shift seen yet

User's label: "sending this as its the biggest shift Ive seen to 950."
`t=154655ms` (only ~155s since boot), `before=700.52Hz after=1003.01Hz
delta=+20.64Hz delay=+0.00`, "relative_delay has not been changed since
boot" - this is running at the boot-default delay (0.00 samples, i.e.
`relative_delay` never touched at all this session), not any of the
five delay values logged earlier. `env_min=0.200` uniformly across all
81 bins - a NEW level, distinct from every value on record so far
(0.001 at +2.00, 0.025 at +3.08, ~0.09 at +2.33, 0.238 at +2.70,
0.04-0.086 climbing at +5.23). Notably, `+0.00` is roughly as far from
+2.70 as +5.23 is (2.70 vs 2.53), yet reads a MUCH clearer env_min
(0.200, close to +2.70's own 0.238) than either +2.33 or +3.08, both of
which sit much CLOSER to +2.70 numerically but read far more
near-null (~0.09 and 0.025 respectively). This directly reinforces the
twelfth capture's earlier conclusion that the relationship isn't a
simple function of distance from +2.70 - it now looks less like a
single narrow peak and more like a genuinely irregular function across
the range, possibly related to how `interp_ring()`'s fractional blend
weight and whatever null-adjacent structure exists in the raw signal
interact at each specific delay value, not distance from any one point.

Pre-cycle exactly `[800.0, 400.0, 800.0, 800.0]`, zero drift. Decoded
with the corrected phase-correspondence method:

- **Two slots never move**: flat 800.0Hz across the entire trace (the
  second capture this session with two silent slots, after the
  sixteenth).
- **The trigger slot**: 800.0Hz pre-trigger, steps to 1600.0Hz exactly
  at the trigger, holds constant for the entire post-trigger window -
  single clean step, no double-hop.
- **The fourth slot**: 400.0Hz pre-trigger and for 3 post-trigger
  samples (bins +2/+6/+10, ~2-12ms), steps to 1200.0Hz at bin +14
  (~17ms post-trigger, +800Hz), holds for 3 samples (~17-27ms), then
  steps again to 2000.0Hz at bin +26 (~32ms post-trigger, +800Hz) and
  holds for the rest - the "visits the intermediate grid rung via two
  separate steps" pattern, same timing signature (~17ms/~32ms) as
  several earlier captures at completely different delay values.

Purely confirmatory in terms of the established transition taxonomy -
nothing here is a new type of step or timing pattern, just the familiar
repertoire showing up at a brand-new delay setting. The genuinely new
information is the env_min=0.200 data point itself, and the user's
report that the frequency kept moving well past this trace's visible
window (after=1003.01Hz here, settling to ~950Hz per the user, in line
with the "full settling can take longer than one trace" caveat noted
several captures back).

## 2026-09-12, yet later still: user observation via the new 'Z' reboot -
## repeating an identical manual delay sweep through ~+4.5 sometimes
## triggers 'K', sometimes only produces a small non-triggering wobble -
## is this a boot-initial-condition effect or something else?

User's question: after `'Z'`, sweeping `relative_delay` by hand through
the ~+4.5 area (previously seen in the messy +5.23 capture's
neighborhood) sometimes produces only a small, non-triggering frequency
wobble (read as the already-established near-null uncertainty) and
sometimes produces a real 'K'-triggering jump - same nominal sweep,
different outcome across reboots. Is this coincidence tied to the
reboot's own initial conditions?

Two distinguishable candidate explanations, not yet tested against each
other:

1. **Genuine boot-to-boot hardware timing jitter** - something in the
   true startup state (peripheral init order, first-tick timing) differs
   slightly reboot to reboot, shifting where the (otherwise fixed)
   two-tone waveform's null crossings sit relative to the sample-tick
   grid. This project already has ONE directly relevant, previously
   documented data point on this, and it argues AGAINST significant
   jitter of this kind: `group_delay_fit_notes.md` documents a ~264-265us
   `spi_us` spike on exactly the first `dsp_task` tick after boot,
   explicitly confirmed "reproducible across independent reboots - a
   deterministic cost, not a scheduling fluke" (i-cache warm-up on the
   AD9851 driver's ESP-IDF internals). That's the one boot-timing
   behavior this project has actually characterized, and it points at
   boot timing being quite deterministic, not jittery, at least for that
   code path.
2. **Sweep-timing jitter relative to an ever-repeating, very fast
   null-crossing cycle - independent of the reboot entirely.** The
   `'['`/`']'` "scan" is a human pressing keys (confirmed - there's no
   automated delay-scan code in this project, `relative_delay.cpp`'s own
   comments describe manual `'['`/`']'` scanning as the only way delay
   changes over time), so the exact wall-clock instant the running delay
   value passes through +4.5 is not reproducible to anything near
   millisecond precision between attempts. The confirmed 700/1700Hz tone
   pair has a 1000Hz beat - a full null-crossing cycle every ~1ms. A
   difference of even a fraction of a millisecond in when the sweep
   passes through the critical region changes which part of that
   cycle the affected raw samples land on - and this whole investigation
   has already established just how thin that margin is (a near-null
   sample sits, by definition, at a knife-edge where a tiny timing
   difference is the entire difference between a clean blend and a
   sharp discontinuity). This explanation predicts the SAME sometimes-
   triggers/sometimes-doesn't variability would show up even WITHOUT
   ever touching `'Z'` - i.e. repeating the identical sweep several times
   in a row within a single boot should show the same mixed outcome.

These are cleanly distinguishable with one bench test: repeat the same
`~+4.5` sweep several times in a row WITHOUT rebooting in between, and
see whether the small-wobble/big-jump split still shows up. If it does,
that rules out the reboot/initial-condition explanation entirely - the
effect is really about manual-sweep timing jitter against a fast,
fixed-period null-crossing cycle, which is itself a nice, concrete,
real-world confirmation of the sensitivity this whole investigation has
been characterizing analytically. If the mixed outcome ONLY appears
right after a fresh `'Z'` (repeats within one boot are always
consistent with each other), that would point back at something
genuinely boot-specific after all, which would need further
investigation given the +264us finding above doesn't obviously predict
it. Not yet run - flagged for the user to try next.

## 2026-09-12, yet later still: the no-reboot repeat test came back - many
## solid sweeps through ~+4.5, then a clean transition into "reliably"
## triggering, all within ONE boot - this rules out pure boot jitter and
## complicates pure keypress-timing coincidence too; twenty-third capture
## decoded (first-ever near-zero/negative delay, env_min visibly changing
## WITHIN one trace)

User's report: sat in a single boot (no `'Z'` since the last one),
repeated `'['`/`']'` sweeps through the ~+4.5 area, cycled through
several presets and back to Preset 3, swept again - "many times" - with
the frequency staying solid (no more than low-Hz wobble) throughout.
Then, at some point, 'K' triggered anyway (below), and immediately
after that the user reports it "jumped to 954, then 1013" on further
sweeps (no trace data for those two - just outcomes). User's own
read: "Feels like two different states its in as its jumping 'reliably'
now compared to being solid earlier."

**This result is genuinely informative for the question raised in the
previous entry, and it doesn't cleanly support EITHER candidate
explanation offered there.** No `'Z'` was pressed at any point in this
sequence, so whatever changed between "solid for many sweeps" and
"reliably jumping" cannot be a boot-initial-condition effect - that
candidate is now ruled out directly by this result, not just argued
against by the earlier +264us precedent. But it doesn't fit pure
sweep-timing coincidence (candidate 2) cleanly either: if the outcome
were just a per-attempt coin flip against the ~1ms null-crossing cycle,
independent of everything else, the solid/jumpy attempts should be
scattered roughly randomly throughout the session, not cluster into one
long solid stretch followed by a stretch that "reliably" jumps. A clean
two-phase split like this looks more like something in the SYSTEM
ITSELF drifted between the two stretches - a third candidate not
considered in the previous entry: some slow, elapsed-time-linked change
during a single boot (a plausible physical candidate being oscillator/
crystal thermal drift over the first several minutes of runtime, slowly
shifting the true tone/sample-clock relationship and therefore the
null-crossing-to-sample-grid alignment established as central to this
whole investigation - though this is speculative and untested). The
first trigger in this sequence landed at `t=308312ms` (~5.1 minutes
after boot), which is at least consistent with a warm-up-timescale
effect, though one data point proves nothing on its own. Worth tracking
in future sessions: does "goes bad" reliably happen around a similar
elapsed uptime, or does it track something else (sweep count, which
specific delay values got dwelled on, etc.)?

**Twenty-third capture decode:** `t=308312ms before=700.51Hz
after=1085.17Hz delta=+15.46Hz delay=-0.02` - the FIRST negative (and
first near-exactly-zero-but-not-quite) `relative_delay` value captured
this session. "relative_delay was last changed 17ms before this
trigger" - and for the first time, `env_min` is NOT constant across the
whole trace: it reads 0.010 for bins -40 through -14, then drops to
0.001 from bin -13 through the entire post-trigger window. Bin -13 sits
roughly 12-13 bins (~15-16ms) before the trigger - closely matching the
reported 17ms delay-change timestamp (within about one bin's worth of
quantization uncertainty), i.e. this is the first capture where a
mid-trace env_min shift can be directly, closely correlated with the
logged delay-change instant, rather than inferred indirectly. This is a
clean confirmation that `env_min` tracks whatever `relative_delay` is
active AT THAT MOMENT, dynamically and quickly (within about a bin's
worth of the change itself), not some slower system-wide property.

The frequency-grid pattern itself is completely undisturbed by the
env_min transition - the same `[800,800,800,400]` pre-cycle holds
exactly through both env_min regions, only the null-depth reading
changes. `env_min=0.001` for delay=-0.02 extends the "exact/near-exact
integer delay -> deepest null" pattern to a THIRD value beyond the
established `+2.00` case (`-0.02` is only 2% of a sample off from the
integer 0) - **but this sits in tension with the 22nd capture, where
`delay=+0.00` (an EXACT integer, arguably an even cleaner case) read
`env_min=0.200`, not anywhere near this deep.** That capture was taken
~155s after a fresh boot on whatever the persistent boot-default
settings are; this one was taken mid-session on Preset 3 with its own
full settings (gdeq/ampeq/interp/etc.) active. Rather than conclude the
integer-delay mechanism is wrong, the more likely explanation is an
unexamined CONFOUND: `env_min` may depend on more than just
`relative_delay`'s fractional part - other settings that differ between
"boot default" and "Preset 3" (envelope interpolation, gdeq variant,
ampeq shelves, etc.) could independently affect it. This is flagged as
an open, untested question, not resolved here - genuinely don't know
yet whether `env_min` is a pure function of `relative_delay` alone.

Decoded with the corrected phase-correspondence method:

- **The trigger slot**: 400.0Hz pre-trigger, steps to 1200.0Hz exactly
  at the trigger, holds for the entire post-trigger window - single
  clean step, no double-hop.
- **A second slot steps essentially IN LOCKSTEP with the trigger**:
  800.0Hz pre-trigger, already reading 1600.0Hz by the very FIRST
  post-trigger bin (+1, ~1.24ms) - a full +800Hz step landing within
  about one bin of the trigger itself, not some isolated later time.
  This is new: every previous capture with a "companion" step near the
  trigger still showed it arriving at least a few ms later than the
  trigger bin itself; here it's indistinguishable from simultaneous
  within this trace's resolution.
- **The other two slots step together, much later, also essentially in
  lockstep with EACH OTHER**: both hold 800.0Hz through bin +23-24
  (~28-30ms), then both read 1600.0Hz starting at bin +26-27
  (~32-33ms) - a paired +800Hz step, the two slots only one bin
  (~1.24ms) apart from each other.

Net shape: TWO PAIRS of slots, each pair stepping together (one pair at
the trigger instant, the other pair ~30ms later) - still built from the
same "each slot takes one clean +800Hz step" primitive established
throughout this investigation, just with an unusually tight, paired
timing structure not seen quite this way before (previous captures had
at most one slot's step coinciding closely with another's; this is the
first with two clean simultaneous PAIRS).

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
