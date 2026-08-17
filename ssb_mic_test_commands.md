# ssb_mic_test.ino — Serial Command Reference

Send any of these single characters over the serial monitor (921600 baud). Each command prints a `->` confirmation line showing the new state.

## Signal source

| Key | Effect |
|---|---|
| `t` | Two-tone test signal (700/1900Hz by default) |
| `s` | Single clean tone (1000Hz) — isolates DSP/RF chain from mic-side artifacts |
| `m` | Live mic input |
| `p` | Envelope step test — 4Hz square wave direct to PWM, carrier held fixed. For measuring the analog reconstruction filter's step response with a scope, independent of everything else in the chain |
| `y` | FM isolation test — pure sinusoidal frequency modulation (1200Hz mod, 3000Hz peak deviation), envelope held fixed, bypasses `ssb_dsp_process_sample()` entirely. Isolates the AD9851/SPI/delay-line chain from the Hilbert/DSP math |
| `h` | AM isolation test — pure sinusoidal amplitude modulation (1200Hz mod, 100% depth), `freq_dev_hz` locked at 0, also bypasses `ssb_dsp_process_sample()`. Isolates the RSET/PWM/analog-filter/transistor path |
| `T` | Steps the two-tone pair through `TWOTONE_BAND_PRESETS` (`test_signals.cpp`), low to high center frequency: 300/500, 700/900, 1500/1700, 2500/2700, 3500/3700Hz (all 200Hz spacing, each a clean single-point probe), then the original 700/1900Hz pair (1200Hz spacing, kept last for reference — its wide spacing averages alignment across a much bigger span, so it's not directly comparable to the other five), wrapping around. Switches into two-tone mode too if not already there. For sweeping the pair across the audio band without a recompile, to empirically map how much relative-delay retuning each band needs — see the group-delay-equalizer band-mapping note below |

## Phase/envelope relative timing

| Key | Effect |
|---|---|
| `[` | Decrease relative delay by 0.05 samples (5µs) |
| `]` | Increase relative delay by 0.05 samples (5µs) |

Signed, bipolar, fractional (interpolated between ring-buffer samples). **Positive** delays the *phase* path (AD9851) relative to envelope. **Negative** delays the *envelope* path relative to phase. Range roughly ±6 samples. Current best-known value: **-0.20 to -0.25 samples**.

## PWM duty-cycle range (envelope → RSET mapping)

Separate from master gain — this only remaps envelope's own output into a duty-cycle range, controlling where the signal sits relative to the BS170 gate's characterized sweet spot (~2.3V ±0.75V).

| Key | Effect |
|---|---|
| `u` | Raise duty range offset by 2% |
| `j` | Lower duty range offset by 2% |
| `i` | Widen duty range span by 2% |
| `k` | Narrow duty range span by 2% |

Default: offset 0.20, scale 0.90 → duty range 20%–100% (actually caps at ~92% due to the scale).

## Envelope group-delay equalizer

| Key | Effect |
|---|---|
| `g` | Toggle two cascaded first-order digital all-pass sections on the envelope path, fitted to flatten the *original* (non-Bessel) 2-pole Sallen-Key filter's group-delay dispersion across 100–4300Hz (numerically fit against a real LTspice sweep of that circuit — see `ENV_GDEQ_A1`/`ENV_GDEQ_A2` in `envelope_gdeq.h` for the full derivation). Goal: get that filter's better stopband rejection without its dispersion-driven IMD penalty, instead of the Bessel filter's compromise. **Not yet validated on real hardware.** |

Off by default. Enabling it pushes the envelope path's overall delay up by ~265µs (it can only add delay, not remove it) — re-tune `[`/`]` from scratch afterward; theoretical starting point is roughly **+2.65 samples**, a different regime from the Bessel filter's -0.20 to -0.25 samples.

## Envelope pre-distortion

| Key | Effect |
|---|---|
| `D` | Toggle a measured-curve lookup table (`envelope_predistort.h`) that corrects the *static* (memoryless) nonlinearity of the whole envelope→RF-amplitude chain — PWM/RC filter, BS170 gate transfer curve, and the AD9851's own RSET-to-DAC-current relationship, all as one measured end-to-end curve. A different problem from `g`'s: this is amplitude vs. commanded level (present even with a constant carrier), not delay vs. frequency. **REPLACES** the `u`/`j`/`i`/`k` linear offset/scale mapping while on — those knobs have no effect until `D` is toggled off again. **Not yet validated beyond the measurement itself.** |

Off by default. Table derived from a real hardware sweep (single-tone + master-gain steps, PWM ranging at full 0–100% span) — see `envelope_predistort.h` for the full derivation and the curve's shape. Now on its 2nd revision: 65 points built from 95 densely-measured dBm readings (down to 0.1dB steps via `'.'`/`','`) plus 17 gate-voltage readings, up from the original 33-point/19-measurement table. Dead below ~31% duty, steep turn-on through ~31–55%, then a gently-compressing climb that plateaus by ~95% duty rather than the full 100% the first revision assumed.

## Envelope-null floor

| Key | Effect |
|---|---|
| `x` | Raise the envelope-null floor by 0.02 |
| `z` | Lower the envelope-null floor by 0.02 (min 0.00) |

Two-tone envelopes dip toward zero at destructive-interference nulls, driving the commanded duty into the predistort LUT's steepest, most sparsely-characterized region (36–55% duty). `envelope_floor_apply()` (see `envelope_floor.h`) smoothly compresses envelope's whole `[0,1]` range into `[floor,1]` (peak still maps to 1, only the bottom is raised) so it can't be driven that low.

**Two earlier versions were tried and removed** — see `envelope_floor.cpp`'s header comment for both postmortems:

1. Also froze `freq_dev_hz` (phase rate) below the floor, meant to guard against `atan2()` noise near I=Q=0 — wrong for a two-tone signal (the fast phase rotation at a null is genuine required signal content, not noise) and produced a hard phase discontinuity: no effect until the floor got large enough (~0.08 in testing), then products jumping to 0dB right at that threshold as the accumulated phase debt snapped back.
2. A hard clamp (`min(envelope, floor)`) on the envelope side alone — left a derivative discontinuity at every crossing, which a two-tone signal hits often (near every null); showed up as nearly every product rising gently with each floor increment, a new distortion source confounding the actual question being tested. Replaced with the current continuous affine remap, which has no threshold and no kink anywhere in the trajectory.

Floor starts at 0.00 (off) — raising it trades some true-null carrier suppression (the deepest measured nulls only bought ~58dB anyway, see `envelope_predistort.h`) for keeping envelope out of the LUT's worst region. Applied unconditionally, right after `ssb_dsp_process_sample()`/the ENVSTEP/FMTEST/AMTEST equivalents, before `g`'s group-delay equalizer or `D`'s pre-distortion touch the envelope — so it sees the true raw signal, not a downstream-massaged version of it.

## Audio processing

| Key | Effect |
|---|---|
| `e` | Toggle EQ (HPF + presence peak) on/off |
| `c` | Toggle compressor on/off (automatic makeup gain applied) |
| `+` | Master gain +1.0dB |
| `-` | Master gain -1.0dB |
| `.` | Master gain +0.1dB (fine step) |
| `,` | Master gain -0.1dB (fine step) |

Master gain scales the *whole* chain (phase + envelope together, inside `ssb_dsp`) — different from the PWM range knobs above, which only touch envelope. `.`/`,` give 0.1dB resolution for dialing in precise gain-sweep measurement points (e.g. re-measuring `envelope_predistort.h`'s calibration table at finer steps) without 10 presses of `+`/`-` per dB.

## RF output

| Key | Effect |
|---|---|
| `o` | Toggle AD9851 RF output on/off (clean power-down via the chip's own control bit — preserves last-set frequency) |

## ADC / diagnostics

| Key | Effect |
|---|---|
| `f` | Cycle the ADC anti-alias low-pass filter: off → Butterworth → Chebyshev → off. Both are 4th-order (two cascaded biquads), same 3kHz -3dB point; Chebyshev trades a small in-band ripple (~1.4dB) for noticeably more stopband rejection above cutoff — see `adc_capture.h`/`ssb_adc_filter.h`. For A/B testing whether an artifact comes from the filter, and which filter family/order helps most |
| `v` | Mute/unmute the once-per-second `[timing]`/`[adc]`/`[dsp]` diagnostic block — mute this before adjusting other settings if you want to actually see the confirmation lines |
| `r` | Reset all diagnostic counters/watermarks for a clean measurement window (doesn't touch any of the settings above, only the stats) |

## Presets

| Key | Effect |
|---|---|
| `0`-`9` | Load a preset from `settings.h` — sets every lever above (audio source, relative delay, PWM offset/scale, gdeq, ADC LPF mode, EQ, compressor, master gain, RF output, envelope pre-distortion, envelope-null floor) in one command. Boot banner lists the current names (dynamically, from the array's own size — always up to date). |
| `P` | Print the current value of every one of those same levers as a single comma-separated line, wrapped in `{ ... },` and in exactly `PersistentSettings`'s field order — copy/paste it straight into the `settingsPresets[]` array in `settings.h` as a new preset. Rename the placeholder `"Live"` name (and add a numbered comment above it, matching the existing presets' style) after pasting. |

Edit the `settingsPresets` array in `settings.h` to change them, or dial in levers live and use `P` to generate the line instead of hand-typing values — check the live boot banner or `settings.h` itself for the current name/count rather than this doc, since the preset list changes often during active tuning. There's a compile-time check tying the array size to the `'0'`-`'9'` range (`settings.h`'s `static_assert`), so resizing it without updating `loop()`/`setup()`/`serial_commands.cpp`'s preset-select block — and the boot banner's own preset listing loop, which now reads the array size directly rather than a hardcoded count — fails the build instead of silently misbehaving.

## Current testing defaults (compiled in, before any preset is loaded)

- Two-tone mode on at boot
- EQ and compressor both off
- Master gain: -2dB
- Relative delay: 0 samples
- Envelope group-delay equalizer: off
- `MAX_FREQ_DEV_HZ`: 8000Hz. Originally raised from 2800Hz just to measure the real unclamped two-tone peak deviation (`[dsp] max_unclamped`, see `diagnostics.cpp`) while chasing a ~+100Hz two-tone frequency offset; that measurement came back ~4800-4900Hz — well above the old 2800Hz ceiling, meaning that clamp was routinely engaging on real signal peaks, not just as an edge-case safety limit. Whether that clamping was actually the *cause* of the +100Hz offset was never confirmed either way. Separately, real-hardware mic white-noise testing showed the freq-dev clamp performs better set higher, so **8000Hz is being kept for now** rather than reverted to 2800Hz — not yet landed on a final permanent value.

## Quick workflow reminders

- Send `v` first if you're about to do fine-tuning — otherwise confirmations get buried in the 1-second diagnostic spam.
- Levels take a moment to settle on the spectrum analyzer after any change — wait before reading.
- `[timing]` line's `mode=` field tells you which signal source is actually active (goes through the same name mapping used for `t`/`s`/`p`/`y`/`h`'s own confirmation lines, so `ENVSTEP`/`FM TEST`/`AM TEST` show up correctly there too, not just `TWOTONE`/`SINGLETONE`/`mic`), and `gdeq=` tells you whether the group-delay equalizer was on during that measurement window.
- Recommended workflow for validating `g`: `p` (envelope step) with `g` off vs on, scope the RSET step edge directly — flatter group delay should show as a cleaner edge. Then `h` (AM isolation) with `g` off vs on, to check whether it has any effect on the still-unexplained AM-to-PM sideband asymmetry (-2.4kHz nulls out, +2.4kHz stuck at -40dB). Re-tune `[`/`]` (starting near +2.65 samples) before judging real two-tone/IMD results with `g` on.
- Empirically mapping envelope/phase delay mismatch vs. frequency (no lab equipment needed beyond the RF spectrum analyzer already in use): `T` to step to a band, `[`/`]` to find the delay that minimizes IMD/spurs for that band, capture a spectrum, repeat for the next `T` press. The resulting per-band "best delay" values trace out the actual (as-built) delay-vs-frequency curve, which can be fed back into refitting `ENV_GDEQ_A1`/`ENV_GDEQ_A2` against real measured data instead of the original LTspice simulation.
