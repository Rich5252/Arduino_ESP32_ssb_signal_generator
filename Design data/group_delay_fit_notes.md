# Envelope group-delay equalizer — fitting notes

Source data: `SallenKey_LP_filter_BC337.txt` (LTspice AC sweep of the actual
original 2-pole Sallen-Key RSET reconstruction filter circuit, including the
BC337 buffer stage — not an idealized 2-pole formula).

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
