# ssb_mic_test.ino — Serial Command Reference

Send any of these single characters over the serial monitor (921600 baud). Each command prints a `->` confirmation line showing the new state.

## Signal source

| Key | Effect |
|---|---|
| `t` | Two-tone test signal (700/1900Hz by default) |
| `s` | Single clean tone (1000Hz) — isolates DSP/RF chain from mic-side artifacts |
| `m` | Live mic input |
| `p` | Envelope step test — 4Hz square wave direct to PWM, carrier held fixed. For measuring the analog reconstruction filter's step response with a scope, independent of everything else in the chain |

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

## Audio processing

| Key | Effect |
|---|---|
| `e` | Toggle EQ (HPF + presence peak) on/off |
| `c` | Toggle compressor on/off (automatic makeup gain applied) |
| `+` | Master gain +1.0dB |
| `-` | Master gain -1.0dB |

Master gain scales the *whole* chain (phase + envelope together, inside `ssb_dsp`) — different from the PWM range knobs above, which only touch envelope.

## RF output

| Key | Effect |
|---|---|
| `o` | Toggle AD9851 RF output on/off (clean power-down via the chip's own control bit — preserves last-set frequency) |

## ADC / diagnostics

| Key | Effect |
|---|---|
| `f` | Toggle ADC low-pass filter bypass (raw vs. filtered) — for A/B testing whether an artifact comes from the filter |
| `v` | Mute/unmute the once-per-second `[timing]`/`[adc]`/`[dsp]` diagnostic block — mute this before adjusting other settings if you want to actually see the confirmation lines |
| `r` | Reset all diagnostic counters/watermarks for a clean measurement window (doesn't touch any of the settings above, only the stats) |

## Current testing defaults (compiled in)

- Two-tone mode on at boot
- EQ and compressor both off
- Master gain: -2dB
- Relative delay: 0 samples
- `MAX_FREQ_DEV_HZ`: 8000Hz (temporarily raised from the original 2800Hz for diagnostic testing — revert before considering this production-ready)

## Quick workflow reminders

- Send `v` first if you're about to do fine-tuning — otherwise confirmations get buried in the 1-second diagnostic spam.
- Levels take a moment to settle on the spectrum analyzer after any change — wait before reading.
- `[timing]` line's `mode=` field always tells you which signal source is actually active, so you can't accidentally misread results against the wrong source.
