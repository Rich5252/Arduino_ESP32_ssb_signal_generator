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

## freq_dev slew-rate limiter

| Key | Effect |
|---|---|
| `{` | Tighten the `freq_dev_hz` slew-rate limit (turns it on at 2000Hz/sample if currently off, then steps down 250Hz/sample at a time, floor 100Hz/sample) |
| `}` | Loosen the limit (steps up 250Hz/sample, then turns fully off past 8000Hz/sample) |

The phase-side counterpart to the envelope-null floor above — see `ssb_dsp.h`'s `ssb_dsp_set_freq_dev_slew_limit_hz()` doc comment for the full derivation. This limits how fast `freq_dev_hz` (the FM deviation fed to the AD9851 each sample) is allowed to *change* from one sample to the next — a different thing from `MAX_FREQ_DEV_HZ`, which limits its *value*. Verified numerically (a double-precision reimplementation of the exact DSP algorithm run alongside the real fast-math code, on synthetic two-tone input): real content, away from any envelope null, never asks `freq_dev_hz` to change faster than ~60Hz/sample even on busy high-center-frequency bands, while a genuine destructive-interference null asks for ~8000Hz/sample in a single step — a 100x+ gap with nothing in between. That single-sample swing is a real, correctly-computed requirement (it represents the true instantaneous phase reversal at the null), but a fast transient in frequency is inherently wideband in the spectrum. Limiting the slew trades a little reconstruction fidelity right at the null for less spectral splatter, without touching ordinary content.

Deliberately not a repeat of either envelope-floor mistake above: it doesn't freeze `freq_dev_hz` (no phase debt to snap back later — the AD9851 just integrates whatever frequency word it's handed each sample, so a slower-than-ideal ramp through the null *is* the applied modulation, not a deferred correction), and it isn't a hard value clamp (a slew limiter's output is continuous by construction, no derivative kink at any threshold). Applied inside `ssb_dsp_process_sample()`, after the `max_unclamped_freq_dev_hz` diagnostic captures the true unlimited peak and before the existing `MAX_FREQ_DEV_HZ` magnitude clamp (which still applies on top, unchanged).

Off by default (same convention as `g`/`D`/`x`/`z`) — not yet validated on real hardware beyond the numeric derivation above.

## Envelope output interpolation

| Key | Effect |
|---|---|
| `I` | Toggle 4x envelope output interpolation on/off |

Right now the commanded PWM duty only changes once per DSP tick (`SAMPLE_RATE_HZ` = 16000) — a zero-order-hold staircase feeding the analog Sallen-Key reconstruction filter, whose first spectral image sits at 16kHz. Checked against the same real LTspice sweep behind `envelope_gdeq.h`: the filter only provides -27dB of attenuation at 16kHz, but -63dB at 64kHz — so linearly interpolating the envelope between DSP ticks up to 4x the update rate (64kHz) pushes that image out to where the filter actually buries it, +36dB "for free." Directly inspired by QRP Labs' QMX SSB firmware (see project history/`G0UPL.pdf`), which does the same thing (28x, driven by its own DAC/CPU constraints — 4x here is a deliberately modest first step, not a re-derivation of that number) and reported it eliminating visible overshoot/undershoot on the amplitude waveform entirely.

Implemented (v4, after three earlier real-hardware failures — see below) by speeding up the EXISTING sample timer itself to `ENVELOPE_INTERP_FACTOR` (4) x `SAMPLE_RATE_HZ` (64kHz), and only running the full DSP pipeline (Hilbert transform, atan2, sqrt, freq_dev, envelope, ADC, AD9851, diagnostics — everything) on 1 in `ENVELOPE_INTERP_FACTOR` of those wakes ("full" ticks, still true `SAMPLE_RATE_HZ` — every filter constant in the codebase keeps assuming that rate unchanged). The other 3 wakes just walk a linear-interpolation ramp between the last two full-tick envelope values and call the same plain `envelope_output_write_pwm()` the code always used. No second timer, no ISR, no extra task, no LEDC hardware fade — it all happens inside `dsp_task` itself, a single already-proven-safe FreeRTOS task on Core 0.

Took four attempts to land here, all from real-hardware failures, not simulation — recorded because each one rules out an approach that looks reasonable on paper: v1 (a second 64kHz `gptimer` whose ISR did the interpolation math *and* the `ledc_set_duty()`/`ledc_update_duty()` write directly) rebooted the ESP32 outright — general LEDC driver calls aren't guaranteed IRAM/ISR-safe. v2 (deferred just the write to a dedicated task, notified via `vTaskNotifyGiveFromISR`) still crashed, with a "Coprocessor exception" panic — the ISR still did the interpolation *arithmetic*, and on this chip's Xtensa cores the FPU is itself a coprocessor whose context only survives a *task* switch, so any float op in a true ISR is unsafe. v3 (moved the arithmetic into the task too, then rebuilt around the LEDC peripheral's own hardware duty-fade engine instead of any software timer) stopped crashing, but exposed two more problems in turn — a dedicated task waking 64,000 times/second on Core 1 starved `loop()`/`handle_serial_commands()` of CPU time; and once that was fixed via the hardware fade engine, the LEDC's free-running PWM clock (78125Hz, no integer relationship to `SAMPLE_RATE_HZ`) introduced ~12.8us of unsynchronized timing jitter per tick, visible as the envelope's effective frequency jittering. v4 (the user's own suggestion) sidesteps all of it by not using a second clock domain or extra task at all — `dsp_task` has always lived on Core 0, physically separate from `loop()`/Serial on Core 1, so running it faster carries none of v3's starvation risk, and there's only ever one clock. v4's own first cut still had a bug, found on real hardware EVEN WITH 'I' OFF (envelope timing jittery, IMDs shuffled up and down): it tracked "which of the 4 fast ticks is this" with a bare software counter incremented once per wake, but `ulTaskNotifyTake(pdTRUE, ...)` silently coalesces multiple real hardware ticks into one wake if `dsp_task` is ever even briefly late (a cache stall, a moment of contention) - at 64kHz that's a real possibility, and once it happens the counter permanently desyncs from the true hardware phase, so "full" ticks start firing at the wrong, irregular offsets indefinitely (a form of sample-clock jitter on the DSP rate itself). Fixed by using `ulTaskNotifyTake()`'s own return value (how many ticks actually elapsed, not just "at least one") to advance the counter, so it always equals the true cumulative hardware tick count and the phase can never drift. That fixed the OFF-state jitter, but the envelope still looked jittery with 'I' ON - a second, related bug (v4.1): the ramp itself was step-COUNTED (one `on_interp_tick()` call = one step), which is fragile against the exact same coalescing effect, now applied to the ramp - and it turns out to fire on almost every cycle, not rarely, because a full tick's own DSP processing (tens of us) is comparable to or longer than one fast sub-period (~15.6us), so 1-2 of the following interp ticks routinely elapse and coalesce away before dsp_task gets back to blocking, losing that many ramp steps by an amount that varies with exactly how long that tick's processing took - a real, structural, content-dependent source of jitter in the rendered envelope shape. Fixed by making the ramp time-BASED instead: each write computes its value directly from `esp_timer_get_time()` minus the true tick-start timestamp, divided by the sample period, rather than from a step count - so whatever calls DO happen (however many, however irregularly spaced) always land on the mathematically correct point on the ramp, and a coalescing event just means fewer visible steps that cycle, not a wrong value at the ones that do render. Still moving around after that (v4.2): the phase-lock fix only checked `fast_tick_count % ENVELOPE_INTERP_FACTOR == 0`, which assumes a coalescing event always lands EXACTLY on a multiple of 4 - it doesn't have to. If a full tick's own DSP processing occasionally takes long enough that MORE than 4 real periods elapse before `dsp_task` gets back to blocking, the count can jump straight past the next boundary (3 -> 8, skipping 4) - the modulo check then stays false until the count next happens to land exactly on a multiple, meaning that period's real DSP sample is skipped outright (a genuinely missing sample, not just a cosmetic ramp step), and the following full tick is delayed further still while waiting to re-land cleanly - compounding the original overrun instead of absorbing it. Fixed by comparing which `ENVELOPE_INTERP_FACTOR`-sized GROUP the count falls into rather than its exact remainder - any update that crosses one or more group boundaries is recognized as a full tick immediately, however far past the exact boundary a coalesced count landed, so a bad overrun still costs the one sample it made unrecoverable but can no longer cascade into delaying subsequent ones. Worth checking the existing `[timing]` diagnostic block's `overruns=`/`wakeup jitter: max_gap_us=.../late_ticks_total=` line - if those are climbing, `dsp_task`'s own per-tick processing time is what needs investigating next, separately from this counter logic. See `ssb_mic_test.ino`'s `dsp_task` and `envelope_interp.h` for the full design and failure history.

Off by default (same convention as `g`/`D`/`x`/`z`/`{`/`}`) — not yet validated on real hardware.

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
| `0`-`9` | Load a preset from `settings.h` — sets every lever above (audio source, relative delay, PWM offset/scale, gdeq, ADC LPF mode, EQ, compressor, master gain, RF output, envelope pre-distortion, envelope-null floor, freq_dev slew-rate limit, envelope output interpolation) in one command. Boot banner lists the current names (dynamically, from the array's own size — always up to date). |
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
