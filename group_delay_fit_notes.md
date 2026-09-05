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

---

## 2026-09-04, later same day — shelf2 validated on real hardware (`ga_Trial2_TF.txt`): magnitude gain confirmed, dispersion roughly doubled, IMD marginally worse

**What was measured.** Same TFA sweep method as `ga_Trial1_TF.txt`
(`'g'`+`'a'` both on), this time with the two-stage `ampeq` (shelf1+shelf2)
flashed. Re-parsed both files with an identical pipeline (100–300Hz
passband-average magnitude reference; phase unwrapped, then 181-point
Savitzky–Golay smoothed, then `-dphase/domega` for group delay — same
convention as every other real-hardware analysis in this project) so
Trial1 and Trial2 are a clean apples-to-apples comparison, not just each
compared separately to its own prediction. Note: re-running this pipeline
from scratch on Trial1 gives 75.7µs p-p (vs. the 81.7µs previously
reported) — a small, expected difference from minor smoothing/derivative
implementation details between passes, not a real change in the
measurement; flagging this explicitly per this project's own past mistake
of conflating two different metric definitions (see the 2026-09-03 "own
analytical error" entry above) — the important number here is the
Trial1-vs-Trial2 RATIO, computed with one consistent pipeline, not the
absolute p-p figure compared across different analysis passes.

**Magnitude — real, substantial, a bit short of predicted.** Net loss at
8000Hz: Trial1 (shelf1-only) measured -16.03dB (matches the -15.70dB
prediction to 0.33dB, consistent with the original validation). Trial2
(shelf1+shelf2) measured -9.30dB — a real **+6.7dB** improvement from
adding shelf2, genuine and large, but short of the +10.00dB/net -5.70dB
the analytic model predicted (a ~3.6dB shortfall at the single most
aggressive point in the design — expected some gap here given shelf2 sits
closest to Nyquist of anything tried yet, but worth a closer look if this
stage is revisited). Mid-band (500–1900Hz) essentially unchanged between
trials, as designed. 3100Hz measured net actually improved slightly beyond
prediction here too (Trial1 +0.12dB overshoot → Trial2 -0.43dB, prediction
was -0.02dB) — real point-to-point measurement noise at the few-tenths-dB
level, not a concern.

**Group delay — dispersion roughly doubled, and the top end grew MORE than
predicted.** p-p over 100–8000Hz: 75.7µs (Trial1) → 157.5µs (Trial2), a
**2.1×** increase — matches the qualitative prediction (combining Trial1's
own measured dispersion with shelf2's analytic contribution projected to
~167µs, see `gdeq_ampeq_delay_chart_v5.html`) closely. Point-by-point delta
(Trial2 minus Trial1) tracks shelf2's own predicted delay curve well in
shape and sign across most of the band (500Hz -8.4µs vs. predicted
-10.85µs; 1900Hz -11.4 vs. -13.34; 3100Hz -21.0 vs. -19.44 — a very close
match; 4300Hz -36.0 vs. -32.34), but **at 8000Hz the real jump (+85.1µs)
notably exceeds the analytic prediction (+62.27µs)** — real hardware shows
more top-end delay growth than the idealized shelf model right at the edge
of the band, worth remembering if this stage's gain is ever pushed higher
still. Mean delay barely moved either time (197.1µs → 196.4µs), consistent
with both shelf stages' near-zero predicted mean contribution.

**IMD — marginally worse, and that result is itself informative.** User's
real two-tone report: shelf1+shelf2 IMD was marginally worse than
shelf1-only, not dramatically worse. Read against the numbers above, that
means roughly +6.7dB of amplitude correction and roughly +82µs of extra
p-p dispersion landed close to a wash on two-tone IMD in this UNREFIT
state — neither effect cleanly dominated. That's consistent with (not
proof of) this project's working model built up over this session's
earlier findings that IMD depends on both amplitude symmetry (the `eq`
finding, 2026-09-03) and delay/phase behavior near envelope
nulls/transients (the null-floor/`freq_dev` findings, same day) — it's
exactly why testing shelf2 without a gdeq refit first was flagged in the
entry above as not a clean test of "does more amplitude correction help":
both variables moved together, and a genuinely fair test needs the delay
side held flat.

**Decision, as stated by the user 2026-09-04:** revert to shelf1-only as
the better real-world compromise for now, while recording a wider range of
IMD products to compare between the two configurations before deciding
anything further. Shelf2 itself is NOT being removed from the firmware —
still selected via the same `'a'` flag alongside shelf1 (see
`envelope_ampeq.h`/`.cpp`) — this is a config/testing decision, not a code
reversion, so shelf2 stays available to re-test later, ideally after the
still-outstanding gdeq refit against the combined analog+shelf1+shelf2
phase response. Chart: `gdeq_ampeq_delay_chart_v6.html` (real Trial1 vs.
real Trial2, magnitude and group delay side by side).

Shortly after, shelf1 and shelf2 were split into independent enable flags
(`'a'`/`'A'`, see `envelope_ampeq.h`'s matching entry and
`serial_commands.cpp`) specifically so this comparison could continue
without reflashing between configs.

---

## 2026-09-04, later still — `A` (shelf2) preliminary IMD report: a candidate mechanism, unconfirmed

**The report, verbatim:** "A mixed bag so far. Reduces HF IMDs with eq off
but otherwise not a lot of change." First real use of the newly-independent
`'A'` toggle. Two observations to explain: (1) `eq` OFF + `A` ON reduces
HF IMDs; (2) otherwise (implicitly, `eq` ON, or other conditions not yet
specified) `A` does little.

**Why this is surprising at first glance.** `envelope_ampeq` (both shelves)
only ever touches the `envelope` value - the amplitude signal that drives
the RSET/PWM/analog-filter path. It has no access to, and cannot affect,
`freq_dev_hz` - the instantaneous-frequency signal that goes straight to
the AD9851 over SPI, entirely bypassing `envelope_ampeq`/`envelope_gdeq`.
The two are computed from the same analytic signal `z(t)` but diverge
immediately after (`envelope = |z(t)|`, `freq_dev` from `angle(z(t))`'s
derivative) and are never recombined in the digital domain - they only
combine physically, at the RF output, as (RSET-driven PA gain) ×
(AD9851's instantaneous-frequency-modulated carrier). This project's
established null/"nemesis" mechanism (see the dated entries elsewhere in
this file) is that with `eq` OFF, a true envelope null causes `freq_dev`
to violently sign-flip - a PHASE-domain event. `envelope_ampeq` cannot
prevent that flip; it never sees `freq_dev` at all. So how could enabling
`A` change the resulting IMDs?

**Candidate mechanism (NOT YET CONFIRMED - needs a real test, not just
this reasoning):** the RF output's actual amplitude at the null instant is
set by the PHYSICAL RSET/PWM/analog-filter chain's response to the digital
`envelope` value, not the digital value itself. The analog filter's own
severe high-frequency roll-off (the entire reason `envelope_ampeq` exists)
doesn't just attenuate steady-state high frequencies - it also SLOWS the
physical envelope's response to FAST transients, and a null's cusp
(a brief, sharp dip when `eq` is off) is exactly this kind of fast,
high-frequency-rich event. Before `A`: the physical RSET-driven PA gain
may not be dropping all the way to the digital envelope's true near-zero
value AT the null instant - filter lag smears/softens the dip in time,
so the gain trace sits somewhat higher, for somewhat longer, around the
null than the digital envelope itself specifies. If that's true, the
erratic `freq_dev` energy occurring at that same instant gets multiplied
by a not-quite-zero gain and leaks into the RF output as IMD. Enabling `A`
restores high-frequency fidelity to the envelope path (that's its whole
purpose), which could let the physical RSET-driven gain track the true
digital envelope more precisely and more quickly - meaning it actually
DROPS closer to zero, and does so more promptly, right when the null (and
its `freq_dev` transient) occurs. That would tighten the physical
"gate" on the erratic phase energy at exactly the moment it matters,
without `A` ever touching `freq_dev` directly - an AMPLITUDE-domain fix
for a problem whose ROOT CAUSE is in the phase domain, working by
tightening how well amplitude suppresses phase garbage at the RF stage,
not by preventing the phase garbage from existing.

**Why this predicts observation (2) as well, which is a good consistency
check (not proof):** with `eq` ON, there's no true null and no `freq_dev`
sign-flip to begin with (the presence-peak amplitude asymmetry already
keeps the envelope minimum well above zero - see the 2026-09-03 entries).
If the mechanism above is right, `A`'s benefit specifically depends on
there being erratic phase energy AT a null for the tightened gate to
suppress - with `eq` on, there isn't any, so `A` would be expected to do
"not a lot" - matching the user's second observation. A hypothesis that
predicts BOTH halves of an odd-looking report from one mechanism is worth
taking seriously, but two data points is not confirmation - still needs a
real test.

**What would confirm or kill this, not yet run:**
- Same two-tone test, `eq` off, `A` off vs on, scoping the RSET/envelope
  output directly right at a null crossing (same technique already used to
  see the null-floor phenomenon in the `eq` investigation) - if the
  mechanism is right, `A` on should show the physical envelope trough
  reaching visibly closer to zero, faster, than `A` off.
- A direct look at which IMD PRODUCTS specifically improved ("HF IMDs" is
  currently just the user's own characterization, not a specific
  frequency/order list) - the mechanism above predicts improvement should
  be concentrated at higher-order products (further from carrier), which
  are the ones most sensitive to a brief, wideband instant of leakage,
  rather than a broad, even improvement across all orders.
- Whether `g` was on or off during these particular runs, and at what
  relative-delay setting - not recorded yet, and relevant because `g`'s
  timing changes exactly when the envelope path's own event (including a
  null) lines up against `freq_dev`'s event in time.
- A repeat under identical conditions to rule out run-to-run/level-setting
  noise before treating "reduces HF IMDs" as a settled real effect.

**2026-09-04, confirmed on the bench:** user scoped the RSET/envelope
output directly and reports the null IS quicker and deeper with shelf 1,
and quicker/deeper still with shelf 2 - exactly the first prediction
above. This is real, independent confirmation of the candidate mechanism
(better HF envelope-path fidelity -> the physical RSET-driven gain tracks
the true digital envelope more precisely and faster at a null, tightening
how well amplitude gates the `freq_dev` transient) - upgraded from
hypothesis to bench-confirmed. Still open: the IMD-product-specificity and
same-conditions `eq`-on comparison items above.

---

## 2026-09-04 — refitting gdeq for the `a`+`A` case: real improvement, but a genuine trade-off, not a clean win

**Why:** user's own framing, following the null-depth confirmation above -
"Maybe we should refine the grp delay again to optimise that for the a+A
case." This is the gdeq refit flagged as outstanding since the shelf2
work began (envelope_ampeq.h's "Interaction with gdeq" note, and the
2026-09-04 entries above) - gdeq's current coefficients
(a1=0.026173, a2=0.236810) were fit against the bare analog filter alone,
before either shelf existed, and are known to pass both shelves'
dispersion straight through uncorrected.

**Method - no new hardware sweep needed.** Rather than requesting a fresh
`'g'` OFF + `'a'`+`'A'` ON TFA sweep, the "bare" (analog+shelf1+shelf2,
NO gdeq) phase curve was reconstructed directly from the existing
`ga_Trial2_TF.txt` measurement: since gdeq is a purely digital cascade
applied earlier in the same signal chain the TFA sweep measures end-to-end,
and its current coefficients are exactly known, `phase_bare = phase_measured
- phase_gdeq_analytic` is valid by straightforward LTI cascade algebra (all
stages are linear time-invariant, so phases simply add in a cascade).
**Validated by self-consistency**, not just asserted: re-adding the
current coefficients' analytic phase to the reconstructed bare curve
reproduces the real Trial2 measurement almost exactly - p-p 157.5us and
mean 196.4us either way, matching the direct measurement from the
2026-09-04 Trial2 entry above to the digit. Reconstructed bare curve: p-p
88.4us, mean 71.1us over 100-8000Hz - the mean matching the previously-
established analog-alone mean (71.2us) almost exactly is a second,
independent consistency check (both shelves have near-zero predicted mean
delay contribution, so total mean should track analog-alone's, and it
does).

**Optimization result - a real ceiling, not a coefficient-search failure.**
Grid search + Nelder-Mead (same method as every prior gdeq fit), 2/3/4
cascaded `ssb_allpass1_t` sections, minimizing p-p group delay over
100-8000Hz against the reconstructed bare curve:

| Sections | Best p-p achievable | Coefficients |
|---|---|---|
| 2 | 72.6us | a1=a2=-0.139115 |
| 3 | 71.3us | all three ≈ -0.08750 |
| 4 | 70.7us | all four ≈ -0.06398 |

More sections barely help - each additional section converges to a
smaller, near-identical coefficient rather than a meaningfully different
curve shape, meaning the achievable floor for THIS filter type (cascaded
single-real-pole all-pass) sits around ~71-73us p-p for this particular
bare curve shape, confirmed via both a coarse grid search and a global
differential-evolution search (both landed on the same floor
independently). **2 sections is therefore the right choice** - same
architecture already in envelope_gdeq.h, just new coefficients, and
extra sections buy essentially nothing here.

**Why the ceiling is so much higher than the original analog-alone fit's
(18.5us):** the bare curve here has a materially harder shape - it rises
from 100Hz to a first local peak near 3100Hz (~82us), FALLS to a local
minimum near 5000Hz (~39us), then rises sharply to a second, higher peak
near 7800Hz (~125us) right at the edge of Nyquist (8000Hz at this Fs).
That's two interior extrema plus a steep near-Nyquist edge, vs. the
original analog-only curve's single smooth hump. A first-order all-pass
section's own delay curve (`(1-a^2)/(1+2a*cos(w)+a^2)`) is smooth and can
only produce ONE hump over the full band - cascading sections helps when
each section can be given a genuinely different shape/location, but here
the optimizer keeps landing on near-identical coefficients per section,
meaning this curve's shape doesn't offer that flexibility to exploit. The
steep near-Nyquist rise is shelf2's own signature (its predicted delay
peaks at +86.3us right at 7037Hz, see the 2026-09-04 shelf2 entry above) -
consistent with shelf2 being the harder half of this curve to flatten.

**The trade-off - improvement is NOT uniform across frequency, read this
before flashing anything:**

| Freq | current (old gdeq coeffs) | candidate refit (a1=a2=-0.139115) | delta |
|---|---|---|---|
| 500Hz | 142.9us | 209.0us | +66.1us (worse) |
| 700Hz | 147.4us | 212.0us | +64.6us (worse) |
| 1700Hz | 167.1us | 218.7us | +51.6us (worse) |
| 1900Hz | 170.4us | 218.7us | +48.3us (worse) |
| 3100Hz | 191.3us | 214.6us | +23.3us (worse) |
| 4300Hz | 179.3us | 174.3us | -5.0us (~same) |
| 8000Hz | 290.5us | 217.9us | -72.6us (much better) |

The refit lowers OVERALL p-p by flattening the curve around a higher
common level, which concretely means it fixes the 8000Hz peak dramatically
but ADDS 24-66us of delay at 500-3100Hz relative to what's flashed right
now - and 500-1900Hz are the two-tone's own fundamental frequencies (700,
1900Hz) plus the IMD3-lower offset (500Hz). Since real IMD very plausibly
depends on the exact relative TIMING between the envelope and `freq_dev`
paths at the frequencies that carry the actual signal power (not just on
minimizing an abstract p-p number across the whole band equally), this
redistribution needs a real two-tone IMD comparison on the bench to judge
- a smaller p-p number is not automatically a "better" result for IMD, only
a flatter one on paper. A restricted-band variant (fit only over
100-4300Hz) was also tried for comparison: it flattens that sub-band
beautifully (27.3us p-p there) but drives 8000Hz UP to 311us - WORSE than
doing nothing - confirming the same fundamental tension from the opposite
direction and ruling that option out as a serious candidate.

**Mean delay barely moves** (196.4us -> 195.5us, about -1us), so `'['`/`']'`
relative-delay re-tuning should need only a small nudge if this is tried,
not a from-scratch search the way the very first gdeq introduction needed.

**Not yet done:** implementing this in `envelope_gdeq.h` (would need a
decision on HOW - see the open question below), a real `'g'` OFF +
`'a'`+`'A'` ON TFA sweep to directly confirm the reconstructed bare curve
(the self-consistency check above is strong, but every other gdeq fit in
this project has been validated against a fresh direct sweep before
trusting it, not just reconstructed from an existing one), and the actual
two-tone IMD comparison once flashed. Chart:
`gdeq_refit_a_A_candidate_v7.html`.

**Open architectural question, not yet resolved:** gdeq's coefficients are
currently a single fixed compile-time pair per filter/Fs combination
(`#if`/`#elif` on `SAMPLE_RATE_HZ`/`ENV_FILTER_VARIANT`). The RIGHT
coefficients now depend on ampeq's shelf state too - analog-alone (current
values, correct for `'a'`/`'A'` both off), analog+shelf1 (`'a'` on, `'A'`
off - never separately refit, still uses the analog-alone values, known
suboptimal per the 2026-09-03 g+a entry), and analog+shelf1+shelf2 (`'a'`+
`'A'` both on - this entry's new candidate). Three genuinely different
optimal pairs for three reachable configurations. Options for handling
this, not yet decided: (a) keep one fixed pair as a compromise (accepting
it's not optimal for every ampeq state); (b) make gdeq's coefficients
runtime-selectable, chosen automatically from `a`/`A`'s current state
(more correct, more code); (c) leave the `#define`s as a manually-edited
value swapped in only when specifically bench-testing the `a`+`A`
combination, same way different firmware variants have already been
swapped in and out through this session.

**Implemented, 2026-09-04, same day: option (c).** `envelope_gdeq.h` now
has both coefficient pairs in the file at once, gated by a new
`ENV_GDEQ_USE_AA_CANDIDATE` compile-time flag (0 = default, the existing
2026-09-03 analog-alone fit; 1 = this entry's a+A candidate). Chosen over
(a)/(b) because the a+A candidate is still an unconfirmed, non-uniform
trade-off (see the table above) that would actively regress the
currently-recommended default config (`'a'` on, `'A'` off) if it silently
replaced the existing values - a manual, obvious, one-line flip keeps the
validated default safe while making the candidate one edit away to
bench-test, consistent with how every other A/B comparison in this session
(slew-rate limits, ampeq shelf states) has been done. Revisit (b) - real
runtime switching - if the a+A candidate proves out on the bench and ends
up wanted as a standing option rather than an occasional test.

**2026-09-04, same day: bench IMD result — no measurable difference, candidate does NOT prove out.** `aAgdeq_optimise_comparison.xlsx`, two two-tone spectrum-analyzer sweeps at identical `PersistentSettings` (`g`, `a`, `A` all on; `eq`/`comp` off), run 1 = default coefficients (`ENV_GDEQ_USE_AA_CANDIDATE=0`), run 2 = the a+A candidate (`=1`). Noise floor agreed to 0.08dB between runs (-108.976 vs -108.9dBm) and every reference level agreed to ~0.1dB, confirming the two runs were a genuinely controlled A/B, not a different RF setup.

Checked every IMD/spread product's absolute level against the measured noise floor rather than trusting the raw relative-dB readings at face value. Every product sitting >13dB clear of the floor (-500 through -6500Hz, +3100 through +10300Hz - i.e. essentially the whole set that matters for judging real IMD) moved by only 0.05-1.1dB between runs, split roughly evenly between "better" and "worse" with no consistent direction - ordinary spectrum-analyzer read repeatability, not a filter effect. The handful of larger swings (-8900Hz, -10100Hz, +11500Hz, +12700Hz, all 1.5-5.3dB) are exactly the products whose absolute level sits at or below the noise floor in one or both runs (several read *below* the stated floor, which is only possible as floor-noise fluctuation, not a real spectral line) - meaningless for this comparison. "Low 10k leakage" (-6.0dB) is the one exception that doesn't fit the floor-proximity explanation (both readings 8-14dB clear of floor) and is being left as an unexplained single outlier rather than force-fitted to a story, pending a repeat measurement.

"Pwr mid 750Hz band" showed a real, floor-clear 9.95dB difference (-26.282dBm run1 -> -36.228dBm run2, candidate quieter) - the only large, unambiguous swing in the whole comparison. But this specific metric has its own history in this project (see the "GPIO13 5kHz pulse" / interp-tick section, 2026-09-02 entries): it previously tracked interp-tick float-ramp cost, FPU-context switching, and wake-rate effects - none of which changing `ENV_GDEQ_A1`/`A2`'s numeric *value* touches, since both coefficient pairs run through the exact same `ssb_allpass1_process()` multiply-adds at identical cost. No mechanism connects a coefficient-value swap to a 10dB change on that specific reading, so this is being treated as session-to-session environmental drift (PSU, warm-up, ambient pickup - the user's own hypothesis), not a real effect of the candidate, and is not being counted as evidence either way.

**Conclusion: the a+A candidate is IMD-indistinguishable from the current default on this bench, despite predicting ~2.2x better group-delay p-p dispersion (157.5us -> 72.6us).** This is another data point - alongside the 2026-09-01 case where `g` made 3rd-order IMD worse despite improving predicted dispersion - that the p-p group-delay figure on its own is not a reliable predictor of real two-tone IMD outcome on this hardware. **Decision: `ENV_GDEQ_USE_AA_CANDIDATE` stays at its default (0).** The candidate's known non-uniform cost (worse at 500-3100Hz, see the entry above) isn't bought back by any measured IMD benefit, so there's no reason to carry the trade-off. The flag itself stays in the file (harmless, one line, already validated to compile either way) in case a future ampeq/gdeq change reopens the question, but this specific candidate is not recommended for use.

## 2026-09-04 — closing the ampeq/gdeq optimization line: practical limits reached on this hardware

Stepping back across the whole `a`/`A`/`g` line of work in this file: every stage found a real, measurable, directionally-correct improvement in the physical quantity being targeted (shelf1/shelf2's magnitude flattening toward 8000Hz; the original `g` fit's ~3.5-3.9x group-delay p-p reduction; the a+A-specific refit's further ~2.2x reduction on top of that), each backed by real hardware TFA/spectrum measurements, not just simulation. But translating those into IMD wins has been inconsistent and, in the two most recent, most-refined attempts (shelf2, and now the a+A gdeq refit), essentially a wash or a net negative once actually bench-tested. That's a real, repeatable pattern across this project, not one bad result: `g` alone made 3rd-order IMD worse on 2026-09-01 despite improving predicted dispersion; shelf2 came back "marginally worse" on 2026-09-04 despite a genuine +6.7dB real magnitude gain; the a+A gdeq candidate came back indistinguishable despite a ~2.2x p-p improvement. Meanwhile the single biggest, most reliable IMD win found in this entire project (the `eq`/amplitude-mismatch mechanism, see the 2026-09-03 sections above) came from a completely different lever - not magnitude flatness, not delay flatness, but keeping the envelope off a true zero at destructive-interference nulls.

**User's summary, and the working conclusion this project is adopting:** envelope accuracy matters in both time (group delay) and shape (amplitude/magnitude vs. frequency) - both are real, both have been shown to move things - but on THIS hardware (this analog reconstruction filter, this ADC/DAC/PWM chain, these achievable coefficient/filter-order constraints), continued refinement of `a`/`A`/`g` specifically has reached the point of diminishing, inconsistent returns rather than clear wins. Further gains are more likely to come from the amplitude-mismatch/true-zero-null mechanism (see the new section below) than from squeezing more precision out of the shelf/all-pass filters. Not a decision to revert or remove any of `a`/`A`/`g` - all three remain in the firmware, independently toggleable, still individually the best-known settings for what they each target - just a decision to stop spending further optimization effort specifically on refitting them further.

## 2026-09-04, same day — testing the amplitude-mismatch hypothesis directly: a runtime tone-ratio control (`'R'`)

**Motivation.** The 2026-09-03 finding (see "`'x'` makes IMDs worse... confirms amplitude" above) established that `eq`'s IMD benefit comes from the presence peak's ~3.34dB net amplitude mismatch between the two tones keeping the envelope minimum `|A1-A2|` off a true zero, which prevents `freq_dev` from flipping sign at the null (a genuine derivative discontinuity when the tones are exactly equal). That finding came from an INCIDENTAL side effect of `eq` - a filter built for a completely different purpose (HPF + presence peak) happened to also fix this. The natural next question, raised this turn: does deliberately controlling the tone-amplitude ratio directly, with `eq` fully off, reproduce (or exceed) the benefit on its own - confirming the mechanism cleanly, without `eq`'s other effects (the ~126µs differential HPF phase-delay documented in the 2026-09-03 "what is `eq` actually doing" section, or the presence peak's own frequency-response shaping) as confounds?

**Why this is a clean test, mathematically.** With the two-tone signal written as the real part of `A1*e^(j*w1*t) + A2*e^(j*w2*t)`, the instantaneous envelope is `|A1*e^(j*w1*t) + A2*e^(j*w2*t)|`, which reaches its minimum value `|A1-A2|` whenever the two phasors are exactly opposed (once per beat period, at the classic two-tone "null"). When `A1=A2` exactly, that minimum is a literal zero - the trajectory passes through the origin, `freq_dev` (proportional to `d(phase)/dt` of the analytic signal) is undefined in the limit and flips sign discontinuously in the discrete-time implementation (the 2026-09-03 entry measured +1300Hz either side, -6700Hz at the null sample for the 700/1900Hz pair at 16kHz). Any `A1 != A2` makes that minimum strictly positive, so the trajectory passes NEAR the origin but never through it - `freq_dev` still swings hard near a null (this is a real, unavoidable EER/polar-transmitter characteristic per the Zhuang/Waheed/Staszewski citation, not something a tone-ratio choice removes entirely) but does so continuously, without the sign-flip discontinuity that most directly drives spectral splatter. This depends ONLY on the amplitude ratio - not on which tone is scaled, not on relative phase/timing (a pure delay shift moves WHEN a null occurs, never whether it's a true zero) - so it isolates the exact mechanism identified on 2026-09-03 with nothing else riding along.

**Implementation.** Added a new `'R'` serial command (`test_signals.cpp`/`.h`), cycling `TONE_RATIO_PRESETS`: equal (0dB, unchanged default), -1dB, -3dB, -6dB, -10dB, -20dB, scaling tone2 down relative to a fixed tone1 (`TWOTONE_AMPLITUDE`). Chose to scale DOWN only, never up, so the combined constructive-interference peak (`TWOTONE_AMPLITUDE * (1 + gain)`) never exceeds today's existing 0.9 peak at any step - no new headroom/clipping risk introduced, unlike boosting one tone which would have pushed peaks toward or past 1.0 at the larger ratios. -3dB (`0.707946` linear) is deliberately included as the step closest to the ~3.34dB mismatch `eq`'s presence peak was measured imposing on this exact 700/1900Hz pair, so an `'R'` @ -3dB (eq off) vs. `eq`-on comparison at the same pair, same everything else, is the single most direct falsification test available - if the hypothesis is right, those two should come back close to equally clean; if `'R'` at -3dB doesn't reproduce it, the HPF or some other `eq` side effect is doing more of the work than the 2026-09-03 analysis credited. Independent of `e`/`a`/`A`/`g`, matching this project's consistent convention of orthogonal, freely-combinable toggles for isolating one variable at a time. Not persisted in `PersistentSettings` (same precedent as `'T'`'s band selection - a bench-testing tool, resets to equal/0dB on boot, not a saved-preset lever). Verified via a standalone syntax/logic check (extracted the exact generator + preset-cycling code into a stub harness, compiled clean with `gcc -fsyntax-only -Wall -Wextra`) and manual brace-balance check of both edited files; not yet bench-tested on real hardware.

**Practical motivation, if confirmed: a built-in "Tune" signal that sidesteps the EER null problem by construction.** The conventional two-tone test/tune signal is defined as equal-amplitude specifically to give a clean, symmetric, standardized IMD reading - but on a polar/EER architecture like this one, equal amplitude is exactly the condition that maximizes the null-crossing sign-flip and its spectral splatter. If a small, fixed, deliberate amplitude offset between the two tones (no `eq`, no HPF, no presence-peak coupling, no other DSP correction) reliably cleans up the splatter the way `eq` does, the practical implication is straightforward: any built-in tune/test signal on this hardware should be GENERATED with that offset from the start (a firmware constant, not a live control), rather than inheriting the traditional equal-level convention from architectures that don't have this problem. That would make a clean tune signal available with zero dependence on `g`/`a`/`A`/`eq` all being correctly tuned first - useful specifically because a tune signal's whole point is being a known-clean reference independent of other settings. This does not solve the general problem for real speech/music (which also passes near the phasor origin at times, per the cited literature) - it is specifically a win for a purpose-built test/tune tone, where the amplitude ratio is a free design parameter with no meaning to preserve.

**Not yet done:** any real-hardware measurement with `'R'` - this section documents the reasoning and the implementation, not a result. Recommended workflow: `T` to a tone pair, `e`/`c`/`g`/`a`/`A` all off, `'r'` to reset diagnostics, step `'R'` through its presets and read IMD at each, compare directly against the same pair with `eq` on (`'R'` back to equal/0dB) as the reference point this is trying to match or beat.

## 2026-09-04, later same day — hypothesis confirmed on the bench, `'R'` reworked to +/-3dB symmetric

**Result.** User confirmed on the bench: `'R'`'s amplitude mismatch alone, with `eq` fully off, reproduces the IMD benefit - "that proves the hypothesis." This closes the falsification test proposed in the previous section: the amplitude-mismatch mechanism identified on 2026-09-03 is now confirmed as sufficient on its own, independent of `eq`'s HPF or its ~126µs differential phase-delay side effect, using nothing but a deliberate tone-amplitude offset.

**Nuance the user caught: `eq` boosts, it doesn't attenuate.** `eq`'s presence peak makes tone2 (1900Hz, the upper tone) LOUDER than tone1, not quieter - the +3.60dB@1900Hz vs +0.40dB@700Hz split from the 2026-09-03 "what is `eq` actually doing" section nets to tone2 being the bigger of the two, not the smaller. The original `'R'` implementation could only test the opposite direction (tone2 scaled down, 0 to -20dB) - it happened to still confirm the underlying mechanism (which depends only on the magnitude of `|A1-A2|`, not on which tone is larger - see the derivation in the previous section), but wasn't actually replicating `eq`'s own direction. Worth having on record as a small but genuine gap between "confirms the mechanism" and "reproduces `eq` exactly" - the two are related but not identical claims, and this session's own falsifiability standard is why the distinction surfaced at all rather than being glossed over.

**Rework: range narrowed to a symmetric +/-3dB, both directions now reachable.** Per the user's request ("+/- 3dB is enough"), `TONE_RATIO_PRESETS` (`test_signals.cpp`) is now seven 1dB steps: `-3, -2, -1, 0 (equal), +1, +2, +3` dB, replacing the previous six-step 0-to-(-20dB) down-only list. The wider steps (-6/-10/-20dB) are dropped as unnecessary - the effect was already established well inside +/-3dB, and there's no remaining open question those wider points would have answered. Positive dB entries now let `'R'` reach `eq`'s own boost-tone2 direction directly, closing the nuance above.

**Implementation change: constant combined peak instead of a fixed tone1 + multiplier.** The original down-only version kept tone1 fixed at `TWOTONE_AMPLITUDE` and multiplied tone2 by a gain <= 1.0, which was safe (peak could only shrink) but can't extend to a boost direction without risking `TWOTONE_AMPLITUDE*(1+gain)` exceeding 1.0 and clipping. Reworked to hold the COMBINED constructive-interference peak constant (`2*TWOTONE_AMPLITUDE = 0.9`, unchanged from today) at every ratio, splitting it unevenly instead: for a desired linear ratio `r = tone2/tone1 = 10^(dB/20)`, `tone1 = peak_budget/(1+r)`, `tone2 = r*tone1`. At `r=1` (equal/0dB) this reduces to exactly `0.45/0.45`, bit-identical to today's fixed values, so boot behavior and every existing preset/measurement remain unaffected. At the extremes (+3dB/-3dB) the split is `0.373/0.527` or `0.527/0.373` - always summing to exactly `0.900`, verified numerically (a standalone harness printed the full 7-step cycle: peak stayed 0.90000 to 5 decimal places at every step, and the equal-index split matched `0.45000/0.45000` exactly, as expected from the algebra). This also has a nice side benefit for the comparison itself: every ratio step now drives the same overall RF level, so `'R'` sweeps are directly comparable in level as well as ratio, not just individually clipping-safe.

**API note:** `test_signals_get_tone2_gain()` now derives its return value from the two live amplitude statics (`tone2_amplitude/tone1_amplitude`) rather than being a separately-cached variable, so it can't drift out of sync with what `generate_twotone_sample()` is actually doing - a small robustness improvement made while reworking this, not a behavior change (nothing else in the codebase calls this getter yet).

**Verification:** standalone harness (extracted generator + preset-cycling logic, `gcc -Wall -Wextra`) run end-to-end through all seven presets, confirming: combined peak invariant (0.90000 at every step), correct sign/magnitude at each dB point, and exact match to today's legacy `0.45/0.45` at the equal preset. Not yet re-flashed/re-measured on real hardware with the new range specifically - the earlier down-only range is what was bench-confirmed; the new symmetric range is expected to behave the same (same mechanism, same or smaller now-max magnitude at 3dB vs the old range's much larger extremes) but hasn't itself been re-verified on the bench.

## 2026-09-04, later still — reworked `'R'` re-flashed, real IMD comes back very close to `eq` on: hypothesis closed

**Result.** User re-tested on real hardware with the reworked (peak-conserving, symmetric +/-3dB) `'R'`, in `eq`'s own boost-tone2 direction, and reported IMD "very close to eq on." This is the direct, matching-direction version of the falsification test proposed earlier this session - tone2 louder than tone1, same sense as `eq`'s presence peak actually applies, rather than the earlier confirmatory-but-opposite-direction (tone2-quieter) test. **User's own conclusion: "that answers that one."** Closing this question: the amplitude-mismatch mechanism identified 2026-09-03 fully accounts for `eq`'s IMD benefit on this two-tone signal - reproducible with a plain, independent amplitude offset, no HPF, no presence-peak frequency shaping, no other `eq` side effect required.

**Where this leaves things.** `eq`'s real-world benefit on THIS synthetic two-tone signal is now understood essentially completely: it is the presence peak's incidental amplitude-mismatch side effect, not its intended HPF or tone-shaping function - confirmed both by isolating the mechanism analytically (2026-09-03) and now by reproducing it independently with `'R'` alone, in `eq`'s own direction, on real hardware. Doesn't reopen or contradict anything about `eq`'s value for real mic/speech content (where the HPF's hum/rumble rejection and the presence peak's intended tonal shaping both still matter for reasons unrelated to this two-tone-specific mechanism) - this closes the question specifically for why `eq` measured as the single biggest IMD win on the synthetic two-tone test. The practical "Tune signal" implication from the earlier section now has direct bench support behind it, not just the mechanism-confirms-in-principle status it had before this entry.

**Still open, not addressed by this result:** the minimum mismatch actually needed (this test used +3dB; whether +1dB or +2dB gets most of the way there, useful for picking the smallest offset for a built-in tune signal, hasn't been checked), and whether this generalizes beyond the specific 700/1900Hz pair `'R'` was tested at (a different tone spacing changes the null geometry, though the underlying `|A1-A2|` mechanism should hold regardless).

**2026-09-04, later still: 1dB judged sufficient for practical use, pending a comparative bench check.** User's working expectation is that +1dB (the smallest nonzero step `'R'` offers) is already enough for a practical tune-signal offset, well short of the +3dB used to confirm the mechanism above - consistent with the theory, since ANY nonzero mismatch removes the true-zero condition, and the falsifiable question was always "is the remaining near-zero dip still small enough to matter," not "does a small mismatch work at all." Comparative tests across `'R'`'s range - and likely across other `T` band pairs, per the second open item above - planned for the next session, for completeness rather than because this result is in doubt. No firmware change implied either way: `'R'`'s existing +1dB preset already covers this directly, nothing new needs building to run the comparison.

## 2026-09-05 — the promised comparative sweep, and a genuinely separate finding: 300/500Hz leaks USB into LSB

**The planned comparison.** `Tone_Level_Ratio_IMDs.xlsx` - 3rd-order IMD across all seven `'R'` presets at the 700/1900Hz pair (the default `'t'` pair, `g`/`a`/`A`/`eq`/comp all off). Both sidebands' 3rd-order product improve monotonically moving away from 0dB in either direction (Low: -32.2dB@0dB -> -40.7dB@+3dB -> -34.0dB@-3dB; High: -31.4dB@0dB -> -34.3dB@+3dB -> -42.7dB@-3dB) - each direction preferentially cleans up the IMD product on ITS OWN side (boosting tone2 helps the low-side product more, attenuating tone2 helps the high-side product more), both clearly better than equal at every step in both directions. Consistent with, and a nice quantitative confirmation of, the `|A1-A2|`-only mechanism established 2026-09-03/04 - no new mechanism needed to explain this table.

**Separately, the user's own side note: "all 200Hz spaced tone pairs generate low IMDs at all `'R'` values ~-50dB EXCEPT 300/500 that has a high output at -300Hz from carrier - different issue."** Investigated this because "USB leak to LSB?" is a specific, checkable hypothesis, not just a curiosity - and it checks out cleanly, with a mechanism already half-documented in this file.

**Mechanism, confirmed by direct computation of the live 65-tap Hilbert FIR (`generate_hilbert_coeffs()`, `ssb_dsp.c`, `HILBERT_TAPS=65`, `SAMPLE_RATE_HZ=16000` - both current values).** `ssb_dsp_process_sample()`'s I (direct) branch is a single tap off the same delay line (`float I = handle->delay_line[i_idx]`) - a PURE integer-sample delay, with magnitude exactly 1.0 at every frequency, no exceptions. The Q (Hilbert) branch is the actual 65-tap FIR convolution, which only approximates a unity-magnitude 90-degree phase shift - and, being a Type-III antisymmetric linear-phase FIR, its phase is EXACTLY -90 degrees at every frequency by construction (confirmed numerically: -90.000 degrees at every frequency checked, 100-3700Hz) - so the entire opposite-sideband leakage in this design is a pure amplitude-imbalance problem between I and Q, with no phase-error contribution at all. Computed `|H(f)|` for the Q branch directly from the live coefficients (referenced to the same `center`-tap delay the I branch uses, matching the actual code path exactly):

| Tone (Hz) | `\|H\|` (dB) | Predicted image (opposite-sideband) rejection |
|---|---|---|
| 100 | -7.65dB | 7.7dB |
| 200 | -2.68dB | 16.3dB |
| **300** | **-0.72dB** | **27.6dB** |
| **500** | **+0.03dB** | **54.3dB** |
| 700 | +0.02dB | 61.3dB |
| 900 | -0.01dB | 66.6dB |
| 1500-3700 (all `T` pairs above 700/900) | -0.02 to +0.02dB | 58-67dB |

(Image rejection from amplitude-only I/Q imbalance: `IRR = 20*log10((1+a)/|1-a|)` for linear amplitude ratio `a = |H(f)|` - the standard phasing-method formula, applicable here without a phase term since phase is exact.)

**This exactly explains the reported pattern.** Every other `T` band pair (700/900 through 3500/3700) has BOTH its tones sitting in the >=61dB-rejection region - clean, matching the reported "~-50dB" floor (measurement/noise-floor limited, not Hilbert-FIR limited). The 300/500 pair is the only one with a tone (300Hz specifically, not 500Hz - 500Hz alone is already excellent at 54dB) sitting in the FIR's genuine low-frequency rolloff region, ~27dB predicted image rejection - 25-30dB worse than every other pair, easily enough to produce a distinctly "high" spur report. And the direction matches exactly: this firmware defaults to `SSB_SIDEBAND_USB` (`dsp_state.cpp`), where a positive audio frequency commands a positive `freq_dev` (RF tone above carrier) with no sign flip (`ssb_dsp.c`'s `if (sideband == SSB_SIDEBAND_LSB) freq_dev = -freq_dev;` only applies in LSB mode) - so the 300Hz tone's imperfectly-cancelled image genuinely lands on the LSB side, below carrier, at exactly the reported -300Hz offset. **Confirmed: this is a USB-into-LSB leak, caused by the Hilbert FIR's own magnitude error at 300Hz, not by anything `'R'`/`eq`/`a`/`A`/`g` touch.**

**Correctly a separate, pre-existing issue, exactly as the user's own note called it.** This has nothing to do with the amplitude-mismatch/two-tone-null mechanism this session has been chasing - it's a structural limit of the current 65-tap Hilbert design's low-frequency edge, present since `HILBERT_TAPS` was set, unrelated to `eq`, `'R'`, `a`/`A`, or `g`. It would affect ANY real content near 300Hz and below (a real low male voice fundamental, not just this synthetic test pair) with the same opposite-sideband penalty, regardless of any of this session's other settings.

**Not yet done / options, not yet decided on:** (1) confirm the predicted ~28dB figure against the actual measured spur level (the spreadsheet's note is qualitative - "high output" - no dB figure given); (2) if this matters in practice, `HILBERT_TAPS` could be raised (config.h already notes 129 was "briefly tested" for an unrelated question, so there's precedent this doesn't come free - more taps push the good-flatness point lower in frequency at the cost of more group delay and more per-sample compute, the same tradeoff `relative_delay.h` already discusses); (3) simplest no-code option: avoid placing a `'T'`-style reference/tune tone at or below ~300Hz on this firmware, since image rejection there is genuinely, structurally worse than everywhere else in the band - relevant to the "Tune signal" design discussion in the section above, which should now also account for tone PLACEMENT (avoid <=300Hz), not just amplitude ratio.

## 2026-09-05 — Trial3 ("optimised gd") TFA sweep: does NOT show the a+A candidate's predicted improvement

**Context.** `gaA_Trial3_Optimised_gd_TF.txt` - a real-hardware `'w'` chirp/TFA sweep, presumably captured with `ENV_GDEQ_USE_AA_CANDIDATE=1` (the a+A-specific gdeq refit from the 2026-09-04 entries above) and `a`+`A` both on, following up on the earlier "not yet validated on real hardware for the a+A case" gap. User asked: "does this look as expected?"

**Short answer: no.** Processed with the exact same pipeline validated against Trial1/Trial2 (Savitzky-Golay smoothing, window=181/~4.2kHz span, polyorder=3, applied to the FULL 0-24kHz sweep before restricting to the 100-8000Hz analysis band - restricting first and smoothing second was tried initially and produces a spurious edge artifact right at 8000Hz that inflates p-p to ~228us; smoothing first, as established, is the correct order and is what all figures below use):

| | p-p, 100-8000Hz | Mean delay |
|---|---|---|
| Trial2 (real hardware, default coeffs `a1=0.026173/a2=0.236810`) | 157.5us | 196.4us |
| **Trial3 ("optimised", real hardware)** | **154.8us** | **196.8us** |
| Candidate's own prediction (`a1=a2=-0.139115`, from the 2026-09-04 refit) | 73.0us | 197.1us |

Trial3's p-p (154.8us) is statistically indistinguishable from Trial2's (157.5us) - nowhere near the candidate's predicted 73.0us. Checked further: Trial3's group-delay curve tracks Trial2's MEASURED curve to an RMS of only 2.8us across the whole 100-8000Hz band (well within normal measurement repeatability - compare to the ~2us RMS the original 2026-09-03 fit itself achieved against its own real-hardware validation), while it diverges from the candidate's OWN predicted curve by RMS 48.6us - worst right at 8000Hz, exactly where the candidate was supposed to help most (predicted 217.9us, measured 357.5us - a 140us miss, in the WRONG direction). Directly comparing Trial3's raw phase against Trial2's raw phase (interpolated onto a common grid) confirms this at the source: mean phase difference over 100-8000Hz is only -1.06 degrees (std 1.46 degrees) - far too small to be a genuinely different pair of `ssb_allpass1_t` coefficients (`a1=a2=-0.139115` vs `0.026173/0.236810` predicts tens of degrees of differential phase shift by 8000Hz, not ~1 degree).

**Conclusion: Trial3's phase/group-delay response matches the DEFAULT gdeq coefficients, not the a+A candidate.** This looks like `ENV_GDEQ_USE_AA_CANDIDATE` was NOT actually 1 in the firmware that produced this sweep - most likely the flag wasn't flipped before building, or the build was compiled but not actually reflashed to the board, or the wrong binary was flashed. **Recommended before re-testing:** confirm `ENV_GDEQ_USE_AA_CANDIDATE` reads `1` in the checked-out `envelope_gdeq.h`, do a clean rebuild, reflash, and re-run `'w'` - ideally with a `'P'`/boot-banner check first to confirm `g`/`a`/`A` are all actually on for the sweep, since a config mismatch on any of those would also change the curve shape in ways that could be mistaken for "the candidate didn't work."

**Separate observation, not yet explained: Trial3's magnitude runs ~6-7dB below Trial2's from 300Hz up to ~5500Hz, narrowing to ~1.6-2.8dB by 6700-8000Hz.** This does NOT look like an ampeq shelf-state difference (shelf1/shelf2 are both high-shelves with ~0dB effect below their corners - a shelf-state change would show ~0dB difference at 300-1900Hz and a GROWING gap toward 8000Hz, not a roughly flat gap that SHRINKS toward the top of the band). More consistent with an overall level/gain difference between the two capture sessions (master gain, chirp drive level, or TFA/ADC channel calibration) than with any DSP coefficient change - worth checking that both sweeps used the same `PersistentSettings` (particularly master gain) and the same measurement-chain configuration before drawing any conclusion from the magnitude channel specifically. Not yet investigated further - flagging it here since it's a second reason this file doesn't read as a clean, isolated re-test of the gdeq coefficient change alone.

**Chart:** `gdeq_trial3_candidate_check.html` - five series (bare/no-gdeq reconstruction, default-predicted, Trial2-measured, candidate-predicted, Trial3-measured), makes the mismatch immediately visible: the two measured curves (solid) sit on top of each other and on the default's dashed prediction, while the candidate's own dashed prediction sits well apart from both, especially above 5kHz.

**2026-09-05, confirmed: `ENV_GDEQ_USE_AA_CANDIDATE` was not `1` in the flashed build.** User confirmed directly - the diagnosis above was correct. Trial3 is therefore not a real test of the a+A candidate at all; it's effectively a third repeat measurement of the default coefficients (consistent with how closely it tracked Trial2). **The a+A candidate remains untested on real hardware** - the "not yet validated" status from the 2026-09-04 entries still stands unchanged; nothing about the candidate's predicted 73us p-p or its non-uniform trade-off has been confirmed or refuted by this trial. Next step, when there's a reason to revisit it: flip the flag, verify it reads `1` in the built source before flashing, reflash, and re-run `'w'` - the same reconstructed-curve prediction (`gdeq_refit_a_A_candidate_v7.html`) is still the thing to check the new sweep against.

## 2026-09-05, later same day — corrected Trial3 re-run: a+A candidate CONFIRMED on real hardware

**Context.** User rebuilt/reflashed with `ENV_GDEQ_USE_AA_CANDIDATE` actually set to `1` this time and re-ran `'w'`, producing `Corrected_gaA_Trial3_Optimised_gd_TF.txt` (same format, 1025 points, 0-23998Hz). Question: "better?"

**Yes - this is a clean, positive real-hardware validation.** Processed with the same validated pipeline (smooth full 0-24kHz sweep first with Savitzky-Golay window=181/polyorder=3, then restrict to 100-8000Hz, then differentiate):

| | p-p, 100-8000Hz | Mean delay |
|---|---|---|
| Trial2 (real hardware, default coeffs `a1=0.026173/a2=0.236810`) | 157.5us | 196.4us |
| First Trial3 attempt (flag not actually set - see above, effectively a repeat of Trial2) | 154.8us | 196.8us |
| Candidate's own prediction (`a1=a2=-0.139115`) | 73.0us | 197.1us |
| **Corrected Trial3 (real hardware, `ENV_GDEQ_USE_AA_CANDIDATE=1` confirmed active)** | **87.7us** | **195.7us** |

Corrected Trial3 tracks the candidate's own predicted curve closely: RMS difference from the candidate's predicted curve is just 2.7us across the full 100-8000Hz band, versus RMS 50.1us from the default coefficients' curve (Trial2) and RMS 50.1us from the default coefficients' own prediction - i.e. it now sits firmly with the candidate's prediction and firmly apart from the default's, the reverse of the first Trial3 attempt. Confirmed at the phase level too (not just derived group delay): raw phase for corrected Trial3 differs from Trial2's raw phase by a real, frequency-shaped amount this time - mean -42.4 degrees, std 19.2 degrees, ranging from -67.4 degrees to +11.4 degrees, peaking around -64.5 degrees near 4300Hz. That's on the right order of magnitude for genuinely different `ssb_allpass1_t` coefficients (`a1=a2=-0.139115` vs `0.026173/0.236810`), unlike the first attempt's <2-degree difference which gave away that the flag hadn't taken effect.

Mean delay also matches closely (195.7us measured vs 197.1us predicted, within 1.4us) - so this isn't just a p-p-shape coincidence, the whole curve including its DC-ish offset lines up with the candidate's analytic prediction.

**Conclusion: the a+A gdeq candidate (`a1=a2=-0.139115`) is now validated on real hardware for GROUP DELAY.** It delivers a genuine, substantial dispersion improvement over the default coefficients - measured p-p 87.7us vs Trial2's measured 157.5us, roughly a 1.8x reduction - matching its own predicted 73.0us closely enough (within measurement noise/smoothing bandwidth) to trust the underlying refit math. This is the first successful real-hardware confirmation of this candidate after the earlier misflashed attempt.

**Important scope caveat - this validates group delay, not IMD.** The earlier `aAgdeq_optimise_comparison.xlsx` comparison (2026-09-04/09-05, "No difference!") found no measurable IMD benefit from `ENV_GDEQ_USE_AA_CANDIDATE`. That comparison predates this session's discovery that a flag can silently fail to take effect in a build - it has NOT been checked for the same flag-not-set problem, and given how easily it happened here, that earlier "no IMD difference" finding should now be treated as unconfirmed rather than settled. If the IMD question matters going forward, it's worth re-running that comparison with an explicit flag/build check (e.g. a boot-banner or serial readback confirming which coefficient set is actually compiled in) before trusting the result either way. Until then: group delay dispersion is confirmed improved; whether that translates to an audible/measurable IMD improvement remains open.

**Chart updated:** `gdeq_trial3_candidate_check.html` now shows the corrected Trial3 data (title and legend updated to "Trial3, corrected"; verdict box rewritten with the numbers above). The corrected measured curve (orange solid) now visually tracks the candidate's predicted curve (orange dashed) closely across the whole band, clearly separated from Trial2's measured curve (blue solid) - re-verified via Playwright screenshot in both light and dark mode, including a working hover tooltip showing all five series' values at a given frequency.

## 2026-09-05, later still — bench finding: the a+A candidate needs a real, large `relative_delay_samples` retune (2 → 4.5 samples), bigger than the TFA curve predicts

**Context.** User's own bench report with the a+A candidate now flashed and confirmed active: "The new gdeq makes quite big changes but one is the delay has to be set to 280us (4.5samps) to get near to minimum IMDs." Previous (default-coefficient) bench-tuned optimum for the same `g`+`a`+`A`+`eq` config: 2 samples (125us). **This corrects the previous entry's claim that no `relative_delay_samples` retune would be needed** - that claim was based on the two curves' near-identical MEAN delay over the full 100-8000Hz band (196.4us default vs 195.7us candidate), which turns out not to be the right predictor for this quantity. Real hardware, not the TFA mean, is authoritative here.

**Size of the shift:** 2 samples -> 4.5 samples = **+2.5 samples (+156us)** at SAMPLE_RATE_HZ=16000 (62.5us/sample).

**How far the TFA curve difference goes toward explaining it - checked two ways, both fall well short:**

1. *Local delay at the fundamental tone frequencies (700/1900Hz, the default two-tone pair).* Interpolating both real-hardware TFA-derived group-delay curves (Trial2 default vs. corrected-Trial3 candidate) at the actual tone frequencies:

   | Freq | Default (Trial2) | Candidate (Trial3, corrected) | Diff |
   |---|---|---|---|
   | 500Hz | 143.1us | 206.8us | +63.8us |
   | 700Hz | 147.3us | 210.6us | +63.2us |
   | 1900Hz | 170.4us | 222.2us | +51.8us |
   | 2156Hz | 175.2us | 233.6us | +58.4us |
   | 4300Hz | 178.9us | 174.3us | -4.6us |

   Predicts roughly **+1 sample (+52 to +64us)** - right direction, well short of the +2.5 samples measured.

2. *Local delay at the beat frequency and its harmonics.* Physically more correct for a two-tone signal: the envelope `|z(t)|` and `freq_dev` (`d(angle(z(t)))/dt`) of a two-tone analytic signal are periodic at the DIFFERENCE frequency f2-f1 (1200Hz for 700/1900Hz) and its harmonics, not at f1/f2 themselves - so this is arguably the more relevant place to read the TFA curve. Checked at 1200/2400/3600/4800/6000/7200Hz:

   | Harmonic | Default | Candidate | Diff |
   |---|---|---|---|
   | 1200Hz (1st) | 157.4us | 217.6us | +60.2us |
   | 2400Hz (2nd) | 179.1us | 218.2us | +39.1us |
   | 3600Hz (3rd) | 190.6us | 201.9us | +11.3us |
   | 4800Hz (4th) | 172.8us | 155.8us | -17.1us |
   | 6000Hz (5th) | 214.6us | 175.4us | -39.2us |
   | 7200Hz (6th) | 273.6us | 206.5us | -67.2us |

   This is a WORSE predictor, not better - the sign flips by the 4th harmonic and the plain average across harmonics comes out near zero (-2us unweighted, +25us with 1/n amplitude-falloff weighting). Doesn't get anywhere near +156us, and doesn't cleanly pick a direction either.

**This isn't a new failure mode - the same gap already existed for the default coefficients.** The default gdeq's own bench-optimal relative delay (125us) already sat 71us BELOW its own curve's full-band TFA mean (196.4us) - a ~1.1-sample discrepancy that predates this session. For the candidate, the bench-optimal (281us) now sits 85us ABOVE its curve's TFA mean (195.7us) - a similarly-sized discrepancy, but with the sign flipped. So "read the IMD-optimal relative delay off the TFA group-delay curve" was never a tight prediction even for the already-shipped default coefficients - consistent with `settings.h`'s own header comment describing the TFA-derived figure as "a starting hint... not a substitute for" bench re-tuning.

**Working explanation (not fully confirmed): the thing being time-aligned isn't a stationary tone.** TFA group delay characterizes a linear filter's small-signal delay of a swept SINE - a stationary, narrowband concept. What actually needs aligning between the phase and envelope paths is the sharp `freq_dev` transient at the destructive-interference null (the same broadband, non-stationary event this project's null-crossing/EER investigation has already characterized at length - see the 2026-09-03 entries above). A short, sharp transient has spectral support spread across a wide band, not concentrated at any single frequency or narrow harmonic set, so no single point (or small set of points) on a linear TFA curve should be expected to predict how its alignment optimum moves when the allpass coefficients reshape the delay curve. This is offered as the most plausible mechanism, not a confirmed one - no direct measurement of the null-transient's own effective group-delay-weighting has been done.

**Conclusion:** the a+A candidate's real bench-optimal relative delay is genuinely, substantially different from the default's (+156us / +2.5 samples) - correctly anticipated in DIRECTION by the local-tone-frequency TFA reading, but only explains about a third of the MAGNITUDE. Bench re-tuning (not the TFA curve) remains the authoritative source for `relative_delay_samples` any time `envelope_gdeq`'s coefficients change - this project's own `settings.h` header comment already said as much; this is now a second, larger confirming data point. `relative_delay_samples` for any a+A-candidate preset should be set to the bench value (4.5 samples / 281us), not derived from the TFA mean or local-tone reading.

**2026-09-05, correction (same day, shortly after): the 4.5-sample figure was a measurement artifact - `ENVELOPE_INTERP_FACTOR` (x4 interp) had been left on during that retune.** User: "realised that the x4interp had got switched on. With that off a more sensible 2.83 samples now." **This retracts the "broadband/non-stationary transient" explanation above** - it isn't needed. With the confound removed, the true bench-optimal shift is 2 samples -> 2.83 samples (+0.83 samples, +52us), which matches the LOCAL-TONE-FREQUENCY TFA prediction from earlier in this entry almost exactly: interpolating the real Trial2-vs-corrected-Trial3 group-delay curves at 1900Hz gave +51.8us (+0.829 samples) - predicting 2+0.829 = **2.829 samples**, against the measured 2.83. The simple "read the local TFA group delay at the actual tone frequency, add the difference to the old bench-tuned value" approach was the right one all along; the earlier beat-harmonic reading (which predicted ~0 net shift) was the less relevant one, and the apparent factor-of-2.5 residual that prompted the broadband-transient speculation was entirely explained by the interp-factor mismatch, not a real gap in the group-delay-based prediction method. **`relative_delay_samples` for an a+A-candidate preset: 2.83 samples (~177us), not the earlier 4.5.**

## 2026-09-05, later still — full-spectrum comparison: close-in IMD better, but a much larger far-out spur forest appears with the a+A candidate

**Context.** User uploaded `2gaA_spectra.txt` (default gdeq) and `3gaA_spectra.txt` (a+A candidate), both `g`+`a`+`A`+`eq` on, 700/1900Hz two-tone, real-hardware RF spectra (6740 points, 11.868Hz resolution, 0-80kHz, carrier at a ~25kHz bench test frequency - AD9851 output set low enough to capture directly). User's own read: "the new gdeq fit gives a complex result. In general I would rate it worse even though the close-in IMDs are better. I see this pattern with the white noise into mic too although less obvious."

**Confirmed quantitatively - both halves of that read are real, and they're visible in the same two spectra.** Converted both to dBc (relative to each file's own two-tone level) and located the carrier by the two main tones' spacing (1198.67Hz measured, matching 1900-700=1200Hz almost exactly in both files; fc2=24994.3Hz, fc3=25006.2Hz).

**Close-in classic two-tone IMD (3rd-11th order, `f1-nS`/`f2+nS`, S=1200Hz spacing) - candidate is better at every order checked:**

| Order | Low side | Default (2gaA) | Candidate (3gaA) | Diff | High side | Default | Candidate | Diff |
|---|---|---|---|---|---|---|---|---|
| 3rd | -500Hz | -34.2dBc | -41.5dBc | -7.3dB | 3100Hz | -29.8dBc | -35.7dBc | -5.9dB |
| 5th | -1700Hz | -29.4dBc | -32.9dBc | -3.4dB | 4300Hz | -26.8dBc | -30.3dBc | -3.5dB |
| 7th | -2900Hz | -30.2dBc | -31.9dBc | -1.7dB | 5500Hz | -28.5dBc | -30.3dBc | -1.8dB |
| 9th | -4100Hz | -34.1dBc | -34.6dBc | -0.5dB | 6700Hz | -32.8dBc | -33.1dBc | -0.3dB |
| 11th | -5300Hz | -37.1dBc | -37.2dBc | -0.1dB | 7900Hz | -36.3dBc | -37.9dBc | -1.6dB |

Every one of these ten products is equal or better (more negative) with the candidate - confirms "close-in IMDs are better" directly.

**Far out (roughly 4-16kHz from carrier, both sides) - a much larger, denser forest of additional lines, and it's substantially worse with the candidate.** Binning both spectra's local maxima in 1kHz windows across -20kHz to +14kHz and comparing: in the -16kHz to -8kHz window the candidate's peak levels run **+11 to +19dB higher** than the default's in the same windows (e.g. -15000..-14000Hz: default max -59.0dBc vs. candidate -47.9dBc; -9000..-8000Hz: default -62.0dBc vs. candidate -43.1dBc); similar-sized jumps appear from +4kHz to +10kHz above carrier. The BROADBAND floor between these lines (median level per bin) is essentially unchanged or even very slightly better with the candidate - this is not a raised noise floor, it's specifically that many more discrete lines reach much higher levels in that region. Direct peak-finding in the affected band turned up dozens of these lines in the candidate's spectrum that don't reach the same threshold in the default's (e.g. candidate has resolved lines at -8901, -8094, -7703, -6896, -6504, -5697, -5293, -4902, -4498, -4095Hz offset all above -45dBc, where the default's corresponding region is comparatively sparse).

**These are real intermodulation products of the same two tones, not an unrelated spur.** GCD(700,1900)=100Hz, so every genuine nonlinear product of a 700/1900Hz two-tone signal must land on the 100Hz grid - and the observed fine spacing in both spectra (many lines roughly 95-210Hz apart, superimposed on the familiar n\*1200Hz classic-IMD comb) matches that grid closely. This is consistent with a much richer intermodulation spectrum than the classic low-order polynomial picture predicts - expected from a genuinely sharp, near-discontinuous nonlinearity (the `freq_dev` sign-flip at the destructive-interference null, this project's own long-documented mechanism - see the 2026-09-03 entries above) rather than a smooth polynomial one. A sharp, repeating (once per beat cycle, 1200Hz) transient generates a dense harmonic/intermodulation comb, not just the handful of low-order products a mild nonlinearity would produce.

**Working read: this fits the same trade-off this session already found in the group-delay data, not a new, unrelated problem.** The a+A candidate's refit reduces PEAK-TO-PEAK dispersion over the full 100-8000Hz band, but it does so by moving delay OFF the 5-8kHz region and ONTO the 100-3000Hz region (confirmed twice already - the `relative_delay` local-tone-frequency analysis above, and the beat-harmonic table before that). A worse/different local delay match around the actual null-transient's frequency content could plausibly sharpen or reshape that transient in a way that spreads its intermodulation energy into more and higher lines further from the carrier, even while the classic near-in products (which sit closest to the region the candidate's curve improved) get better. This is offered as a plausible connection to already-established mechanisms, not a proven one - no direct check has been done of how the far-out comb responds to `relative_delay` retuning specifically (as opposed to the classic near-in IMD, which is what the 2.83-sample retune was optimized for).

**Open question, resolved:** were these two spectra captured with `relative_delay_samples` already at each configuration's own bench optimum (2 samples for 2gaA, 2.83 samples for 3gaA)? **User confirmed: "e&oe they were both at their optimised delay settings."** So the far-out forest is NOT a delay-misalignment artifact - it's a genuine, apples-to-apples property of the candidate coefficients at their own correctly-tuned operating point.

**Bottom line so far: not a clean win.** The a+A candidate measurably helps the classic near-in two-tone IMD metric this project has used throughout, and measurably hurts a broader intermodulation comb further out - a genuinely mixed, two-sided result, matching the user's own qualitative read exactly. User also reports the same pattern, "less obvious," with white noise into the mic - consistent with this being a real spectral-shaping trade-off (not a two-tone-specific artifact) rather than a plain regression. **Not recommending the a+A candidate for adoption on this evidence** - the group-delay dispersion improvement and close-in IMD improvement are real, but so is the far-out spectral cost, and this project's own working principle (any single scalar metric - p-p dispersion, or classic low-order IMD - has repeatedly failed to predict overall real-world quality on this architecture) applies here again.

**Chart:** `gaA_spectra_comparison.html` - full overlaid spectra (-20kHz to +14kHz from carrier, dBc), shaded bands marking the close-in (better) and far-out (worse) zones, hover tooltip, plus the close-in IMD table above. Verified via Playwright in both light and dark mode.

## 2026-09-05, later still — can the gdeq filter itself be improved? Checked the design space directly: no, not within this architecture

**Question.** With the delay-misalignment explanation for the far-out spur forest ruled out, user asked: "So can gdeq filter be improved? It obviously now has a big influence."

**First checked whether the far-out degradation is even visible to the linear group-delay model this whole refit is built on.** Computed local slope (d&tau;/df) on the two MEASURED curves (Trial2 default, Trial3-corrected candidate) at several Savitzky-Golay smoothing windows (121/151/181/221/261 points) to check robustness: at every window tested, the candidate's max local slope in the 2.8-4.5kHz zone is 1.2-2.7x steeper than the default's (e.g. at window=181, the validated pipeline's own setting: default 33.6&micro;s/kHz vs. candidate 72.4&micro;s/kHz). This is a real, robust feature of the MEASURED curves, not a smoothing artifact.

**But the ANALYTIC prediction (the same "bare analog+shelf1+shelf2 curve + 2-section allpass" model this whole design process is built on) does NOT show this.** Computed RMS and max local slope of the predicted total curve for both coefficient sets on the actual fit grid used for the a+A refit:

| | p-p | mean | RMS slope | max\|slope\| |
|---|---|---|---|---|
| Default (predicted) | 157.2&micro;s | 196.5&micro;s | 38.1&micro;s/kHz | 156.5&micro;s/kHz |
| a+A candidate (predicted) | 72.4&micro;s | 195.6&micro;s | 34.1&micro;s/kHz | 135.0&micro;s/kHz |

The model says the candidate is EQUAL OR BETTER on every one of these metrics, including local smoothness - the opposite of what the measured curves show in the 2.8-4.5kHz zone specifically. So the group-delay model doesn't reproduce the effect that's apparently driving the far-out spur growth.

**Searched the design space anyway, to see if a smoother alternative exists that the original p-p-only optimization might have missed.** Re-ran the same grid+polish optimization method already used for this project's fits, against the identical target curve, with several different objectives:

- p-p + &lambda;&times;RMS(slope), &lambda; = 0 to 2.0, 2 sections (a1, a2 free, not tied): converges to essentially the SAME point (a&approx;-0.1386) at every &lambda; tested - p-p and smoothness aren't in tension in this parameter space, so there's nothing to trade off.
- Pure RMS-slope minimization (ignoring p-p entirely): best found a&asymp;-0.0604, RMS slope 33.6&micro;s/kHz (barely better than the candidate's own 34.1) but at the cost of WORSE p-p (78.0 vs. 72.4&micro;s) - the candidate is already close enough to this smoothness-only optimum that there's no meaningful headroom left.
- 3-section and 4-section cascades, pure p-p minimization: p-p improves only marginally (72.4 &rarr; 71.1 &rarr; 70.4&micro;s, ~1-2&micro;s each) while MEAN delay balloons (195.6 &rarr; 258.2 &rarr; 320.7&micro;s) - a bad trade, not a free improvement; more sections buy essentially nothing here.

**Conclusion: within this filter architecture (cascaded first-order allpass sections fit against the analog+shelf1+shelf2 phase response), the a+A candidate is already at or extremely close to the Pareto-optimal point for both p-p dispersion and local smoothness simultaneously - there's no better coefficient choice available for a same-kind refit to find.** This means the far-out spur forest is NOT something explainable or fixable by more/better group-delay-domain filter design - the theory this whole design process is built on doesn't predict a difference in the 2.8-4.5kHz zone, so a "smarter" refit using the same theory can't be expected to find or fix whatever is actually happening there.

**What's actually going on is therefore outside this model - most likely one of:** (1) a genuine real-hardware non-ideality or nonlinear interaction specific to these coefficient values that pure linear group-delay theory doesn't capture (something about how the null-crossing transient's actual instantaneous waveform, not just its average delay, interacts with this particular allpass response), or (2) measurement variability between the two separate sweep sessions (Trial2 and Trial3-corrected were captured on different occasions, not back-to-back in one sitting) - this project has already documented real session-to-session/environmental effects elsewhere (PSU noise, interp-tick timing) that could plausibly contribute here too.

**Recommended next step, if this is worth chasing further:** a same-session, same-sitting A/B - flip only `ENV_GDEQ_USE_AA_CANDIDATE`, change nothing else (same warm-up, same bench setup, same few minutes), and re-capture both spectra back-to-back. If the far-out forest gap survives that tighter control, it's confirmed as a real coefficient-dependent effect worth its own investigation (at the waveform level, not the filter-design level, since filter design has been checked and is already near-optimal). If the gap shrinks substantially, session-to-session variability was a bigger contributor than assumed. Either way, redesigning gdeq itself is very unlikely to be the fix - that avenue is now checked and effectively exhausted for this architecture.

## 2026-09-05, later still — `ENV_GDEQ_USE_AA_CANDIDATE` replaced with a live `'G'` runtime toggle, for exactly that recommended same-sitting A/B

**Request:** "Yes make it flipable between gdeq curves. g & G?" - directly enabling the recommended next step above (a same-session A/B) without a reflash between the two coefficient sets, which is what made the earlier delay-misalignment and flag-not-set mixups (this file's own 2026-09-05 entries) possible in the first place - a runtime toggle removes both failure modes at once (no rebuild/reflash to get wrong, and both configs can be captured in one sitting).

**Implementation.** `envelope_gdeq.h`/`.cpp`: the old compile-time-exclusive `#if ENV_GDEQ_USE_AA_CANDIDATE` block (which `#define`d only ONE of `ENV_GDEQ_A1`/`A2` depending on the flag) is replaced with BOTH coefficient pairs always defined - `ENV_GDEQ_A1`/`A2` (default) and `ENV_GDEQ_A1_AA_CANDIDATE`/`A2_AA_CANDIDATE` (candidate) - plus a new `ENV_GDEQ_HAS_AA_CANDIDATE` macro (1 only on `ENV_FILTER_PNP_BC327_ATTN` @ `SAMPLE_RATE_HZ`=16000, the only filter/Fs this candidate was ever fit for; 0 everywhere else, with a harmless fallback definition so the .cpp still compiles). New runtime API: `envelope_gdeq_get_use_aa_candidate()` / `envelope_gdeq_set_use_aa_candidate(bool)` - the setter re-initializes both `ssb_allpass1_t` sections via `ssb_allpass1_init()` (documented cheap/task-context-safe, and it zeroes state as a side effect - same no-stale-x1/y1 reasoning `envelope_gdeq_set_enabled()`'s off->on reset already uses, so switching live never feeds a mismatched coefficient/state pair into the next sample) and is a no-op on builds where `ENV_GDEQ_HAS_AA_CANDIDATE` is 0.

**`'G'`** (serial_commands.cpp, mirrors the `'g'`/`'a'`/`'A'` toggle pattern exactly) switches between the two live, printing the key 2026-09-05 finding (better close-in IMD, worse far-out spur forest, not currently recommended) and the correct `relative_delay` bench-optimum reminder (2 vs. 2.83 samples, g+a+A config) every time it's used, so the caveat is unavoidable on the bench rather than something to remember from a doc. Guarded to a plain "no effect on this build" reply when `ENV_GDEQ_HAS_AA_CANDIDATE` is 0. The `'w'` chirp status line and boot banner were updated to report which coefficient pair is active, same as they already report `'g'`/`'a'`/`'A'`'s on/off state.

**Persisted** via a new trailing `PersistentSettings` field, `env_gdeq_use_aa_candidate` (off/false by default, same convention as every trailing field before it - existing presets' positional initializers zero-fill it, and it's included in the `'P'`-style paste-a-preset-line dump in the correct trailing position). Applied on preset load via `envelope_gdeq_set_use_aa_candidate()`, same reset-on-change handling as the live toggle.

**Not a recommendation change** - `env_gdeq_use_aa_candidate` defaults to off/false, same as every existing preset; this is purely a mechanism change (compile-time flag -> runtime toggle) motivated by the bench workflow, not a signal that the candidate is now trusted. The mixed real-hardware result from the entries above stands.
