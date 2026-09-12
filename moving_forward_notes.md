# Moving forward: CPU/timing roadmap and open threads

Running log for forward-looking architecture/timing questions that aren't
tied to a specific bug fix - started 2026-09-09 off the back of the README
work, when three "what's worth tackling next" questions came up: ESP-IDF
toolchain levers for CPU/core control, whether the noise floor could be
reference-clock related, and whether the AD9851 write time is worth
attacking directly. Keep adding dated entries here as this thread develops,
same convention as `group_delay_fit_notes.md`/`ssb_mic_test_commands.md`.

## 2026-09-09: three "what's next" questions, real profiling data reused, one new lead

**Q1: ESP-IDF toolchain features for CPU/core control.** Checked the
project's actual Arduino IDE board settings (screenshot) rather than assume
defaults. Findings:

- `CPU Frequency: 240MHz (WiFi)` - pinned, not "Auto", so dynamic frequency
  scaling is not in play and `esp_pm_configure()` frequency-locking isn't
  needed here; the concern is already structurally satisfied.
- `PSRAM: Disabled` - rules out the PSRAM cache-coherency class of issue
  (`esp_cache_msync`) entirely.
- `Flash Mode: QIO 80MHz` - no faster mode available on this module (no OPI
  wiring on the Super Mini boards), so nothing actionable, but it's mild
  supporting evidence for the existing cache-stall theory: a flash
  cache-line fetch at 80MHz QIO is meaningfully slower than internal SRAM,
  so any non-IRAM-resident code on the hot path pays a real penalty of
  about the right order of magnitude.
- `Arduino Runs On: Core 1` / `Events Run On: Core 1` - confirms Core 0 is
  actually left exclusively to `dsp_task` as designed; not a new lead, just
  verification.
- `USB Mode: Hardware CDC and JTAG`, `USB CDC On Boot: Enabled` - **new
  candidate**, not yet ruled out by anything checked so far. The native
  USB-Serial-JTAG peripheral is live and doing its own housekeeping
  (enumeration, SOF handling) continuously on Core 1, a source distinct
  from the ADC/timer/I2C activity already investigated in the cache-stall
  work. Worth an A/B against `USB Mode: TinyUSB (USB-OTG)` if the leading
  hypotheses below don't pan out.

Other levers discussed but not yet needed/actionable: `vTaskCoreAffinitySet()`
(FreeRTOS-SMP) as a finer-grained version of the `xTaskCreatePinnedToCore()`
already in use; `esp_intr_alloc(..., ESP_INTR_FLAG_IRAM, ...)` for any new
ISR added to the timing-critical path; ESP32-S3 cache-line-size Kconfig
options exist in principle but Arduino IDE's board support doesn't expose
sdkconfig editing, so that lever needs a build-system migration to reach -
not justified by the problem size yet.

**Q2: could the noise be reference-clock related (external xtal, dividing
down the AD9851's 30MHz to sync with the ESP32)?** Real-hardware two-tone
measurement (user's own bench report): AD9851 output is "about 10Hz wide at
-40dB and nearly 100Hz wide at -60dB", with small discrete sidebands at
roughly +/-2, 4, 6Hz, but mostly wideband noise/jitter. Two separate
conclusions from this one measurement:

- The wideband noise/jitter component matches, by signature, the
  already-diagnosed cross-core cache-stall/scheduling-jitter mechanism
  (the same class of bug behind the `dac_task_enabled` regression and the
  gptimer/ADC-ISR priority collision fix). It does not look like reference-
  oscillator phase noise. **Conclusion: the AD9851's own 30MHz reference
  is clean enough that a shared/external/synced clock is not indicated -
  shelve that idea** unless a future spectrum-analyzer session finds
  close-in skirts that scale with the 6x multiplier specifically.
- The discrete +/-2/4/6Hz sidebands are a distinct finding. Lines that
  closely spaced to the carrier are essentially never a reference-clock
  effect - they're the signature of something in the firmware perturbing
  the envelope/phase on roughly a 1-2 second cadence (2/4/6Hz reading as
  harmonics of a ~1Hz fundamental, or of a non-sinusoidal disturbance's own
  repetition rate). The one existing, already-instrumented candidate that
  fits: `diagnostics_service()`'s long-window print fires every 1000ms
  (`diagnostics.cpp:667`, `now - last_timing_print_ms >= 1000`). **This is
  exactly the test the project's own 2026-09-07 note already proposed and
  apparently never followed up on** ("Recommended next step: re-run muted
  ('v') to see if it's a smaller residual of the already-fixed Core-1/
  Serial stall mechanism" - `ssb_mic_test_commands.md:300`). Re-running the
  two-tone measurement with diagnostics muted (`'v'`) is a single,
  no-rebuild experiment that tests both the wideband-jitter hypothesis and
  the discrete-sideband hypothesis at once. **Not yet run as of this
  entry.**

**Q3: is the AD9851 write worth attacking directly, and where's the real
CPU budget going?** User confirmed: yes, but the ~32us unexplained stall
(already found and left unresolved in the 2026-09-07 entry) is the obvious
thing to tackle first, not the AD9851 write itself. Restating why, since
it reframes the original question: the `write_us=33` figure isn't all
AD9851 write - the profiled breakdown was `prep_us=2, spi_us=17` (19us
actually inside `ad9851_set_frequency()`), with the remaining 14us being
ampeq/predistort/relative_delay/LEDC-write/diagnostics bookkeeping that
happens to share that timestamp bucket. The true bit-bang cost is 17us for
40 bits, which is CPU-loop-bound (three `GPIO_OUT_W1TS/W1TC` register
writes per bit, no clock divider to "double" since this is bit-banged, not
hardware SPI) - shrinkable via fewer register writes per bit, or via moving
the whole transfer to the RMT peripheral (viable but nontrivial: RMT drives
one GPIO per channel, so DATA/W_CLK/FQ_UD would need three synchronized RMT
channels via the sync-manager feature, plus a per-call byte encoder for the
DATA channel since that data changes on every call - up to 16000x/sec).
That's real engineering effort for maybe 5-10us of headroom.

The 32us stall, by contrast, is confirmed to occur even with zero AD9851/
DSP/ADC activity in that code path (`max_busy_us=64` against a 62.5us
budget at 16kHz - the tick already overran in that capture), and its cause
was explicitly left open ("points at an external interrupt/scheduling
event"). Agreed target. Audit plan, in order:

1. **Mute-diagnostics test** (see Q2 above) - cheapest, already proposed,
   tests two symptoms at once. Do this first.
2. **Scope-bracket the exact window**, same technique that found the
   gptimer/ADC-ISR priority collision previously: the 32us currently lands
   inside the timestamp bucket labeled "adc" even in two-tone mode where no
   ADC code runs there, so a `TIMING_DEBUG_GPIO`-style toggle at that
   segment's real start/end will show whether it's one contiguous slow
   stretch or a gap (gap = preemption by something else; contiguous slow
   = something in that segment itself).
3. **`esp_intr_dump()`** - one call, full inventory of every registered
   interrupt's core/priority. Cheap sanity check before guessing further.
4. **USB Mode A/B** (see Q1) - `Hardware CDC and JTAG` vs `TinyUSB
   (USB-OTG)`, if 1-3 don't land it.

## 2026-09-09, later same day: USB Mode A/B attempted (item 4) - inconclusive, and a data-reading caveat worth flagging

Tried the `USB Mode: TinyUSB (USB-OTG)` A/B suggested above (item 4 of the
32us audit plan). Two results:

- **Practical problem, likely just a mode quirk, not a finding:** the board
  wasn't recognized as a COM port under this mode. This matches a known
  ESP32-S3 Arduino behavior difference - `Hardware CDC and JTAG` exposes a
  bootloader-level virtual COM port immediately (even before the sketch
  runs), while `TinyUSB (USB-OTG)` only enumerates once the firmware itself
  reaches the point of initializing the USB stack, and some boards need a
  manual boot-mode button sequence to be seen reliably during upload. Given
  this, and that `Hardware CDC and JTAG` is the known-good, stable mode for
  day-to-day work, **recommend reverting to `Hardware CDC and JTAG` for now**
  rather than fighting the enumeration quirk - this A/B isn't worth the
  daily friction unless items 1-3 come up empty.
- **Data captured despite the port issue:** the monitor did show
  `[core1]   idle hook calls/s=19712-19715 (cross-check)` once per second.
  Important caveat, from the code's own comments (`diagnostics.cpp:138-148`,
  `CORE1_IDLE_GAP_THRESHOLD_US=10`): this specific line is explicitly
  documented as an unreliable cross-check that **reads low** - it only
  counts an idle-hook call as "idle" when consecutive calls are ≤10us
  apart, so a real blocked wait (e.g. `loop()`'s `delay(10)`, which spaces
  idle-hook calls by the OS tick period, not microseconds) is mostly
  invisible to it. The code comment explicitly points at "`[core1]` busy
  breakdown's delay% below" as the trustworthy figure instead - which
  wasn't captured this time. **This single line, on its own, can't be used
  to conclude anything about whether TinyUSB helped, hurt, or made no
  difference to Core-1 headroom or the 32us stall.** To actually use this
  experiment, next time capture the full `[core1] busy breakdown` line
  (cmd/adc_svc/diag/delay%) and the `[timing]` busy/overrun/max_busy_us
  line from the same window, under both USB modes, for a real comparison.
- "RF default output was running and was no different to normal" - a
  qualitative/by-ear check, not a scoped measurement of the 32us stall or
  the +/-2/4/6Hz sidebands specifically. Treat as a weak, not rigorous,
  negative data point against the USB-Serial-JTAG-housekeeping hypothesis -
  worth remembering, not worth ruling the hypothesis out on.

**Net effect: item 4 stays open, but isn't the efficient next step.** Given
the enumeration hassle and the inconclusive readout, the mute-diagnostics
test (item 1, `'v'`, no rebuild, no USB-mode juggling, tests two hypotheses
at once) is still the right next experiment to actually run.

## 2026-09-09, later still: mute-test run, and a strong, clean hit - continuous Serial diagnostic output is a major noise source, 20Hz-spaced sidebands

**Correction (see next entry): the earlier +/-2/4/6Hz close-in sideband
measurement was actually taken with diagnostics MUTED ("v off"), not
streaming as first assumed here from the boot-default value.** That
measurement and the 20Hz broadband finding below are two independent
phenomena, not the same one at two intensities - see the correction entry
for the full picture. Keeping the reasoning below as originally written
since it's still the right explanation for the 20Hz broadband effect
itself, just not for the close-in lines.

This session's actual mute/unmute A/B (toggling `'v'` live) gives a much
cleaner and stronger result than the earlier close-in measurement did:

**With continuous Serial diagnostic output running, RF noise gets
noticeably worse - dominant sidetones appear at ~20Hz spacing, high enough
to swamp the underlying carrier noise.** This is a clear, reproducible,
user-confirmed A/B, not a hypothesis.

**Best-fit cause, checked against the actual code:** `diagnostics_service()`
has exactly two periodic Serial outputs, both gated by `s_diag_muted`
(`diagnostics.cpp:661` and `:667`):

- a fast status line every 45ms (`now - last_print_ms >= 45`) - 1/0.045 =
  **22.2Hz**, a close match to the observed ~20Hz spacing given normal
  by-eye/by-ear spacing precision;
- a slow "long-window" line every 1000ms - 1Hz, a poor match for a 20Hz
  spacing and not the likely cause here.

No other ~50ms-period activity exists in the codebase (checked `diagnostics.cpp`,
`ssb_mic_test.ino`, `serial_commands.cpp`, `adc_capture.cpp` for anything else
near that cadence - only the 45ms/1000ms diagnostic lines and `loop()`'s own
`delay(10)`, which runs faster still). **The 45ms fast diagnostic line is
the leading suspect for the 20Hz broadband sidetones** - see the correction
entry below for why this is now understood as a SEPARATE mechanism from the
close-in +/-2/4/6Hz lines, not a bigger version of the same thing.

This also plausibly ties into the still-open 32us stall investigation:
`diag_room_for()` exists specifically because unguarded `Serial.printf()`
calls can block on a full USB CDC TX buffer (the same class of bug behind
the earlier-fixed multi-ms Core-1 stall) - a 45ms-cadence burst of several
printf calls is exactly the kind of Core-1 Serial activity already suspected
of bleeding into Core-0 timing via cross-core interference, the same
mechanism family as the `dac_task_enabled`/I2C regression and the ADC-ISR
priority collision, both already confirmed causes elsewhere in this project.

**Recommended next steps, in order:**

1. **Adopt muted-by-default as standard practice for any noise-sensitive
   RF measurement** (two-tone, IMD, noise floor) - this alone looks like it
   removes a dominant noise source, not a marginal one.
2. **Slow the 45ms fast line down** (e.g. to 200-500ms) or gate it out of
   any noise-critical test mode entirely, then re-measure with diagnostics
   *unmuted but slowed* to confirm the 45ms cadence specifically (not
   Serial activity in general) is what's producing the 20Hz spacing - this
   isolates "printing at all" from "printing this often."
3. Re-run the 32us-stall scope-bracketing test (audit item 2, above) with
   diagnostics unmuted vs. muted, now that there's a concrete reason to
   suspect the 45ms line specifically rather than Serial output in general.

## 2026-09-09, later still: correction + new nuances - two INDEPENDENT noise mechanisms, not one, plus a fade-then-reappear behavior on the 20Hz effect

User clarified the mute state of the earlier close-in measurement directly,
which corrects the previous entry's assumption: **the +/-2/4/6Hz close-in
sideband measurement was taken with diagnostics MUTED ("v off").** So that
finding and the 20Hz broadband finding above are NOT the same mechanism at
different intensities - they're two separate things:

- **Close-in +/-2/4/6Hz lines**: present with diagnostics fully muted.
  Cannot be caused by the 45ms/1000ms Serial print lines (both gated off
  entirely by `s_diag_muted`) or anything else gated the same way. Still
  unexplained - the diagnostics-print hypothesis from the earlier entries
  does NOT apply here and should not be treated as having accounted for
  this. Needs its own investigation (see "Next" below).
- **~20Hz broadband bursts**: confirmed to require diagnostics UNMUTED
  ("if I stop output and use 'V' those outputs cause the higher noise") -
  the 45ms-print-cadence explanation from the previous entry stands for
  this one specifically.
- **The two affect different parts of the spectrum**: user reports the
  20Hz-spacing effect shows up in the broadband noise, while "the very
  narrow band noise around the carrier" (i.e. the close-in +/-2/4/6Hz
  region) doesn't change much when toggling diagnostics on/off. Consistent
  with them being genuinely independent mechanisms rather than one thing
  measured two ways.
- **Measurement caveat, noted by the user and worth keeping attached to
  this data going forward:** the narrowband (close-in) display is a
  0.18Hz-resolution FFT with averaging, "quite slow to respond" - so
  "doesn't change much" there is measured through a slow-responding,
  heavily-averaged instrument. That's good for confirming the close-in
  lines are a persistent/steady-state feature (not a fluke), but it also
  means a real small transient change in that region could be smoothed
  away by the averaging and simply not show up in this display. Don't
  over-read "no change" as "definitely zero effect" until checked with a
  faster-responding capture if it ever matters enough to chase.

**New, so-far-unexplained wrinkle on the 20Hz broadband effect itself:**
left running for long periods with diagnostics unmuted, the bursts "almost
disappear" - but toggling `'v'` off and back on makes them reappear. This
doesn't fit a simple "the 45ms print always costs a fixed amount and always
produces a fixed spur" model; it points at some state that drifts toward a
quieter equilibrium over time (a USB host-side buffer/backlog settling, a
queue depth stabilizing, `diag_room_for()`'s available-headroom check
behaving differently once the host's receive side has caught up, or
something similar) and gets reset back to a "fresh/bursty" condition by the
mute/unmute toggle itself. User noted this resembles "a few other things
I've seen" in this project - i.e. this may be another instance of a
recurring pattern here (something that changes character over a long run)
rather than a one-off. Not investigated further yet - flagging so it isn't
lost, since it complicates any future "confirm the 45ms line causes this"
experiment (a quick toggle-and-look test will see the effect; a long
steady-state run might not, and that's expected now, not a contradiction).

**Next, for the still-unexplained close-in +/-2/4/6Hz lines:** since these
persist muted, the diagnostics print theory is ruled out for them
specifically. Worth testing next with a plain, unmodulated, idle carrier
(no test signal, no envelope/PWM activity at all) under the same muted
conditions: if the +/-2/4/6Hz lines persist even then, that points outside
this project's own DSP/envelope chain entirely (measurement-receiver LO
drift, mains-related hum products, thermal/power-supply drift on the bench)
rather than at firmware; if they disappear, that points at something
running continuously regardless of mute state within the DSP/envelope path
itself (e.g. the Hilbert FIR, `envelope_floor`/`gdeq`/`ampeq`/`predistort`
processing, or - checked and considered unlikely by design, since the
ADC/DSP/PWM clock relationships were deliberately kept at exact integer
ratios - a beat between two of the project's own internal clock domains).
Not yet run.

## 2026-09-09, later still: cleanly isolated to the AM path - PM is spotless, AM carries a genuine ~1Hz-spaced disturbance, unrelated to Serial

Two controlled isolation-test measurements, much cleaner than the earlier
general two-tone captures:

- **Pure sine/CW tone (PM path only)**: very clean, >-70dB at +/-2Hz, and
  that floor is admittedly window-limited (Hann window sidelobes, not a
  real spur) - so the PM/frequency path is, as far as this measurement can
  tell, essentially spotless. Confirmed not affected by Serial.
- **`'h'` (`AUDIO_SRC_AMTEST`, pure sinusoidal AM isolation test,
  `AM_TEST_MOD_HZ=1200Hz`, `config.h:440`)**: carrier clean, the wanted AM
  sidebands (at fc+/-1200Hz) are good, but there's a real comb of
  **~1Hz-spaced satellite lines around the AM sideband complex** starting
  at about -40dB relative to the AM sideband (-55dB relative to carrier).
  **Confirmed not affected by Serial activity either** - a different,
  independent confirmation from the earlier muted-close-in measurement,
  now specifically isolated to the AM path.

**Why this is a strong result:** `test_signals.h` documents that
ENVSTEP/FMTEST/AMTEST all bypass `ssb_dsp_process_sample()` entirely -
`'h'` writes the envelope directly, so this signal never touches the
Hilbert FIR, `atan2`/`sqrt`, or the DC-blocking filter at all. A ~1Hz
disturbance appearing on this signal must therefore come from somewhere
downstream of raw envelope generation: `envelope_floor` (off by default -
floor starts at 0.0), `envelope_gdeq`, `envelope_ampeq`,
`envelope_predistort`, the PWM/LEDC write itself, or the analog RC/BS170/
RSET output stage - or it's genuinely external to the firmware (bench
power-supply ripple/slow control-loop "breathing", thermal drift, a
mains-related beat). Checked `envelope_gdeq.cpp`/`envelope_ampeq.cpp`/
`envelope_predistort.cpp`/`relative_delay.cpp` for any internal
timer/counter/periodic-recompute logic - found none; they're all pure
per-sample static functions with no notion of wall-clock time, so nothing
obviously self-explains a ~1Hz component from the DSP correction chain
itself. That pushes the likely explanation toward either the PWM-output/
hardware side, or something external to the board entirely - a real
possibility for RF work, since a slow ~1Hz "breathing" ripple on a supply
rail feeding the RSET/PWM/RC stage is a classic symptom of a switching
regulator's control loop or a marginal linear regulator, and wouldn't show
up in any firmware code review at all.

Also worth noting: since `AM_TEST_MOD_HZ=1200Hz` and these satellite lines
sit right around the 1200Hz AM sideband complex (not around the carrier
itself), what's being seen is a slow disturbance modulating the wanted
1200Hz AM tone - "modulation of a modulation" - which fits equally well
with either a firmware-side envelope disturbance or a power-rail ripple
affecting the RSET reference while the PWM duty is actively swinging.

**Recommended next steps, in order:**

1. **Static/unmodulated envelope test**: command a fixed, unchanging duty
   (no `'h'` sweep at all - e.g. a direct duty override) and check whether
   the ~1Hz lines are still present. If yes, that strongly implicates
   something external and continuous (supply ripple, thermal drift) rather
   than anything reacting to a changing envelope. If they disappear, that
   points at something that only manifests when the envelope is actively
   moving - narrowing back toward the correction chain or the PWM update
   mechanism itself.
2. **Toggle `'D'`/`'g'`/`'a'`/`'A'` one at a time during the `'h'` test** -
   a cheap way to check whether predistort, gdeq, or either ampeq shelf is
   involved, without new instrumentation.
3. **Scope the supply rail feeding the RSET/PWM/RC filter stage directly**,
   independent of firmware - if a ~1Hz ripple or slow oscillation shows up
   there, this is a power-supply/analog finding, not a code one.

Not yet run.

## 2026-09-09, later still: "no sidebands on the carrier" reframes the leading hypothesis toward real sample-timing jitter, not supply ripple; scope "jitter" is likely a separate, benign artifact

Two more pieces of information, and they point in a genuinely useful
direction - one revises the ranking of last entry's three next-step
hypotheses, the other answers a separate question about what the scope is
showing.

**"The sidebands do not occur on the carrier" is the important new
detail.** Worked through what this implies physically: if the ~1Hz
disturbance were a gain/amplitude-domain effect - supply ripple or thermal
drift multiplying the whole envelope, including its steady/DC part - it
would produce matching sidebands around the carrier too, since a
multiplicative ripple scales everything the envelope touches equally. It
does not do that here. What DOES naturally produce sidebands proportional
to a signal's own rate of change, while leaving a constant/DC term
essentially untouched, is jitter in *when* each envelope sample is
actually written out (a first-order Taylor argument: a timing error
delta(t) perturbs the reconstructed signal by approximately
delta(t) * (rate of change of the signal at that instant) - zero for a
flat/DC term, non-zero and growing with amplitude/frequency for a moving
AC term like the 1200Hz AM tone). **This matches the observation exactly**
and reframes the leading hypothesis: this looks like real per-sample
envelope-write TIMING jitter, not external amplitude/gain-domain ripple -
i.e. plausibly the SAME broad jitter/scheduling story already being chased
elsewhere in this thread (the gptimer/ADC-ISR history, the still-open 32us
stall), rather than a new, separate power-supply finding.

This sharpens the static-envelope test from the previous entry into an
actual falsification test, not just a fishing expedition: **if this really
is sample-timing jitter, a perfectly static/unmodulated envelope should
show essentially none of the ~1Hz artifact, even if the underlying tick
jitter is fully present** - because jitter can only smear content where
the signal has slope, and a static envelope has none anywhere. If the
static test still shows the ~1Hz lines, that would rule out simple
write-timing jitter and point back to a genuine amplitude/gain-domain
mechanism (supply ripple etc.) after all. **Revised priority order: run
the static-envelope test first** (previous entry's item 1); the supply-rail
scope check (previous entry's item 3) drops to "only if the static test
comes back clean" rather than being pursued in parallel.

Also worth reconciling why the PM-only sine test (previous entry) came back
spotless if the same shared per-sample tick could be jittery: a
frequency-word write landing slightly early/late doesn't produce a
proportional phase error the way a mistimed voltage/duty value does - the
AD9851 free-runs its own DDS core between updates, so the "sensitivity" of
the two paths to the same absolute timing error is genuinely very
different by the physics involved, not necessarily evidence that the PM
path's timing is jitter-free while the AM path's isn't.

**Separate question, separate (mostly benign) answer: the timing wobble
seen directly on the scope, on both the AM test's modulating sine and the
two-tone envelope.** This is very likely explained by the modulation
frequency not being an exact integer submultiple of `SAMPLE_RATE_HZ`
(16000Hz), not by anything in the firmware. The zero-order-hold PWM/RC
reconstruction produces a staircase that repeats bit-for-bit, at the same
phase, every single cycle ONLY when `Fs/F` is an exact integer - a scope
free-running relative to the ESP32's clock, triggered on a level/slope of
that same asynchronously-sampled waveform, will otherwise see the
staircase's phase walk cycle-to-cycle purely as a consequence of that
non-integer ratio, producing exactly the kind of apparent time-domain
"jitter" described here - completely independent of whether the underlying
sample clock has any real timing error at all. **The 1600Hz two-tone
difference-frequency experiment (`Fs/F = 16000/1600 = 10`, an exact
integer) coming back visibly more stable is a clean, correct, textbook
confirmation of exactly this mechanism** - a strong piece of evidence this
particular scope observation is a benign, well-understood viewing artifact,
not a project bug.

**Important: this scope-domain explanation does NOT explain away the
spectral ~1Hz sidebands from the `'h'` AM test.** A spectrum-analyzer/FFT
measurement doesn't care about scope trigger phase at all, so that finding
stands on its own and is the one worth chasing with the static-envelope
test above. Two genuinely separate questions, two different (and both
now reasonably well-understood) answers.

## 2026-09-09, later still: `'D'` toggle - no change; ruled out test-tone-generation accuracy; PWM quantization flagged as a plausible amplitude contributor (not a spacing explanation)

**`'D'` (predistort) toggled off during the `'h'` AM test: no change to the
~1Hz sidebands.** One correction-chain suspect cleared; `'g'`/`'a'`/`'A'`
not yet tried the same way.

**User asked whether this could be a test-tone-generation accuracy issue
(integer math?).** Checked the actual code
(`test_signals.cpp:211-216`, `test_signals_generate_amtest()`): not integer
- it's a plain running phase accumulator in float32 (`s_amtest_phase +=
two_pi * AM_TEST_MOD_HZ / (float)SAMPLE_RATE_HZ`, `sinf()` each sample,
wrap-by-subtraction past `two_pi`). **Ruled out as the cause**: float32
rounding in this accumulator (both the fixed bias in the per-sample
increment, since 1200/16000 isn't exactly representable in binary, and
running-sum round-off) is on the order of 1e-7 relative. The observed
sidebands are ~1% relative to the AM sideband (-40dB) / ~0.18% relative to
carrier (-55dB) - five to six orders of magnitude larger than tone-
generation float rounding could produce. Not the source.

**Adjacent detail worth keeping attached, not a rival explanation:**
`RSET_MOD_LEDC_RES` is 10-bit, so PWM duty quantizes in steps of about
1/1023 (~0.1% of full scale) - actually in the right order of magnitude for
the observed amplitude, unlike the float math. But a static quantization
step on its own produces distortion at harmonics of the modulating tone
(2400Hz, 3600Hz... for a 1200Hz AM tone), not a comb spaced at ~1Hz -
so quantization is a plausible source of there being visible products at
all, while the ~1Hz spacing still needs its own slow-periodic explanation
(the timing-jitter hypothesis from the previous entry) to be doing the
modulating/dithering. Consistent with, not competing against, that
hypothesis.

**Static-envelope test (previous entry's top next step) remains the
highest-value thing to actually run - still not yet done.**

## 2026-09-09, later still: RESOLVED - the ~1Hz sidebands were the measurement receiver's own RX IF AGC, not this project at all; a new, separate, well-understood item found underneath (100Hz PSU ripple)

**Root cause found: the receiver's RX IF AGC was pumping the level at 1Hz,**
producing exactly the ~1Hz-spaced comb described across the last several
entries. Turning the RX AGC off made it clean, "at least to measurement
error level." **This closes out the whole close-in/AM-path sideband thread
- it was never a firmware, timing, or PWM-quantization issue on this
project's side at all; it was an artifact of the measurement chain.**

Worth being honest about the process here: the "no sidebands on the
carrier" physical reasoning two entries up (pointing at real per-sample
envelope-write timing jitter over gain-domain ripple) was a sound deduction
*given the data available at the time*, and would have been the right
conclusion if the cause had genuinely been internal - but the actual
source was external to the DUT entirely, which no amount of reasoning about
this project's own code could have anticipated. The static-envelope test
that was queued up as the next step is now moot for this specific finding
(though it may still be worth keeping in mind as a general clean-test
technique for anything AM-related in the future). **AM path noise floor,
once the receiver's own AGC artifact is excluded, now looks clean to
measurement-error level - genuinely good news for the whole EER chain.**

**What's left underneath, now clearly resolved as a separate item: 100Hz-
spaced spurs at -50dB.** This is a textbook, well-understood signature -
50Hz mains rectified/smoothed to DC typically leaves ripple at 100Hz (twice
mains frequency, from full-wave rectification) on a supply rail. This is a
hardware/power-supply finding, not a code one - nothing in this project's
firmware runs anywhere near 100Hz in a way that would explain it. -50dB is
a fairly clean level as bench supplies go; not flagged as urgent unless a
future target spec needs better, in which case the usual levers are
additional decoupling/LC filtering on whichever rail feeds the RSET/PWM/RC
stage, or a cleaner/better-regulated supply for that stage specifically.
Not investigated further here - noting it as understood and low-priority
unless it becomes a limiting factor later.

## 2026-09-09, later still: RESOLVED - the 400Hz "sub-IMD IMD" family on the two-tone test, confirmed by direct falsification test; likely retroactively implicates the long-running "spur forest" investigation

**User question:** noticed a family of products at 400Hz spacing sitting
BETWEEN the main 1200Hz-spaced IMD ladder on the default 700/1900Hz
two-tone test, tracking the main ladder's strength up and down ("sub-IMD
IMDs").

**Diagnosis:** the 700/1900Hz pair's envelope beats at their difference,
1200Hz - the expected spacing for AM/envelope-domain sidebands around the
carrier. But `SAMPLE_RATE_HZ` (16000) is NOT evenly divisible by 1200Hz
(16000/1200 = 13.33...). It IS evenly divisible by 400Hz: `GCD(16000,
1200) = 400`, and 40 samples (2.5ms) contains exactly 3 whole cycles of
1200Hz. That means the digitally-sampled realization of the envelope's
distortion products can only be truly self-similar cycle-to-cycle at
400Hz, not 1200Hz - so any quantization/rounding nonlinearity in the chain
(PWM's 10-bit duty steps, the predistort LUT, envelope generation itself)
that's sensitive to exact sample-to-cycle phase will leak real spectral
energy at multiples of 400Hz, riding on top of the "intended"
continuous-domain 1200Hz harmonic ladder - the same underlying mechanism
producing both, which is exactly why the 400Hz family's strength tracks
the 1200Hz ladder's strength. Same principle as the earlier 1600Hz
single-tone scope experiment (`Fs/F` integer -> clean), but showing up here
as genuine spectral content rather than a scope-triggering artifact,
because there's a real per-sample quantization nonlinearity involved this
time, not just a display/triggering effect.

**CONFIRMED by direct test: switching the two-tone pair to 700/1700Hz
(1000Hz spacing, `16000/1000 = 16` exactly - Fs-commensurate) "killed them
dead."** Clean, decisive falsification-test result - exactly the outcome
the mechanism above predicts, and about as strong a confirmation as this
kind of hypothesis ever gets.

**Retrospective implication, worth flagging clearly:** the legacy
700/1900Hz pair (1200Hz spacing) is the ONE two-tone preset that is NOT
Fs-commensurate - the `'T'` command's other five band presets are all
200Hz-spaced, and `16000/200 = 80` exactly. This means the entire
multi-day "spur forest" / gdeq-refit investigation in
`group_delay_fit_notes.md` (2026-09-05 through 09-07 - the a+A candidate,
candidate B, the "group-delay theory doesn't fully explain the real
spectral behavior" conclusion, envelope-domain-nonlinearity-or-unidentified-
mechanism left as the two open candidates) **was very possibly
contaminated by this exact same non-commensurate-sampling artifact the
whole time**, without anyone knowing to suspect it. This isn't proof that
mechanism is wrong, but it's a real confound that was never accounted for
in that investigation and should be closed off before trusting any of its
conclusions further.

**Recommended next step, high value:** re-run the core two-tone gdeq/ampeq/
spur-forest characterization on an Fs-commensurate pair (e.g. 700/1700Hz,
already confirmed clean above, or another 16000-divisor spacing) instead of
the legacy 700/1900Hz pair. This should give a materially cleaner picture
of the correction chain's real behavior, stripped of this newly-identified
sampling-grid artifact - quite possibly resolving or substantially
simplifying the still-open "why doesn't group-delay theory fully predict
the real spectral behavior" question from that entire investigation. Not
yet re-run.

**General practical takeaway for all future two-tone/IMD testing:** always
pick a tone spacing that evenly divides `SAMPLE_RATE_HZ` (16000) to avoid
this class of artifact contaminating results - the existing 200Hz-spaced
`'T'` presets already satisfy this by construction; the legacy 700/1900Hz
pair does not and should probably be treated with caution (or retired as
the reference pair) going forward.

**Two further observations on the same 700/1700Hz (Fs-commensurate) test,
both consistent with rather than separate from the finding above:** (1) the
general/broadband noise floor reads roughly 10dB lower than on the
non-commensurate pair; (2) it's noticeably less affected by the Serial-
diagnostic interference (the 45ms-cadence ~20Hz-broadband effect from
earlier). Plausible, not rigorously quantified, explanation for both: a
general noise-floor sweep likely wasn't fully resolving the 400Hz-family
comb into distinct lines the way the dedicated hi-res FFT did for the AM
work, so some of what read as "floor" before may have actually been this
same artifact, unresolved by the sweep's bin width - removing it would
plausibly drag the aggregate reading down too. The reduced Serial
sensitivity most likely isn't Serial activity doing less; it's that with
the dominant non-commensurate artifact gone, whatever Serial does contribute
now stands out as a smaller, separable effect against a quieter baseline
rather than being compounded with a bigger, messier problem. **Reinforces
the "retire 700/1900Hz as the default reference pair" recommendation above
even more strongly** - that legacy pair was very likely conflating several
genuinely separate issues (this sampling-grid artifact, the Serial effect,
and any real analog/PSU content) into one confusing picture, and testing on
an Fs-commensurate pair going forward should make each of them much easier
to isolate individually.

**Process lesson, worth keeping for future work:** the evidence for this
was sitting in the codebase's own comments the whole time -
`ssb_mic_test_commands.md:16` already described the `'T'` command's
200Hz-spaced presets as "clean single-point probes" while separately
flagging the 700/1900Hz pair as different (wider spacing, "kept last for
reference") - a real, already-documented difference in behavior between
test conditions that was never cross-examined for *why*. This project
already has a strong habit of diffing known-good vs. known-bad source
files when something regresses (see the `dac_task_enabled` investigation
earlier this session); the same discipline applies to test conditions
themselves - when one test configuration is reliably cleaner than another,
that consistent difference is a diffable fact worth asking "what's actually
different here?" about, not just a convenient probe to keep using.

## 2026-09-09 (cont'd) - random ~40Hz two-tone frequency jump (open, unresolved)

After retiring 700/1900Hz in favour of 700/1700Hz, a new symptom appeared
that does **not** match either the null-bias mechanism above or anything
previously logged: the TX frequency sits rock-stable for many minutes, then
makes a sudden discrete **jump** (not a drift) of up to ~40Hz, at no
identifiable trigger. Reported as happening "hands off" - no command sent,
no mode change. Occurs with diagnostics muted (`'v'` off), so it isn't
something the Serial-print path itself is causing.

Ruled out: the deterministic null-bias mechanism (`null_bias_investigation.md`)
is, by its own established theory, a *repeatable, coherent* bias tied to the
exact tone-pair/sample-grid alignment - it cannot manifest as a random,
sporadic jump. Two on-demand diagnostic snapshots taken around suspected
jump events did not catch one in progress: `overruns=0`,
`late_ticks_total=0`, and all `freq_dev`/`tx_freq` values tight and stable
throughout both captures. So the mechanism causing the jump remains
uncaught - on-demand snapshots are single points in time and there's no
continuous record to correlate against when a jump is seen on the SDR.

Proposed next steps (not yet actioned):
1. Re-confirm `dac_task_enabled=0` on the actual hardware build in use (the
   earlier `dac_task_enabled` stray-`1` bug is exactly this class of
   symptom-shape - a stray Core-1 I2C task producing intermittent cache
   stalls - so it's worth a direct re-check even though the user already
   fixed their local copy once this session).
2. Switch from manual on-demand snapshots to passive continuous logging
   (Serial output piped to a timestamped file) so a visually-observed jump
   on the SDR can be correlated against the exact diagnostic state at that
   wall-clock moment, rather than hoping a manual snapshot lands on it.
3. Add a new high-water-mark diagnostic, `max_freq_dev_step_hz` (same
   pattern as the existing `max_prep_us`/`max_busy_us`/`max_unclamped
   freq_dev`) that tracks the largest sample-to-sample change in `freq_dev`
   seen since last reset - this would catch the jump automatically without
   needing to have a snapshot land on the exact tick, and would also
   distinguish a genuine single-tick discontinuity from a fast-but-smooth
   drift. Not yet implemented - offered, awaiting go-ahead since it's a
   firmware change.

New `null_bias2` data point for 700/1700Hz, logged for the record: two
separate captures gave `weighted_bias=-15.99Hz` and, later,
`weighted_bias=-21.07Hz` to `-21.76Hz` for what should be the same tone
pair and mechanism. That spread (-16 to -22Hz) is itself unreconciled - per
`null_bias_investigation.md`'s own theory this number should be
deterministic and repeatable for a fixed tone pair, so either (a) there's
sample-grid-alignment sensitivity in the weighted-mean estimator that
wasn't accounted for in the original characterization, or (b) something
external (like the AGC finding above) is perturbing the underlying
envelope-null statistics between runs. Not investigated further yet. Note
also there is no clean numerological significance to "16-22Hz" the way
there was for the 400Hz family - this is a continuous function of the
Hilbert FIR's response at the specific 700/1700Hz frequencies, not an
integer-arithmetic artifact, so a near-match to "16 samples/cycle" is very
likely coincidental.

## 2026-09-09 (cont'd) - ADC FIFO "anomaly" in the ~465s two-tone capture: explained, benign

The large diagnostic dump taken during the reported "instability
mode...drifting about with high noise level" episode contained a line that
looked seriously wrong on first read:

```
[adc]   fifo: available now min=4294967295 max=0 (want>=5,<64) starve_ticks_total=0 drop_total=37389248
```

(`drop_total` climbing further to `37492384` later in the same capture).
`4294967295` is exactly `0xFFFFFFFFu` and `0` is exactly `0` - both are the
debug counters' own untouched *initial/reset* values
(`adc_capture.cpp`: `s_dbg_adc_fifo_min_available = 0xFFFFFFFFu`,
`s_dbg_adc_fifo_max_available = 0`), and `adc_capture.h:112`'s own comment
documents the expected normal reading as "min=0...starve_ticks_total in the
tens of thousands" - the opposite of what this capture showed. A
`drop_total` in the tens of millions over ~465s is roughly the same order
of magnitude as the total sample count the ADC would produce in that time
(80000 sps x 465s = 37.2M) - i.e. it looked like the ADC FIFO was
overflowing on essentially *every* sample, continuously, for the whole run.

Traced to source and now fully explained, and it is **benign, not a bug**:

- `ssb_mic_test.ino`'s per-tick dispatch (~line 537-568) only calls
  `adc_capture_read_next_sample()` - the function that updates the
  min/max/starve stats - in the `else` branch that's reached when
  `audio_source_t` is none of `AUDIO_SRC_TWOTONE` /
  `AUDIO_SRC_SINGLETONE` / `AUDIO_SRC_ENVSTEP` / `AUDIO_SRC_FMTEST` /
  `AUDIO_SRC_AMTEST` - i.e. only in real mic mode. In `TWOTONE` mode the
  sample comes from `generate_twotone_sample()` instead, and the ADC
  reader is never called at all.
- But `adc_continuous_start()` (`adc_capture.cpp:218`) is called once,
  unconditionally, in `setup()`, and is never stopped or gated on the
  selected audio source - the ADC hardware/DMA/ISR keeps running and
  filling the FIFO from the live mic input the entire time, regardless of
  test mode.
- Put those two together: in two-tone mode the ISR fills the FIFO
  continuously from real hardware, nothing ever drains the tail side, so
  the FIFO fills once and then every subsequent ISR batch overflows -
  incrementing `drop_total` essentially every sample, forever. Since the
  *reading* function (the only place `min`/`max` get touched) is never
  called in this mode, those two stats simply stay frozen at their
  compile-time sentinels the whole run. Every part of the "anomaly"
  is explained by this one fact.

Cross-checked against `[adc] actual=80645-80795 sps (expected=80000)` and
`pool_ovf_total=0` elsewhere in the same capture: those numbers describe
the ADC/DMA driver's own health independent of the FIFO-reader stats, and
they're fine - consistent with "the hardware ADC pipeline itself is
running fine, just unconsumed" rather than any driver-level fault.

**Implication for the "instability/drifting/high noise" report:** this FIFO
behavior is a side effect of leaving the ADC free-running and unused during
two-tone test mode - it does not touch the two-tone signal path at all
(that path is 100% synthetic, generated in `dsp_task` from the phase
accumulators, never touches the ADC FIFO). So it cannot be the cause of the
reported instability, and is very likely present, identically, in *every*
two-tone-mode capture taken so far - it just hadn't been read closely
before. It also means this diagnostic line is not currently useful evidence
in either direction for the two-tone jump investigation above; if it's
worth cleaning up cosmetically (skip the ADC continuous driver, or skip
printing/updating this line, when not in mic mode) that's a small, safe,
optional firmware tidy-up rather than something diagnostic.

**Open discrepancy still not resolved:** the actual `freq_dev`/`tx_freq`
snapshot values recorded throughout this entire ~465s capture are tightly
clustered and stable (`freq_dev` roughly 1198-1202Hz, `tx_freq` locked to
within a few Hz) - i.e. the capture itself does not show the drifting/noisy
behavior that was reported as happening during it. Two possibilities: the
instability happened outside this capture's time window, or it happened
between on-demand snapshots and the spot-sampling simply didn't land on it
(same blind spot as the 40Hz-jump investigation above - reinforces the case
for switching to continuous passive logging rather than manual snapshots
for chasing intermittent events going forward).

## 2026-09-09 (cont'd) - firmware change: null_bias lines now exempt from mute

User's proposal for chasing the random 40Hz jump: force the null-bias
detector's output to always print, so it can be watched live and checked
for a coincident anomaly the moment a jump is seen on the SDR - without
re-enabling the full diagnostic stream and its known ~20Hz broadband noise
side effect (see the "'v' toggle / serial noise" findings earlier in this
log).

Implemented in `diagnostics.cpp`:
- Pulled the `null_bias`/`null_bias2`/`null_bias3` print block out of
  `print_timing_and_adc_block()` into its own function,
  `print_null_bias_block()`.
- `diagnostics_service()` now calls it on its own independent 1000ms timer,
  deliberately **not** gated by `s_diag_muted` - so it keeps printing once
  a second even with `'v'` off, while the 45ms fast line and the rest of
  the 1000ms block stay muted as before.
- `diagnostics_print_now()` (the on-demand snapshot command) updated to
  call it explicitly too, since it no longer comes along for free via
  `print_timing_and_adc_block()`.
- Left `diag_room_for()`'s per-line TX-buffer-safety guard in place on all
  three lines - only the mute gate was removed, not the buffer-overrun
  protection.

Rationale for expecting this to be safe: the ~20Hz broadband noise
mechanism was pinned down earlier to the 45ms fast status line (1/0.045 Hz
= 22.2Hz, the observed spacing) - a continuous, high-rate burst of Serial
traffic. The null_bias block is three short printf calls once per second;
that's a tiny fraction of the USB-CDC bandwidth the fast line stresses, so
it should not reproduce that artifact. **Not yet bench-verified** - worth
a quick spectral check with `'v'` off and just this line streaming, to
confirm no new sidebands appear, before relying on it for the jump hunt.

How to use it for the jump investigation: run two-tone with `'v'` off as
usual (avoiding the fast-line noise), watch the `null_bias2 weighted_bias`
line scroll by once a second, and note whether it makes a discrete,
persistent step at the same moment a jump is seen on the SDR. Since these
stats are cumulative sums since the last `'r'` reset, a single
badly-behaved near-null sample (e.g. a phase-unwrap edge case) could in
principle permanently shift the running weighted average even though it
only affected one 62.5us tick - so a coincident step in this number would
be meaningful evidence tying the jump to the null/Hilbert-phase mechanism,
whereas no change at all would point away from it and toward something
else (SPI write glitch, supply transient, cache stall, etc.).

**RESULT (2026-09-09, same day): tried on the bench - negative for this
hypothesis, but a real, useful finding in its own right.** User watched
`weighted_bias` continuously (after a couple of `'r'` resets and a preset
load) while comparing against the SDR: "No relation between output and
actual freq" - the real transmitted frequency stayed stable while
`weighted_bias` swung by tens of Hz on its own (e.g. -34Hz shortly after a
reset, drifting back toward ~0, then off again past -19Hz, all with no
external event). So `weighted_bias` is **not usable as a live jump
detector** - it doesn't track the real TX frequency at all.

Root cause of the swinging, now understood and logged in detail in
`null_bias_investigation.md`'s new 2026-09-09 update: `weighted_bias` is a
cumulative average since the last `'r'` reset that converges MUCH more
slowly than assumed - tens of seconds to minutes, not the "reset, wait
1-2s, read" recipe the original characterization used - and it responds to
preset changes (gdeq/ampeq/predistort settings shape the envelope through
each null, which shapes where the average is heading), not to the actual
carrier frequency. Bonus payoff: this also explains a previously-unresolved
puzzle from 2026-08-31 (`null_bias_investigation.md` item 4) about a ~15Hz
drift confounding an `'I'` on/off comparison "on the timescale of typing a
sentence" - almost certainly this same slow-convergence behavior, not a
separate mechanism. Practical fallout: the null-bias investigation's
"Confirmed measurement table" may need re-taking with a longer, fixed dwell
time before trusting those weighted_bias numbers as precise per-tone-pair
constants.

Net effect on the jump hunt: back to the two remaining proposed approaches
(re-confirm `dac_task_enabled`, and/or continuous passive Serial logging
correlated against wall-clock SDR observation, and/or the proposed
`max_freq_dev_step_hz` high-water-mark diagnostic) - those instrument the
real freq_dev/tx_freq signal directly rather than a derived slow statistic,
so they remain the more promising paths forward. Still awaiting user
direction on which to pursue.

## 2026-09-09 (cont'd) - leading theory for the random 40Hz jump: a corrupted persistent value, not a DSP/timing effect

`dac_task_enabled` re-confirmed as `0` on the actual hardware - the earlier
known stray-I2C-task bug is ruled out as the cause of this symptom.

Traced the entire `freq_dev_hz` -> TX-frequency pipeline end to end
specifically looking for anything that could hold a WRONG value rather than
just glitch for one tick, since "sits stable for minutes, then a discrete
step that locks and doesn't drift back" is not what a transient glitch
looks like - a transient should self-correct on the very next 62.5us tick.
Findings, all confirmed by reading the actual source:

- `ssb_dsp.c`: `dphi` is computed fresh every tick from that tick's own
  `atan2` phase difference. `prev_phase` stores the raw, wrapped `atan2`
  output (not a running unwrapped total), so one bad sample cannot compound
  into subsequent ticks - each tick's `freq_dev_hz` is fully independent of
  the last.
- `relative_delay.cpp`: the ring buffer is only `PHASE_DELAY_MAX_SAMPLES=8`
  deep and every slot is overwritten every single tick - nothing here can
  persist more than ~500us, and even a corrupted `s_relative_delay_samples`
  would only mistime which recent sample is read, not shift the carrier's
  absolute frequency.
- `AD9851.c`'s `ad9851_set_frequency()`: resends the FULL 40-bit frame -
  FTW bytes AND the control byte (6x-multiplier/power-down/phase bits) -
  freshly computed from the handle's own fields on every single call. A
  one-off SPI/EMI glitch on the wire should be corrected by the very next
  write, 62.5us later.

That leaves exactly two values anywhere in this pipeline that are written
ONCE at boot and never touched again by any command, preset, or reset:
`s_carrier_hz` (`carrier_output.cpp`, the calibrated 14200160Hz base) and
`handle->ftw_reciprocal` (a plain, non-`volatile` `uint64_t` inside the
AD9851 driver struct, computed once in `ad9851_init()`). **Leading theory:**
a one-off corruption of one of these two values would produce exactly the
observed signature - an instantaneous step, with nothing in the normal code
path ever re-deriving or refreshing either value, so it stays wrong
("locks") until another such event nudges it again, in either direction
("stepped -28 to +28... currently spot on correct" reads as two independent
glitches that happened to roughly cancel, not one thing self-correcting).

Two flavors of root cause under this theory, worth distinguishing:
1. **A genuine host-side memory-safety bug elsewhere in the firmware** (a
   stray pointer, an unbounded array write, a stack overrun) scribbling
   over that exact RAM location. If so, `s_carrier_hz`/`ftw_reciprocal`
   are just the most VISIBLE victims (a frequency shift shows up
   immediately on an SDR) - other persistent state elsewhere (gdeq/ampeq
   filter coefficients, envelope calibration constants, master gain) could
   in principle be getting silently corrupted the same way without any
   equally obvious symptom to notice it by.
2. **A hardware-level single-event upset** - an SRAM bit flip from ESD or
   RF pickup. Worth taking seriously specifically because this is a
   transmitter bench: the project's OWN RF PA/antenna field sitting right
   next to the ESP32's digital section is a very plausible coupling path,
   distinct from ordinary cosmic-ray-type soft errors.

**Open questions this raises, if the theory holds up** (per user's own
observation - logging for when this is picked back up):
- Self-inflicted RFI vs. external/cosmic single-event upset vs. a genuine
  firmware memory-safety bug - each points to a completely different fix
  (shielding/grounding/decoupling vs. nothing actionable vs. an audit for
  unbounded writes elsewhere in the codebase).
- If self-RFI: does jump frequency correlate with RF drive level, antenna
  proximity, or SWR? A low-power vs. full-power A/B would test this
  directly.
- Does the ESP32-S3 have any SRAM ECC/parity protection on the region these
  variables live in? If it does and a flip still gets through, that leans
  away from a simple cosmic-ray-style soft error and toward RFI or a real
  firmware bug (ECC would normally catch/correct single-bit soft errors but
  not a bug that legitimately writes to the wrong address).
- Is corruption confined to just these two variables, or could other
  silent, harder-to-notice persistent state (filter coefficients, envelope
  calibration) be affected too, just without an equally obvious symptom?
- Practical mitigation independent of root cause: could the firmware
  periodically re-assert the known-good value of `s_carrier_hz`/
  `ftw_reciprocal` (e.g. once a second) as a cheap self-healing measure,
  separate from ever finding the true root cause?

**Implemented (2026-09-09):** the canary diagnostic, across `AD9851.c`/`.h`,
`carrier_output.cpp`/`.h`, and `diagnostics.cpp`:

- `AD9851.c`/`.h`: added `ftw_reciprocal_known_good`, an independent shadow
  copy of `ftw_reciprocal` taken once in `ad9851_init()` right after the
  real value is computed, stored in a different struct field/RAM address.
  New `ad9851_get_canary()` returns both values for comparison. A
  mismatch between them is strong evidence of exactly the corruption this
  theory predicts (not airtight - a corruption event could in principle
  hit both fields at once - but a single-address bit flip or stray write
  hitting two different fields simultaneously is far less likely).
- `carrier_output.cpp`/`.h`: new `carrier_output_get_carrier_hz()` returns
  the live `s_carrier_hz`, checked against the compile-time `CARRIER_HZ`
  constant itself - immune to RAM corruption, since it's baked into the
  comparison code rather than sitting in a second variable. New
  `carrier_output_get_canary()` forwards to the AD9851 driver's canary
  check.
- `diagnostics.cpp`: new `print_canary_block()` prints two lines,
  `[canary] carrier_hz=... (boot=...) OK` and `[canary]
  ftw_reciprocal=0x... (boot=0x...) OK` (or `MISMATCH! first seen at
  t=...ms` if either check fails - a latched high-water-mark timestamp,
  same pattern as `max_busy_us` etc., that survives even if a later read
  happens to match again). Called from the SAME ungated (mute-exempt)
  1000ms timer as `print_null_bias_block()`, so it keeps checking even
  while running muted for the jump hunt, and also added to the on-demand
  snapshot (`diagnostics_print_now()`). Deliberately EXCLUDED from
  `diagnostics_reset()` (the `'r'` command) - a canary meant to catch a
  rare, possibly once-per-session event must not get silently cleared
  every time someone starts a fresh measurement window; it only clears on
  reboot.

Files delivered: `AD9851.c`, `AD9851.h`, `carrier_output.cpp`,
`carrier_output.h`, `diagnostics.cpp`. Next jump (if it recurs) should show
up directly as a `MISMATCH` line with a timestamp, confirming or refuting
the theory outright rather than by inference.

## 2026-09-09 (cont'd) - two new data points against the theory, both logged for when this resumes

1. **User has only ever seen this jump in TWO-TONE mode (not confirmed -
   just never noticed it elsewhere).** This is a real constraint on the
   theory if it holds up: a corrupted persistent value (stray firmware
   write or a physical RAM bit-flip) shouldn't care what audio source mode
   is active - it's mode-agnostic by nature. The one thing genuinely
   unique to two-tone versus mic/single-tone/AM-test is that its envelope
   hits an exact, repeated zero every cycle - real voice essentially never
   does, and the other test modes don't either. Went looking specifically
   at code that behaves differently right at an envelope null:
   - `test_signals.cpp`'s two-tone generator - no arrays/indexing at all,
     just two scalar phase accumulators. Clean.
   - `ssb_dsp.c`'s `fast_atan2`/`fast_sqrt` - `fast_atan2` explicitly
     guards `x==0 && y==0`, `fast_sqrt` explicitly guards `x<=0`. Both
     handle the degenerate null case safely, no undefined behavior found.
   - `envelope_predistort.cpp`'s LUT lookup - envelope is clamped to
     [0,1] before the index is computed, index is always 0..64 with an
     explicit early-return at the top bound. Bounds-safe, and read-only
     regardless.
   All three obvious null-adjacent suspects came back clean - doesn't rule
   out the "two-tone-specific" angle (something subtler in the Hilbert FIR
   or gdeq/ampeq filter state under near-zero envelope could still be it),
   but also doesn't confirm it. Equally plausible mundane explanation:
   two-tone is simply the mode run continuously for the longest unattended
   stretches while testing, so it's had more opportunities to show up,
   independent of any real mode-specific mechanism. Worth checking next
   time: was mic/single-tone ever run for a comparably long stretch without
   incident, or just never run that long?
2. **No jump seen on this run since the canary code was added.** Worth
   treating cautiously for now - the jump has historically been rare and
   unpredictable (stable for many minutes to longer, then one discrete
   event), so a short quiet stretch isn't strong evidence either way. BUT
   if a much longer soak stays quiet, that's actually a more specific clue
   than "problem fixed": adding a few new static variables/functions shifts
   where everything else lands in RAM at link time. A true external event
   (ESD, RF pickup, a cosmic-ray-style soft error) wouldn't care about the
   linker map and would still eventually corrupt SOMETHING, just maybe not
   a variable that produces a visible symptom next time. A bug that's
   sensitive to memory layout - a stray write landing wherever something
   happens to sit at a fixed offset - is the classic signature of a real
   buffer overrun or wild pointer elsewhere in the firmware, not a hardware
   event. So a long quiet stretch would paradoxically lean TOWARD "real
   firmware bug, now coincidentally not landing on s_carrier_hz/
   ftw_reciprocal" rather than "problem solved" - worth remembering not to
   declare victory even if this stays quiet for a while; the canary staying
   green is reassuring but not proof nothing is still happening elsewhere
   in memory.

**Follow-up same day:** user recalls switching between modes MIGHT have
been associated with the big jumps previously (not certain). Checked the
obvious candidates in the switching/preset code path itself for a
mode-switch-triggered bug:
- `serial_commands.cpp`'s digit-preset handler: `preset = c - '0'` for
  `'0'-'9'` indexes `settingsPresets[10]`, which has its own
  `static_assert(sizeof(settingsPresets)/sizeof(settingsPresets[0]) == 10,
  ...)` in `settings.h` - exactly matches the 10 possible digits, no
  off-by-one possible.
- `test_signals.cpp`'s `'T'` two-tone band cycling
  (`s_band_index = (s_band_index + 1) % TWOTONE_BAND_COUNT`) and `'R'`
  tone-ratio cycling (`s_tone_ratio_index = (s_tone_ratio_index + 1) %
  TONE_RATIO_COUNT`) both use proper modulo wraparound - never run past
  their array bounds either.

All clean - no smoking gun in the switching path, consistent with
everything else checked so far this session. Now also confirmed: since the
canary code was added, small (few-Hz) nudges on preset switch are
observed, but NO big/locked jumps - the few-Hz nudges are almost certainly
the SAME benign null-bias-mechanism response characterized earlier today
(different presets carry different relative_delay/gdeq/ampeq settings,
which legitimately reshape the near-null envelope trajectory and shift
where the null-crossing resolution lands by a few Hz) - not a red flag,
and worth clearly distinguishing from the big jumps this whole thread is
actually chasing.

Suggested next test, not yet run: deliberately cycle through several mode/
preset switches back-to-back for a few minutes (rather than switching once
and then sitting idle) while watching the canary lines, to actively try to
provoke a big jump rather than waiting passively for one. If that
reproduces it, the switch path becomes the prime suspect after all (worth
a second, closer look at the code above, e.g. for a race between Core 1's
handler and Core 0's dsp_task reading the same settings mid-update); if it
still never reproduces that way, that's evidence the earlier "hands off,
sits stable for minutes" description was the more accurate one, and
switching isn't the trigger.

**Follow-up same day:** user searched a ~2000-line capture (deliberate
preset-abuse testing plus general running) for `MISMATCH` - none found, and
no big jumps observed either. Canary and symptom continue to track each
other exactly as the theory would predict (if the corruption isn't
happening, the canary has nothing to flag) - consistent with, but not yet
confirming, either explanation from the earlier entry (genuinely
quiet/rare, or masked by the memory-layout shift from adding the canary
code itself). Still no positive detection to confirm the theory outright -
the canary has not yet actually caught anything, so it remains untested in
the sense that matters (it would need to fire at least once, coincident
with an observed jump, to move this from "plausible" to "confirmed").
Recommend continuing the same passive monitoring plus the earlier-suggested
mic/single-tone comparable-duration test (see the "only ever in two-tone"
entry above) - the longer this stays clean across a range of modes, the
more it either supports "actually fixed by the layout shift" or starts to
suggest the original trigger condition was rarer/more specific than
thought, rather than confirming the theory either way.

## 2026-09-09 (cont'd) - REGRESSION FOUND AND FIXED: my own mute-exemption change was likely causing a new "noisy" symptom

User reported a new symptom: the board would go into a "noisy" mode (high
close-in background noise) with no jumps, cleared by a reboot, and
described it as "looks like when 'v' is on but it isn't." That description
is a near-exact match for a self-inflicted regression from earlier today's
work: `print_null_bias_block()` and the (at-the-time) `print_canary_block()`
were both deliberately exempted from `s_diag_muted` so they could be
watched live while muted (see the null_bias entries above). That meant
there was now ALWAYS 5 lines/sec of Serial traffic happening regardless of
`'v'`'s actual state - i.e. "muted" no longer actually meant silent. Given
the earlier-established mechanism (continuous Serial diagnostic output ->
~20Hz broadband noise, tied to print rate/volume), this fits the reported
symptom exactly, and explains "looks like v is on but it isn't": part of
the diagnostic stream WAS effectively always unmuted, independent of what
`'v'` displayed.

**Fixed, same day:**
- `print_null_bias_block()`'s exemption reverted - it's back under the
  normal `!s_diag_muted` gate in `diagnostics_service()`, same as
  everything else in that 1000ms block. Its only reason for being exempt
  (watching it live while muted, to catch a real jump) is gone anyway -
  that experiment already ran and showed weighted_bias doesn't track the
  real jump at all (see the earlier RESULT entry above). Still reachable
  on demand via `diagnostics_print_now()` regardless of mute state.
- The canary was redesigned rather than just re-gated, since it's still
  genuinely useful to run while muted. Split into two functions:
  `canary_check_background()` (called unconditionally, every
  `diagnostics_service()` call, still outside the mute gate) only prints
  `[canary] ... MISMATCH! first seen at t=...ms` the FIRST time either
  check transitions from OK to bad - it is now a complete no-op (no Serial
  traffic at all) during normal, healthy operation, so it can stay
  mute-exempt without contributing any background noise. Checking every
  call instead of once a second is a free improvement while at it - lower
  latency to catching a real event, since there's no cost when nothing's
  wrong. `canary_print_status()` is the explicit "show current OK/MISMATCH
  state" version, called only from the on-demand snapshot
  (`diagnostics_print_now()`), where printing unconditionally is fine
  since it's a one-off requested read, not a background stream.

Net effect: `'v'` muted should now mean genuinely, completely silent again
(matching its original pre-2026-09-09 behavior), while the canary keeps
watching in the background for free and will still report a corruption
event immediately if one ever occurs. Files changed: `diagnostics.cpp`
only. **Not yet bench-verified that this actually fixes the reported
noisy-mode symptom** - next occurrence (or lack thereof) is the real test;
worth specifically trying to reproduce the noisy mode again now that the
mute-exemption regression is removed, to confirm this was really the
cause rather than a coincidence.

## 2026-09-10 - new jump occurrence: smaller (±8Hz), sideband-like, canary silent

User reports a new jump event this morning: TX frequency stepping ±8Hz
(smaller than the earlier ~28-40Hz events) and, distinctively, producing
audible/visible sidebands at an ~8Hz offset rather than (or in addition to)
a clean single-direction relocation. **No `[canary] ... MISMATCH` reported
at the time** - this is the most important new fact, since the 2026-09-09
canary was built specifically to catch corruption of `s_carrier_hz` or
`ftw_reciprocal`, the two leading suspects from that investigation. If this
event is confirmed to have happened with the canary genuinely silent
throughout, that's evidence against those exact two variables for THIS
occurrence - either this is a different/smaller mechanism than the original
40Hz jumps, corruption is hitting some other persistent value the canary
doesn't cover, or (see below) this isn't digital-state corruption at all.
**Not yet confirmed the user specifically checked the log for a MISMATCH
line at the jump timestamp** rather than just noting the absence of any
gross error/crash - worth nailing down before drawing conclusions.

Also reported: the two recovered audio tones shift by the same number of Hz
each event, which the user reads as ruling out a "scaling factor." Worth
flagging that this doesn't actually discriminate between the live theories
at these frequencies - both tones sit within ~1-2kHz of each other on a
14.2MHz carrier, so even a genuine proportional (ppm-level) error - whether
from `ftw_reciprocal` corruption or real REF_CLK frequency instability -
would produce two absolute Hz shifts too close to distinguish from an
additive shift given typical receiver/counter resolution. The equal-shift
observation is real and worth recording, but it doesn't yet separate
"something scaled the whole 14.2xxx MHz number" from "something added a
constant to it."

Separately, user re-confirmed the existing `null_bias`/`weighted_bias`
diagnostic ("reported freq bias") does not track this symptom either - it
sat reporting ~10Hz while the actual transmitted frequency (checked against
the receiver) was spot-on nominal. This matches the null-bias
investigation's own prior finding (`null_bias_investigation.md`,
2026-09-09 update: "this metric's wandering does NOT track the actual
transmitted frequency") - not new evidence about the jump mechanism, just a
second confirmation that `weighted_bias` is the wrong tool for chasing this
specific symptom and should be disregarded when characterizing it.

**New candidate mechanism worth taking seriously given today's specific
signature (small magnitude, sideband-like, canary silent, occurred "this
morning" specifically):** genuine REF_CLK reference-oscillator instability
rather than digital-state corruption - e.g. thermal drift/warm-up wobble in
the uncompensated XO (`carrier_output.h`'s own `CARRIER_HZ` comment already
establishes this crystal is "not a precision reference"), supply-rail
ripple reaching the oscillator, or old-fashioned crystal microphonics
(mechanical vibration on the bench modulating the XO). Any of these would
produce real, physical FM sidebands on the transmitted carrier - matching
"generates 8Hz sidebands" more literally than a discrete digital step would
- and would leave every digital value (canary included) completely
correct, since nothing in RAM actually changed; the reference frequency
itself just wavered. This is a different failure class from the
2026-09-09 "corrupted persistent value" theory and would need a different
kind of evidence to confirm (e.g. correlating jump timing with time-since-
power-on/thermal state, touching/tapping the board or XO can while
monitoring, or scoping the REF_CLK line itself if feasible) rather than
more firmware auditing.

**Same session, second occurrence, minutes later: -25Hz jump, canary
confirmed clean.** User explicitly checked this time - no `[canary]
MISMATCH` around either event, and `weighted_bias` again reported an
unrelated, stable-ish number (14Hz) while the real jump was -25Hz. Two
canary-clean events in one sitting, one -8Hz-ish and one -25Hz, is
meaningfully stronger than the single quiet stretch logged on 2026-09-09 -
that entry only had absence of jumps to go on; this is jumps actively
happening WITH the corruption detector watching and finding nothing.
Shifts weight further away from `s_carrier_hz`/`ftw_reciprocal` corruption
as the mechanism (at least as those two specific variables) and toward
either (a) corruption of some other, not-yet-instrumented persistent value,
or (b) the REF_CLK/analog-instability theory above, or (c) a digital
glitch that never lands in a persistent variable at all - e.g. a one-tick
corruption of `tx_freq` between computation and the SPI write, which the
canary (checking `s_carrier_hz`/`ftw_reciprocal` themselves) would never
see even if it happened every single time.

**Open questions / next steps, not yet actioned:**
1. Whether these jumps LOCK (persist until the next event, matching the
   original 2026-09-09 "40Hz jump" behavior) or self-correct on their own
   shortly after - not yet stated either way this session. This is the
   single most useful missing fact: a self-correcting wobble points at
   REF_CLK/analog or a transient one-tick glitch; a locked step that stays
   put points back at persistent-state corruption, just not the two
   variables currently instrumented.
2. The previously-proposed `max_freq_dev_step_hz` high-water-mark
   diagnostic (tick-to-tick delta on `delayed_freq_dev_hz`/`tx_freq`,
   flagged in the 2026-09-09 entries as "awaiting user direction") would
   directly settle whether an event this size ever shows up upstream, in
   the digital signal itself, before the SPI write - if it never does even
   while a real jump is observed at RF, that's strong evidence the fault is
   downstream of the digital math entirely (REF_CLK/analog/hardware, or
   something in the AD9851 SPI transfer itself). Worth implementing now
   given jumps are actively reproducing this session.
3. If genuinely not locking and not upstream, look for a thermal/
   mechanical/supply correlation ("this morning," cold bench) rather than
   continuing the memory-corruption audit.
4. Given the smaller, varying magnitude (-8ish, then -25) and different
   character (sidebands) versus the original ~28-40Hz jumps, consider
   explicitly whether this is the SAME symptom recurring or a second,
   distinct issue - don't merge the two without more data.
5. Reconfirm `weighted_bias`/`null_bias` stays excluded from this
   investigation's evidence base - two more data points (10Hz-reported/
   spot-on-actual, then 14Hz-reported/-25Hz-actual) both reconfirm it does
   not track this symptom.

**Same session, third occurrence: -25Hz -> +25Hz -> back to 0, over ~30
seconds, hands-off, low ambient noise floor.** This is a materially
different shape than anything logged before - not a step that locks
(the 2026-09-09 original description) and not even a single discrete jump,
but a smooth-ish symmetric excursion and return with zero user
interaction. This is very hard to explain with a one-off RAM/persistent-
value corruption model (would need two independent, oppositely-signed
corruption events, ~15s apart, of coincidentally similar magnitude, that
happen to land back on the original value - implausible as coincidence).
It fits a genuine physical/analog wander far more naturally - REF_CLK
thermal drift or warm-up creep, supply-rail-induced pulling, or crystal
microphonics - all of which would plausibly swing and relax back over a
timescale of seconds, especially on a low-cost, uncompensated XO
(`carrier_output.h`'s own `CARRIER_HZ` comment already establishes this
part is not a precision reference) sitting right next to this project's
own RF PA. Noted but not yet explained: why a 30-second period specifically
- worth watching whether repeats land near the same duration (would
suggest a specific thermal/electrical time constant somewhere) or vary
widely (would argue against a single clean mechanism).

**`max_freq_dev_step_hz` diagnostic implemented (2026-09-10),
`diagnostics.cpp` only** - direct answer to open item 2 above, and the
user independently arrived at wanting this at the same time. Tracks the
largest tick-to-tick delta in `tx_freq` (the exact ground-truth Hz value
handed to `ad9851_set_frequency()` every 62.5us tick, per
`carrier_output.h`'s own doc comment) since the last reset:
- New statics next to `s_dbg_tx_freq`: `s_dbg_prev_tx_freq`/
  `s_dbg_have_prev_tx_freq` (comparison state), `s_dbg_max_freq_dev_step_hz`
  (the high-water mark itself), `s_dbg_max_freq_dev_step_from_hz`/`_to_hz`
  (the two consecutive tx_freq values straddling the worst step, so the
  actual before/after Hz is visible, not just the delta), and
  `s_dbg_max_freq_dev_step_at_ms` (latched timestamp, same since-boot clock
  family as the `[canary]` timestamps, for cross-checking against wall-
  clock SDR observation).
- `diagnostics_set_tx_info()` (the existing per-tick hot-path hook, already
  IRAM_ATTR, already receiving `tx_freq` as its "ground truth" parameter -
  no new call site needed anywhere) now also computes `abs(tx_freq -
  prev_tx_freq)` and updates the high-water mark. Uses
  `esp_timer_get_time()`, not `millis()`, since this runs on the dsp_task
  hot path.
- New line in `print_timing_and_adc_block()`, right after the existing
  ad9851-breakdown line, inside the same `#if AD9851_ATTACHED` guard, same
  `diag_room_for()` per-line gating as everything else in this file:
  `[dsp]   max_freq_dev_step: <N>Hz (<from> -> <to> Hz, at t=<ms>ms)`.
  Automatically included in the on-demand snapshot (`diagnostics_print_now()`)
  too, since that already calls `print_timing_and_adc_block()`.
- **Resets with `diagnostics_reset()` ('r')**, unlike the canary - a
  deliberate difference: the canary must survive resets because it's
  chasing an assumed-extremely-rare, possibly-once event, but this stat is
  meant to be zeroed right before a monitoring stretch and read after,
  matching the "r, wait, read" pattern every other test in this
  investigation already uses. `s_dbg_have_prev_tx_freq` resets to `false`
  (not just zeroing `s_dbg_prev_tx_freq`) so the tick right after a reset
  never compares against a stale pre-reset value - same reasoning already
  used for `s_last_samples_total`/`s_last_callback_count` in this file.

**What this is for:** settles whether an event of this size ever appears in
the digital signal itself, before the SPI write. If the next real jump (or
the next slow excursion, given the -25/+25/0 shape above) shows 0 or
near-0 here while the SDR clearly shows the frequency moving, that's fairly
decisive evidence the fault is downstream of every bit of this firmware's
math - REF_CLK, the AD9851's internal 6x-multiplier PLL, or the SPI
transfer itself - and further firmware auditing of the freq_dev/delay-line/
carrier-addition path would not be the productive next step. If it DOES
show a matching step, that points back at something in this digital chain
after all, just not `s_carrier_hz`/`ftw_reciprocal` (already ruled out by
the canary staying clean across all three occurrences logged today) - a
`tx_freq` corruption between computation and the SPI write, or an
`ad9851_set_frequency()`/SPI-level fault, would become the next things to
look at specifically. Not yet bench-verified - awaiting the next jump.

## 2026-09-10 (cont'd) - USB supply theory sharpened: two-tone's active FTW churn is a plausible reason the jump/wobble is mode-specific, plus why "too-clean" step transitions don't argue against it

Follow-on reasoning session (no new bench data yet) tying the still-open
"only ever seen in two-tone" observation (2026-09-09 entry above) together
with the REF_CLK/analog-instability theory from the 2026-09-10 entries,
plus a specific plan for the next bench session.

**Mechanism proposed:** USB bus power is a plausible noise source for
exactly the failure signature seen so far - switching noise/ripple/ground
bounce on the 5V rail, which (if it reaches the AD9851's REF_CLK oscillator
or its internal 6x-multiplier PLL bias, even indirectly through shared
supply/ground) would perturb the transmitted frequency while leaving every
digital value this firmware computes untouched. That matches everything
logged so far: canary clean, `tx_freq` itself rock-stable in the diagnostic
stream, `weighted_bias` uncorrelated - because the fault sits after
everything the firmware computes, same conclusion the `max_freq_dev_step_hz`
diagnostic (2026-09-10, above) was built to test for directly.

**New piece: why two-tone specifically, tying back to the still-open
"only ever in two-tone" item.** In the sine/CW and AM/PWM-envelope test
modes, `freq_dev_hz` sits at (or very near) zero, so the FTW value sent to
the AD9851 over SPI is nearly identical tick to tick - low bit-toggling
activity on that bus. In two-tone mode, `freq_dev_hz` is actively swinging
every tick (the whole basis of the phase-modulation two-tone synthesis), so
the FTW - and therefore the SPI/bit-bang switching activity on `carrier_output.cpp`'s
write path - changes almost every tick. More bit-toggling means more
instantaneous current draw on the same 5V rail feeding the AD9851. If USB
power is marginal (higher source impedance, less transient headroom than a
bench supply), two-tone mode is uniquely positioned to provoke this kind of
supply-induced REF_CLK/PLL disturbance, while sine/AM's near-static FTW
writes wouldn't stress the rail the same way. This is a second variable
alongside the supply swap itself, not a competing theory - worth watching
during the test below.

**Objection considered and resolved: the discrete step-like transitions
(e.g. -46 -> +46 -> 0) seemed "too exact" to be ordinary analog noise.**
This doesn't argue against the hardware theory. A marginally-locked PLL
(the AD9851's 6x multiplier working off a noisy/borderline reference)
doesn't have to drift smoothly - it can snap between a small number of
discrete quasi-stable lock states as the loop re-acquires, producing clean,
repeatable steps rather than continuous wander, even though the root cause
is still analog/supply-side. A crisp step is consistent with "PLL hunting
under a noisy reference," not just with "something in firmware changed by
exactly one value" - so it does not discriminate against this theory the
way it first appears to.

**Planned test (not yet run):** swap the board to a clean external supply
(off USB bus power) and re-run the same 700/1700Hz two-tone setup
(2026-09-09's Fs-commensurate pair) for an extended, hands-off stretch -
ideally past whatever timescale produced jumps/excursions before (the
30-second excursion and the multi-minute-stable-then-jump pattern both
logged above). Watch both the receiver/SDR and this project's own
diagnostics (`max_freq_dev_step_hz`, the `[canary]` lines) concurrently.
Interpretation:
- If jumps/wobbles stop entirely on clean power: about as close to
  confirmation as this investigation gets without instrumenting REF_CLK
  directly - points the fix at power delivery (external supply, or
  decoupling/filtering on the rail feeding the AD9851/REF_CLK) rather than
  firmware.
- If they still occur on clean power: rules out USB-specific noise
  specifically, narrows the remaining candidates to general supply ripple
  from whatever regulator is used, thermal effects, or a genuine AD9851/PLL
  issue independent of supply quality - and reopens the digital-corruption
  and REF_CLK-instability theories from the 2026-09-09/09-10 entries above
  on more equal footing.

Also worth deliberately noting during this same test whether the
smooth-excursion-and-return shape (item 3, 2026-09-10 above) or the
sudden-locked-step shape (2026-09-09 original) recurs, and whether it
correlates with sine/AM-vs-two-tone mode as the bit-toggling theory above
would predict - both would be useful cross-checks to fold into the next
entry here.

## 2026-09-10 (cont'd) - regulator doesn't fix it; delay-mash reproducer captured, and the data KILLS the Core-1-serial-stall theory, pointing instead at the AD9851 SPI-clock/level-shifter margin

**External 5V regulator result:** reduced the general noise floor, did NOT
stop the frequency-shift/"stick" symptom. This substantially weakens the
USB-power theory (previous entry) as the cause of the jump/stick
specifically - the noise-floor improvement is real and separate, but the
actual symptom this thread is chasing survived a clean supply.

**New reproducer:** rapid repeated relative-delay keypresses (`'['`/`']'`/
`';'`/apostrophe) sometimes make the TX frequency change and then "stick" -
not settle back on its own even once the key-mashing stops. First real,
provokable trigger this whole investigation has had, versus "hands-off,
random, unpredictable."

**Bench test of the Core-1-serial-write-stall theory (previous entry):**
user ran 8 rapid fine-decrease presses (-1.92 -> -2.27 samples) followed by
~30 back-to-back on-demand (`'V'`) snapshots. Result: **theory does not
hold up.** Despite sustained, heavy USB-CDC TX buffer pressure the whole
time (`skip_total` climbed steadily from 27 to 459 across the capture -
`diag_room_for()`'s guard was firing constantly), `overruns` stayed 0 the
entire capture, `late_ticks_total` stayed 0, and wakeup jitter never
exceeded `max_gap_us=71` against a 62us nominal - nowhere near the
multi-ms stall the theory needed to disrupt an in-progress AD9851 write.
Canary (`carrier_hz`/`ftw_reciprocal`) stayed `OK` throughout. **Conclusion:
sustained Core-1 Serial/USB buffer congestion, even fairly severe, does not
measurably disrupt Core-0 dsp_task's real-time scheduling in this system** -
the unguarded-`serial_reply()` theory is not supported by this data and is
deprioritized as an explanation for THIS symptom (the unguarded-buffer-write
issue in `serial_commands.cpp` is still real on its own terms - see below -
just not confirmed as this symptom's cause).

**One genuine digital-signal event was captured:** `[dsp] max_freq_dev_step:
8562Hz (14193164 -> 14201726 Hz, at t=2186828ms)` - a real one-tick swing in
the ground-truth `tx_freq` value, computed before the SPI write even
happens. Cross-checking against the `[adc]` long-window-since-reset counter
(reading 51-54s across this same capture) places this event inside the
observed window, plausibly coincident with the delay-mashing. But every
snapshot immediately before and after shows `freq_dev`/`tx_freq` back at the
normal ~1198-1202Hz / 14201358-362Hz cluster - a single self-correcting
transient, not a locked value. Best-fit explanation: an ordinary (if
unusually large) near-envelope-null atan2/Hilbert transient - matches the
separately-logged `[dsp] freq_dev: max_unclamped=7991Hz (limit=20000Hz)
clip_count=0` reading elsewhere in the same capture, i.e. multi-kHz
transients near nulls aren't apparently rare in this signal, just usually
smaller. Consistent with dsp_task's already-established (2026-09-09)
per-tick-fresh-computation model - predicts exactly this spike-and-recover
shape, not a genuine stick, and doesn't need a corruption theory to explain
it.

**Important structural conclusion from this capture:** since the digital
`tx_freq` value shows no SUSTAINED anomaly anywhere in this window, if an
RF "stick" happened during this same test, the fault has to live downstream
of everything this firmware computes and sends over SPI - i.e. in the
AD9851 SPI/level-shifter/chip-latch chain itself, not in dsp_task's math,
its scheduling, or Core-1 Serial activity.

**This reopens an already-flagged, never-fully-verified risk:**
`carrier_output.cpp`'s own init comment documents bumping `spi_clock_hz`
from 2000000 to 4000000 "conservatively," through discrete BS170 inverting
level-shifter stages "whose real switching speed hasn't been characterized,"
citing `AD9851.h`'s own TIMING note that getting this wrong produces
"silently wrong output with no obvious symptom short of a spectrum
analyser" - and explicitly says to verify the transmitted frequency stays
exactly correct before pushing higher. That verification apparently never
got circled back on. A marginal W_CLK/FQ_UD edge through an uncharacterized
level-shifter stage occasionally failing to fully/reliably latch would be
completely invisible to every diagnostic this firmware can compute
(freq_dev, tx_freq, canary, weighted_bias all correct/clean - exactly what's
been observed across every occurrence logged this entire thread) while
still producing a real, possibly non-self-correcting, RF frequency error.
Fits the accumulated data better than any theory tried so far.

**Recommended next test, cheap and fully reversible:** revert
`carrier_output.cpp`'s `spi_clock_hz` from `4000000` back to `2000000` and
retry the same rapid delay-mash reproducer. If sticking stops or gets much
rarer at the lower, previously-safe clock rate, that's strong confirmation
of an SPI/level-shifter timing-margin cause - fix would be dropping the
clock back down permanently (or characterizing/upgrading the level-shifter
stage if the higher rate is wanted later). If it reproduces identically at
2MHz, that rules this out and points back toward genuine AD9851 PLL
quasi-stable-lock behavior (previous entry's theory) as the more likely
remaining explanation. Not yet run.

## 2026-09-10 (cont'd) - slew limiter confirmed OFF; every firmware-side candidate in the freq/phase chain is now eliminated by direct test - AD9851/PLL hardware hysteresis is the only theory left standing

Quick, decisive check: user confirmed the freq_dev slew-rate limiter
(`'{'`/`'}'`) is at `SSB_DSP_FREQ_DEV_SLEW_UNLIMITED_HZ` - off. Per
`ssb_dsp.c`'s own logic, at that sentinel the clamp branches never fire, so
`slew_limited_prev_freq_dev_hz` just tracks `freq_dev` exactly every tick -
a complete no-op. **The slew-limiter theory (previous entry) is dead.**

That closes out the last remaining candidate in the digital freq/phase
chain. Tallying everything eliminated by direct A/B test this session,
against the same "sticks after two-tone delay-sweeping" symptom:

- USB bus power -> external 5V regulator: noise floor improved, symptom
  unchanged. Weakened.
- SPI clock 2MHz vs 4MHz through the BS170 level-shifters: **no difference
  observed.** Eliminated.
- Generic "continuous FTW churn stresses the AD9851/SPI path" (independent
  of Hilbert/nulls) - tested via FMTEST, which drives a continuously,
  smoothly varying `freq_dev_hz` with zero DSP/Hilbert involvement:
  **no disruption seen on sine or FM.** Eliminated.
- Core-1 Serial-write stall disrupting dsp_task's real-time AD9851 write -
  tested via a rapid-keypress + heavy 'V'-spam capture: overruns/late-ticks/
  wakeup-jitter all stayed clean despite real, sustained USB-CDC buffer
  pressure. Eliminated.
- relative_delay.cpp's own two-ring implementation "getting out of step" -
  re-read specifically for this: both rings share one write index that
  dsp_task alone advances once per tick, and `freq_back`/`env_back` are
  both derived from the same single local `delay` read in the same
  function call - no code path exists for them to desync from each other.
  Eliminated as an implementation bug (though see below for what this
  question actually pointed at).
- freq_dev slew-rate limiter persistent state (this entry): confirmed off,
  therefore inert. Eliminated.

With those gone, **every stage in the digital freq/phase pipeline has now
been confirmed to recompute fresh every tick from its current inputs**,
same conclusion the 2026-09-09 "corrupted persistent value" audit reached
for `prev_phase`/the relative-delay ring/the AD9851 driver's own
full-frame-resend behavior, now extended to cover the slew limiter too (the
one stateful component that audit didn't check, and the one this thread's
"out of step" question productively surfaced - it just turned out to be
switched off). There is no remaining firmware-side mechanism in this chain
that could hold a WRONG value across multiple ticks once slew limiting is
ruled out - a genuine near-null atan2 transient (confirmed real, up to
8562Hz, previous entries) can only ever last one tick before every
downstream stage recomputes cleanly again.

**Conclusion: AD9851/PLL hardware hysteresis is now the only theory left
standing that isn't contradicted by a direct test.** Working picture: a
large near-null FTW step (the null-bias mechanism's known noisy-near-zero
atan2 behavior, amplified/relocated by whichever historical sample the
delay-line interpolation happens to be reading as delay is swept) kicks the
AD9851's internal 6x-multiplier PLL hard enough that it re-acquires to a
nearby but wrong lock point, which a subsequent numerically-correct FTW
write doesn't automatically clear (the PLL's own loop dynamics, not the
digital value sent, would govern whether/how it re-locks) - until another
large kick (further delay changes, which reshuffle where the interpolated
near-null noise lands) happens to knock it back toward the right point, or
doesn't. This fits "usually unsticks, not always" as well as anything
tried so far, and is now the leading explanation essentially by
elimination, not by direct confirmation.

**Decisive test, not yet run:** next time the RF output sticks during a
delay sweep, immediately pull an on-demand snapshot (`'V'`). If `freq_dev`/
`tx_freq` in that snapshot ALSO shows the stuck/wrong value, that resurrects
a firmware explanation somewhere not yet found (and would need
`max_freq_dev_step_hz`/canary cross-checked at the same instant). If
`freq_dev`/`tx_freq` reads back to normal (the expected ~1200Hz-ish cluster
for the loaded delay setting) while the RF output is still audibly/visibly
stuck at the wrong frequency, that's about as close to a direct confirmation
of the AD9851/PLL theory as this investigation can get without instrumenting
the chip's PLL lock/loop-filter node directly. Also worth trying as a
workaround probe: does a hard AD9851 reset (re-running `ad9851_init()`'s
full reset sequence, not just another `ad9851_set_frequency()` write) clear
a stuck state that repeated normal writes don't? If yes, that's strong,
practical confirmation this lives in the chip's internal load/PLL state,
and points toward a periodic self-healing hard-reinit as a pragmatic
mitigation independent of fully explaining the AD9851's internal behavior.

## 2026-09-10 (cont'd) - root cause found and fixed: the bit-bang transport was running unthrottled the whole time, `spi_clock_hz` was dead code under it

Two quick user answers settled this:

1. **The AD9851 has no built-in frequency rate limiter** - it's a pure
   phase-accumulator NCO (output = SYSCLK x FTW / 2^32, recomputed fresh
   from whatever FTW is currently latched), and the chip's only PLL is the
   fixed 6x REFCLK multiplier that generates SYSCLK from the reference
   oscillator - unrelated to the tuning word. The previous entry's "PLL
   settles to a nearby-but-wrong lock point" framing over-extended a
   generic PLL-hysteresis idea onto a chip that doesn't have a closed-loop
   output path at all - retracted.
2. User confirmed measuring the real bit-bang edge rate directly: **~7MHz
   unthrottled, ~4MHz measured as the safe limit** through this board's
   BS170 level shifters.

That sent a re-read of `AD9851.c`, which found the actual bug: **`AD9851_USE_BITBANG`
is `#define`d to 1** - the compiled transport is the raw-GPIO bit-bang
branch, not the hardware-SPI branch. `ad9851_init()`'s bit-bang branch never
read `cfg->spi_clock_hz` at all (only the dormant `#else` hardware-SPI
branch does) - so **the earlier "2MHz vs 4MHz, no difference" A/B test
(previous entries) was an unintentional null test.** Neither setting ever
reached the real signal path. The true edge rate was whatever raw
back-to-back `REG_WRITE()` GPIO toggles produce, with zero delay anywhere
in the loop - matching the user's own ~7MHz scope measurement.

This also gives a cleaner explanation for the two-tone-vs-FMTEST split than
the earlier "continuous churn" framing: it's not whether FTW changes every
tick (true for both), it's how ABRUPTLY. FMTEST sweeps smoothly, so
consecutive writes mostly flip a few low-order bits. Two-tone's near-null
atan2 noise (confirmed real, up to 8562Hz single-tick swings) can flip many
DATA bits at once, including many fresh 0-to-1 transitions - exactly the
demanding case for a BS170 inverting stage, whose LOW-to-HIGH edge is a
passive, pull-up-charged RC transition (much slower than the actively-
driven HIGH-to-LOW edge - see `AD9851_INVERTING_LEVEL_SHIFT`'s comment).

**Fix implemented, all in `AD9851.c`/`AD9851.h`/`carrier_output.cpp`:**
- New `ad9851_edge_delay()` (cycle-accurate busy-wait via
  `esp_cpu_get_cycle_count()` - `esp_rom_delay_us()`'s 1us granularity is
  two orders of magnitude too coarse for this, ~125ns half-period target).
- `cfg->spi_clock_hz` is now actually honored under the bit-bang transport
  too: `ad9851_init()` derives `half_period_cycles` from it
  (`esp_rom_get_cpu_ticks_per_us() * 1e6 / (2 * spi_clock_hz)` - 30 cycles
  / ~125ns at the existing 4000000 setting, which is already the user's
  measured-safe value, so no config number needed changing, just what it
  actually controls).
- `ad9851_set_frequency()`'s bit-bang loop now spends this delay in three
  places per transfer: after each DATA-line change (before W_CLK's
  sampling edge), after W_CLK's own falling call (the AD9851-side
  rising/sampling edge itself, before ending the pulse), and once after
  FQ_UD's final falling call (the actual latch edge).
- Updated `AD9851.h`'s doc comments (`spi_clock_hz` field + the
  `AD9851_USE_BITBANG` transport comment) so this dead-code trap can't
  silently reappear/mislead again.

**Not yet bench-verified, and one real risk flagged for the next test:**
this adds roughly 2 x 125ns x 40 bits = ~10us to the AD9851 write itself
(plus one more ~125ns at the very end) - `write_us` was already observed
around 25-31us in this session's captures, and total `max_busy_us` was
already sitting around 42-48us against the 62.5us tick budget (adc=4,
dsp=22 typical). **Adding ~10us risks pushing dsp_task into overrun** -
check `[timing] max_busy_us`/`overruns` immediately after reflashing. If
it's over budget, the cheapest lever is dropping one of the two per-bit
delay calls (accepting slightly less margin on one edge) or raising the
effective target somewhat (still well under the ~7MHz danger zone) - the
right trade-off needs the scope to confirm signal integrity is still fine
at whatever settles inside budget, which can't be verified from here.

**Next test:** reflash, confirm `overruns=0` still holds, then re-run the
exact two-tone delay-sweep reproducer (1-3 samples, sweep and hold) that
produced sticking before. If sticking stops or gets much rarer, this is
confirmed as the root cause. If it still happens identically, the DATA/
W_CLK/FQ_UD lines need to go on the scope directly to see what the real
edges look like now, since the theory would then need to be revisited.

## 2026-09-10 (cont'd) - CONFIRMED FIXED on real hardware, then trimmed back for a noise regression

**Confirmation:** user re-ran the exact reproducer (repeated relative-delay
key input while in two-tone) on the fixed firmware. Frequency swept
smoothly with the delay input as expected, and - the actual bug this whole
thread was chasing - **held exactly where it should the instant delay input
stopped, with no spontaneous jumping and no sticking away from the expected
value.** This is a clean, decisive real-hardware confirmation: the BS170
level-shifter edge-timing theory (bit-bang transport running unthrottled at
~7MHz through a level shifter only good for ~4MHz, `spi_clock_hz` silently
dead code the whole time under `AD9851_USE_BITBANG`) was the actual root
cause of the random TX-frequency jump/stick investigation that's run since
2026-09-09.

**Cost of the fix, and a real regression:** same test session reported
"very noisy" with diagnostics muted (`'v'` off, ruling out the already-known
45ms-print/20Hz mechanism). Diagnostics showed why: `spi_us` (the bit-bang
loop itself) went from 16-17us to 30us, pushing `max_busy_us` to 58 against
the 62.5us tick budget (adc=4 dsp=16 write=40) - only ~4us of headroom left,
and wakeup jitter crept up too (`max_gap_us` 71->78). Two plausible,
non-exclusive causes: (1) that shrunk real-time margin reopening this
project's own recurring cross-core-scheduling-jitter noise mechanism (the
`dac_task_enabled` regression, the ADC-ISR priority collision - same
family, different trigger); (2) the ~13-14us write-path slowdown is a
genuine, newly-introduced shift in group delay (~0.2 samples at 16kHz) that
the existing `relative_delay`/`gdeq` tuning was never calibrated against -
this thread has already shown fractional-sample misalignments this size
matter (group_delay_fit_notes.md's sub-0.1-sample compromise windows, the
0-50Hz shift over a 2-sample delay sweep).

**Trimmed same day:** the fix used two `ad9851_edge_delay()` calls per bit
(settle DATA before W_CLK's sampling edge, then settle the sampling edge
itself before ending the pulse) plus one after FQ_UD's final edge - belt-
and-suspenders. Removed the second per-bit one (`AD9851.c`,
`ad9851_set_frequency()`'s bit-bang loop) - the DATA-settle delay is the
load-bearing one (it's what stops the chip sampling a still-transitioning
bit); the sampling-edge-settle-before-ending-the-pulse one was the extra
margin. Should roughly halve the added cost (back toward spi_us~23-24us,
max_busy_us~51-52us, restoring ~6-7us of headroom) while keeping the
confirmed fix intact. FQ_UD's own final-edge delay (the actual latch, only
once per transfer - cheap) was left in place.

**Not yet re-verified on the bench.** Next test: reflash, re-check
`[timing] max_busy_us`/`overruns`/`spi_us` (expect meaningfully lower than
58/40/30 but still elevated vs. the original 25/16-17 baseline), re-run the
same delay-sweep reproducer to confirm sticking is STILL gone with only one
per-bit delay, and separately assess whether the reported noise increase
has resolved. If sticking reappears with only one delay, that means both
per-bit edges genuinely needed protection and the second delay should be
restored (accepting the tighter margin) rather than left out.

**2026-09-11: trim re-verified on the bench, then reverted pending a clean comparison.**
Reflashed with only the DATA-settle delay (second per-bit delay removed,
FQ_UD final-edge delay kept). `[timing]` came back as hoped:
`max_busy_us=46` (`adc=5 dsp=16 write=29`), `spi_us=20`, `overruns=0`,
`late_ticks_total=0`, `max_gap_us=71` - margin restored to ~16us against
the 62.5us budget, wakeup jitter back at the pre-fix 71us baseline (not the
elevated 78us seen with both delays in). So the margin regression from the
day before is resolved by the trim, as expected.

But the same capture also caught a real hands-off TX jump: delay control
untouched (not the active-sweep reproducer), `[dsp] max_freq_dev_step:
10387Hz (14192168 -> 14202555 Hz, at t=308407ms)` - a single-tick
transient, same family as the 8562Hz/8199Hz ones already attributed to
ordinary near-null atan2 noise (`freq_dev: max_unclamped=7994Hz,
clip_count=0` in the same capture; canary OK throughout; `freq_dev`/
`tx_freq` nominal in every other snapshot). The digital math computed
correctly and the receiver still saw a real shift - the same pattern every
jump in this thread has shown, before and after the SPI fix.

This matters for the trim's coverage specifically because the near-null
atan2 noise that produces these swings happens every cycle on its own, not
only when the delay-sweep reproducer is actively provoking it - so the
same "abrupt FTW step outruns the BS170's slow settling edge" mechanism the
fix targets could still fire spontaneously during ordinary two-tone
playback, just less often than under active sweeping. A single hands-off
capture can't tell whether (a) dropping the second per-bit delay reopened
part of the gap the fix closed, or (b) this is a separate mechanism that
was always going to survive the SPI fix regardless of which delays are in
place.

**Decision: restored the second per-bit delay** (`AD9851.c`,
`ad9851_set_frequency()`'s bit-bang loop - see that call site's comment for
the full reasoning) rather than leave the ambiguity open, since it's the
direct way to separate the two explanations and the cost (margin back down
toward the 2026-09-10 46(?)->~58us figures) is fully reversible. Plan:
run a similarly long hands-off two-tone stretch on this build.
  - If hands-off jumps stop with both delays restored: confirms both edges
    genuinely needed protection, the trim was premature, and the tighter
    margin is the real price of the fix (leave both delays in going
    forward).
  - If hands-off jumps still occur with both delays restored: strong
    evidence this specific jump is a separate mechanism the SPI fix was
    never going to touch - the trim can go back in (it wasn't the problem),
    and the -45Hz-class hands-off jump needs its own investigation
    (candidate next step: check whether it correlates with anything at
    t=308407ms - ADC/DSP task timing, a diagnostics print, cross-core
    activity - the same way the `dac_task_enabled`/ADC-ISR-priority jitter
    mechanisms were found).

Not yet re-verified on the bench with both delays back in - that's the
next test, alongside confirming `max_busy_us` lands back near the
2026-09-10 post-fix figures (~58us) rather than something worse.

**2026-09-11, later same day: user pushback on the hardware framing - reports the hands-off jump "moves with delay" and is worse near the optimum-IMD compromise window, and suspects overload-induced data corruption rather than an external/hardware cause. Investigation below finds two separate, unrelated explanations - one confirms a real diagnostic red herring, the other identifies the jump itself as an already-diagnosed, already-understood DSP/physics effect, not corruption.**

**(1) The `[adc] fifo drop_total` growth (millions of drops, `min=4294967295 max=0` frozen across every snapshot) is a dead/misleading diagnostic in TWO-TONE mode, not evidence of real data loss on the signal path being tested.** Traced via `adc_capture.cpp`/`ssb_mic_test.ino`: `adc_capture_read_next_sample()` - the only code that drains `s_adc_fifo_tail` and updates `s_dbg_adc_fifo_min_available`/`max_available` - is only called in the `else` branch of `ssb_mic_test.ino`'s audio-source dispatch (line ~550), which `AUDIO_SRC_TWOTONE` never reaches (it calls `generate_twotone_sample()` instead, line 541). Meanwhile `adc_conv_done_cb()` (the ISR) keeps pushing real mic-input samples into `s_adc_fifo` at ~80kHz regardless of mode - by design, per that file's own comment, since the ADC runs unconditionally. With nothing ever draining the tail, the FIFO fills within milliseconds of boot and from then on nearly every subsequent ISR sample hits the "FIFO full" branch and increments `s_dbg_adc_fifo_drop_count` (confirmed by the growth rate across this capture: ~78.6k drops/sec, essentially the full 80kHz ADC rate) - and `min_available`/`max_available` stay frozen at their init sentinels forever because the function that updates them is never called. So this large, alarming-looking number is just the unused mic path silently discarding a mic signal nobody is reading while two-tone testing runs - it says nothing about the integrity of the actual two-tone signal being measured. Real, but harmless in this mode; worth fixing only because it's actively misleading (this is what triggered the "data corruption" suspicion this round) - e.g. skip incrementing/printing it when the audio source isn't a mic-driven one, or gate `adc_conv_done_cb` itself off outside mic modes. Not fixed yet - flagging for a decision, not applying unrequested.

**(2) The jump itself (10349-10387Hz single-tick `tx_freq` steps, delay-position-sensitive, worse near the optimum-IMD window) matches this project's own already-completed EER/polar-transmitter null-crossing investigation from 2026-09-01 through 09-04 - it is very likely NOT new corruption, and NOT something the AD9851/BS170 edge-timing fix could ever touch.** Cross-referencing `group_delay_fit_notes.md` and `ssb_mic_test_commands.md`'s "Is `eq`'s IMD benefit the highpass or the presence boost?" section:

  - With equal-amplitude two-tone (`eq` off, tone ratio 0dB), the analytic-signal trajectory passes exactly through the origin at every destructive-interference null. That produces a genuine derivative discontinuity in `freq_dev` - not noise, not a bug - modeled at the time as a ~9640Hz worst-case single-sample swing for this project's 700/1900Hz pair (real bench measurement then showed a sign-flip: "+1300Hz either side, -6700Hz right at the null"). The two values captured this session (10349Hz, 10387Hz) are the same order of magnitude and same signature (one-tick, digitally-correct-but-large, canary OK, `clip_count=0` since it's well under `MAX_FREQ_DEV_HZ`=20000). This is the textbook Kahn/EER "bandwidth expansion when the trajectory passes near the constellation origin" effect (Zhuang/Waheed/Staszewski, IEEE TCAS-I 2010) - already researched and cited in this project's own notes on 2026-09-03, not a new mechanism.
  - **Already bench-confirmed fix exists and was NOT re-enabled for this session as far as the pasted capture shows**: `eq` (`'e'`) or the dedicated `'R'` tone-amplitude-ratio control (even ~1dB of tone mismatch) moves the trajectory off the true origin entirely, eliminating the sign-flip/large-swing condition at the source - confirmed on real hardware 2026-09-04 ("that proves the hypothesis... very close to `eq` on"). This is a DSP-domain fix for a DSP-domain (well, physics-domain) effect - unrelated to `AD9851.c`'s BS170 edge-settle delay, which only concerns how faithfully a given commanded FTW step reaches the chip, not whether that step should be small in the first place.
  - **Already investigated and explicitly REJECTED for this specific problem: tightening `freq_dev_slew_limit_hz`.** The 2026-09-01 entry in `group_delay_fit_notes.md` found tightening the slew limiter made things monotonically WORSE (8000Hz worse -> 12000 better -> 20000 better still, trend toward fully unlimited) because it just clips a legitimate large excursion rather than preventing the trajectory from needing one - confirmed every preset in `settings.h` currently sets `freq_dev_slew_limit_hz = SSB_DSP_FREQ_DEV_SLEW_UNLIMITED_HZ`, which now reads as the deliberate conclusion of that investigation, not an oversight. Do NOT reach for the slew limiter as a fix for this round's jump.
  - **The delay-position sensitivity the user is now reporting is also already-documented, not new**: the 2026-09-01 entry notes "which discrete sample lands nearest the true continuous-time null is alignment-dependent... the delay setting shifts exactly which sample lands closest" - i.e. moving `relative_delay` changes exactly how close some sample gets to the singularity, hence how large the worst-case one-tick swing is at that setting. This is consistent with, and likely fully explains, "more sensitive at particular delays especially where the optimum IMDs are."

**Net read:** the AD9851 second-delay restoration (this file's entry above) is still the right test for the SPI-write "sticking" symptom it targets, but is not expected to touch this specific jump - this jump looks like a separate, already-understood, already-solved-on-the-bench DSP/physics effect (true envelope null under equal-amplitude two-tone), not overload-induced corruption and not something more edge-settle margin can fix. **Suggested next step, cheaper than the long hands-off SPI comparison run**: check whether `eq` or `'R'` (tone ratio) was engaged for this session's capture; if both were off/equal, re-run with `'e'` on or `'R'` at +/-1-3dB and see whether the `max_freq_dev_step` events (and the "worse near the IMD-optimum delay" pattern) disappear - that would confirm this is the known null-crossing effect and not a new one, and separates it cleanly from the still-open hands-off-jump question the AD9851 comparison run is trying to answer.

**2026-09-11, later still: reverted bit-bang to fully unthrottled (original high-speed) pending a proper re-scope of the drive signals.** User realized the original "4MHz measured as the safe upper limit" bench conclusion (2026-09-10) may not have actually verified the toggle RATE - the scope session was focused on rise-time shape and didn't separately confirm the signal being measured was really throttled to 4MHz rather than the original unthrottled ~7MHz-equivalent rate. Since that measurement is the sole evidentiary basis for every `ad9851_edge_delay()` call this thread has added/removed/restored/trimmed since, it needs re-taking with both rate and rise time checked together before any of that throttling work can be trusted. Reverted via a new `AD9851_BITBANG_EDGE_DELAY_ENABLED` flag (`AD9851.c`, set to `0`) guarding all three call sites (DATA-settle, W_CLK-sampling-edge, FQ_UD-latch) rather than deleting them - a one-line flip back to `1` restores exactly the 2026-09-10/11 state (with both per-bit delays, per the most recent restore) once the re-scope is done, without re-deriving anything. `half_period_cycles` computation in `ad9851_init()` is left in place (harmless, cheap, one-time) so re-enabling needs no other change. **Next bench step: re-scope DATA/W_CLK/FQ_UD at this genuinely-unthrottled rate, this time explicitly confirming the real toggle rate on the scope (not just rise-time shape) before judging whether it's clean.** If it turns out the original "safe at 4MHz" rise-time measurement really was taken on the unthrottled signal all along, that would mean the unthrottled rate was fine on this hardware from the start, and the whole edge-delay mechanism (and its real busy_us/margin cost) may never have been needed - to be confirmed, not assumed, by the re-scope.

**2026-09-11, later still: clarifying two different "20k"/"slew limit" knobs the user asked about, since they're easy to conflate and have opposite tuning history.** There are two independent mechanisms in `ssb_dsp.c`, both are Hz-denominated, and only one is currently near "20k":
  - **`max_freq_dev_hz` (the magnitude CLAMP, `config.h:405` = `MAX_FREQ_DEV_HZ` = `20000.0f`)** - hard-clips `|freq_dev|` after everything else runs. Compile-time only (set once in `ssb_dsp_init()` from `cfg->max_freq_dev_hz`; no live serial command touches it - would need a rebuild+reflash to test a different value). **This is very unlikely to be involved in the current jumps**: both captured events this session (max_unclamped=7994-7997Hz, max_freq_dev_step=10349-10387Hz) sit under 20000Hz, and the log's own `clip_count=0` confirms the clamp never actually engaged during this capture. Lowering it wouldn't currently change anything; it isn't "causing" what's being seen.
  - **`freq_dev_slew_limit_hz` (the RATE limiter)** - limits how fast `freq_dev` can change tick-to-tick, live-adjustable via `'{'`/`'}'` (`ssb_dsp_lower_/raise_freq_dev_slew_limit()`). Every preset in `settings.h` sets this to `SSB_DSP_FREQ_DEV_SLEW_UNLIMITED_HZ` (effectively off) - and per the 2026-09-01 entry in `group_delay_fit_notes.md` (cross-referenced in this file's entry above), that's the deliberate result of prior bench testing that found TIGHTENING this made two-tone IMD monotonically WORSE (8000Hz worse -> 12000 better -> 20000 better still -> trend toward fully unlimited) because it clips a legitimate large excursion instead of preventing the trajectory from needing one. If "test the Hz slew limit" means this one: it's currently off, testing it means turning it ON (`'{'` a few times, starts at 2000Hz per `FREQ_DEV_SLEW_START_HZ`), and prior data already predicts it'll trade a smaller `max_freq_dev_step` for worse two-tone IMD - worth confirming directly against THIS specific jump rather than assuming the 2026-09-01 result transfers unchanged, but going in with that expectation already set.
  Which one the user meant when they said "currently at 20k" wasn't confirmed - the clamp is the only one actually AT 20k right now, so that's the more literal read, but flagged both since the fix history differs completely between them.

**2026-09-11, later still: added a `[dsp] post-step trace` diagnostic to directly answer "does it recover automatically, or stick?" instead of arguing it from first principles.** User's objection, and a good one: if this is a genuine one-tick DSP transient (the EER null-crossing theory above), `freq_dev` is recomputed fresh from `atan2(Q,I)` every single sample with no persistent memory (the one thing that WOULD carry state tick-to-tick, the slew limiter, is currently off per the entry above) - so it should recover within one 62.5us tick, not stay shifted. The existing `max_freq_dev_step` high-water-mark can't actually show this either way - it only ever records the two values straddling the single worst step, nothing about what happens afterward. Added a small ring capture (`diagnostics.cpp`, `FREQ_STEP_TRACE_LEN=8`) that arms on every new record-breaking step and records that tick's (post-jump) `tx_freq` plus the next 7 ticks, printed as `[dsp]   post-step trace: <8 values>` in the periodic block once filled. **This will show directly, on the next bench run, whether tx_freq snaps back within a tick or two (supports the EER-null-transient read) or stays elevated for many ticks (would instead point at something persistent - a latched/corrupted value, a stuck delay-line entry, or the PWM/envelope path desyncing from the phase path, all raised as live alternatives this round and none yet ruled out beyond the existing carrier_hz/ftw_reciprocal canary, which has stayed clean across every jump seen so far).** Note this trace is independent of the AD9851 bit-bang revert above - it captures at the DSP/`ad9851_set_frequency()`-call boundary regardless of transport timing, so it'll work identically whether or not the edge-delay flag is re-enabled later.

**Open, not yet answered:** whether `'e'`/`'R'` (tone-ratio) were engaged for the session that produced the pasted capture - still the fastest way to test the EER-null-crossing explanation directly (see this file's entry above). Also still open: the PWM/envelope-path desync possibility the user raised - not yet investigated at all; if the post-step trace shows a genuine "stick" rather than a recover, that becomes the next thing to check (envelope_output.cpp/envelope_interp.cpp's own tick bookkeeping against dsp_task's, looking for the same class of cross-context assumption issue already fixed once in `ssb_mic_test.ino`'s `audio_source_t src` single-read pattern).

**2026-09-11, later still: unthrottled bit-bang confirmed on the bench (spi_us=16, matches the original pre-2026-09-10 baseline exactly) - and the jump still occurs at full speed, with no edge delays active at all.** New capture: `spi_us=16` (`prep_us=2`, `prep+spi=18` vs `write_us=27`) - matches this project's own pre-throttling baseline ("spi_us 16-17us") almost exactly, confirming `AD9851_BITBANG_EDGE_DELAY_ENABLED=0` is doing what it's supposed to and the drive signals are now genuinely running at the original ~7MHz-equivalent unthrottled rate again, ready for the re-scope. A `max_freq_dev_step: 8003Hz (14193359 -> 14201362 Hz, at t=98071ms)` event was captured in this same run - i.e. **the jump still happens with the AD9851 driver back to its original, long-proven-fine (rise-time-wise, at whatever rate it actually was) unthrottled form, no `ad9851_edge_delay()` calls anywhere in the transfer.** This is independent, stronger evidence for this file's earlier read: this specific jump-type symptom lives entirely upstream of the AD9851 transport (confirmed now under BOTH the throttled and fully unthrottled transport), not in BS170 edge settling - consistent with the EER/polar-transmitter null-crossing explanation, not the bit-bang timing theory. `gdeq=off` in this capture (was `ON` previously) - per `ssb_mic_test_commands.md`'s 2026-09-07 entry ("`g` makes almost no difference" to the related AM-to-PM effect) this isn't expected to matter for this specific mechanism, noted for completeness only.

**2026-09-11, later still: the new `[dsp] post-step trace` line never printed in that capture - self-inflicted repeat of a lesson already documented in this exact file.** Root cause: it requested `diag_room_for(200)`, but `diag_room_for()`'s own header comment (real hardware measurement, already on record in `diagnostics.cpp`) says `Serial.availableForWrite()` maxes out around ~162 bytes even at this board's fully-drained resting state - a 200-byte request can structurally never succeed, the same failure this project already diagnosed once for the old all-or-nothing block guard. Confirmed on the bench: `max_freq_dev_step` (a 140-byte request) printed repeatedly in the same window the new line never appeared in once. Fixed: shortened the line and dropped the request to 130 bytes (worst case ~93 bytes actual), matching the sizing convention every sibling line in this file already uses. **Not yet re-verified - needs a re-flash and another capture to confirm it actually prints this time and to finally get real data on the "does it recover" question**, which is still open.

**2026-09-11, later still: "can an IIR filter's state get corrupted forever?" - yes structurally, no NaN/Inf guard exists anywhere in this DSP chain, and checking it turned up one real (if narrow) bug, now fixed.** Good question, and a genuinely different risk class from the freq_dev/atan2 computation discussed earlier in this file: that computation is memoryless (recomputed fresh from `atan2(Q,I)` every sample, so a bad tick can't outlive itself), but IIR filters (`biquad_process`/`ssb_allpass1_process`/`ssb_shelf_biquad_process` in `ssb_dsp.c`, `compressor_process`'s envelope follower, `ssb_adc_filter.c`'s ADC anti-alias biquads) all carry `x1/x2/y1/y2`-style feedback state that references its OWN past output - a bad value there doesn't get flushed by fresh input the way an FIR tap or a fresh atan2 call does; the feedback formula keeps citing it every subsequent sample. `flush_denorm()` (applied throughout) only catches near-zero subnormals, not NaN/Inf/absurdly-large-but-finite values - grepped the whole project, no `isnan`/`isinf` check exists anywhere in this DSP chain. So IF a bad (non-decaying) value ever got into one of these states by some means, it could genuinely persist indefinitely, unlike the phase/frequency math.

  - **Checked whether the actual null-crossing event could organically seed one**: `fast_atan2()`'s `x==0.0f && y==0.0f` case is explicitly handled (returns 0.0f, no division), and every other branch only ever divides `ay/ax` or `ax/ay` where the divisor's branch condition (`ax>=ay`) guarantees it's the larger of the two - no way to reach a 0/0 or divide-by-zero from this function. `fast_sqrt()` explicitly returns 0 for `x<=0`. So no NaN path was found from the specific null-crossing event this thread has been chasing - the "digital math computed correctly" read from earlier in this file still stands for that specific mechanism.
  - **Found instead, while checking this: `ssb_dsp_set_eq_enabled()` (`ssb_dsp.c`) didn't reset `eq_hpf`/`eq_presence`'s biquad state on re-enable**, unlike `ssb_dsp_set_compressor_enabled()` right next to it (which explicitly zeroes `comp.env` on re-enable, with its own comment explaining exactly why: "so it doesn't resume from a stale value... avoids a jump/thump") and unlike `envelope_gdeq_set_enabled()`'s identical off->on reset. Toggling `'e'` off then back on was feeding whatever `x1/x2/y1/y2` happened to be frozen at back into the very next sample - a real, demonstrable "resume from stale state" bug, exactly the shape of thing being asked about. **Fixed** to mirror the compressor's own established pattern. Not "forever" in practice, though - these are RBJ-cookbook biquads with normal (BIBO-stable) coefficients, so a stale-state transient decays within a few dozen samples, not indefinitely; the real-world symptom would be a brief click/thump right at the `'e'` toggle instant, not an ongoing corruption during steady playback.
  - **Net read**: this fixed bug only fires on an `'e'` toggle, so it's unlikely to be the direct cause of the spontaneous mid-run jumps this thread has been chasing (those aren't reported as coinciding with a toggle) - worth asking whether `'e'` was being toggled during any of the captures that showed a jump, just to rule it out. But it's a real bug regardless, worth having fixed, and it validates the general concern: this DSP chain genuinely has no defense against a persistent-state corruption once introduced, anywhere feedback state exists. `ssb_adc_filter.c`'s ADC LPF and `envelope_gdeq.cpp`'s allpass both already reset correctly on their own mode-transition paths (checked directly) - `eq_enable` was the one inconsistent case.
  - **Not yet done**: an actual live canary for this (e.g., an `isnan`/`isinf` check on `eq_hpf.y1`/`eq_presence.y1`/the gdeq allpass state/`comp.env`, printed the same way `ftw_reciprocal`'s canary is) - would settle "is this happening right now" empirically instead of by code inspection, same evidence-first approach as everything else in this file. Offered, not yet built - build it if the post-step trace doesn't cleanly explain the jump once that's re-tested.

**2026-09-11, later still: added the isnan/isinf canary for every IIR-style persistent filter state in this project, following the same latch pattern as the existing `carrier_hz`/`ftw_reciprocal` canary.** User confirmed `eq` wasn't in use for current testing (so the fixed `ssb_dsp_set_eq_enabled()` bug isn't the direct cause of the jumps being chased) but asked for the general test anyway - good call, since the underlying "can a filter's own feedback memory get stuck" question is independent of which stage happens to be enabled right now. Implemented via `isfinite()` (the standard single-call equivalent of `!isnan(x) && !isinf(x)` - false for either, true otherwise) on each stage's fed-back state:

  - `ssb_dsp.c`/`.h`: new `ssb_dsp_get_iir_canary()` checks `eq_hpf`/`eq_presence` (`y1`/`y2`) and the compressor's `comp.env`.
  - `envelope_gdeq.cpp`/`.h`: new `envelope_gdeq_get_canary()` checks both allpass sections' `y1`.
  - `envelope_ampeq.cpp`/`.h`: new `envelope_ampeq_get_canary()` checks both shelf biquads' `y1`/`y2`.
  - `adc_capture.cpp`/`adc_capture.h`: new `adc_capture_get_lpf_canary()` checks both the Butterworth and Chebyshev cascades' `z1`/`z2` (both stages) - not exercised by two-tone/synthetic modes (confirmed earlier this session that `adc_capture_read_next_sample()`, the only code that runs these filters, isn't called for those sources), but included for completeness/future mic-mode use.
  - `diagnostics.cpp`: wired all four into `canary_check_background()` (checked every tick unconditionally, latches into one of five new `s_dbg_canary_{eq,comp,gdeq,ampeq,adclpf}_bad_since_ms` fields the FIRST time any relevant stage goes non-finite, prints a `MISMATCH` line only on that transition - same silent-while-healthy design as the existing two canaries) and `canary_print_status()` (the `'v'`/on-demand path - prints one compact `[canary] iir_state: OK (eq/comp/gdeq/ampeq/adc_lpf)` line when everything's finite, expanding into per-module detail lines only when something isn't). Sized each new print line to 90-130 bytes, well under the ~162-byte ceiling `diag_room_for()`'s own header comment documents for this board - learned that lesson the hard way earlier today with the post-step trace line, not repeating it here. Latches are NOT reset by `diagnostics_reset()`/`'r'`, same reasoning as the existing two: a rare corruption event shouldn't silently vanish just because someone started a fresh measurement window.

**Not yet re-verified on the bench** - needs a reflash. Once it's running: a steady `[canary] iir_state: OK` (or the two individual OK lines it now sits alongside) across a long hands-off run would mean no IIR state has gone bad by any means checked so far, keeping the "IIR corruption" theory open only as a not-yet-observed possibility rather than a confirmed mechanism; any `MISMATCH` line would be a genuinely new, actionable data point - note its `first seen at t=...ms` against the `[dsp] max_freq_dev_step`/post-step-trace timestamps to see whether the two ever correlate.

**2026-09-11, later still: first real bench data from the reflashed build - ~4.7 hours unattended two-tone run, good news across the board.** The one `max_freq_dev_step` event on record (10401Hz at t=1076763ms, ~18 minutes after boot) never recurred for the remaining ~4.5+ hours - the high-water-mark register simply never updated again. The now-working post-step trace shows it recovering to steady state (14202562 -> 14201359 -> ... settling to +/-4Hz) within exactly one tick and staying there. Both canary families (`carrier_hz`/`ftw_reciprocal` and the new `iir_state`) read `OK` on every single snapshot across the whole run - no NaN/Inf, no carrier/FTW divergence, ever. Timing stayed rock solid the entire time: `overruns=0`, `late_ticks_total=0`, `max_busy_us=46` against a 62us budget throughout, `wakeup jitter` pinned at a steady `max_gap_us=70` (not growing). `spi_us=20` confirms the earlier full-speed-bit-bang revert is in effect and still well inside budget. Net read: a single-tick, fully-recovering, never-repeating excursion with every corruption canary clean the whole time is much better explained by the already-documented EER null-crossing physics than by an ongoing corruption mechanism - nothing in this run contradicts that theory, and several things (the clean canaries especially) argue against the "corrupted persistent value" theory from the 2026-09-09 entry above.

  - **Correction to this file's own dac_task_enabled note above**: re-checked and `dac_task_enabled` is confirmed `0` in the actual `config.h` on this build (not just "should be" - directly confirmed both by source and by live data: `dac_code` read exactly `0` on every single snapshot across the whole 4.7-hour run despite envelope moving between 0.663-0.664, which is only possible if `dac_task` genuinely never ran to pick up a value). Closes out the "re-confirm `dac_task_enabled=0` on the actual hardware build in use" item from the 2026-09-09 entry above with hard evidence, not just a source read.
  - New-to-this-run diagnostic output, not previously called out in this file: `null_bias`/`null_bias2`/`null_bias3` (from `null_bias_investigation.md`) show a stable `weighted_bias` of +73.24Hz for the active 700/1700Hz pair, and the user separately confirmed the SDR has read "within a few Hz" whenever they've checked - consistent with `null_bias_investigation.md`'s own confirmed-accurate `weighted_bias` predictor.

**2026-09-11, later still: added a test-tone dither ('Q', test_signals.cpp/.h) to try fixing the null-bias measurement artifact, in response to a direct question about dithering - untested.** Two different "null" problems exist in this project's notes and it's important not to conflate them: `group_delay_fit_notes.md` (2026-09-03) researched dithering for the OTHER one (the EER/polar "theoretically infinite phase bandwidth at the origin" problem) and found no literature precedent for it there - the established fix is an upstream I/Q trajectory reshape instead. This addition targets `null_bias_investigation.md`'s problem instead: that file's "Root mechanism identified" section already explains the bias is coherent specifically because the two-tone test tones are exact phase-accumulator multiples of `SAMPLE_RATE_HZ`, so every null in a run lands at an identical sample-grid position and whatever tiny bias one null produces, every null produces identically - a structurally different situation from the EER problem, and one that's a much more natural fit for dithering (same class of fix as breaking a limit-cycle/idle-tone with ADC dither).

  - **What it does**: `Q` toggles a small (+/-`TWOTONE_DITHER_MAX_HZ`, 0.5Hz default) frequency offset on tone2 only (tone1 stays exactly at nominal as an undithered reference), redrawn from `esp_random()` every `TWOTONE_DITHER_UPDATE_HZ` (4Hz default) and linearly ramped toward sample-by-sample in between so the instantaneous frequency varies continuously rather than in steps. Off by default (`TWOTONE_DITHER_ENABLED=0`, `config.h`) - `t`/`T`/`R` behave exactly as before unless `Q` is pressed. New config constants, `test_signals_get/set_twotone_dither_enabled()`, and the `'Q'` handler (mirrors `'T'`/`'R'`'s "switches into two-tone mode too" convention) are the only touch points - `ssb_dsp.c` and the rest of the real signal chain are completely untouched.
  - **Not yet compiled or bench-tested.** `esp_random()`/`<esp_random.h>` usage specifically needs verifying against the installed Arduino-ESP32 core - same "NOT COMPILER-VERIFIED, no toolchain available in this environment" caveat this whole project already carries.
  - **Validation plan** (documented in `null_bias_investigation.md` and `test_signals.h`): compare `[dsp] null_bias2` (`weighted_bias`) across several `r` resets on the same tone-pair preset, `Q` off vs. on, with a LONG dwell before reading each time - the 2026-09-09 entry above already found `weighted_bias` drifts for tens of seconds to minutes after a reset even with nothing changed, so a quick point-read would just compare noise to noise, not the effect of `Q`.

**2026-09-11, later still: first bench data with `Q` ON, +/-0.5Hz, 700/1900Hz pair - "a noisy board peak +/- about 6Hz from zero", compared against this file's own 700/1900 baseline (`null_bias_investigation.md`'s confirmed-measurement table: `weighted_bias` -20.72Hz, SDR ~8Hz, dither off).** `esp_random()`/`<esp_random.h>` compiled and ran without incident - first real confirmation the "not compiler-verified" caveat above wasn't hiding a build problem. Read as a real-time board display bouncing roughly symmetrically around 0Hz with about a 6Hz peak excursion, rather than settling on a fixed off-zero constant the way the undithered case does. Timing/canaries stayed clean through this capture (`overruns=0`, both canary families `OK`); the single `max_freq_dev_step` event on record recovered within one tick per the post-step trace, consistent with every prior capture. Read this cautiously, not as a clean isolated A/B: the tone pair was already sitting on 700/1900 before and after, but this was the first capture taken right after reflashing the `Q` build, so settle-time and any residual state from the previous session weren't independently controlled for. The magnitude (~6Hz peak) is in the same ballpark as the undithered baseline's ~8Hz SDR reading, not obviously smaller - consistent with the classic dither signature of trading a coherent, repeatable bias for a randomized spread of similar overall magnitude, rather than a magnitude reduction. Whether that's a net win depends on what the downstream use of the reading cares about (a fluctuating-but-zero-centered read vs. a stable-but-offset one) - not yet assessed.

**2026-09-11, later still: dither reduced to +/-0.05Hz (`TWOTONE_DITHER_MAX_HZ`, config.h), still 700/1900Hz pair - "continuous shifting -10 -> +3Hz" observed on the board, compared against the +/-0.5Hz capture immediately above.** `weighted_bias` itself barely moved between the two captures (-16.4Hz at 0.5Hz dither vs. -17.4Hz at 0.05Hz dither, essentially flat) despite the dither amplitude changing by 10x - if dithering worked by smoothly smearing the null bias in proportion to how far it pushes tone2 off-grid, the averaged bias should have shrunk toward the undithered baseline as amplitude shrank, and it didn't. The real-time observed spread also stayed roughly the same overall size at both amplitudes (~12-13Hz peak-to-peak at 0.05Hz dither vs. ~12Hz at 0.5Hz dither) - same rough magnitude of swing 10x apart in dither depth is the signature of a threshold/discontinuity effect, not a smoothly graded one: once dither is nonzero at all, it looks sufficient to occasionally knock a null onto the other side of whatever discrete sample-grid boundary drives the coherent bias in the first place (per "Root mechanism identified" in `null_bias_investigation.md`), producing a close-to-full-scale jump regardless of how small the nudge was. Timing/canaries clean throughout (`overruns=0`); the one `max_freq_dev_step` event on record again recovered within a tick. **Open discrepancy, not yet resolved**: `weighted_bias`'s -17.4Hz average sits well outside the user's reported real-time range (-10 to +3Hz, midpoint ~-3.5Hz) - two live hypotheses: (1) `weighted_bias` and whatever instrument produces "the board" reading are different statistics of the same signal (different averaging/settling behavior) and simply don't have to agree numerically; (2) the continuous re-dithering (a fresh random target every 250ms at `TWOTONE_DITHER_UPDATE_HZ=4.0`) may be preventing that instrument from ever reaching steady state, making some or all of the "continuous shifting" a measurement-methodology artifact rather than a property of the RF signal itself. Asked the user what "the board" is and how it derives its reading, and proposed a follow-up (slow `TWOTONE_DITHER_UPDATE_HZ` way down, e.g. to 0.2Hz, at small amplitude, to see whether the board's reading can settle between updates) to discriminate between the two - not yet run.

**2026-09-12: "the board" identified (SDRUno "Aux SP" FFT panadapter, 0.18Hz/bin, RX ref accurate to +/-1Hz) — resolves most of the `weighted_bias`-vs-observed-range discrepancy from the two entries above via ordinary integration-time/sampling-variance math, no instrument-settling mechanism needed.** Full derivation in `null_bias_investigation.md`'s "2026-09-12" entry. Short version: 0.18Hz/bin implies each Aux SP snapshot integrates roughly 5-10s of signal (~20-40 independent `Q` dither realizations, since a fresh target is drawn every 250ms per `TWOTONE_DITHER_UPDATE_HZ=4.0`), while `weighted_bias` (`env2_sum`/`env2_dphi_sum`, `ssb_dsp.c`) is a plain lifetime accumulator since the last `'r'` reset - by ~700s into a run it's averaged across ~2800 realizations, 70-130x more than one Aux SP snapshot. Given the already-established threshold-like (not smoothly graded) sensitivity of this effect, a small-N snapshot is expected to have much higher variance than the large-N firmware average even though both estimate the same underlying mean - ordinary sampling variance, not a sign either measurement is wrong. Cleanest next test (not yet run): toggle `Q` OFF and watch Aux SP on the same pair to see whether it's already scattering by several Hz snapshot-to-snapshot for unrelated reasons, or only does so with dither on.

**2026-09-12, later same day: correction to the "Aux SP identified" entry above (it's exponential-averaging, not a fixed-window FFT snapshot), plus two new hardware facts and a direct answer on whether dither should have centered the frequency on zero.** User corrected the earlier fixed-window/independent-snapshot model: Aux SP actually uses exponential averaging, which has a fixed noise floor that never shrinks with more observation time (unlike `weighted_bias`'s plain lifetime accumulator, whose variance keeps shrinking the longer it runs) - this still predicts persistent, non-settling scatter on Aux SP, just via a different mechanism than originally described. Two new facts: the user's own visual read of the moving trace is only reliable to about +/-5Hz (cursor-based static reads are much better); and the AD9851's reference is a plain 30MHz XO that shifts ~20Hz if physically disturbed while cooling, though stable once warmed - a real confound of comparable magnitude to the effect being chased, and one `weighted_bias` is immune to (it's computed purely in the digital/audio domain) while Aux SP is not (it reads the actual on-air RF, including any real XO drift). On the direct question - `weighted_bias` improved with `Q` on (-20.72Hz undithered -> -16.4Hz at 0.5Hz dither -> -17.4Hz at 0.05Hz dither) but plateaued well short of zero and was insensitive to a 10x change in dither amplitude, which reads as `Q` successfully doing its designed job (decorrelating which sample-grid alignment each null lands on) but landing on the genuine alignment-averaged mean of the underlying near-null bias mechanism, which this evidence suggests is NOT itself zero-mean - so full centering on zero was probably never a fully justified expectation, and closing the remaining ~17Hz gap likely needs one of the "Targeted"/"Principled" null-handling fixes already on record in `null_bias_investigation.md` rather than further dither tuning. Full reasoning and two proposed follow-up tests (repeat on an already-near-zero pair like 1500/1700; use Aux SP's cursor-based static reads instead of the live trace) logged in that file's "2026-09-12, later same day" entry.

**2026-09-12, later still: found the mechanism behind "changing relative_delay (or switching presets) shifts the measured tone frequency," and it exposes a real gap in every `weighted_bias` reading taken so far.** User reported (back on 700/1700, now reporting Aux SP as an offset from a 1000Hz nominal center) that `'['`/`']'` and preset switches (e.g. 1 & 3) repeatedly shift the measured tone frequency, sometimes a few Hz, sometimes ~25Hz, often snapping between two values rather than moving smoothly. Root cause: `relative_delay_apply()` (`relative_delay.cpp`) runs AFTER `ssb_dsp_process_sample()` and, for the positive delay every two-tone preset uses, holds `freq_dev_hz` back relative to an UNTOUCHED `envelope` - so it directly controls whether the well-documented near-null `freq_dev` spikes land on envelope~0 (suppressed, as `weighted_bias`'s whole env^2-weighting design assumes) or get exposed to non-negligible transmitted power (a real, coherent contamination of the actual radiated spectrum, not a diagnostic artifact). Since the spikes recur at the identical coherent alignment every cycle, a fixed delay produces a fixed "locked-in" bias, and because the spike is narrow/near-discontinuous, small delay changes near a good alignment barely matter while crossing the spike's window can swing the bias tens of Hz - matching everything the user described. Confirmed directly: preset 1 has `relative_delay_samples=0.00`, preset 3 has `2.00` (`settings.h`) - a 125us swing baked into the preset switch itself. Bigger finding: `null_bias`/`weighted_bias` are accumulated INSIDE `ssb_dsp_process_sample()`, i.e. pre-delay - `ssb_mic_test.ino` already had a comment flagging this blindness and reasoning "a pure sample delay can't change frequency content," which is true of `freq_dev_hz` in isolation but misses that the effect lives in the cross-alignment between two DIFFERENT signals. Net: every `weighted_bias` number gathered anywhere in this investigation (including both `Q`-dither results) is blind to whatever delay/preset is doing to the transmitted spectrum - Aux SP has been the only window into it. Also very plausibly the same mechanism behind this project's whole delay-tuning-for-IMD history. Proposed (not yet actioned, pending user decision): add a post-delay null-bias variant fed from `delayed_freq_dev_hz`/`delayed_envelope`. Full writeup in `null_bias_investigation.md`'s "2026-09-12, later still" entry.

## Open items carried from earlier sessions, still unresolved

- `MAX_FREQ_DEV_HZ` currently `20000.0f` (config.h:405) - a widened
  diagnostic value per its own comments, never re-optimized to a final
  number (comment nearby still says "8000Hz is being kept for now" from an
  earlier edit pass that didn't get updated). Not part of this thread but
  flagged here so it doesn't get lost.
- `dac_task_enabled` root-cause (stray `1` left in a local config.h,
  re-enabling Core-1 I2C activity) - user fixed locally; not written up in
  the project notes per user's explicit "don't worry about it" on
  2026-09-09, recorded here only for continuity of this log.
