# Envelope group-delay equalizer — fitting notes

**2026-09-01: now covers TWO analog filter hardware variants** — see the
"2026-09-01 refit" section below for the new PNP BC327 + gate-attenuator
filter's fit, how it compares to the original BC337 filter this document
started with, and an open question about whether its extra insertion loss
could hurt IMD3/IMD5 despite the improved delay flatness. Also see the
final section for a related (separate) finding: this filter's TF itself
shifts with DC operating point.

Source data (original entry below): `SallenKey_LP_filter_BC337.txt`
(LTspice AC sweep of the actual original 2-pole Sallen-Key RSET
reconstruction filter circuit, including the BC337 buffer stage — not an
idealized 2-pole formula).

## Method

1. Parsed the sweep's magnitude (dB) and phase (degrees) vs frequency.
2. Built a cubic spline of unwrapped phase vs frequency and differentiated
   it analytically to get group delay: `tau(f) = -(1/2*pi) * dphi/df`.
   Cross-checked against raw central-difference derivatives on the sweep's
   own (non-uniform, ~10 points/decade) frequency points — agreement within
   ~0.02us in-band, ~0.2us worst case near 3.2kHz. The spline is fit only
   over 100Hz-20kHz (the sweep's own phase reverses direction above ~65kHz,
   an artifact of the transistor buffer stage's own high-frequency rolloff,
   irrelevant to the audio band and excluded from the fit).
3. Fit two cascaded first-order digital all-pass sections (`ssb_allpass1_t`,
   Fs=10000Hz to match `SAMPLE_RATE_HZ`) by minimizing the peak-to-peak
   spread of (analog + digital) combined group delay over a dense grid from
   100-4300Hz (covers the 700/1900Hz two-tone fundamentals, the 1200Hz beat
   and its harmonics through the 4th, and the +4300Hz 5th-order IMD
   product), using Nelder-Mead (multiple starting points, all converged to
   the same optimum).

## Result

| | Peak-to-peak dispersion, 100-4300Hz |
|---|---|
| Analog filter alone | 29.8us |
| Best possible with **one** all-pass section | 19.2us (barely better — the analog curve is non-monotonic, one section can't match that shape) |
| **Two** sections, `a1=0.194594`, `a2=-0.136698` | **0.85us** |
| (for reference: the Bessel filter redesign, measured on real hardware) | 2.7us |

Mean combined delay after equalization: ~265us (2.65 samples @ 10kHz) — up
from ~66-78us analog-alone, because an all-pass filter can only add delay,
never remove it. The phase/envelope relative-delay line
(`s_relative_delay_samples`) needs to be re-tuned from roughly **+2.65
samples** once this is enabled, a different regime from the Bessel filter's
-0.20 to -0.25 samples.

See `group_delay_equalizer_fit.png` for the delay-vs-frequency plot.

## Status

Numerically fit against simulated (LTspice) data only — **not yet validated
on real hardware**. Implemented as `ssb_allpass1_t` in `ssb_dsp.h`/`.c`
(generic reusable primitive) and instantiated with these coefficients in
`ssb_mic_test.ino`, toggled via serial command `g`. Recommended validation
path: `p` (envelope step test) with `g` off/on on a scope first (cleaner
step edge = working), then real two-tone/spectrum-analyzer testing after
re-tuning `[`/`]`.

## 2026-08-16 refit: SAMPLE_RATE_HZ raised to 16000

Once the AD9851 write path was rewritten to bit-bang (see `AD9851.c`),
`SAMPLE_RATE_HZ` was raised from 10000 to 16000 (see `config.h`'s header
comment). All-pass coefficients don't rescale proportionally with Fs, so
this fit was re-run at Fs=16000 — same source data
(`SallenKey_LP_filter_BC337.txt`), same method (steps 1-3 above,
unchanged), same 100-4300Hz fit grid (that's an absolute-Hz criterion
covering the two-tone fundamentals/beat/IMD product, unrelated to Fs), only
`Fs=16000` substituted into the digital all-pass group-delay formula.
Global optimum confirmed via a coarse (a1,a2) grid scan before polishing
with Nelder-Mead (multiple starts), not just taken on the first
convergence.

| | Peak-to-peak dispersion, 100-4300Hz |
|---|---|
| Analog filter alone (Fs-independent) | 29.8us |
| **Two** sections @ 10000Hz, `a1=0.194594`, `a2=-0.136698` | 0.85us (see above) |
| **Two** sections @ 16000Hz, `a1=-0.023900`, `a2=0.447131` | **14.7us** |
| (exploratory, not wired in) Three sections @ 16000Hz | 10.1us |
| (exploratory, not wired in) Four sections @ 16000Hz | 5.2us |

**Why two sections do noticeably worse at 16000Hz**: a first-order digital
all-pass section's delay-vs-frequency curve is shaped over the section's
full 0-Nyquist range. At 10000Hz the 100-4300Hz fit band spans up to
`4300/5000 = 86%` of Nyquist; at 16000Hz the same absolute Hz band only
spans `4300/8000 = 54%` of Nyquist — the same two sections have
proportionally less curvature available across that narrower fraction of
their own range, so they can't bend to match the analog filter's
non-monotonic peak as tightly. Confirmed not an optimizer-convergence
artifact (grid search found the same optimum the Nelder-Mead polish did);
confirmed it's a real structural limit of the 2-section architecture at
this Fs, not a bug. Adding a third and fourth cascaded section (cheap —
`ssb_allpass1_process` is one multiply-add per sample, and the current
[timing] budget at 16000Hz has margin to spare) recovers most of the
difference if 14.7us turns out to matter once this is validated on real
hardware; not implemented yet since it wasn't asked for.

Mean combined delay after equalization at 16000Hz: ~163.2us (2.611 samples
@ 16000Hz) — down from ~265us (2.65 samples @ 10000Hz) even though both are
"2.6-ish samples", because the coefficients themselves are different, not
because of the sample-rate conversion. `relative_delay_samples` in
`settings.h` was rescaled by the Fs ratio (1.6x) as a first-order
approximation when this change went in, but that rescale doesn't know
about this ~102us shift in the digital filter's own contribution — see
that file's header note for the expected direction of the residual
re-tuning error.

## 2026-09-01 refit: new PNP BC327 + gate-attenuator filter

The RSET analog output stage was redesigned — PNP BC327 driver with a gate
attenuator, replacing the original BC337 NPN buffer. New source data:
`SallenKey_LP_filter_PNP_BC327__RSET_Driver__Gate_Attn.txt`. Same method as
above (steps 1-3, unchanged), same 100-4300Hz fit grid, Fs=16000 only so
far (10000Hz not refit for this filter — not needed, `SAMPLE_RATE_HZ` is
16000 in the current build). Global optimum confirmed via grid scan +
Nelder-Mead polish, same cross-check discipline as the 16000Hz BC337 refit.
Both filters now live side by side in `envelope_gdeq.h`, selected at
compile time by `ENV_FILTER_VARIANT` (`config.h`) — currently set to the
new filter, since that's what's on the bench.

### Analog filter comparison (LTspice, 100-4300Hz)

| | BC337 (old) | PNP BC327 + attn (new) | Delta |
|---|---|---|---|
| Group delay, mean | 68.8us | 93.4us | +24.6us |
| Group delay, peak-to-peak | 29.8us | 41.1us | +11.3us |
| Insertion loss @ 200Hz | -0.84dB | -2.53dB | -1.7dB |
| Insertion loss @ 4300Hz | -6.33dB | -10.42dB | -4.1dB |
| Insertion loss @ 8000Hz | -15.72dB | -22.65dB | -6.9dB |

The new filter is both slower and lossier than the old one — the gate
attenuator is adding delay and loss on top of what the buffer alone cost.
The loss grows with frequency, so it bites hardest exactly where the
equalizer below can't help (see the all-pass caveat further down).

### Digital equalizer fit result

| | Peak-to-peak dispersion, 100-4300Hz |
|---|---|
| Analog filter alone (PNP BC327 + attn) | 41.1us |
| **Two** sections @ 16000Hz, `a1=a2=0.622515` | **13.7us** |
| (for reference: BC337's own 16000Hz fit, above) | 14.7us |

`a1=a2` is a genuine optimum here, not a stuck/degenerate Nelder-Mead
result — a coarse (a1,a2) grid scan independently converges to the same
point (13.69us at the grid resolution, 13.67us after polishing), same
discipline as the BC337 16000Hz refit above. Two IDENTICAL cascaded
sections happen to flatten this filter's particular dispersion shape
better than any opposite-sign pair does — worth remembering as a real
possible outcome of this fit method, not a bug, if it recurs on a future
filter revision.

Mean combined delay after equalization: ~131.6us (2.105 samples @
16000Hz) — LESS than the BC337 filter's 163.2us/2.611 samples, despite
this filter's analog stage being slower to start with, because `a1=a2`
here needs less cumulative added delay to flatten this particular
dispersion shape. `relative_delay_samples` needs re-tuning from roughly
**+2.105 samples** for this filter (a different regime from BOTH the
Bessel filter's -0.20 to -0.25 samples AND the BC337 Sallen-Key fit's
+2.611 samples) — the presets in `settings.h` still reflect the BC337
filter's by-ear/scope tuning and have not been re-tuned for this hardware
change; expect to redo that tuning pass on the bench, not just carry the
old numbers over.

### Open question: does the extra insertion loss hurt IMD3/IMD5?

An all-pass equalizer is unity-magnitude by construction — the digital fit
above flattens DELAY only and does nothing whatsoever about the amplitude
loss in the table above. For the 700/1900Hz two-tone pair specifically,
the IMD3 offsets are 2f1-f2=500Hz and 2f2-f1=3100Hz, and the IMD5 offsets
are 3f1-2f2=1700Hz and 3f2-2f1=4300Hz (the latter being the same 4300Hz
the fit grid's upper bound was originally chosen around).

**Correction, 2026-09-01:** the raw old-vs-new dB gap includes the new
design's own resistive gate attenuator (~2.5dB nominal), which shows up in
the LTspice sim as a flat ~-1.68dB offset already present at 100-300Hz,
well below the filter's own corner. A flat, frequency-independent loss
like that is just headroom/gain to trim back with master gain — it isn't
a filter-shape effect and isn't relevant to edge-rounding/IMD. The number
that actually matters here is the *excess above that flat baseline* —
extra loss minus ~-1.68dB:

| Offset | Product | BC337 | PNP+attn | Raw extra | **Excess (shape only)** |
|---|---|---|---|---|---|
| 500Hz | IMD3 | -0.85dB | -2.56dB | -1.7dB | **-0.03dB (negligible)** |
| 1700Hz | IMD5 | -1.17dB | -3.27dB | -2.1dB | **-0.42dB** |
| 3100Hz | IMD3 | -3.19dB | -6.29dB | -3.1dB | **-1.41dB** |
| 4300Hz | IMD5 | -6.33dB | -10.42dB | -4.1dB | **-2.41dB** |

So the genuine frequency-shape penalty is roughly half what the raw
numbers suggested, and at the IMD3 lower offset (500Hz) it's essentially
zero — the two filters track almost identically there. The concern
doesn't disappear (3100/4300Hz still show 1.4-2.4dB of real extra
roll-off past what the attenuator alone accounts for), but it's smaller
and more concentrated above ~3kHz than first framed, so this leg of the
"why doesn't the equalizer help IMD" argument (see Status below) should
be weighted accordingly — a real but secondary factor, not the dominant
one.

Physical concern, not yet confirmed either way: the same envelope-null
events that drive `freq_dev_hz` toward the `MAX_FREQ_DEV_HZ` (8000Hz)
clamp (see `null_bias_investigation.md`) also produce the envelope's own
fastest, most broadband transient — content that plausibly extends well up
into the range where this filter now attenuates harder. Flattening delay
doesn't stop that transient from being rounded off by the extra loss up
there, and edge-rounding at a null is exactly the kind of envelope-domain
distortion that shows up as IMD. Whether this matters in practice hasn't
been measured — the direct check is a real two-tone IMD3/IMD5
spectrum-analyzer comparison, BC337 vs. PNP+attn, each with its OWN
properly-retuned `relative_delay_samples`, using the project's existing
`imd_comparison_spectrum.png`/`imd_comparison_table.csv` workflow. Not run
yet — parked here alongside the null-bias and `I`-regression items as
something to chase once the group-delay retune itself is validated on
the bench.

### Simulated (mid-DC) vs. measured hardware (mid-DC) — sanity check

Comparing this LTspice run's phase shape (zeroed at 200Hz, since the sim's
reference node isn't the same point as the full measured chain — offset/
scale mapping, PWM, etc. aren't in the sim) against the real `EnvFilterTF_
mid_DC.txt` hardware sweep taken with the sine-chirp test mode: the two
track reasonably well but aren't identical. The sim over-predicts phase
lag by up to ~11° around 2-4kHz, closing to within a few degrees by
6-8kHz — a decent guide, not an exact match, consistent with real
component tolerances the sim doesn't model. Only the mid-DC bias point has
been simulated so far; the DC-operating-point-dependence itself (measured
on real hardware — see below) hasn't been independently checked against
LTspice at the Lo/Hi bias points yet.

**Explained artifact, 2026-09-01: small blips at 5kHz and its harmonics in
the raw chirp-based TF sweeps (`EnvFilterTF*.txt`) are a measurement-rig
artifact, not real filter behavior.** Traced to short (0.25-7µs), ~5kHz
negative-going glitches on `CHIRP_REF_GPIO` (pin13) - confirmed present in
EVERY audio mode (`t`/`m`, not chirp-specific), at a rate matching
`adc_capture.h`'s `ADC_CONT_SAMPLE_FREQ_HZ`/`ADC_CONT_FRAME_SAMPLES` =
80000/16 = 5000Hz exactly - i.e. `adc_conv_done_cb()`'s own DMA/ISR firing
rate, most likely coupling electrically onto that specific pin rather than
a deliberate GPIO write (both other users of the same physical pin are
compiled out - `ADC_ISR_DEBUG_PIN_ENABLED`/`CMD_DEBUG_PIN_ENABLED` are both
0). **Confirmed NOT present on the actual RSET/PWM signal line (GPIO2)** -
scoped directly, nothing detected - so this only affects the reference/sync
channel the external TF rig reads, never the signal actually being
measured, and none of the group-delay/DC-dependence conclusions elsewhere
in this document are affected by it. Should mostly be filtered out easily
(a 0.25-7µs blip is tiny against most of the sweep's own edge-to-edge
interval - e.g. ~1ms at 1kHz), possibly interacting with the reference's
own high-frequency sampling-jitter limitation (see the chirp mode's
`ssb_mic_test_commands.md` entry) right at the top of the sweep, where the
true reference period itself approaches 200µs. Not chased further - the
actual TF data was fine, this only explains a previously-unexplained
cosmetic wrinkle in it.

### Status

**2026-09-01, real-hardware result: far from decisive — leans toward "off."**
`p` step test with `g` toggled: a slight steepening of the edge as
predicted, but the lead-in is slower and the lead-out shows noticeable
ripple. Two-tone and mic/white-noise IMD testing: with `[`/`]` re-tuned by
ear in both states, comp-off gives similar or slightly *better* IMD than
comp-on. This directly contradicts the narrowband delay-flatness
prediction above, so it's worth being explicit about why, not just
recording the result:

**Quantified, same day — but confounded with `'I'`.** Two logged `Live`
presets, delay re-tuned in each to minimize 3rd-order IMD: `relative_delay=
1.00, env_gdeq_enable=false, envelope_interp_enable=false` gave Low 3rd IMD
-41.41dB / High 3rd IMD -34.17dB; `relative_delay=3.00, env_gdeq_enable=
true, envelope_interp_enable=true` gave -34.50dB / -29.97dB — a real
4.2-6.9dB degradation, reference levels matched to within 0.5dB so it isn't
a normalization artifact. **But `envelope_interp_enable` flipped ON
together with `env_gdeq_enable` between these two runs** (see the
Catmull-Rom/null-flattening cross-reference two sections up), so this pair
does not isolate the equalizer's own effect — it measures "gdeq+`I`
together" vs "neither." The +2.0 sample delay swing between the two runs
is a good sanity check (matches the fitted equalizer's own +2.105 sample
mean delay closely), but doesn't help separate the two enabled features.
**Still needed: `env_gdeq_enable=true` with `envelope_interp_enable=false`
(delay re-tuned) vs. the Run-1 baseline, and the reverse
(`env_gdeq_enable=false`, `envelope_interp_enable=true`)**, to find out
whether the ~4-7dB hit is coming from the equalizer, from `'I'`'s
already-known null-rounding issue, or both. Until that decomposition is
run, don't treat this pair as a decisive indictment of the equalizer
specifically.

**Decomposition, same day — first isolating run in.**
`env_gdeq_enable=true`, `envelope_interp_enable=false`, delay re-tuned for
min 3rd IMD, landed at **relative_delay=1.10**: Low 3rd IMD -30.90dB, High
3rd IMD -27.17dB (ref levels within 0.6dB of the other two runs, not a
normalization effect). Full three-way table:

| | Off (delay=1.00) | gdeq+`I` (delay=3.00) | gdeq alone (delay=1.10) |
|---|---|---|---|
| Low 3rd IMD | -41.41dB | -34.50dB | -30.90dB |
| High 3rd IMD | -34.17dB | -29.97dB | -27.17dB |

gdeq alone is the WORST of the three, not an intermediate case — 10.5dB
worse than off on Low, 7.0dB worse on High, worse even than gdeq+`I`
together. So `'I'` is not masking gdeq's damage; gdeq itself is the bigger
single contributor of the two.

**But flag before treating this as final:** the tuned delay (1.10) sits
only +0.10 samples above the off-state optimum (1.00), nowhere near the
fitted equalizer's own predicted +2.105 sample mean delay addition that
`envelope_gdeq.h`'s "IMPORTANT SIDE EFFECT" note documents as the correct
operating point with gdeq on. The gdeq+`I` run's own tuned delay (3.00)
landed much closer to that predicted region. A search converging at +0.10
instead of near +2.1 looks like it found a shallow local minimum close to
the off-state value rather than the theoretically-predicted one — possibly
because the search didn't sweep far enough out. **Before trusting -30.90/
-27.17dB as gdeq's true best case, re-run the `[`/`]` search for gdeq-alone
specifically probing delay ≈ 1.8-2.4 samples** (centered on +2.105), not
just refining near 1.10. If that region turns out worse still, this is a
genuinely decisive result against the equalizer for this filter; if it's
meaningfully better, the -30.90/-27.17dB numbers above were an artifact of
an incomplete delay search, not the equalizer's real ceiling.

**Follow-up, same day: delay=1.40 tuned for min Low 3rd IMD specifically.**
Low improved to -33.34dB (+2.44dB vs. the 1.10 run), but High barely moved
(-27.40dB, 0.24dB worse - within noise). Both still well short of the
off-state baseline (-41.41/-34.17dB). Two takeaways: (1) Low kept
improving as delay increased from 1.10->1.40, consistent with the search
not having reached far enough yet - worth continuing toward the fitted
+2.105 region rather than stopping here; (2) High did NOT track Low's
improvement, which raises the separate possibility that Low and High 3rd
IMD don't share one jointly-optimal delay for this filter+equalizer
combination - a genuine low/high delay tradeoff, not just an unfinished
search. Continuing the sweep to ~1.8-2.4 while watching BOTH sidebands
together (not just Low) should distinguish the two: both improving further
= search wasn't finished; Low improving while High keeps drifting worse =
a real tradeoff.

**Follow-up, same day: delay=2.00 tuned for min High 3rd IMD specifically.**
High improved only modestly to -28.18dB (+0.78dB vs. the 1.40 run) - the
best High result yet, and notably this delay (2.00) sits almost exactly at
the fitted equalizer's own predicted +2.105 sample operating point. But
Low did NOT keep improving out to this delay - it got substantially WORSE,
-33.34dB (at 1.40) -> -27.36dB (at 2.00), a 6.0dB drop. Full gdeq-alone
sweep now:

| Delay | Low 3rd IMD | High 3rd IMD |
|---|---|---|
| 1.10 | -30.90dB | -27.17dB |
| 1.40 | -33.34dB (best Low so far) | -27.40dB |
| 2.00 | -27.36dB | -28.18dB (best High so far) |

This resolves the "unfinished search" question for Low - it has a real
interior minimum near delay~1.4 within the range tested, not a monotonic
trend still climbing toward +2.105. The low/high tradeoff from the
previous entry is now well supported: Low's best point (~1.4) and High's
best point so far (~2.0, right at the theoretical prediction) are
different operating points, and **neither sideband's individually-best
case gets remotely close to the off-state baseline** (-41.41/-34.17dB) -
Low's best is still 8.1dB worse, High's best is still 6.0dB worse, picked
independently and generously (not even simultaneously achievable at one
delay). Significant: 2.00 is essentially the theoretically "correct" delay
for this filter+equalizer and it still doesn't recover anywhere near
baseline on either sideband - this is no longer well-explained by "hadn't
reached the right delay," since the right delay (by the fit) has now been
tested directly.

**Follow-up, same day: sweep closed out.** Confirmed on the bench: past
delay=2.00, Low 3rd, High 3rd, AND the higher-order products all get
WORSE, not just one sideband trading against another. This resolves the
last open thread above - there's no point further out worth chasing, the
gdeq-alone delay sweep is now bracketed on both sides of a genuine
interior optimum region (~1.4-2.0 samples), and even the best achievable
point within that region for each sideband individually stays 6-8dB worse
than off. **This is no longer explainable as an unfinished or mistuned
delay search - the fit's own predicted +2.105 sample operating point has
been tested directly (delay=2.00) and does not recover anywhere near
baseline performance.**

Re: the gdeq+`I` run (delay=3.00) beating gdeq-alone's best on both
sidebands (-34.50/-29.97 vs -33.34/-28.18) - since pushing gdeq-alone's
own delay toward 3.00 makes everything worse (confirmed above), that
combined run's better numbers can't be "more delay helping." `'I'` must be
doing something genuinely independent of gdeq there - plausibly not the
purely-harmful factor it was assumed to be for 3rd-order IMD specifically,
even though it's still a documented, real problem for OTHER symptoms (the
`'p'` step-response ripple, the audibly-worse two-tone stability in
`null_bias_investigation.md`). Those are different failure modes and don't
have to move together - worth keeping `'I'`'s effect on 3rd-order IMD as
its own separate open question rather than assuming it's uniformly bad.

**Working conclusion, real-hardware-confirmed:** gdeq's IMD3 degradation on
this filter is a genuine property of the equalizer itself, not a
delay-tuning artifact - bracketed on both sides of its own predicted
operating point and consistently 6-8dB worse than leaving it off, even at
each sideband's individually-best delay. Combined with the three
mechanisms discussed earlier (fit-window/wideband mismatch, the
DC-operating-point dependence, and the smaller-than-first-estimated but
real amplitude penalty above ~3kHz), the most likely explanation is that
group-delay-only correction simply isn't addressing this filter's dominant
real-world distortion mechanism - **recommendation stands and is now
stronger: leave `env_gdeq_enable` off by default for
`ENV_FILTER_PNP_BC327_ATTN`**, and don't invest further bench time
re-tuning its delay for this filter revision.

**Real (mic-path) audio, same day: gdeq's effect all but vanishes into a
much bigger existing problem.** All the numbers above came from
`AUDIO_SRC_TWOTONE` (digitally-generated, clean two-tone injected straight
into the DSP chain, EQ/compressor bypassed). Repeating with `AUDIO_SRC_MIC`
(real ADC front end, `eq_enable=true`, `compressor_enable=true`, ADC LPF
in Chebyshev mode - i.e. the actual on-air signal path) gives a very
different picture:

| Run (mic path) | Low 3rd IMD | High 3rd IMD |
|---|---|---|
| Off (delay=0.98) | -30.16dB | -10.25dB |
| gdeq alone (delay=1.48) | -29.27dB (-0.89dB) | -10.43dB (-0.18dB) |
| gdeq+`I` (delay=2.88) | -29.56dB (-0.53dB) | -10.58dB (-0.33dB) |
| Off, repeat (delay=0.93) | -30.02dB | -10.24dB |

Repeatability is excellent (the two off-state runs agree to 0.14dB/0.01dB),
so this is a clean measurement, and against that noise floor gdeq's real
effect here is small - still consistently in the "worse" direction on Low
(~0.5-0.9dB) as in every other test, but close to a rounding error on High
(~0.2-0.3dB), nothing like the 6-10dB hit on the synthetic two-tone test.

Far more significant: the ABSOLUTE levels. High 3rd IMD sits at only
**-10.2 to -10.6dB across every mic-path run, gdeq on or off** - 24dB worse
than the -34.17dB synthetic-two-tone-off baseline; Low 3rd is ~11dB worse
too (-30dB vs -41dB). Since this preset adds `eq_enable`/`compressor_
enable` (both off in every prior two-tone run) and switches the ADC LPF to
Chebyshev, something in that real chain - the compressor is the leading
suspect (a classic dominant IMD3 source), possibly compounded by the EQ or
the Chebyshev ADC filter - is producing an IMD3 floor far worse than
anything the envelope filter or its equalizer contributes. -10dB IMD3 is
a genuinely poor number for SSB (normal targets are -30dB or better).

**Net effect on the gdeq decision: unchanged (still leave it off - it
never helped, costs nothing to disable), but it's now clearly not where
the real audio-quality problem lives.** The compressor/EQ/ADC-chain
IMD3 floor is a much bigger, separate issue worth chasing next - start by
toggling `eq_enable`/`compressor_enable`/ADC LPF mode independently on
`AUDIO_SRC_MIC` (same delay-tuned-per-config approach used throughout this
sweep) to find which stage is actually responsible for the ~24dB gap.

**Follow-up, same day: compressor and EQ both ruled out as the dominant
cause.** `delay=0.98`, `gdeq`/`I` off throughout, ref levels matched to
within ~1.3dB across all three runs (not a level-mismatch artifact):

| Run | Low 3rd IMD | High 3rd IMD |
|---|---|---|
| Comp ON, EQ ON (17.3dB gain, baseline avg) | -30.09dB | -10.25dB |
| Comp OFF, EQ ON (30.3dB gain) | -31.20dB (1.1dB better) | -10.83dB (0.6dB better) |
| Comp OFF, EQ OFF (30.3dB gain) | -31.79dB (1.7dB better than baseline) | -9.88dB (0.4dB WORSE than baseline) |

The compressor accounts for ~0.6-1.1dB - real, but tiny next to the ~24dB
gap. EQ's effect is mixed and small: helps Low a little further but makes
High slightly worse when removed (mildly protective there, if anything -
the opposite of "EQ is a distortion source"). Both now ruled out as the
dominant cause; High 3rd IMD stays pinned near -10dB regardless.

**Leading remaining hypothesis: the injected two-tone stimulus itself**,
upstream of everything this firmware controls (external generator/
soundcard/cabling/mic preamp feeding the mic input) - if that source
already carries ~-10 to -12dB IMD3 of its own, no internal DSP option can
move a floor set before the ADC ever sees it. Direct check: spectrum-
analyze the injected two-tone signal itself, or loop it back ahead of the
mic preamp, before chasing more internal toggles. Remaining untested
internal variable: `ADC_LPF_MODE_OFF` on `AUDIO_SRC_MIC` (every mic-path
run so far has been Chebyshev) - worth one data point, but a weaker bet
than the source-purity hypothesis given how little compressor/EQ moved
things.

- **The fit window doesn't cover what these tests excite.** The 100-4300Hz
  grid was chosen for two-tone/IMD spectral relevance, but a step edge and
  white noise both carry energy well outside it. Outside the fit window
  the combined (analog + digital) delay curve was never constrained and
  can diverge — plausibly exactly the lead-out ripple being seen. The
  digital side's contribution is well-behaved on its own (a real,
  non-oscillatory pole pair, `a1=a2=0.622515`, confirmed as a genuine
  optimum, not degenerate), so this points at analog/digital delay
  *mismatch above ~4.3kHz* rather than the digital fit misbehaving by
  itself.
- **An all-pass equalizer cannot touch the amplitude problem, and part of
  the amplitude problem is real (though smaller than first estimated).**
  Correcting for the new design's own ~2.5dB gate attenuator (a flat
  offset, not a shape effect — see the table above), the genuine
  frequency-dependent excess loss at the IMD3/IMD5 offsets is more like
  ~0dB at 500Hz growing to ~1.4-2.4dB at 3100/4300Hz, not the 1.7-4.1dB
  raw gap quoted earlier. Rounding/attenuating envelope content above
  ~3kHz is still a plausible secondary distortion mechanism a delay-only
  fix can't touch, but it's no longer the strongest leg of this
  explanation — the delay-dispersion-outside-fit-window and DC-bias-point
  mismatch factors below are doing more of the work.
- **The fit is only exact at one DC bias point.** The equalizer's
  coefficients were fit against the mid-DC analog TF; the DC-dependence
  finding below shows the real filter's phase (and presumably amplitude)
  shifts by 7-14% in crossing frequency across the duty range that real
  audio actually sweeps through. A fixed narrowband fit at one bias point
  can't track that, whereas by-ear `[`/`]` tuning implicitly compromises
  across the whole swept range — which is a plausible reason hand-tuned
  "off" can match or beat the "theoretically correct" fixed comp.
- **Possible confound, not yet isolated: `envelope_interp.h`'s Catmull-Rom
  stage (`'I'`) rounds off the exact same null transient, upstream of the
  analog filter.** Its own v4.2 validation note already found a smooth
  cubic can't represent a two-tone null's hard fold (a genuine derivative
  discontinuity — `2A|cos(...)|` looks locally like a V there) and rounds
  it instead, unlike the plain straight-line ramp it replaced, which
  reproduces a V-shaped fold naturally. `envelope_interp_on_full_tick()`
  is the last digital stage before the PWM write, i.e. immediately
  upstream of this filter's own extra HF roll-off — so if `'I'` was
  enabled during the two-tone/mic comp-on/off comparison above, the null
  transient may already have been smoothed digitally before the analog
  filter or the equalizer ever saw it, which would mask/confound the
  filter-only question this Status section is trying to answer. `'I'` is
  already separately parked in `null_bias_investigation.md` (2026-08-31)
  as an unresolved, user-confirmed "audibly worse" regression — not
  something to treat as a known-neutral background setting. Not yet
  confirmed whether `'I'` was on or off during the tests behind this
  Status entry. Recommended: repeat the comp-on/comp-off two-tone/mic
  comparison with `'I'` forced off, to get a clean read on the filter/gdeq
  question in isolation from this still-open interpolation issue.

None of these four are mutually exclusive; the honest read is that several
are plausibly working against the equalizer at once for this filter, and
the delay-flatness gain it does deliver in the fit band isn't large enough
to outweigh them. **Current recommendation: leave `env_gdeq_enable` off by
default for `ENV_FILTER_PNP_BC327_ATTN`** (already true — off is the
compiled-in default in `envelope_gdeq.cpp`, unaffected by this finding)
and don't spend further effort re-fitting the all-pass sections for this
filter. If this filter's IMD is to be improved further, the insertion-loss
table above says the amplitude penalty is the more promising thing to
chase — either in the analog design itself (less loss in the 3-4.3kHz
region) or with an actual IMD3/IMD5 spectrum-analyzer comparison (comp on
vs. off, each re-tuned) to replace this by-ear read with numbers, rather
than more delay-equalizer iteration. The coefficients and
`ENV_FILTER_VARIANT` plumbing stay in the firmware since they're correct
for what they do (narrowband delay flattening) and cost nothing while
disabled — this is a "don't reach for this tool on this filter" finding,
not a "the fit was wrong" finding.

## 2026-09-03 refit: real-hardware TFA data (Hi-Z buffered), fit band widened to 8000Hz

Requested after two developments: (1) the TFA front end had a loading issue
that was making measured phase inconsistent across the envelope's DC
operating range — this is very plausibly the same effect the "DC-operating-
point dependence" section below documents from LTspice-vs-hardware
comparisons, or at least a contributor to it. Hi-Z buffers added to the TFA
front end fixed it — phase now reads consistently across the envelope
range. (2) Real `freq_dev_hz` excursions have been observed out to the
`MAX_FREQ_DEV_HZ` clamp (8000Hz), well past the old 100-4300Hz fit grid's
upper edge — the fit had never been asked to behave out there.

Source data: `Group_delay_off__on_F_Phase.txt` (the same real-hardware TFA
sine-chirp sweep already used for the 2026-09-03 same-day dispersion
report in `ssb_mic_test_commands.md` — see the correction paragraph below).
~1000 points, 0–24kHz, unwrapped phase, `g` off and `g` on both captured
(the `g` on curve reflects the OLD/superseded 0.622515/0.622515
coefficients, still compiled in at measurement time).

### Method

Same structure as every prior fit in this document (two cascaded
`ssb_allpass1_t` sections, peak-to-peak minimization via grid search +
Nelder-Mead polish, multiple starts) — only the source data and fit band
changed:

1. Parsed the raw TFA sweep's `g`-off phase column.
2. Extracted group delay via `tau(f) = -(1/360) dPhase/dFreq`, Savitzky-
   Golay smoothed before differentiating (same technique the earlier
   same-day dispersion report used — but see the correction below on
   smoothing-window choice).
3. **Smoothing-window sensitivity check (new this round).** The real TFA
   sweep is far denser (~1000 points/24kHz) and noisier point-to-point
   than the LTspice sweeps every earlier fit in this document used. A
   light window (31 points, ~700Hz span — matching this project's other
   TFA chart work) leaves the extracted group-delay curve with tens-of-us
   swings above ~2kHz that don't shrink monotonically the way a real
   2-pole-plus-buffer-stage filter's phase should. Computed the residual
   (raw phase minus smoothed) and its RMS (0.5–2.6° depending on band,
   worst near 3–4.3kHz) — enough, once differentiated, to plausibly
   produce swings that size from noise alone, not real filter structure.
   Re-ran the fit's own analog-curve peak-to-peak across a range of
   smoothing spans (61/91/121/151/181/221/261 points ≈ 1.4–6.1kHz span):
   it stabilizes to within ~2us of its converged value from a 181-point
   (~4.2kHz) span onward. Used 181 points as the fit target — wide enough
   to be clear of the noise floor, not so wide it would smooth away a
   genuine single in-band hump if the real filter has one.
4. Fit grid: 100–8000Hz (dense, 800 points), replacing the old
   100–4300Hz grid. `Fs=16000Hz` throughout (no other Fs refit this
   round).

### Result

| | Peak-to-peak dispersion, 100–8000Hz | Mean delay |
|---|---|---|
| Analog filter alone (real hardware, Hi-Z-buffered TFA) | 71.2us | 71.2us |
| **Two** sections, `a1=0.026173`, `a2=0.236810` | **18.5us** (~3.9x) | 196.5us |

Mean added delay: ~125.4us (**2.006 samples @ 16000Hz**) — close to, and
slightly less than, the superseded LTspice fit's 2.105 samples.
`relative_delay_samples` needs re-tuning to roughly this new starting
point if/when `g` is re-enabled for testing, same as every prior
coefficient change in this file.

Checked over the OLD 100–4300Hz sub-band with the NEW coefficients: 18.5us
p-p — identical to the full-band figure (the equalized curve's worst-case
points both happen to fall inside the old band already), so this refit is
a strict widening of validated coverage, not a regression within the
previously-fitted range.

### Old coefficients applied to the new real-hardware analog data (context)

Useful sanity check on why "just extend the old fit's claimed range"
wouldn't have worked: applying the OLD (2026-09-01, LTspice-fit)
`a1=a2=0.622515` to this same real analog curve —

| Band | Analog alone (measured) | With OLD coefficients |
|---|---|---|
| 100–4300Hz (old fit's own band) | 36.8us p-p | 18.5us p-p (tracks the LTspice prediction of 41.1→13.7us reasonably well) |
| 100–8000Hz (new band) | 71.2us p-p | **449.2us p-p** |

The old coefficients still behave inside the band they were actually fit
for. Outside it, they diverge badly — a first-order all-pass section's
delay curve keeps changing all the way to Nyquist, and `a=0.622515` is
steep enough that the un-fit region past 4300Hz was always going to blow
up once anyone looked. This is the concrete argument for why extending
frequency coverage needed a real re-fit, not a documentation change.

### Correction to the same-day (2026-09-03) real-hardware dispersion report

The dispersion numbers first delivered this session (146.1us→147.2us p-p
over 100–4300Hz, "essentially no improvement from `g`", written up in
`ssb_mic_test_commands.md`'s earlier 2026-09-03 entry and the accompanying
chart) used the lighter 31-point/~700Hz smoothing window, the same one
this project's other TFA analyses have used against much sparser LTspice
data. Applied to this denser, noisier real-hardware sweep, that window
was too fine — re-running the SAME `g`-off/`g`-on measured curves (not a
model, the actual measured phase in both states) at the 181-point window
justified above gives a materially different picture:

| Band | `g` off (measured) | `g` on (measured, OLD coeffs) | |
|---|---|---|---|
| 100–4300Hz, 31-pt window (original report) | 146.1us p-p | 147.2us p-p | "no improvement" |
| 100–4300Hz, 181-pt window (this correction) | 36.6us p-p | 14.3us p-p | **real ~61% reduction, close to the 41.1→13.7us LTspice prediction** |

So the equalizer's OLD coefficients were doing closer to what they were
designed to do within their own fit band all along — the original "no
improvement" finding was substantially a smoothing-window artifact on
noisy real data, not a genuine equalizer failure at 100–4300Hz. This does
**not** overturn the separate, independently-confirmed 2026-09-01
real-hardware IMD finding (that document's own repeatable, delay-swept
3rd-order IMD measurements never depended on this phase-noise/smoothing
question) — it only revises the group-delay/TFA side of the picture. Left
the original entry in `ssb_mic_test_commands.md` in place with this
correction appended rather than edited away, per this project's usual
convention. Going forward, TFA group-delay analyses on real (not LTspice)
hardware sweeps should default to a wider smoothing window and a
convergence check like the one in the Method section above, not the
31-point window carried over from the LTspice-fitting era.

### Status

Numerically fit, not yet IMD-validated on real hardware (see the
group-delay validation immediately below, which is a different, narrower
claim than IMD validation). This refit directly addresses two of the four
suspected causes behind the 2026-09-01 negative IMD result (the narrow
100-4300Hz fit window, and — pending confirmation — the DC-bias-point
measurement inconsistency the Hi-Z buffers were added to fix); it does
nothing about the other two (the unity-magnitude all-pass structure still
can't correct this filter's real insertion-loss penalty; the
`envelope_interp`/`'I'` confound). **`g` stays off by default** until the
same real-hardware IMD3/IMD5 delay-sweep procedure documented above is
re-run against these new coefficients — a better-fitted equalizer is not
a re-validated one.

### 2026-09-03, same day: group-delay side confirmed on real hardware

New coefficients (`a1=0.026173`, `a2=0.236810`) flashed to the board and
re-measured with the TFA — a genuine third data point, not the same sweep
re-analyzed. Source: `Group delay off  on F Phase Refit on TF meas.txt`.

**Column-order note:** this file's two phase columns come out swapped
relative to the earlier file's off-then-on convention — its first column
matches the earlier *predicted* equalized curve almost exactly, and its
second matches the earlier *measured* analog-only curve, not the labels
the column order would otherwise suggest. Caught by a physical sanity
check: interpreting the columns at face value gives a NEGATIVE mean added
delay, which an all-pass network cannot produce (it can only add delay).
Swapping the interpretation fixes that and lines up near-exactly with the
independently-derived prediction — strong enough agreement that this
isn't ambiguous, but worth checking the TFA channel routing before the
next sweep so this doesn't have to be re-diagnosed from the data every
time.

| | Peak-to-peak, 100-8000Hz | Mean delay |
|---|---|---|
| Analog alone (this sweep) | 71.3us | 73.3us |
| Analog alone (previous sweep, for comparison) | 71.2us | 71.2us |
| **g on, new coefficients (measured)** | **20.3us** | 198.4us |
| g on, new coefficients (predicted, from the fit above) | 18.5us | 196.5us |

Mean added delay, measured: 125.2us (**2.003 samples @ 16000Hz**) — within
1us of the fit's own 125.4us/2.006-sample prediction. The measured "on"
curve tracks the predicted curve to within ~2.1us RMS (4.4us worst-case)
across the whole 100-8000Hz band. The two independent analog-alone
measurements (this sweep vs. the previous one) agree to within ~2.3us
RMS too — good evidence the Hi-Z buffer fix is giving repeatable
analog-filter measurements session to session, not just within one
sweep.

**What this does and doesn't confirm:** the group-delay/dispersion
prediction from the refit above is now validated directly on the bench,
not just modeled — both the shape (RMS agreement with the predicted
curve) and the headline numbers (added delay within 1us, p-p within
~2us) check out. This is NOT the same as IMD validation — see the Status
note above and `envelope_gdeq.h`'s header comment: the 2026-09-01 finding
that the OLD coefficients hurt 3rd-order IMD involved mechanisms (real
insertion loss, the possible `'I'` confound) that a group-delay TFA sweep
can't see either way. **`g` stays off by default** until the IMD
delay-sweep is re-run against these coefficients specifically.

## 2026-09-03, later same day: IMD delay-sweep with the NEW coefficients — real improvement, at a delay never tested before

**New real-hardware finding, user-reported:** with the refit coefficients
(`a1=0.026173`, `a2=0.236810`) on the bench, the optimum `relative_delay`
now sits at roughly **4 samples** — and unlike the old-coefficient sweep,
this optimum is consistent between two-tone and mic-noise stimuli (the
2026-09-01 sweep never achieved that consistency). Other delay values
still null specific higher-frequency products better, but the spread
across delay settings is much narrower than it was with the old
coefficients. Separately: **with `g` on at two-tone, the higher-order IMD
products are now significantly reduced** — a real, user-observed
improvement, not predicted or claimed by anything in this document before
now.

**This does not contradict the 2026-09-01 "closed-out" sweep.** That
sweep (a1=a2=0.622515) tested delay 1.10→1.40→2.00 and confirmed
everything got monotonically worse past 2.00 — but that result is
specific to THOSE coefficients. The new coefficients are a materially
different digital filter (much milder: 0.026/0.237 vs. 0.622/0.622), with
its own delay-vs-IMD landscape. A ~4-sample optimum for this filter is a
new, independent result, not a re-test of previously-explored territory —
the old sweep's "past 2.00 always worse" finding was never in a position
to rule this out, because it was never testing this filter.

### Is this a magnitude (insertion-loss) compensation effect? No — mechanism ruled out by construction

User's hypothesis, worth taking seriously: could the IMD improvement be
`g` incidentally compensating for the analog filter's own gain roll-off,
rather than (or in addition to) flattening group delay? **Ruled out
analytically, not just empirically**: `ssb_allpass1_t` implements
`H(z) = (a + z⁻¹)/(1 + a·z⁻¹)`, and for any real `|a| < 1` this has
`|H(e^jω)| = 1` at every frequency — a true all-pass network, unity
magnitude BY CONSTRUCTION, independent of the coefficient value. Two
cascaded sections are still unity magnitude (product of two unity-
magnitude responses). There is no mechanism by which `envelope_gdeq_process()`
can be altering the envelope's amplitude spectrum, only its phase — so it
cannot be responsible for compensating the insertion-loss shape measured
below, on this filter or any other. (A real DSP implementation has
floating-point rounding, but that's a many-orders-of-magnitude-too-small
effect to explain an audible/measurable IMD change — not a real
candidate mechanism.)

**Working explanation instead:** the ~4-sample delay region simply wasn't
part of the 2026-09-01 sweep (which used a different, steeper filter and
stopped exploring past 2.00 samples once every metric was getting worse
there). The new, milder coefficients' own optimum landscape is different,
and this real-hardware result says the group-delay flattening this
equalizer does IS paying off for IMD now that the right operating delay
has been found for it — consistent with, not contradicting, the original
design intent. Left as a working explanation rather than a closed case:
the mechanism is plausible and the magnitude explanation is ruled out,
but a full re-run of the old delay-sweep/IMD-table methodology (this time
around the ~4-sample region, both two-tone and mic-noise, both sidebands)
would turn this from "user observed it and here's why it's probably real"
into the same level of confirmed-and-quantified result the earlier
(negative) finding had. Not yet done — recommended next step whenever
there's bench time, since it would let `g`'s default flip from off to on
with actual evidence behind it, not just a promising anecdote.

### Real-hardware insertion-loss measurement (amplitude+phase TF, `g` off)

Separate from the IMD question but raised in the same message: the
analog filter's gain roll-off itself, now measured directly (not just
LTspice-predicted). Source: `TF meas.txt` — same sweep as the very first
group-delay TFA file (phase columns match point-for-point), now exported
with its amplitude channel included. Amplitude column is a linear ratio;
converted to dB and referenced to the 100–300Hz passband average (a flat
gain offset there reads as 0dB, so this table is already "excess loss
above the passband," no separate attenuator-baseline correction needed
the way the LTspice comparison in the 2026-09-01 section required).
Light smoothing (31-point Savitzky-Golay — residual RMS 0.05–0.48dB
depending on band, much cleaner than the phase channel needed):

| Frequency | Role | Measured loss (real hardware) | LTspice prediction (2026-09-01, PNP+attn) |
|---|---|---|---|
| 700Hz | two-tone f1 | -0.45dB | — |
| 1900Hz | two-tone f2 | -1.85dB | — |
| 500Hz | IMD3 lower offset | -0.28dB | ~-2.56dB raw / -0.03dB excess-only |
| 1700Hz | IMD5 lower offset | -1.55dB | ~-3.27dB raw / -0.42dB excess-only |
| 3100Hz | IMD3 upper offset | -4.52dB | ~-6.29dB raw / -1.41dB excess-only |
| 4300Hz | IMD5 upper offset, old fit-band edge | -8.75dB | ~-10.42dB raw / -2.41dB excess-only |
| 8000Hz | `MAX_FREQ_DEV_HZ` limit | **-21.70dB** | ~-22.65dB raw |

Real hardware and the LTspice sim agree well at the high end (8000Hz:
-21.7 measured vs. -22.65 predicted) and are in the same ballpark
through the middle of the band, with real hardware showing somewhat less
loss at the low-to-mid frequencies than the sim's raw (non-excess-
corrected) numbers — consistent with the sim's raw figures including the
gate attenuator's flat offset, which this measurement's passband-relative
referencing already excludes by construction. Bottom line: the roll-off
is real, confirmed on the bench, and substantial — nearly 22dB down by
8000Hz, the same frequency the null-transient content and `freq_dev`
excursions live at.

**Not yet compensated.** A magnitude correction would need an actual
gain-shaping filter (shelf/peaking IIR, or similar) — `ssb_allpass1_t`
structurally cannot do this (see above). A full inverse response would
need up to ~+22dB of boost at 8000Hz relative to the passband, which
raises real questions before building it: how much of that boost is
actually recoverable headroom vs. how much just raises the noise floor
and quantization/PWM-resolution noise at exactly the frequencies where
`freq_dev` already pushes hardest, and whether a full inverse or a
capped/partial shelf is the better real-world tradeoff. Parked here as
the next concrete step — see the accompanying conversation for the
scoping question before implementation starts.

## Related: DC-operating-point dependence (envelope filter TF, not group delay)

Separate from the group-delay/equalizer work above: real hardware testing
with the sine-chirp test mode (`w`, see `ssb_mic_test_commands.md`) found
that this analog filter's transfer function itself shifts with the
envelope's DC operating point (duty-range offset/scale, not master gain,
which only scales swing around a fixed mean) — the -90° phase-crossing
frequency moved from 2626Hz (low DC) to 2441Hz (high DC), a -7.1% shift,
growing to -14% by the -180° crossing (6899Hz -> 5930Hz). Confirmed as a
real, monotonic, hardware-measured effect, not noise. Not yet incorporated
into the equalizer fit above (which assumes one fixed analog TF) — the
group-delay equalizer's own coefficients would, in principle, need to vary
with DC operating point too if this turns out to matter enough to chase
further. Parked here since it's the same underlying analog filter this
whole document is about, even though it surfaced from the TF measurement
work rather than the group-delay refit itself.

## 2026-09-03, later same day: envelope magnitude equalizer built (`envelope_ampeq.h`/`.cpp`, key `a`) — NOT YET VALIDATED

Follow-up to the insertion-loss measurement section above. Discussed the
scoping question directly: a full inverse response needs up to ~+22dB of
boost at 8000Hz, which risks trading a well-characterized amplitude
problem for an unknown noise-floor/PWM-quantization/headroom problem —
agreed to start with something more modest at the high end and measure
before going further.

**Why a shelf, not the same shape as the existing presence EQ.** Before
picking a filter shape, checked whether the existing pre-Hilbert
audio-path presence EQ (`biquad_set_peaking`, `ssb_dsp.c`, an RBJ
peaking/bell biquad — currently fc=2200Hz, Q=1.0, +4dB, the "4dB presence
increase" setting) could do this job. Computed its response directly
(not assumed): it peaks at +4.00dB at 2200Hz, crosses half-gain (+2dB) at
roughly 1412Hz and 3264Hz, and is back to +0.00dB by 8000Hz. A peaking
filter is symmetric and returns to unity gain on both sides of its
center by construction — it cannot address a loss that keeps climbing
monotonically out to 8000Hz. The measured insertion loss (previous
section) is monotonic, not a dip, so the right shape is a shelf: constant
plateau gain above its corner, not a bump that decays away again.

**The new primitive.** Added a public `ssb_shelf_biquad_t` (Direct-Form-I,
RBJ cookbook coefficients) to `ssb_dsp.h`/`.c`, alongside the existing
`ssb_allpass1_t` — `ssb_shelf_biquad_set_highshelf()`,
`ssb_shelf_biquad_reset()`, `ssb_shelf_biquad_process()`. Named
`ssb_shelf_biquad_t` rather than the more obvious `ssb_biquad_t` because
`ssb_adc_filter.h` already defines its own, differently-shaped
`ssb_biquad_t` (Direct-Form-II-Transposed, used for the ADC low-pass
filters) — the first version of this code used `ssb_biquad_t` and hit a
"multiple definition" link error at compile time, caught immediately when
the user tried to flash it; renamed to fix. Kept separate from the private peaking-EQ biquad
already in `ssb_dsp.c` (that one stays wired one specific way into the
pre-Hilbert audio_fx chain; this one is the general-purpose public
primitive, reused for the envelope-path shelf below). Unlike
`ssb_allpass1_t`, this is explicitly NOT unity-gain — that's the whole
point.

**The chosen shelf.** `envelope_ampeq.h`/`.cpp` (mirrors
`envelope_gdeq.h`/`.cpp`'s structure — same `#if SAMPLE_RATE_HZ ==
16000 && ENV_FILTER_VARIANT == ENV_FILTER_PNP_BC327_ATTN ... #error`
gating, so this shelf can't silently get applied to a different
filter/Fs it wasn't chosen for). Corner `ENV_AMPEQ_SHELF_FREQ_HZ =
2500Hz`, plateau gain `ENV_AMPEQ_SHELF_GAIN_DB = 6.0dB`, RBJ high-shelf
S=1. Chosen by inspection against the measured loss table, not by
numerical optimization (there's no obvious single objective for a
2-parameter shelf the way there was for the 2-parameter all-pass
group-delay fit). Net (measured loss + shelf gain) at the table's key
frequencies:

| Frequency | Measured loss alone | Shelf gain | Net (residual) loss |
|---|---|---|---|
| 500Hz | -0.28dB | +0.01dB | -0.27dB |
| 700Hz | -0.45dB | +0.03dB | -0.42dB |
| 1700Hz | -1.55dB | +0.95dB | -0.60dB |
| 1900Hz | -1.85dB | +1.38dB | -0.47dB |
| 3100Hz | -4.52dB | +4.41dB | -0.11dB |
| 4300Hz | -8.75dB | +5.69dB | -3.06dB |
| 8000Hz | -21.70dB | +6.00dB | -15.70dB |

Recovers most of the loss through the two-tone/IMD-offset region
(500–3100Hz), meaningfully reduces it at the old 4300Hz gdeq fit-band
edge, and deliberately leaves most of the 8000Hz-region loss
uncorrected — the conservative "modest" step, not a full inverse.

**Wiring.** New key `a` (serial_commands.cpp) toggles it, mirroring `g`
exactly, including reset-on-off→on-transition. Applied in the main
`dsp_task` pipeline right after `envelope_gdeq_process()` (order doesn't
matter — both LTI — placed there purely for code locality) and in the
`AUDIO_SRC_CHIRP` early-intercept path right after gdeq's own chirp-path
call, so the `w` chirp/TFA workflow's amplitude channel can validate this
shelf's actual on-bench correction directly, the same way the phase
channel already validated gdeq's refit. `PersistentSettings` gained a
new trailing field `env_ampeq_enable` (off by default, same zero-fill-
safe convention as every other trailing field); the `P` dump and preset-
load paths were updated to match.

**Interaction with gdeq.** Both LTI, so cascade order doesn't change the
combined response mathematically. Unlike `ssb_allpass1_t`, a high-shelf
biquad's magnitude isn't flat, so it does have its own (uncharacterized)
small group-delay contribution near its corner — not yet folded into
gdeq's fit. If the combined on-bench group delay measurably diverges from
the gdeq-alone prediction once `a` is enabled, that's the first place to
look.

**Status: NOT YET VALIDATED ON REAL HARDWARE.** Off by default. Next
step is a `w` chirp/TFA sweep with `a` ON to check the amplitude channel
against the predicted net-correction table above, then real two-tone/mic
IMD testing — same validation sequence gdeq went through.

## 2026-09-03, later same day: `a` validated on real hardware — magnitude tracks prediction, but it reintroduces real group-delay dispersion

Source: `ga_Trial1_TF.txt`, a real TFA sweep with both `g` and `a` on
(amplitude+phase, same instrument/convention as `TF meas.txt`). First
real-hardware two-tone/mic IMD feedback on `a` came in alongside it: a
genuine positive effect — it balances the upper/lower sideband IMD levels
and brings the LF-region higher-order IMDs down to match the HF-region
ones. `eq` (`e`, the pre-Hilbert HPF+presence chain) still has the
biggest overall positive effect on IMD, separately discussed below.

**Magnitude side: tracks the predicted net-correction table well.**
Referenced to its own 100–300Hz passband average, same method as the
insertion-loss measurement:

| Frequency | Predicted net (design table above) | Measured net (this sweep) |
|---|---|---|
| 500Hz | -0.27dB | -0.41dB |
| 700Hz | -0.42dB | -0.54dB |
| 1700Hz | -0.60dB | -0.54dB |
| 1900Hz | -0.47dB | -0.85dB |
| 3100Hz | -0.11dB | +0.12dB |
| 4300Hz | -3.06dB | -2.43dB |
| 8000Hz | -15.70dB | -16.03dB |

Agreement is within a few tenths of a dB at every point except 1900Hz
(0.38dB off) — well within normal real-hardware measurement variation,
nothing here suggests the shelf isn't doing what it was designed to do.

**Group-delay side: a real, quantified side effect, not just the
theoretical caveat `envelope_ampeq.h` flagged.** Compared against the
earlier `g`-only validation sweep (100–8000Hz, same 181-point
Savitzky–Golay convention as every other group-delay figure in this
document):

| | `g` only (measured, earlier validation) | `g`+`a` (measured, this sweep) |
|---|---|---|
| p-p dispersion | 19.4µs | **81.7µs (~4×)** |
| mean absolute group delay | 198.6µs | 197.2µs (essentially unchanged) |

The shelf's own group delay, computed directly from its RBJ coefficients
(fc=2500Hz, +6dB, S=1, no measurement involved): swings from about
-37µs just below its 2500Hz corner to +20µs above it (p-p ≈65µs, i.e.
almost the entire size of the measured increase) while averaging only
≈0.4µs across the band (i.e. almost the entire reason the absolute mean
barely moved). Adding this theoretical shelf-alone curve to the `g`-only
measured curve (simple LTI superposition) reproduces the measured `g`+`a`
curve's shape to within ~7µs RMS — not as tight as gdeq's own ~2µs
RMS fit-to-measurement match (expected: this combines two separately-
smoothed noisy real sweeps rather than smoothing one already-combined
measurement), but more than enough to confirm the mechanism: **the shelf
is not phase-transparent, and gdeq's fit has no way to know about it**
(gdeq was fit purely against the analog filter's own phase response,
before ampeq existed). Chart: `gdeq_ampeq_delay_chart_v4.html`.

**Not a contradiction of the magnitude result or the user's IMD report**
— both stand as measured. It's a second, independent effect worth
tracking: right now the shelf's amplitude-symmetry benefit is winning on
the bench, but if `a`'s gain is ever pushed higher (getting closer to the
full ~22dB inverse this was deliberately capped short of), the shelf's
own delay swing will grow too — it scales with how much correction the
shelf is asked to make, the same way its magnitude effect does. Worth
re-running the IMD delay-sweep (the one that found ~4 samples optimal for
`g` alone) with `a` also on, since its own group-delay contribution could
shift where that optimum sits.

## 2026-09-03, later same day: is `eq`'s IMD benefit the HPF or the presence boost?

User's real-hardware report: `eq` (`e`) still has the single biggest
positive effect on IMD of everything tried so far — cleans up
inter-modulation tones broadly, not just the sideband-symmetry effect
`a` produces. Asked whether this is specifically the 300Hz highpass
stage's influence.

**Can't be isolated with the current code — `eq_enable` gates both
stages as one unit.** `ssb_dsp.c`'s `ssb_dsp_process_sample()` runs
`biquad_process(&h->eq_hpf, ...)` then `biquad_process(&h->eq_presence,
...)` back to back, both inside the single `if (handle->eq_enable)`
block — there's no way to toggle the 300Hz HPF and the 2200Hz/+4dB
presence peak independently via serial command today.

**Working hypothesis, not yet confirmed: more likely the HPF.** This
project has already established (`null_bias_investigation.md`,
2026-08-31) that `wrap_pi()`'s resolution of the two-tone envelope's
phase behavior right at destructive-interference nulls carries a real,
measurable bias — and that investigation's own reproduction workflow
explicitly calls for `e` (and `c`) to be off before taking a clean
`null_bias`/`null_bias2` reading, i.e. this project already treats `eq`
as a confound for null-crossing phase behavior, not just a tone-coloring
effect. A 300Hz 2nd-order highpass removes near-DC content (mic/ADC
offset, sub-audio drift, mains hum) from the audio *before* the Hilbert
transform — exactly the kind of content that would show up as an
envelope-baseline/near-null error once split into envelope+phase, and
that's a plausible mechanism for a broad, general IMD cleanup rather than
just a narrowband tone-coloring effect. The 2200Hz/+4dB presence peak, by
contrast, has no obvious general IMD mechanism at the two-tone
frequencies in use (500–1900Hz) — it's there for perceived voice
intelligibility, not distortion.

This is a hypothesis worth testing directly, not a conclusion — the
cleanest way is to split `eq_enable` into two independently-toggleable
flags (HPF-only, presence-only) so both can be A/B'd on the bench the
same way `g`/`a` are. Not yet done — parked here pending the user's
go-ahead, see the accompanying conversation.

## 2026-09-03, later same day: why `eq` can't just be added to the `w` chirp/TFA test loop the way `g`/`a` were

User asked whether `eq` could be added into the `w` sine-chirp/TFA test
loop the same way `g` and `a` were. Structurally it can't be done the
same trivial way, for an architectural reason worth recording:

`g` (`envelope_gdeq_process()`) and `a` (`envelope_ampeq_process()`) both
operate *downstream*, on an already-extracted envelope AMPLITUDE value —
that's exactly the domain `test_signals_generate_chirp()` synthesizes
directly, so calling them in the `AUDIO_SRC_CHIRP` fast-path (which
completely bypasses `ssb_dsp_process_sample()` — see
`ssb_mic_test.ino`'s own comment on this) was a same-domain, one-line
addition each time.

`eq` (`ssb_dsp_set_eq_enabled()`, the HPF+presence chain) operates
*upstream* — on the raw single-channel AUDIO sample, *before* the
Hilbert transform that splits it into envelope and phase in the first
place. It lives entirely inside `ssb_dsp_process_sample()`, which the
chirp fast-path bypasses by design. There is no "envelope value" for
`eq` to run on at the point the chirp injects its sweep — `eq`'s effect
is on the audio signal that DETERMINES both the envelope and the phase
channels together, not a post-processing step on either one alone.

Characterizing `eq` with a TFA-style sweep the way `g`/`a` were
characterized would need a genuinely different test mode: a swept sine
fed in as the raw AUDIO sample (i.e. through `ssb_dsp_process_sample()`
itself, `eq` on vs. off), with the resulting `envelope` AND `freq_dev_hz`
outputs captured as the TFA's two channels — a legitimately new test
signal generator (`test_signals.h` pattern), not a one-line addition to
the existing envelope-domain chirp. This would actually be a more
complete tool than the current chirp, since it would show `eq`'s
combined effect on both the envelope and phase paths together (which is
exactly the domain a real two-tone/mic signal lives in) rather than
either path in isolation. Not yet built — parked here as a scoped,
concrete next step pending the user's go-ahead, see the accompanying
conversation.

## 2026-09-03, later same day: what is `eq` actually doing to the 700/1900Hz two-tone signal?

Follow-up to the HPF-vs-presence question above — computed the two
stages' actual RBJ response (exact coefficients from `biquad_set_highpass`/
`biquad_set_peaking` in `ssb_dsp.c`) at the default two-tone pair
(`TWOTONE_F1_HZ`/`TWOTONE_F2_HZ`, 700/1900Hz, `config.h`) and at the
65-tap Hilbert FIR's (`HILBERT_TAPS`, `generate_hilbert_coeffs()`) own
frequency response, to see what's mechanistically available to explain
the reported IMD benefit.

**Ruled out: tone-level balancing.** At 700Hz/1900Hz the 300Hz HPF
removes essentially nothing (-0.14dB / -0.00dB) — it's not shaping the
wanted tones at all. The 2200Hz presence peak, if anything, works
*against* balance: +0.40dB at 700Hz vs. **+3.60dB at 1900Hz** — a real
asymmetric boost of the higher tone, the opposite of what a "levels the
tones" explanation would need.

**Found: the HPF imposes a real, frequency-dependent phase-delay
difference between the two tones, even where it barely touches their
amplitude.** Phase-delay (each pure tone's own time-shift through the
filter, `-phase(f)/(2πf)` — not the same "group delay" this document uses
elsewhere for envelope dispersion, so naming it distinctly here to avoid
confusion) at 700Hz is **-144µs**, at 1900Hz only **-18µs** — a
**~126µs differential shift between f1 and f2**, imposed on the raw
audio sample *before* it ever reaches the Hilbert transform. That's the
same order of magnitude as the delay `g` itself was built to compensate
for downstream, just happening upstream instead. Shifting the relative
timing between the two input tones changes the exact shape of the
resulting envelope/phase trajectory the whole downstream chain
(quantization, PWM, the analog reconstruction filter) has to reproduce —
a concrete, testable candidate mechanism, and one that applies even to
the firmware's own purely-synthetic `'t'`/`'T'` two-tone generator
(`test_signals.cpp` — no real audio front end involved there at all).

**Still relevant if the two-tone signal is ever fed through the real mic
front end instead of the synthetic generator:** the Hilbert FIR's own
frequency response, computed directly from its 65 taps, is flat to
better than 0.1dB from ~500Hz up to Nyquist, but is NOT flat below that —
already -0.72dB at 300Hz, -2.68dB at 200Hz, -7.65dB at 100Hz, and
mathematically forced to exactly zero at DC (the antisymmetric
windowed-sinc construction always nulls there). Any real-world content
down there — mic/ADC DC offset, mains hum, thermal drift — would get an
inaccurate quadrature component once Hilbert-transformed, and since
envelope and phase are both derived from that same analytic signal, the
error leaks into both channels at once — a broadband effect, not tied to
one IMD product, matching a "cleans up IMDs generally" description. This
mechanism has nothing to act on for the *synthetic* two-tone signal
specifically (it's two clean sinusoids, no sub-300Hz content to strip),
so it only applies if the two-tone test in question actually runs through
the ADC/mic path.

**Which mechanism is live depends on which test path was used** — parked
both here since it isn't yet known which (or both) explain the reported
result; see the accompanying conversation. Splitting `eq_enable` into two
independently-toggleable flags (proposed above) remains the clean way to
settle this on the bench rather than reasoning about it further from
coefficients alone.

## 2026-09-03, later same day: confirmed synthetic-only — and a stronger candidate mechanism found

User confirmed all `eq` A/B testing to date uses only the firmware's
synthetic `'t'` two-tone generator, never the mic/ADC path. **This
cleanly rules out the Hilbert-FIR-low-frequency-conditioning mechanism
above** — a mathematically pure two-tone sum has no DC offset, hum, or
drift for the HPF to strip, so there's nothing for that mechanism to act
on here. That leaves the differential phase-delay finding, plus a third,
considerably stronger candidate found while re-checking the combined
(HPF+presence, cascaded, matching the actual code order) response rather
than each stage in isolation:

**The presence peak's asymmetric boost keeps the two-tone envelope from
ever reaching a true zero null.** For two tones of amplitude A1, A2, the
envelope's minimum is exactly `|A1-A2|` and its maximum is `A1+A2` —
textbook two-tone algebra. With `eq` off, `TWOTONE_AMPLITUDE` (0.45,
`config.h`) is applied equally to both tones, so A1=A2 and the envelope
hits a **literal, exact zero** at every destructive-interference null —
the single hardest condition this whole project has spent significant
effort characterizing (the predistort LUT's steepest, most sparsely-
characterized region sits at 36–55% duty specifically because of this;
`null_bias_investigation.md`'s whole subject is a discrete artifact at
exactly this condition; the `'x'`/`'z'` envelope-floor feature exists
specifically to avoid driving the envelope this low).

With `eq` on, the combined HPF+presence response (computed at the exact
coefficients, cascaded in the real code order) is +0.26dB/-176µs
phase-delay at 700Hz and **+3.60dB**/-30µs phase-delay at 1900Hz — almost
all of that dB difference comes from the presence peak (the HPF's own
amplitude effect at both tones is under 0.15dB, negligible). That's a net
**+3.34dB (~1.47×) amplitude mismatch between the two tones**. Redone
through the actual `TWOTONE_AMPLITUDE=0.45` numbers: A1→0.4636,
A2→0.6809 — envelope minimum rises from an exact 0 to **0.217**, i.e.
**-14.4dB relative to the new peak (1.145)**. In other words: `eq`, as
currently wired, is *inadvertently* doing something close to what the
dedicated `'x'`/floor feature does deliberately — keeping the envelope
out of its worst, most-nonlinear region — except it does it as an
unplanned side effect of the presence peak's asymmetric gain, tied to the
specific two-tone pair in use, rather than as a controlled, symmetric,
tunable floor.

**This also reframes the earlier HPF-vs-presence guess — the opposite
way round.** Correcting the working hypothesis from two entries above (that
guessed the HPF was more likely responsible, reasoning from the
null-crossing bias investigation's own test protocol): with the synthetic
generator confirmed as the only signal path in use, the presence peak
looks like the more likely dominant contributor via this null-floor
mechanism, not the HPF. The HPF's own contribution is real but smaller —
the ~126µs (HPF-alone) to ~146µs (combined) differential phase-delay
between the two tones remains a genuine, separate effect, just evidently
a second-order one next to a >14dB null-floor change.

**Falsifiable prediction for the split-toggle test (still the right next
step):** presence-only should reproduce most of `eq`'s current IMD
benefit; HPF-only should show much less. If that holds, it also predicts
that dialing in the existing `'x'` envelope-floor control (with `eq` off
entirely) might reproduce a similar benefit in a cleaner, independently-
tunable way — worth trying directly on the bench, no code changes needed
for that particular check.

## 2026-09-03, later same day: `'x'` makes IMDs worse — resolved, and confirms amplitude (not tone timing) is the mechanism

User tried the `'x'` floor prediction above directly: **it makes IMDs
worse in every case tried, not better.** Separately, and unprompted, the
user reports scoping the envelope signal directly: `eq` off shows real
zeros at the nulls with bad IMDs (particularly the low-order inter-IMD
tones already flagged before, around 200/400Hz); `eq` on shows a visibly
higher envelope floor and cleaner IMDs — an independent, direct
hardware confirmation of the envelope-floor-height finding above, not
just a math prediction anymore. Question asked: is the benefit from the
tones' relative *phase* (the ~126–146µs timing-shift finding) or just
their relative *amplitude*?

**Answer: amplitude — and the reasoning explains why `'x'` backfires.**
Modeled the analytic signal directly (`A1*e^{jw1t} + A2*e^{jw2t}`,
exact for a sum of two positive-frequency tones) and computed the
per-sample instantaneous frequency (`freq_dev`) through one full
700/1900Hz beat cycle at 16kHz, for both conditions:

- **`eq` off (equal amplitude, A1=A2=0.45):** the sample landing nearest
  the null shows `freq_dev` **flip sign** right there — +1300Hz on the
  samples either side, -6700Hz on the one nearest the null. That's not
  just a big number, it's a genuine derivative discontinuity: the
  complex trajectory is passing essentially through the origin, and the
  phase has nowhere to go but reverse direction abruptly. (The exact
  peak value is sample-grid-dependent — how close a given sample lands
  to the true continuous-time null, which is a genuine, unbounded
  singularity in continuous time — which is very likely why the earlier
  delay-sweep found some delay values "null out higher freq better" than
  others with real spread: the delay setting shifts exactly which sample
  lands closest to the true null.)
- **`eq` on (mismatched amplitude, A1=0.4636, A2=0.6809 from the
  computed ampl. response above):** `freq_dev` through the same region
  rises and falls smoothly — 1744→2435→4071→2953→1882→1587→1483Hz — no
  sign flip, no discontinuity, bounded.

The reason is geometric, not about timing: the envelope minimum is
exactly `|A1-A2|`, which depends only on the two amplitudes — a pure
relative-*phase*/timing shift between the tones (what the HPF alone
contributes) only moves *when* the null occurs, never *whether* the
complex trajectory actually reaches the origin. Only an amplitude
mismatch keeps the trajectory bounded away from the origin, which is
what keeps `freq_dev`'s derivative bounded and sign-consistent. So the
answer to "phase or amplitude" is **amplitude** — the ~126–146µs timing
shift from the HPF is real but doesn't touch null *depth*, only its
timing, and null depth is what the phase-derivative singularity actually
depends on.

**This also fully explains why `'x'` fails.** `envelope_floor_apply()`
runs on the envelope value only, after it's already been computed — it
never touches `freq_dev_hz` (confirmed both by re-reading
`envelope_floor.h`'s own doc comment and by the REVISION-1 postmortem
in the "Envelope-null floor" section of `ssb_mic_test_commands.md`,
which describes exactly why freezing `freq_dev_hz` too was tried and
reverted). So with `'x'` engaged and `eq` off, the *actual* input tones
are still exactly equal amplitude — the true analytic-signal trajectory
still passes through the origin, `freq_dev` still takes that same
violent, sign-flipping excursion right at the null — but now the
envelope value being reconstructed at that exact instant has been
artificially forced UP by the floor remap. That means the violent phase
transient now rides on a higher-amplitude carrier instead of a
naturally near-zero one — normally, a deep null's low RF amplitude
somewhat masks how much that transient's spectral spread actually
contributes to the output; `'x'` removes that masking without touching
the transient itself, which is a plausible, mechanistically clean reason
it measures worse rather than better. `eq`'s mismatch, by contrast, fixes
the actual root cause upstream (the trajectory never approaches the
origin in the first place), so there's no transient left to mask.

**Net conclusion:** the improvement `eq` provides is real, amplitude-
driven, and now confirmed on the bench (envelope floor height) as well
as by the math. The HPF's own ~126–146µs tone-timing shift is a real,
separate, and much smaller effect that doesn't touch null depth at all.
The split-toggle test (HPF-only vs. presence-only) is still the cleanest
way to confirm the relative sizes on the bench, but is no longer needed
to answer the phase-vs-amplitude question itself — that one's settled.

## 2026-09-03, later same day: this is a known, named problem in the EER/polar-transmitter literature — research summary

User asked whether this null/phase-flip issue is something the field has
already tackled, and specifically whether noise injection/dithering
could help. Had this researched properly (web search, not recalled from
memory) rather than guessed at. Findings, each with a real source:

- **This is THE well-known Achilles-heel of Kahn-technique/EER/polar
  transmitters**, not a quirk specific to this project's architecture.
  Traces back to L. Kahn's original 1952 EER paper (*Proc. IRE*), is
  covered in the standard survey series (Raab, Asbeck, Cripps, et al.,
  "RF and Microwave Power Amplifier and Transmitter Technologies," Parts
  1–5, *High Frequency Electronics*, 2003–2004), and is precisely
  characterized in **J. Zhuang, K. Waheed, R. B. Staszewski, "A
  Technique to Reduce Phase/Frequency Modulation Bandwidth in a Polar RF
  Transmitter," IEEE Trans. Circuits and Systems I, vol. 57, no. 8, Sept.
  2010, pp. 2196–2207** — which states the phase/magnitude split causes
  bandwidth expansion of "~10× the original signal bandwidth, or
  theoretically infinite... when the signal trajectory passes through or
  near the constellation origin." That "theoretically infinite" framing
  matches this session's own finding almost exactly (the discrete
  `freq_dev` sign-flip at the null sample).
- **The established fix matches what `eq` stumbled into, generalized
  correctly**: Zhuang et al. propose "altering the signal trajectory
  such that it avoids crossing (and proximity of) the constellation
  origin," done in the Cartesian (I/Q) domain **before** the
  envelope/phase split — at a small accepted EVM/ACLR cost. This
  independently validates the direction found this session by
  first-principles reasoning (the fix has to happen on the complex
  signal upstream of the split, not as a post-hoc scalar remap of the
  extracted envelope — exactly why `'x'` failed and `eq`'s upstream
  tone-amplitude mismatch worked). Important nuance: naive radial
  magnitude clamping (push `|z|` up, preserve its angle unchanged) does
  **not** by itself fix anything — `angle(k·z) = angle(z)` for any real
  `k>0`, so scaling magnitude alone leaves the angle/`freq_dev` trajectory
  completely untouched. Whatever "alter the trajectory" means in
  practice, it has to change the angle sequence too, not just rescale
  radius — `'x'` is a clean real-world demonstration of exactly this
  distinction going wrong.
- **Commercial precedent for envelope floor limiting**: US Patent
  7,412,213, "Envelope Limiting for Polar Modulators" (Sequoia
  Communications, filed 2006) describes low-side envelope floor limiting
  specifically to prevent envelope collapse at nulls. Flagging a caveat
  though, not a contradiction: this project's own `'x'` is exactly this
  idea (envelope floor limiting) and it measured worse, not better —
  strongly suggesting that if the patented technique works, it isn't
  doing a naive post-hoc scalar remap of an already-extracted envelope
  value the way `envelope_floor_apply()` does; it's likely applied
  somewhere that also reshapes the phase trajectory. Patent claims not
  independently verified in this pass.
- **Phase-path bandwidth/slew-rate limiting is the other standard,
  complementary technique class** (general EER practice — the phase
  modulator's own finite bandwidth naturally does some of this in analog
  Kahn implementations). This project already has a directly applicable
  tool for it: the `freq_dev` slew-rate limiter (see that section of
  `ssb_mic_test_commands.md`) — not yet specifically tested against this
  null-crossing spike. Cheapest next experiment, no new code needed.
- **Dithering/noise injection: NOT FOUND as an established or precedented
  technique for this specific problem.** Targeted search (EER/polar +
  dithering, phase-discriminator zero-crossing dithering, CORDIC
  near-origin literature) turned up nothing proposing noise injection to
  avoid this exact near-origin angle singularity. CORDIC literature
  treats near-origin error as an accuracy/analysis topic, not something
  dithered away. This doesn't mean it wouldn't work — the general
  principle (statistically avoiding a rare deterministic worst-case
  coincidence, at the cost of a small broadband noise floor, same logic
  as ADC dither) is sound elsewhere in DSP — but it should be treated as
  an untested idea worth a real bench measurement, not an established
  fix being reused.
- **Correction to an assumption made earlier this session**: had
  speculated the exact-zero null might be mainly an artifact of the
  deliberately worst-case, equal-amplitude two-tone IMD test rather than
  a real operational concern. The Zhuang et al. paper demonstrates the
  same bandwidth-expansion problem using a real WCDMA signal, not a
  synthetic two-tone — no support found for the "mostly a torture-test
  artifact" framing, so retracting it. Treat this as a genuine concern
  worth solving properly, not just a two-tone benchmark curiosity.

**Concrete next steps, roughly in cost order:** (1) test the existing
`freq_dev` slew-rate limiter specifically against the null-crossing
spike — already built, zero new code; (2) design a deliberate version of
what `eq` does by accident — a small, controlled adjustment to the
complex analytic signal (after the Hilbert transform, before
magnitude/phase extraction) that keeps the trajectory off the origin
independent of `presence_gain_db`/voice-EQ settings — not yet designed,
needs real thought about what specifically to add/reshape given radial
clamping alone is proven not to work; (3) if wanted, a real bench test of
noise injection, going in with clear eyes that it's unprecedented for
this exact problem, not a known fix being applied.

## 2026-09-03, later same day: slew-rate limiter real-hardware result — and the code's own assumption behind `FREQ_DEV_SLEW_MAX_FINITE_HZ` was wrong

Tested the "cheapest next experiment" from the list above. Real-hardware
result: **tightening the slew limit below 8000Hz/sample makes 3rd-order
IMD worse, even the smallest step down** — but loosening it ABOVE the
firmware's current 8000Hz/sample ceiling, to 12000 and then 20000,
gained 1-2dB in 3rd-order IMD with no other products getting worse (some
improved too). This directly falsifies the assumption written into
`ssb_dsp.c`'s own comment next to `FREQ_DEV_SLEW_MAX_FINITE_HZ`
(currently 8000.0f): *"raw freq_dev itself never exceeds max_freq_dev_hz,
~8000Hz, so a same-sign single-sample swing that large is already the
largest possible."* That reasoning only covers a same-sign swing. It
misses the actual worst case: a swing that crosses through/near zero at
a null flips SIGN, and a sign-crossing swing between two large-magnitude
opposite-sign values can be close to double a same-sign swing's size.

Modeled this directly: simulated the analytic signal for the 700/1900Hz
pair at 16kHz across many different sample-grid phase alignments (since
which discrete sample lands nearest the true continuous-time null is
alignment-dependent — the true continuous-time singularity is unbounded,
but sampling quantizes how close any real sample can get to it, which is
also very likely why the 2026-09-01 delay-sweep found some delay values
"null out higher freq better" than others with real spread — the delay
setting shifts exactly which sample lands closest):

- **`eq` off (true null, equal amplitude):** worst-case single-sample
  `|Δfreq_dev|` over many alignments ≈ **9640Hz** — comfortably explains
  why 8000Hz/sample wasn't enough (still clipping the worst alignments
  hard), why tightening further only clips progressively more of the
  large-but-otherwise-legitimate excursions near a null (hence
  monotonically worse), and why loosening to 12000 then 20000
  progressively reduced how hard that worst-case event gets clipped.
- **`eq` on (mismatched amplitude, no true null):** worst-case
  single-sample `|Δfreq_dev|` over the same alignments ≈ only **1650Hz**
  — again confirms `eq` and a sufficiently loose slew limiter are
  addressing the *same* underlying problem from two different ends: `eq`
  prevents the trajectory from ever approaching the danger zone; a loose
  enough slew limiter just stops harshly clipping the rare event when the
  trajectory does approach it (with `eq` off).

**Practical firmware gap:** `FREQ_DEV_SLEW_MAX_FINITE_HZ = 8000.0f` is
also the ceiling the interactive `'}'` key steps up to before snapping to
fully unlimited — so the empirically useful 12000–20000+ range the user
found **cannot currently be reached from the serial interface at all**,
only by setting `freq_dev_slew_limit_hz` some other way (the setter
itself, `ssb_dsp_set_freq_dev_slew_limit_hz()`, has no upper clamp, only
the lower one at `FREQ_DEV_SLEW_MIN_HZ` — so this was already reachable
without a recompile, just not via `'{'`/`'}'`). Worth raising
`FREQ_DEV_SLEW_MAX_FINITE_HZ` well past 20000 (with real margin over the
~9640Hz worst case found above — the 700/1900Hz pair specifically; a
different two-tone spacing or sample rate would shift that number) so
`'}'` can explore this range interactively, and correcting the stale
comment. Not yet done — see the accompanying conversation for the
proposed value and whether to also touch `FREQ_DEV_SLEW_STEP_HZ`/count
for reasonable button-press granularity over the wider range.

**Not yet done:** a finer sweep between 8000 and 24000+ (only 8000, 12000,
20000 tried so far) to find the true optimum rather than just confirming
the direction of improvement; also worth trying fully unlimited directly
for comparison, since the trend so far (8k worse → 12k better → 20k
better still) is monotonically improving toward looser, and it isn't yet
known whether that keeps improving all the way to off or peaks somewhere
finite.

---

## 2026-09-04 — extending `ampeq`'s magnitude correction toward 8000Hz: a second shelf stage, and its group-delay price

**Request:** "I'd like to extend the gain comp higher in freq. What can we
do there without breaking the group delay. I want to see if this would
improve the higher IMDs further." — a direct follow-on from the 2026-09-03
`ga_Trial1_TF.txt` work, which left the shelf-1-only design still -15.70dB
net at 8000Hz (measured -21.70dB loss, shelf-1 only clawing back +6.00dB
at its plateau).

**Design.** Added a second RBJ high-shelf stage (`ENV_AMPEQ_SHELF2_FREQ_HZ
= 6000.0f`, `ENV_AMPEQ_SHELF2_GAIN_DB = 10.0f`) cascaded after the existing
shelf (`envelope_ampeq.h`/`.cpp`, both under the one `'a'` enable flag —
no new key). Three candidates were compared against the measured
insertion-loss table before picking this one:

| shelf2 (fc, gain) | NET@500 | NET@1900 | NET@3100 | NET@4300 | NET@8000 |
|---|---|---|---|---|---|
| 5000Hz / +8dB  | -0.27 | -0.42 | +0.30 | -1.02 | -7.70 |
| 5500Hz / +9dB  | -0.27 | -0.45 | +0.09 | -1.89 | -6.70 |
| **6000Hz / +10dB** | **-0.27** | **-0.46** | **-0.02** | **-2.52** | **-5.70** |

All three leave 500-1900Hz essentially untouched (shelf2 contributes
<0.02dB there in every case — its corner is high enough not to disturb the
already-good midband). 6000Hz/+10dB was picked because it clears the
3100Hz IMD3-upper offset almost exactly (-0.02dB, vs. the others'
overshoot/undershoot) while recovering the most at 8000Hz of the three.
Full net table with both shelves stacked:

| Freq | Measured loss alone | shelf1+shelf2 correction | NET (was, shelf1-only) |
|---|---|---|---|
| 500Hz  | -0.28dB  | +0.01dB  | -0.27dB (-0.27dB) |
| 700Hz  | -0.45dB  | +0.03dB  | -0.42dB (-0.42dB) |
| 1700Hz | -1.55dB  | +0.96dB  | -0.60dB (-0.60dB) |
| 1900Hz | -1.85dB  | +1.39dB  | -0.46dB (-0.47dB) |
| 3100Hz | -4.52dB  | +4.50dB  | -0.02dB (-0.11dB) |
| 4300Hz | -8.75dB  | +6.23dB  | -2.52dB (-3.06dB) |
| 8000Hz | -21.70dB | +16.00dB | -5.70dB (-15.70dB) |

Poles for the new stage at (6000Hz, +10dB): magnitude 0.654 — comfortably
stable, well inside the unit circle despite the corner sitting closer to
Nyquist (8000Hz at this Fs) than shelf1's.

**Group-delay cost — computed the same way as every gdeq fit in this
project (analytic RBJ shelf phase, `-dphase/dw`, converted to µs), over
50-7999Hz:**

| Stage | p-p | mean | worst points |
|---|---|---|---|
| shelf1 alone (2500Hz/+6dB) | 64.9µs | 0.03µs | -41.6µs @ 1333Hz, +23.3µs @ 3586Hz |
| shelf2 alone (6000Hz/+10dB) | 125.4µs | 0.01µs | -39.1µs @ 5029Hz, +86.3µs @ 7037Hz |
| shelf1+shelf2 combined | 148.6µs | 0.04µs | (delays sum directly — both LTI) |

At the two-tone/IMD-offset frequencies specifically (combined shelf1+shelf2
delay only, not counting the analog filter or gdeq): 500Hz -42.7µs, 700Hz
-45.6µs, 1700Hz -49.5µs, 1900Hz -43.2µs, 3100Hz +0.2µs, 4300Hz -12.4µs,
8000Hz +70.5µs — roughly a 120µs swing across exactly the band this
project has spent the most effort flattening (envelope_gdeq.h).

**Why this can't be designed away.** This is the second time this session
a shelf's own delay contribution has come up (`a`'s original single-shelf
already reintroduced 19.4→81.7µs p-p dispersion when validated against
`ga_Trial1_TF.txt`, see the 2026-09-03 entry above), and it generalizes: a
causal, minimum-phase magnitude filter's gain and phase responses are
locked together by a Hilbert-transform-type relationship — it's the same
reason gdeq's own all-pass sections had to be built as UNITY-magnitude by
construction to get delay correction with zero gain side-effect. There is
no shelf *shape* that recovers more high-frequency gain without incurring
more nearby phase/delay distortion — bigger gain and/or a corner closer to
Nyquist (there is no headroom to push the corner further out; 8000Hz IS
Nyquist at this Fs, so this stage is already about as close to the correction
target as a shelf's own corner can usefully sit) means more delay cost,
full stop. So "extend the correction without breaking group delay" cannot
mean "find a magically phase-transparent shelf" — it means accepting this
stage's delay contribution and then **refitting `envelope_gdeq.h`'s
all-pass coefficients against the new combined (analog + shelf1 + shelf2)
phase response** — the identical numerically-fit-against-real-data method
already used twice in this project for smaller reasons (the BC337→
PNP_BC327_ATTN filter swap; then the LTspice→real-hardware data swap, see
`envelope_gdeq.h`'s history). **Not yet done.** gdeq's current coefficients
(a1=0.026173, a2=0.236810) were fit against the analog filter ALONE,
before shelf1 even existed, and are already known (2026-09-03 g+a trial)
to let shelf1's dispersion straight through uncorrected — adding shelf2 on
top without refitting will make the delay side worse, not better, even if
it helps IMDs.

**Recommended next step (not yet run):** flash this two-stage design,
re-run the `'w'` chirp/TFA workflow to measure the REAL combined
magnitude+phase response (don't trust the analytic prediction alone —
shelf1's magnitude prediction tracked real hardware to a few tenths of a
dB, but gdeq's own history shows real measurements occasionally diverge
from prediction in ways worth catching before committing further), THEN
decide whether the resulting two-tone IMD change is worth doing the gdeq
refit to recover flat delay. Only after that refit would this be a fair
like-for-like test of "does more high-frequency amplitude correction
improve the higher IMDs" — right now, testing shelf1+shelf2 on two-tone
IMD without refitting gdeq would conflate two effects (more amplitude
correction vs. more delay dispersion) and wouldn't cleanly answer the
question asked.
