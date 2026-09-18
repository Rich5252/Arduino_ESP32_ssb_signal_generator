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

**2026-09-12, later still: implemented a per-event jump log ('J') to directly test whether every random TX jump is really explained by null-crossing, per the user's explicit engineering doubt.** User's stated goal: eliminate the random freq jumps, and they're not convinced null-crossing is the only mechanism given how many "strange states" this investigation has turned up. Asked for diagnostics that would help settle it. Every jump diagnostic so far (`max_freq_dev_step`, the post-step trace) only remembers the single worst event per run - one data point isn't enough to prove "always this cause." Added: a `JUMP_LOG_LEN=8` ring capturing every qualifying step (threshold 300Hz, well below "new record") with the POST-delay envelope, `relative_delay_samples`, that tick's `busy_us`, audio source, and a `near_null` classification per event; a running near-null percentage printed automatically in the periodic block; and a new `'J'` command to dump the full ring on demand. Also added the matching post-delay envelope to the existing post-step `tx_freq` trace. Direct test: if `near_null` stays near 100% across many independently-logged events (not just one), that's real support for the existing theory; any consistent `near_null=false` population is hard evidence of a second mechanism. Also settles "does this happen on mic input too" for free, since every entry records its audio source. Full writeup, including the suggested test protocol (long two-tone run vs. comparable mic run, compare via 'J'), in `null_bias_investigation.md`'s "2026-09-12, later still" entry. Two incidental IRAM_ATTR fixes made along the way (`ssb_dsp_get_null_bias_threshold()`, `relative_delay_get_samples()` - both now called from the new hot-path code, neither previously marked, unlike their setters). Not yet bench-tested - no toolchain available in this environment, per this project's standing caveat on every firmware change made here.

**2026-09-12, later still: first 'J' bench data (503384 events, 0% near-null at relative_delay=+4.60) - found and fixed a real bug in the jump log's own near_null classification, plus a compelling unifying hypothesis not yet confirmed.** The eye-catching "0% near-null" turned out to be the diagnostic itself measuring the wrong thing, not evidence against the null-crossing theory: for `delay>0` (every two-tone preset), `relative_delay_apply()` reads its envelope ring at zero lag, so the `delayed_envelope` the jump log was comparing against was the CURRENT tick's envelope, not time-matched to the (4.60-sample-delayed) `freq_dev_hz` value each logged step actually came from. Fixed by adding a third, properly time-matched output (`envelope_at_freq_time`) to `relative_delay_apply()`, used now everywhere the jump log and post-step trace classify near_null. Separately, `+4.60` samples is more than double the largest relative_delay this whole project has ever bench-validated (everything else stays 0.00-2.10) - and the captured data (an exact, repeating 3-value tx_freq cycle, steps summing to zero, recurring every ~490us for 500K+ repetitions, with `busy_us` tracking the same 3-cycle pattern) is consistent with a compelling hypothesis: a badly-mistuned delay may expose EVERY beat's near-null spike instead of an occasional one, turning what's normally a rare, hours-apart glitch into a continuous, metronomic oscillation - i.e. still the same mechanism, just happening far more often due to the extreme delay setting. Not yet confirmed - proposed a direct test (reflash with the fix, compare 'J' at +4.60 vs. a previously-validated delay like 0-2.10) rather than reasoning further from one capture. Full writeup in `null_bias_investigation.md`'s new 2026-09-12 entry.

**2026-09-12, later still: +4.60 was incidental (scanning), and the user reports the most interesting jump activity concentrates around relative_delay~=2 - not "worse the further out you go."** Complicates the "badly-mistuned delay exposes every beat's spike" hypothesis from the previous entry: `relative_delay~=2.00-2.10` isn't a mistuned value in this project's own history - it's exactly the sub-0.1-sample window the delay-tuning saga settled on as the BEST two-tone IMD compromise. If jump activity is also most active right there, that's a possible, previously unrecognized tension between "best IMD" and "fewest random jumps" rather than one delay value fixing both. Asked for clarification on what "interesting" means precisely, and proposed a small delay sweep with the fixed 'J' (0.00/1.00/2.00/2.05/3.00, reset before each) to see whether activity peaks sharply at ~2 (pointing at something specific to landing on an integer sample, where interp_ring()'s interpolation weight stops blending two ring entries) or trends smoothly. Full reasoning in `null_bias_investigation.md`'s newest 2026-09-12 entry.

**2026-09-12, later still: second 'J' capture (delay=+0.90) decoded to a clean 3-state locked cycle, 2 of 3 states not near-null by the blended test - traced to a second diagnostic gap (lopsided fractional blending), now fixed.** 25693 events (~313/s - down from ~2038/s at +4.60, but still far from "rare"), 25% near-null overall. Decoding the 8 printed events showed a clean, repeating 3-state cycle (steps -6400/+7200/-800Hz, summing to zero) - same qualitative signature as the +4.60 capture. Only the +7200Hz step (env=0.036) is near-null; the other two (env=0.235, 0.354) aren't. The bug fix from the previous entry is confirmed working (near_null is no longer stuck at 0%), but this exposed a second, subtler gap: at delay=+0.90 (frac=0.90, a lopsided 90/10 blend), a genuinely near-null RAW sample contributing only 10% weight could easily produce a blended reading like 0.235 without the blended test ever catching it. Added a second output (`envelope_at_freq_time_min` - the smaller of the two raw samples actually being blended) and a second near-null classification (`near_null_either`) to distinguish "was the transmitted result near a null" from "did anything near a null contribute at all." Not yet re-tested - the next capture will show whether `near_null_either` comes back much higher (supporting the null-crossing theory after all) or stays similarly low (real evidence of a null-independent component in this specific cycle). Full writeup in `null_bias_investigation.md`'s newest 2026-09-12 entry.

**2026-09-12, later still: third 'J' capture (delay=+4.28) - `near_null_either` fix confirmed working (2% -> 34%), decoded to a clean 3-state cycle where 2 of 3 transitions ARE explained, but the largest is not by either test - added a third diagnostic (raw freq_dev pair) to dig into that one specifically before concluding a null-independent mechanism exists.** 134573 events, 2% near_null_blended, 34% near_null_either - a real, large jump from the +0.90 capture's numbers, confirming the lopsided-blend fix from the previous entry is doing real work, not just adding noise. Decoding the 8 printed events showed the same "clean repeating N-state cycle" signature as both prior captures: `~14195600 -> ~14199120 (+3520Hz) -> ~14201360 (+2240Hz) -> ~14195600 (-5761Hz)`, repeating. The two smaller transitions (+3520Hz, +2240Hz) both classify `near_null_either` (a near-null raw sample was hiding in a lopsided blend, same mechanism as the +0.90 fix targeted) - real, direct confirmation that the fix is catching genuine cases, not just relabeling noise. But the third and LARGEST transition (-5761Hz, env=0.277/0.247) does NOT classify as near-null by either test - both raw envelope samples sit comfortably above the 0.05 threshold. This is the first data point in this whole investigation where the more permissive test still can't explain a jump. Before treating that as proof of a second, null-independent mechanism, there's a more basic question worth answering first: were the two RAW (undelayed) `freq_dev_hz` values feeding that -5761Hz step already about that far apart in the raw signal (a genuine discontinuity that's real, just not one an envelope-near-null test is built to catch), or are both individually unremarkable and the large DELAYED step is actually an interpolation artifact from blending across a ~4.28-sample lag during a fast-changing part of the waveform? Added a third diagnostic output pair (`raw_freq_dev_near`/`raw_freq_dev_far` - the two individual raw ring values `interp_ring()` blends, not a derived statistic) threaded through `relative_delay_apply()` -> `diagnostics_set_tx_info()` -> the jump log's per-event struct and 'J' printout, to answer this directly on the next capture. Also corrected a stale code comment in `ssb_mic_test.ino` (right at the `diagnostics_set_tx_info()` call site) that argued "a pure sample delay can't change frequency content" as the reason the pre-delay null_bias/weighted_bias diagnostics didn't need to worry about the delay line - true of `freq_dev_hz`'s own spectrum in isolation, but the wrong question, since the whole mechanism this thread has been chasing lives in the CROSS-alignment between the (delayed) freq_dev and the (mostly undelayed, for positive delay) envelope, not in freq_dev's spectrum alone. Not yet re-bench-tested - no toolchain available in this environment, per standing caveat. Also worth flagging for the next round regardless of what the raw-freq_dev result shows: qualifying-jump rates are now measured at hundreds to low-thousands per second even at delay settings within this project's historically-validated range, which raises a calibration question about whether `JUMP_LOG_THRESHOLD_HZ=300` is catching the rare, dramatic "random jump" symptom that motivated this whole diagnostic, or routine two-tone FM modulation dynamics that were always there and simply hadn't been measured directly before. Full writeup in `null_bias_investigation.md`'s newest 2026-09-12 entry.

**2026-09-12, yet later still: fourth/fifth 'J' captures (delay=+1.75) directly confirmed the user's own suspicion - the low-level jump log carries NO signal about when a human-perceptible frequency shift actually happens, because it's saturated by routine per-null churn regardless.** User's stated concern: "the J table changes all the time even if my observation would say steady state," and a capture taken "as soon as I could" after a `'['`/`']'` sweep-triggered jump - explicitly flagged as likely "after the horse bolted." A second capture, taken deliberately right after Aux SP showed a real, user-observed shift (1000Hz -> 962Hz, i.e. a ~38Hz change to what the user actually watches), came back showing the *exact same* repeating 3-state cycle, same Hz values (~14195358/14199363/14201360, cycling via -4007/+6003/-1996Hz steps), same near-null pattern (2 of 3 states `NEAR_NULL(either)`, one not), and the same climbing lifetime `near_null_either%` (32% -> 57% as total qualifying events climbed from 52407 to 520213) as the "nothing happened" capture immediately before it. Bonus finding from the new raw_freq_dev field: the third, non-near-null transition in this cycle (~-2000Hz delayed step) is fed by two RAW freq_dev values ~8000Hz apart (e.g. near=-6805Hz, far=+1206Hz) - i.e. `raw_delta` is roughly 4x the actual transmitted step, confirming the underlying raw signal really is discontinuous at that instant (not an interpolation artifact) even though the envelope reading (0.18-0.29) doesn't dip below the near-null threshold - the freq_dev signal is still visibly "in transition" from the same null-crossing event, just measured slightly off from the reading's deepest point. Net conclusion: this specific 3-state cycle recurs continuously (hundreds of thousands of events accumulate in minutes) regardless of whether the user's own observed/perceived frequency is stable or has just moved - the 300Hz-per-tick threshold is answering a completely different, much smaller and far more frequent question than "did the frequency someone is watching just change."

**Fix, per the user's own proposed direction ("track the mean freq and trigger off that, so it sees what I see"):** added a SEPARATE tool, deliberately independent of the `'J'` log's per-tick threshold. Tracks a fast (50ms tau - several beat-null cycles' worth) and slow (2s tau) EMA pair of `delayed_freq_dev_hz`; when they diverge by more than `SLOW_JUMP_TRIGGER_HZ` (set to 5Hz - the user's own independently-reported Aux SP visual-read tolerance, not a DSP-internal number), that's treated as a change a human would actually notice. On trigger, a coarse (1.25ms/bin) trace spanning ~50ms before and ~50ms after is LATCHED (frozen, unlike `'J'`'s ring which keeps sliding) so a human-reaction-time delay before checking it can't erase the answer. New `'K'` serial command prints a live "still watching" readout if nothing has triggered yet, or the full before/after trace if it has, then re-arms (resyncing the slow EMA to the fast one, to avoid an immediate re-trigger storm while the slow EMA is still catching up) for the next event. Not yet bench-tested - no toolchain available in this environment. Full design writeup and open questions in `null_bias_investigation.md`'s newest 2026-09-12 entry.

**2026-09-12, yet later still: first real 'K' capture was itself a false positive - found and fixed the cause (EMA seeding/resync bias), added a warm-up hold.** Capture: `before=697.40Hz after=704.63Hz delta=+7.23Hz`, only 3 pre-bins filled (fired within ~4ms of boot/reset) - and the 40-bin post-trace shows the exact same repeating cycle (800/800/800/400Hz, period 4 bins, `env_min=0.001` throughout) both before AND after the "trigger," with no visible transition anywhere in the data. Root cause: both EMAs seed from a single RAW sample on the very first tick (here, 800Hz - one extreme of the ongoing periodic churn, not its ~700Hz time-average); the fast EMA (tau=50ms) converges to the true average within tens of ms while the slow EMA (tau=2s) is still sitting almost exactly at the biased seed - diverging past the 5Hz threshold from initialization bias alone, not a real event. The resync-on-re-arm has the same exposure in miniature, since the fast EMA it snaps the slow one to is itself only 50ms-smoothed. **Fixed**: added a warm-up hold (`FREQ_EMA_WARMUP_MS=8000`, ~4x the slow EMA's tau - the standard first-order settling heuristic, ~1.8% residual error left after 4 tau) on the trigger CHECK only (the EMAs keep updating throughout, so they're already converging on real dynamics) - armed at both first boot and every re-arm after a trigger is read. Side effect, also useful: this naturally debounces against another trigger within seconds of the last one. `'K'`'s "still watching" readout now also shows `warmup_left=...ms` so it's clear when the hold - not the threshold - is what's currently blocking a trigger. Not yet bench-tested. Full writeup in `null_bias_investigation.md`'s newest 2026-09-12 entry.

**2026-09-12, yet later still: second 'K' capture (delay=+3.85, pre-warm-up-fix build) looks like the first genuinely real event this feature has caught - a two-stage transition between structurally different repeating cycles, user-flagged as "a spontaneous change."** Unlike the first (false-positive) capture, this one has a full 40/40 pre/post bins - it had been watching for the full 50ms pre-window before firing, not milliseconds after boot. The pre-trace shows the familiar ~800/800/800/400Hz cycle (mean 700.36Hz, matching `before=`), already slowly widening in amplitude even while "steady." At the trigger, one cycle state jumps from ~400Hz to ~1197Hz, and the post-trace settles into a NEW repeating cycle (800/800/~1197/~1603Hz) - then, partway through the SAME 40-bin post window (bin +26), a SECOND, distinct shift happens: the previously-steady 800/800 pair jumps to 1600/1600 (almost exactly double), while the other two cycle elements continue the same drift trend uninterrupted. So one capture shows two separate discrete jumps, both fully bracketed with clean before/after context - exactly the "before and during" shape this feature was built to deliver, in sharp contrast to what `'J'` produced for the same kind of question. Structural aside: nearly every distinct value across the whole capture sits close to an exact multiple of 400Hz (400/800/1200/1600) - a plausible (unconfirmed) link to the 2nd/4th/6th/8th harmonics of a 200Hz two-tone beat frequency, this project's usual tone spacing. Caveat: this build predates the warm-up fix above, so the exact trigger INSTANT could carry a small resync bias if this wasn't the first-ever trigger since boot - but the underlying two-stage cycle change is directly visible in the raw bin values regardless of the EMA math. Full analysis, including the harmonic-order hypothesis, in `null_bias_investigation.md`'s newest 2026-09-12 entry.

**2026-09-12, yet later still: tone pair for the +3.85 'K' capture confirmed as 700/1700Hz - retracts the earlier 200Hz-beat-harmonic guess, but the corrected numbers line up more precisely.** `expected_center_hz=(700+1700)/2=1200Hz` exactly - and the post-trigger cycle's middle rung (~1197-1200Hz) sits almost exactly on it, a much cleaner match than the retracted guess. Re-reading the capture slot-by-slot (not just cycle means) shows both discrete jumps are ~800Hz steps: pre-cycle `[~800,800,800,~400]` -> first jump moves the `~400` slot to `~1197` and one `800` slot to `~1603` (both ~+800Hz) -> second jump (bin +26) moves the remaining two `800` slots to `1600` each (`+800Hz` exactly). All values visited sit on a ~400Hz grid, and 400/800 are exact multiples of 100Hz - the same common factor 700/1700/their 1000Hz beat all share (7x/17x/10x), consistent with this project's established phase-accumulator/SAMPLE_RATE_HZ tone-locking mechanism. Plausible structural link, not yet a full derivation of why 800Hz specifically. Full writeup in `null_bias_investigation.md`'s newest 2026-09-12 entry.

**2026-09-12, yet later still: two more 'K' captures (one "[] scan induced", one "spontaneous") both came back showing a PERFECTLY flat, non-drifting repeating cycle across the entire 50ms pre + 50ms post trace, despite a real 8-11Hz fast/slow gap having triggered them - added two targeted diagnostics rather than guessing further from single captures.** Both traces are essentially bin-for-bin identical before and after (e.g. `800.0/800.0/800.3/399.7Hz` repeating exactly, no visible transition anywhere) - if the signal had truly been this stable for as long as it's been running, both EMAs should long since have converged to within a fraction of a Hz of each other (the cycle-repeat rate is far above either EMA's cutoff). An 8-11Hz gap surviving a locally-flat 100ms window is best explained by a real change that happened AND fully resolved on a timescale longer than 50ms but shorter than the slow EMA's ~2s memory - i.e. a continuous drift too gradual to show a visible slope over 50ms (this project already has direct precedent for continuous multi-second drift, from the +3.85 capture's own within-window creep) but fast enough to separate a 50ms average from a 2s one.

Added two things to test this directly rather than re-guessing from the next single capture: (1) `relative_delay_get_last_change_ms()` (`relative_delay.h`/`.cpp`) timestamps every `'['`/`']'`/`'''`/`';'`/preset-load delay change; `'K'` now reports "relative_delay was last changed Xms before this trigger" (or "has not been changed since boot"), directly correlating a trigger against the user's own recent actions without relying on memory/labels. (2) A second, much-longer-timescale companion trace (`EMA_TREND_LEN=80` samples every `EMA_TREND_SAMPLE_TICKS`=25ms, ~2s of history total, matching the slow EMA's own tau) that snapshots the fast/slow EMA VALUES themselves (not raw bin data) periodically, frozen at the trigger the same way the fine trace is - directly showing whether the gap built up as a smooth multi-second ramp or something more abrupt, without needing to grow the expensive fine-grained trace itself. Not yet bench-tested. Full writeup in `null_bias_investigation.md`'s newest 2026-09-12 entry.

**2026-09-12, yet later still: the 8-second warm-up above was undersized, not wrong - replaced single-sample EMA seeding with a two-stage boot-settle-then-snap init.** A re-run of the "[] scan induced" capture (t=9239ms, well past the 8s hold) fired again, and the new TREND block (added specifically to catch this kind of thing) showed exactly why: `fast` sat flat at 699.07-699.08Hz for the ENTIRE visible 2-second history while `slow` was still visibly, smoothly climbing from 666.23Hz toward it (687.31Hz by the last sample) - a still-converging exponential curve, not noise. That only happens if the original single-raw-sample seed was itself a large outlier (plausibly one of the near-null freq_dev spikes this project has documented elsewhere reaching thousands of Hz) - for a seed error that large, "4 time-constants ~= 98% converged" still leaves a residual of tens of Hz, comfortably enough to keep tripping a 5Hz threshold for seconds past the hold. Root-cause fix: stop seeding `slow` from a raw sample at all. Now only `fast` seeds from the first raw tick; it runs alone for `FREQ_EMA_BOOT_SETTLE_MS=500` (~10x its own 50ms tau, so ~99.9995% converged even from a large seed error); THEN `slow` is snapped directly to `fast`'s already-converged value (a clean transfer, never touching a raw sample), with the existing `FREQ_EMA_WARMUP_MS=8000` hold kept afterward purely as residual insurance rather than doing the main work. New/renamed statics in `diagnostics.cpp`: `s_freq_ema_fast_inited` (was `s_freq_ema_inited`), `s_freq_ema_slow_inited` (new), `s_freq_ema_boot_settle_ticks_left`/`FREQ_EMA_BOOT_SETTLE_TICKS` (new). Both the trigger check and the trend-ring recording are now gated on `s_freq_ema_slow_inited` (in addition to the existing warm-up/state checks) so nothing is evaluated or recorded off a not-yet-meaningful slow value. The re-arm-on-print logic is unchanged (still resyncs `slow=fast` and re-arms the 8s hold) - remains valid since `s_freq_ema_slow_inited` stays true permanently once set at boot. Not yet bench-tested - no toolchain available in this environment, per standing caveat.

**2026-09-12, yet later still: three more real 'K' captures (all delay=+2.70, all env_min pinned at 0.238 for the full 100ms window, all with flat-to-near-flat 2s trends ruling out both the boot-seeding bug above and any delay-change confound) show the same underlying picture in much sharper detail - a cycle transition isn't one instantaneous jump, it's a CASCADE of several independent ~400/800Hz steps landing in different repeating-cycle "slots" at different times over a ~5-40ms window.** Three captures: `delta=+17.93Hz` (before=700.43, after=1093.40), `delta=+21.35Hz` (before=700.58, after=1043.65, delay unchanged for 181535ms beforehand), and `delta=+15.54Hz` (before=700.43, after=1013.41, delay unchanged for 335207ms beforehand, user-labeled "finished up at nominal zero"). The richest (`+21.35Hz`) shows this most clearly: its 4-slot repeating cycle has two "clean" slots that each take one 800Hz step at different times (one AT the trigger, matching how the trigger is defined; the other ~5ms later), and two "near-null-blended" slots whose bin-averaged values keep summing to a constant (1200Hz pre-trigger, matching `expected_center_hz` for 700/1700 exactly) but that constant ITSELF steps twice more, in the same 800Hz quantum - first to 2000Hz (~11ms post-trigger), then to 2800Hz (~26ms post-trigger). Same underlying event, every slot of the cycle eventually reflects it, just staggered in time - and even the "smooth drift" seen in earlier near-null-blended captures looks like it's really discrete 800Hz steps smeared across bins by the blend/average. The `+15.54Hz` capture shows the same staggering with cleaner (non-blended) values: `[800,800,800,400] -> [1200,800,1600,1600]` via four separate steps landing at the trigger, +4, +15, and +22 bins - including the first observed clean 400Hz-sized single step in this whole investigation, which reinforces rather than contradicts the established 400Hz-grid/800Hz-quantum picture. All three captures keep `env_min` pinned at a constant 0.238 across the entire 80-bin window, before and after - continuing to show zero correlation with any near-null envelope dip at the coarse per-bin level. Net implication for the 'K' feature itself: a trigger correctly marks the START of a real transition, but full settling can take several tens of ms longer than the trigger instant - worth remembering when reading any 'K' trace's "after" value, and itself a data point for the boot-settle fix above (multi-stage settling is a real property of this signal, not an artifact of bad EMA seeding). Full step-by-step decode of all three in `null_bias_investigation.md`'s newest 2026-09-12 entry. Not yet asked/confirmed: whether these three were also on 700/1700 - the numbers (1200Hz center, 800/400Hz steps) are consistent with that pair but not independently confirmed for these specific captures.

**2026-09-12, yet later still: confirmed - all three captures above were also on 700/1700Hz.** Closes the open question from the previous entry; the 1200Hz-center/400-800Hz-step grid decode stands as-is, now on a fully-confirmed tone pair rather than an inferred one.

**2026-09-12, yet later still: a genuinely scan-induced 'K' capture (delay=+5.23, "relative_delay was last changed 1ms before this trigger") - first real validation of the delay-timestamp diagnostic, and a qualitatively messier, more null-adjacent signature than the three +2.70 captures above.** User-labeled "scan induced now at 990" - and for the first time, the delay-change timestamp actually confirms it (1ms before the trigger, vs. the two earlier "scan induced"-labeled captures that came back "not changed since boot"/minutes-old, which is exactly why that diagnostic got built). `delay=+5.23` is well outside this project's historically-validated 2.00-2.10 IMD-compromise window - in the same "large/incidental" territory as the earlier +4.28/+4.60 'J' captures. Two things mark this as a different regime from the three clean +2.70 captures logged above: `env_min` is NOT pinned high this time - it climbs from 0.040 (near or below the 0.05 near-null threshold used elsewhere in this project) up to 0.086 over the 81-bin window, i.e. genuinely null-adjacent, not the comfortably-clear 0.238 seen throughout the +2.70 data; and the transition itself is messier - two of the four repeating-cycle slots jump immediately (at the trigger/+4) to non-quantized values that keep drifting for the whole 50ms post-window, while the other two hold clean 800.0Hz values for ~30ms before taking one clean +800Hz step near bin +26/+27. Reads as evidence that the clean, null-independent, 400Hz-grid cascade mechanism characterized above and the messier, genuinely-near-null-influenced behavior documented earlier (`+4.28`/`+4.60` 'J' captures) are two distinguishable regimes, separated by how far delay sits from the tuned ~2.0-2.1 window. Full decode in `null_bias_investigation.md`'s newest 2026-09-12 entry.

**2026-09-12, yet later still: fifth real 'K' capture ("spontaneous," delay unchanged for 147312ms beforehand) reproduces the exact sum-conserved-pair-plus-two-clean-steps signature from the richest +2.70 capture above, and env_min sits at a THIRD, intermediate level (~0.09) not seen before.** Decodes identically in structure to the `+21.35Hz` capture: two "clean" slots each take one isolated +800Hz step at different times post-trigger (~12ms and ~28ms here), while a near-null-blended ADJACENT pair of bins (not same-phase-mod-4, but the two bins straddling a beat-null crossing each cycle) keeps its bin-averaged SUM pinned at 1200Hz pre-trigger (matching `expected_center_hz` for 700/1700 again) and then steps that sum by the same +800Hz quantum to 2000Hz post-trigger - with the very last printed bin (+40) hinting at a further step toward 2800Hz just starting as the 50ms capture window runs out, same cascade timing order-of-magnitude as before. `env_min` this time sits at a steady ~0.089-0.093 across the whole trace - comfortably above the 0.05 near-null threshold (so not "near-null" by the existing test) but well below the ~0.238 seen in the earlier clean captures, a third distinct env_min regime alongside the ~0.238 "clear" and ~0.04-0.09-climbing "scan-induced/messy" ones already on record. Worth watching whether this intermediate level turns out to correlate with anything (not yet investigated). Full decode in `null_bias_investigation.md`'s newest 2026-09-12 entry.

**2026-09-12, yet later still: two more 'K' captures, both at relative_delay=+2.33 (a new value, not the usual +2.70), REPLICATE the intermediate ~0.09 env_min level exactly and surface a new finding - the single bin that defines a trigger is often a one-bin transient overshoot, not a real sample of either the old or new steady state.** Sixth capture (`delta=+14.77Hz`, delay unchanged 300985ms beforehand) and seventh (`delta=+18.65Hz`, delay unchanged 454666ms beforehand) both read `env_min~=0.089-0.093` throughout - matching the fifth capture's ~0.09 level almost exactly, while the three +2.70 captures logged earlier all read ~0.238 and the one +5.23 capture read 0.04-0.086. Two independent captures at the same non-2.70 delay landing on the same env_min level is a real replication, not a coincidence - `env_min` looks like it depends fairly directly on exactly what `relative_delay` is set to, which fits the already-established mechanism (delay controls how the envelope ring's read point aligns with wherever freq_dev's near-null spike falls).

The seventh capture is the cleanest, most complete transition decoded in this whole investigation - four cycle slots snap to new values with no blending noise at all: three settle at a new level +800Hz above their old one (one within ~5ms, two together around ~35ms post-trigger) and the fourth settles at a new level +400Hz above its old one - but that fourth slot's very FIRST post-transition reading (the trigger bin itself) shows 1600.0Hz, a full +800Hz jump, before its very next occurrence drops to the real, sustained +400Hz level (1200.0Hz) it holds for the rest of the trace. The sixth capture shows the identical shape: the trigger bin reads 1199.3Hz, but that slot's next occurrence is 800.0Hz - a real, clean +400Hz step from its ~399.5Hz baseline, with the trigger-instant reading itself never recurring. Read as strong evidence that the ONE bin `'K'` singles out as "the trigger" is specifically likely to straddle a rapid transition and can show a distorted, transient blended value that isn't representative of either the pre- or post-transition steady state - worth remembering when reading any 'K' trace's own trigger-bin line, not just its `before=`/`after=` summary. Full decode of both captures in `null_bias_investigation.md`'s newest 2026-09-12 entry.

**2026-09-12, yet later still: CORRECTION - found and fixed an off-by-one bug in my own slot-analysis, affecting several capture write-ups above; then decoded two more captures with the fixed method.** While decoding an eighth 'K' capture, a continuity check failed for a slot I expected to match cleanly - root cause: `'K'`'s trace labels bins `-40..-1` (pre) then `+1..+40` (post) with no "bin 0", so matching a post-trigger bin back to its pre-trigger slot needs `phase=(bin_index-1) mod 4`, not the same `(bin_index+40) mod 4` formula the pre-trigger side uses - I'd been using the latter for both, silently shifting every post-trigger slot's correspondence by one. This does NOT affect the sum-conservation findings (computed from direct adjacent-bin arithmetic, unaffected by the labeling) or the overall "cascade of clean 400Hz-grid steps over 5-40ms" conclusion, but it DOES retract three specific claims: the "+21.35Hz" capture's phantom second clean slot at bin+4 (that data point is actually the trigger slot's own continuation; the real fourth slot only starts moving at ~48ms, right at the edge of the window); the "nominal zero" capture's claimed second/400Hz-sized step on the trigger slot (the trigger slot actually just holds its one +800Hz step forever - the extra step belongs to a different, uninvolved slot); and, most notably, the entire "trigger bin is a one-bin transient overshoot that never recurs" finding from the two delay=+2.33 captures - re-verified, all four slots in each of those captures take clean, PERSISTING +800Hz steps with no reversion at all. Full corrected re-derivation of all four affected captures in `null_bias_investigation.md`.

**Same entry, continued: eighth and ninth 'K' captures (both delay=+2.33) give a third independent replication of the 1200->2000->2800Hz staged sum-progression, and one of them catches the near-null pair directly triggering 'K' itself.** Both read `env_min~=0.09` again (now four captures at this delay, all matching). The eighth shows the full three-stage progression (1200->2000Hz at the trigger, ->2800Hz about 33ms later) alongside one slot that never moves and one that takes an isolated +800Hz step at ~12ms. The ninth (`delta=+10.37Hz`, the smallest yet) shows the trigger itself firing on a NON-clean value (+532.8Hz) - explained once you check the pair-sum instead of either raw side: the sum holds a clean 1200Hz pre-trigger, steps to 2000Hz exactly at the trigger, and stays there for the rest of the trace, with the "unclean" trigger reading being just an asymmetric split of that clean step across the pair's two blended raw components. This directly explains why a trigger can sometimes land on a value that isn't itself a multiple of 400 - the quantized quantity is the near-null pair's SUM, not necessarily either individual reading. Full decode in `null_bias_investigation.md`.

**2026-09-12, yet later still: tenth 'K' capture (delay=+2.33, `delta=+20.99Hz`) - the cleanest, most fully-quantized transition yet, zero interpolation blending anywhere, including one slot that hops TWICE within the same event.** Fifth capture at this delay, `env_min` again ~0.089-0.093 (five for five now). Pre-cycle exact `[800,400,800,800]` with no drift at all (unlike most other captures, nothing here is fractionally blended near a null). Corrected-method decode: one slot never moves (flat 800.0Hz for the entire 81-bin trace); the trigger slot takes one clean +800Hz step and holds forever; a third slot takes one clean +800Hz step, but very late (~40ms post-trigger); and a fourth slot takes TWO clean +800Hz steps in the same capture - 400.0Hz -> 1200.0Hz almost immediately (~2.5ms post-trigger), then 1200.0Hz -> 2000.0Hz again later (~37ms post-trigger) - visiting three distinct grid levels within one 50ms window. Fully consistent with everything decoded so far, just unusually clean - no drift to disentangle from the discrete steps. Full decode in `null_bias_investigation.md`.

**2026-09-12, yet later still: eleventh 'K' capture (delay=+2.33, `delta=+15.32Hz`) - sixth-for-sixth on the env_min correlation, and this time the TRIGGER slot itself is the one that hops twice.** Another fully-quantized, zero-drift pre-cycle (`[800,800,800,400]`, exact). One slot never moves; one takes a quick clean +800Hz step (~7ms post-trigger); one takes a later clean +800Hz step (~35ms); and the trigger slot itself, having already jumped 400->1200Hz at the trigger, takes a SECOND +800Hz step to 2000.0Hz very late (~44ms post-trigger, right at the edge of the visible window) - mirroring the tenth capture's "one slot hops twice" finding, except this time it's the trigger's own slot doing the double hop rather than a bystander. Full decode in `null_bias_investigation.md`.

**2026-09-12, yet later still: twelfth 'K' capture, a new delay (+3.08, changed 4.6s before the trigger) - genuinely near-null (`env_min=0.025`, below the 0.05 threshold, the first "spontaneous"-labeled capture to cross it) yet the CLEANEST, most extended quantized cascade yet, and it complicates the delay-vs-env_min picture.** User flagged the delay move but wasn't sure it was the direct cause ("this happened later I think") - the delay change was 4614ms before the trigger, longer than the 2s TREND window can confirm or rule out either way. `env_min` sits at a flat 0.025 across the ENTIRE 81-bin trace - genuinely near-null by the existing threshold, unlike every other "spontaneous" capture this session. Despite that, the transition itself is exceptionally clean: the near-null-blended adjacent pair's SUM steps in the established 800Hz quantum not twice or three times but FOUR levels - 1200Hz (pre) -> 2000Hz (~7ms) -> 2800Hz (~27ms) -> 3600Hz (~42ms, right at the window's edge) - the longest staged cascade decoded so far, while two other slots (one silent, one holding the trigger's own single +800Hz step) round out the four-slot cycle. This directly contradicts the earlier working assumption that "near-null envelope = messy, non-quantized" (drawn from the one delay=+5.23 capture) - here, near-null and cleanly-quantized coexist. It also complicates the delay-vs-env_min correlation: `+3.08` is almost exactly as far from `+2.70` as `+2.33` is (0.38 vs 0.37), yet reads `env_min=0.025` versus `+2.33`'s consistent ~0.09 - not a simple function of distance from the +2.70 "sweet spot," more like a narrow, sharply-peaked alignment specific to +2.70 itself. Full decode and discussion in `null_bias_investigation.md`.

**2026-09-12, yet later still: thirteenth 'K' capture, second at delay=+3.08 - `env_min=0.025` again exactly (confirming near-null is a real, repeatable property of THIS delay, not a one-off), and a new step type: a slot jumping TWO quanta at once (+1600Hz) instead of two separate +800Hz hops.** Genuinely spontaneous (delay unchanged for 158246ms beforehand). Zero-drift, fully-quantized pre-cycle again (`[800,400,800,800]`). Two slots never move at all; the trigger slot takes its one +800Hz step and holds forever; the fourth slot holds its original 400.0Hz value for ~17ms post-trigger, then jumps DIRECTLY to 2000.0Hz (a single +1600Hz jump, skipping the intermediate 1200Hz grid point entirely) at ~22ms and holds steady. This is a new pattern - previous "double hop" captures (tenth, eleventh) showed a slot visiting an intermediate grid point via two separate steps at different times; here the slot skips straight past it in one clean jump. Full decode in `null_bias_investigation.md`.

**2026-09-12, yet later still: fourteenth 'K' capture, third at delay=+3.08 - `env_min=0.025` exactly again (third-for-third), and a SECOND replication of the 4-level 1200->2000->2800->3600Hz staged sum-cascade first seen in the twelfth capture.** Genuinely spontaneous (delay unchanged for 311888ms beforehand). One slot never moves; the trigger slot takes its one +800Hz step and holds forever; the near-null-blended adjacent pair's sum steps through all four established levels again - 1200Hz (pre) -> 2000Hz (~7ms) -> 2800Hz (~30ms) -> 3600Hz (~44ms, right at the window's edge) - matching the twelfth capture's progression almost exactly in both level count and rough timing. Between the three back-to-back +3.08 captures now on record, this delay setting looks like a genuinely distinct, repeatable regime: consistently near-null (`env_min=0.025`, unlike +2.33's ~0.09 or +2.70's ~0.238) yet consistently capable of the longest, cleanest quantized cascades decoded so far - reinforcing that near-null and clean-quantization are not opposites here. Full decode in `null_bias_investigation.md`.

**2026-09-12, yet later still: fifteenth 'K' capture, right after loading Preset 3 (`relative_delay=+2.00` exactly, matching that preset's documented value) - the deepest near-null reading yet (`env_min=0.001`, essentially AT the null, ~25x lower than the +3.08 captures' 0.025) yet STILL a fully clean, 800Hz-quantized cascade.** Delay was changed 28659ms (~29s) before the trigger - plausibly related to loading the preset, not certain. Because `+2.00` is an exact integer sample delay, `interp_ring()`'s fractional blend weight is zero - there's no interpolation smearing between adjacent ring entries at all, which likely explains why `env_min` reads a single, unwavering 0.001 across all 81 bins rather than the ~0.09-0.238 range with sub-percent wobble seen at fractional delays. Despite being far deeper into "near-null" territory than any capture so far, the decode is completely clean: one slot never moves, two slots each take an isolated +800Hz step (~7ms and ~33ms), and the trigger slot itself double-hops (400->1200Hz at the trigger, then ->2000Hz again at ~44ms) - same repertoire of behaviors seen throughout this investigation, no extra messiness. This strengthens the case (first raised with the twelfth capture) that near-null envelope readings do NOT imply messy, non-quantized transitions in general - the one capture that WAS messy (delay=+5.23, mid-scan) was very likely messy because of being a large, actively-changing FRACTIONAL delay specifically, not because of proximity to null. Full decode in `null_bias_investigation.md`.

**2026-09-12, yet later still: sixteenth 'K' capture, second at Preset 3's delay (+2.00), genuinely hands-off (~30 minutes since anything was touched) - confirms `env_min=0.001` exactly again, and shows the first capture with TWO completely silent slots.** Same clean, zero-drift, fully-quantized character as the previous capture: two of the four cycle slots never move at all across the entire 81-bin trace (the first time this session two slots have stayed silent in one capture, not just one); the trigger slot takes its +800Hz step and holds forever; the fourth slot takes two separate +800Hz hops (400->1200Hz at ~22ms, then 1200->2000Hz at ~27ms, only 5ms apart) - the "visits the intermediate grid rung via two steps" pattern from captures ten/eleven, not the "single +1600Hz jump" pattern from the thirteenth capture. Second-for-second confirmation that `relative_delay=+2.00` reliably produces `env_min=0.001`. Full decode in `null_bias_investigation.md`.

**2026-09-12, yet later still: seventeenth 'K' capture (delay=+2.00, `delta=-5.01Hz`) - a genuinely NEW category: a smooth, continuous, non-quantized drift, not a discrete jump.** Third confirmation of `env_min=0.001` at this delay. Unlike every capture so far, there is no discrete step anywhere in this trace - the near-null-blended pair simply keeps redistributing smoothly between its two components (one rising ~811.9->820.9Hz, one falling ~387.7->378.3Hz) across the ENTIRE 100ms window, with their SUM pinned at exactly 1200.0Hz throughout, never stepping to 2000 or any other level. The other two slots stay completely flat. This is the smallest-magnitude trigger captured so far (barely over the 5Hz threshold, matching the user's own reported +/-5Hz Aux SP visual tolerance almost exactly) and the first NEGATIVE delta decoded with the corrected method - both consistent with 'K' correctly catching a borderline, continuous drift rather than a discrete event. Confirms 'K' works as designed across two distinct underlying phenomena: the discrete 800Hz-quantized jumps decoded in every capture before this one, and this smooth, continuous, non-quantized redistribution within the near-null pair, likely the same slow beat-crossing-phase precession mechanism just not happening to cross a discrete grid boundary within the visible window this time. Full decode in `null_bias_investigation.md`.

**2026-09-12, yet later still: eighteenth 'K' capture, fourth at delay=+2.00 - fourth-for-fourth on `env_min=0.001`, nothing qualitatively new but a clean confirmation of the established repertoire.** `delta=+21.18Hz`, delay unchanged for 489689ms beforehand. One slot never moves; the trigger slot takes its one +800Hz step and holds forever; one slot double-hops via two separate steps visiting the intermediate grid rung (400->1200Hz at ~7ms, 1200->2000Hz at ~32ms, matching the pattern from captures ten/eleven/sixteen); one slot takes a single, very late +800Hz step (~43ms, near the edge of the window). Full decode in `null_bias_investigation.md`.

**2026-09-12, yet later still: nineteenth and twentieth 'K' captures, fifth/sixth at delay=+2.00, user-labeled a "noisy mode" episode - both decode as ordinary clean, fully-quantized cascades, and the twentieth is the first capture where all four cycle slots step with none left static.** Nineteenth (`delta=+15.61Hz`, hands-off ~10.7min): one slot never moves, the trigger slot takes its one +800Hz step and holds, and two more slots each take an isolated +800Hz step independently but close together (~16ms and ~22ms post-trigger). Twentieth (`delta=+17.68Hz`, hands-off ~13.3min, user: "jumped to 1006 and noise gone"): same clean repertoire, but for the first time this session every one of the four slots takes exactly one +800Hz step somewhere in the window (~trigger, ~14ms, ~27ms, ~46ms) with none remaining static - still fully 400Hz-grid-quantized, just an unusually complete sweep. Both keep `env_min=0.001` (fifth/sixth-for-fifth/sixth at this delay). Neither trace's internal decode looks "noisy" on its own - most likely the user's higher-level "noisy mode" impression was a rapid SUCCESSION of individually-clean triggers like these firing over several minutes as the mean climbed 700 -> ~1018 -> ~1042Hz, rather than any single messy transition. Full decode of both in `null_bias_investigation.md`'s newest 2026-09-12 entries.

**Same episode, continued: twenty-first 'K' capture (`delta=-5.00Hz`, seventh at delay=+2.00, user: "back down to 997") closes out the "noisy mode" run with the second-ever smooth, continuous, non-quantized drift trigger (the category first seen in the seventeenth capture).** No discrete step anywhere - two slots stay flat at 800.0Hz, and the near-null-blended pair smoothly redistributes between its two components across the entire 100ms window (one rising ~814->827Hz, one falling ~386->371Hz) with their sum pinned at a constant ~1199-1200Hz throughout, matching `expected_center_hz` for 700/1700. `env_min=0.001` again (seventh-for-seventh). Smallest-magnitude trigger of the three (right at the 5Hz threshold, negative) - consistent with the smooth-drift mechanism only barely crossing the divergence threshold rather than blowing through it like a discrete 800Hz step. Together, the three captures read as two sharp discrete jumps followed by one small continuous settle, matching the user's own reported sequence (973 -> 1006 -> 997) closely enough to support the "rapid succession of clean events" explanation for the perceived noise. Full decode in `null_bias_investigation.md`.

**2026-09-12, yet later still: added a 'Z' serial command for a clean, on-demand reboot** (`serial_commands.cpp`) - flushes a confirmation reply, short delay, then `ESP.restart()`, so getting back to a known-clean starting point between bench captures no longer needs a physical power-cycle. Not bench-tested.

**Same day: checked the "relative_delay's own ring buffer explains the staggered multi-slot cascade timing" hypothesis against the actual numbers - REFUTED, and the same check surfaced a much bigger candidate finding about the period-4 structure itself.** `PHASE_DELAY_MAX_SAMPLES=8` at `SAMPLE_RATE_HZ=16000` gives the ring a full capacity of only 500 microseconds - two orders of magnitude too short to explain the 1.24-46ms staggered timing seen throughout this investigation. That hypothesis is dropped; what actually causes the staggered timing remains open. But looking for an alternative explanation surfaced something worth taking seriously: each printed 'K' bin is the MEAN of 20 raw ticks (`SLOW_TRACE_BIN_TICKS`), and the confirmed 700/1700Hz tone pair has a 1000Hz beat - a 16-tick period at this sample rate. `gcd(20,16)=4` means any 20-tick-window average of a genuinely 16-tick-periodic signal is arithmetically guaranteed to repeat with period 4 IN BIN-INDEX SPACE, independent of delay, magnitude, or anything else project-specific. That's a strong candidate explanation for why every single capture this whole session - all five delay values, every sign and size of jump - has shown the identical period-4 "slot" structure with zero exceptions: a pattern that universal usually points at the measurement process, not the thing being measured. A quick synthetic simulation supports the qualitative shape but doesn't cleanly reproduce the exact clean levels seen on real hardware, most likely because the simulation doesn't model the real ADC/filter chain feeding the actual atan2-based freq_dev computation. Importantly, this does NOT retract any specific value/timing logged in this investigation, and does NOT explain the staggered cascade timing itself (an alias artifact of this kind would only offset different slots by about one alias period, ~5ms, not the 30-40ms gaps repeatedly observed) - flagged as a testable candidate mechanism, not a settled conclusion. Full numerics writeup in `null_bias_investigation.md`.

**Twenty-second 'K' capture: FIRST at delay=+0.00 (the untouched boot default) - a new env_min data point (0.200) that further undercuts any simple "distance from +2.70" story, user-flagged as the biggest shift seen yet ("sending this as its the biggest shift Ive seen to 950").** `delta=+20.64Hz`, only ~155s since boot. `env_min=0.200` - a new level, and notably close to +2.70's own ~0.238 despite `+0.00` being roughly as far from +2.70 as the messy +5.23 capture was, while the numerically CLOSER +2.33/+3.08 read far more near-null (~0.09/0.025). Decode is otherwise fully in the established repertoire: two silent slots, the trigger slot's single persisting step, and one slot double-hopping via the intermediate grid rung at the same ~17ms/~32ms timing seen at other delays. Full decode in `null_bias_investigation.md`. **Addendum, same capture, re-sent with more context ("hands off from 950 -> 1026"):** the user resent the identical trace (same `t=154655ms`, identical bins - not a new trigger) purely to report that the frequency kept climbing hands-off well past this trace's own `after=1003.01Hz` reading, through ~950Hz and on to ~1026Hz, all without a second 'K' trigger firing. Consistent with the standing caveat that a trace's `after=` value only reflects the fast EMA at the moment the 50ms post-window closes, not necessarily the final settled value - full settling at delay=+0.00 (this session's least-tested setting) apparently continues for at least several more seconds beyond what any single trace can show, without ever diverging by another 5Hz from a shifting baseline sharply enough to re-trigger.

**2026-09-12, yet later still: user question via the new 'Z' reboot - repeating the same manual delay sweep through ~+4.5 sometimes triggers 'K' and sometimes only gives a small non-triggering wobble; is that a boot-initial-condition effect?** Two candidate explanations, not yet distinguished: (1) genuine boot-to-boot hardware timing jitter shifting where the tone's null crossings land relative to the sample grid - but this project's own prior finding (the ~264us first-tick `spi_us` spike, explicitly confirmed "reproducible across independent reboots... not a scheduling fluke") argues boot timing is actually quite deterministic, not jittery; or (2) the `'['`/`']'` sweep is manually keyed (confirmed - no automated delay-scan code exists), so the exact instant it passes through +4.5 isn't reproducible to millisecond precision, and the confirmed 700/1700Hz tone pair's 1000Hz beat means a full null-crossing cycle repeats every ~1ms - enough for ordinary human keypress timing jitter alone to land on either side of the same knife-edge this whole investigation has already established is very thin. One clean test distinguishes them: repeat the sweep several times WITHOUT rebooting in between - if the same mixed outcome still shows up, it's sweep-timing jitter, not the reboot. Flagged for the user to try. Full writeup in `null_bias_investigation.md`.

**2026-09-12, yet later still: the no-reboot repeat test came back, and it's more interesting than either candidate explanation predicted - many solid sweeps through ~+4.5, then a clean transition into "reliably" jumping, all within ONE boot (no 'Z' pressed at all).** This rules out pure boot-initial-condition jitter outright (nothing rebooted). But it doesn't fit pure keypress-timing coincidence cleanly either - a real coin-flip-per-attempt process should scatter solid/jumpy outcomes randomly through the session, not cluster into "long solid stretch, then reliably bad." That points at a third possibility not considered before: something in the system itself slowly drifted between the two stretches during a single boot - a physically plausible (but untested) candidate being oscillator/crystal thermal warm-up drift over the first several minutes shifting the tone/sample-clock alignment this whole investigation has shown is already razor-thin. The first trigger in this run landed at `t=308312ms` (~5.1 minutes after boot) - consistent with, though not proof of, a warm-up timescale. Worth tracking in future sessions whether "goes bad" tracks elapsed uptime specifically.

**Twenty-third capture, decoded from the same episode: first-ever negative/near-zero `relative_delay` (-0.02), and the first capture where `env_min` visibly changes WITHIN one trace, closely tracking a delay change caught mid-window.** `delta=+15.46Hz`, delay last changed 17ms before the trigger - and `env_min` reads 0.010 for the first ~33ms of the pre-window then drops to 0.001 right around where that delay change happened (~15-16ms before trigger, matching the 17ms timestamp within about a bin's resolution) and stays there through the whole post-trigger window. This is the cleanest direct evidence yet that `env_min` tracks whatever delay is CURRENTLY active, quickly. `env_min=0.001` for delay=-0.02 extends the "near-integer delay -> deep null" pattern to a third value (after `+2.00`) - but sits in real tension with the earlier `delay=+0.00` capture reading `env_min=0.200`, not similarly deep, despite also being an exact integer. Most likely explanation: an unexamined confound (that capture was on whatever boot-default OTHER settings were active, not Preset 3's) rather than a breakdown of the integer-delay mechanism - flagged as an open question, not resolved. The transition itself is a new sub-pattern: two slots step essentially simultaneously with the trigger, and the other two step together (also essentially simultaneously with each other) about 30ms later - still the same "one clean +800Hz step per slot" primitive, just an unusually tight paired timing structure. Full decode in `null_bias_investigation.md`.

**2026-09-13: mechanism reference - why does relative_delay shift the output frequency at all (moving on from the Z-reboot coincidence thread).** Short answer, and it's the same mechanism this project already established well before this session's 'K' work (SDR-confirmed via the pre-existing `null_bias2` diagnostic): `relative_delay` isn't a frequency control, it's a pure re-timing tap between the phase and envelope output paths, implemented as linear interpolation between two adjacent raw ring samples. `freq_dev_hz` is a phase-derivative computation that's genuinely, mathematically unstable right at a beat-envelope null (a known, expected property of this whole SSB technique, not a bug - two adjacent raw samples can differ by thousands of Hz there). Since `relative_delay` picks which two samples get blended, different delay values select a genuinely different value out of that instability, and that value is exactly what gets sent to the AD9851 - so it's really transmitted, not a diagnostic artifact. What's still open (and what this whole session's 'K' investigation has been circling): why a value selected at one null-crossing then persists cleanly for tens of milliseconds rather than reverting the next beat cycle. One candidate (a dsp_task tick-skip permanently offsetting the two-tone phase accumulators) was considered and doesn't cleanly hold up on inspection - flagged as still open, not resolved. Full writeup in `null_bias_investigation.md`.

**2026-09-13: answered from source - Preset 2<->3 live switches do NOT preserve AM/PM sync cleanly, because they silently swap group-delay-equalizer filter designs and force-reset that filter's state every time.** Checked `settings.h`'s actual preset table: Preset 2 and 3 are identical except `relative_delay_samples` (2.03 vs 2.00, trivial) and `env_gdeq_variant` (DEFAULT vs CANDIDATE_B - two genuinely different two-section allpass filter designs for the envelope-path group-delay equalizer, not a small tweak of the same one). `envelope_gdeq_set_variant()` unconditionally zeroes both filter sections' internal IIR state on every real value change - a documented, deliberate choice ("switching live never feeds a mismatched coefficient/state pair into the very next sample") but the alternative it picks (force-zeroing a running filter's memory mid-waveform) is itself a real, physical discontinuity in the envelope path, at the exact instant of every 2<->3 switch. This explains all three reported symptoms: something visibly happening on a live switch (yes, a real transient); the "~0.5 sample, not 1" read (consistent with this being a continuous group-delay DIFFERENCE between two distinct filter designs - candidate B's own docs give 136.0us vs default's 156.5us full-band dispersion - not a discrete sample-count artifact at all); and "sometimes 1 and back to 2/3 needed" (Preset 1 has gdeq off entirely, so routing through it also fires the separate off->on reset path in `envelope_gdeq_set_enabled()`, on top of the variant-change reset that already fires on a direct 2<->3 switch - not fully resolved which specifically matters, flagged as needing a bench comparison). Bottom line: these two presets are not safely swappable live without a brief, real envelope-path glitch - that's how the code is written, not a bug. Full writeup in `null_bias_investigation.md`.

**2026-09-13: user asked whether "reset fixes it" confirms a bad/desynced state - yes, but there look to be two separate effects bundled together, and one of them isn't transient at all.** (a) The already-documented gdeq filter-state zeroing is a real but purely transient effect, identical whether reached via Preset 1 or a direct 2<->3 switch - settles out in a handful of samples, says nothing about either preset's steady state. (b) A separate, genuinely NOT-transient structural mismatch specific to Preset 2: its gdeq variant (DEFAULT) was explicitly fit, per `envelope_gdeq.h`'s own history, "against the bare analog filter ALONE, before either [ampeq] shelf existed" and is documented as "known... to NOT flatten well once ampeq's shelf(s) are on" - but Preset 2 itself runs with BOTH ampeq shelves on. Preset 3's variant (candidate B) was specifically fit against the shelves-on curve, so it's actually matched to the config both presets run. This is a permanent mismatch baked into Preset 2's own definition, not something settling time fixes. Suggested a way to tell which the user is actually seeing: a brief, switch-triggered "bad" feeling that settles out is (a); a "bad" feeling that persists indefinitely while just sitting on Preset 2 untouched is (b), and would need Preset 2 re-paired to a shelf-matched gdeq variant, not a reset. Not yet distinguished which dominates. Full writeup in `null_bias_investigation.md`.

**2026-09-15: added automatic 'K' dump-on-capture so hands-off jumps land in the serial log without a keypress, then decoded the twenty-fourth through twenty-sixth captures - the last pair of which shows exactly why this matters.** `diagnostics_service()` (Core 1, once per `loop()` iteration) now checks for a LATCHED slow_trace on its own and prints+re-arms it automatically (header reads `AUTO-CAPTURED` instead of `TRIGGERED`), typically within ~10ms of the capture completing - 'K' still works exactly as before for its live "still watching" readout and as a manual read (the two paths race harmlessly; whichever gets there first wins, the other finds nothing to do). Not bench-tested. Still need something on the PC side actually logging the serial stream to a file for unattended runs to be reviewable later - the firmware change removes the keypress, not the need to capture the output somewhere. Twenty-fourth capture: a third instance of the smooth continuous-drift category, this time showing the near-null pair's sum isn't quite exactly conserved over a full 100ms window (~3Hz drift), just locally. Twenty-fifth: a clean two-then-two staggered cascade to a new {1600,1600,1600,1200}Hz resting cycle, completing within ~31ms. Twenty-sixth (only ~154s later): an unusual +1200Hz single-bin trigger jump (not an 800Hz multiple, the first of its kind logged) with a busier cascade including a very late final-slot step near the end of the window. The interesting part is the PAIR: the twenty-fifth's cascade was heading toward a ~1500Hz resting mean, but the twenty-sixth's own `before` value is back down near the original ~700Hz baseline - either that state genuinely reverted, or (more likely, given the ~66-minute gap before the twenty-fifth was actually read via manual 'K') the reference value silently absorbed whatever happened during that long gap. Exactly the ambiguity the new auto-dump feature removes going forward, since every future trigger's baseline will now be resynced within ~10ms of the previous capture instead of whenever a human next happens to press 'K'. Full decode and technical detail in `null_bias_investigation.md`.

**2026-09-15: first hands-off bench run with the new auto-dump feature - it worked (18/18 events auto-captured, zero manual 'K' needed), and the resulting log surfaced two significant new findings.** User reported "didn't spot any big jumps" in the captured log; checking the actual header lines shows 6 of the 9 excursion cycles in this 23-minute run WERE big (~300-385Hz swings, `before`~700Hz to `after`~1000-1085Hz) - just easy to miss skimming raw text, since the printed `delta=` field is only the modest at-trigger threshold-crossing amount (~5-25Hz), not the actual swing size (`|after-before|`). One new transition sub-type also showed up: a +81Hz event where two slots kept doing the established smooth continuous-drift thing while the OTHER two slots each took a discrete +800Hz step mid-window - the first capture where different slots visibly split into two different behavioral modes at once. **Two bigger findings**, both confirmed at the bin level: (1) every "correction" event that follows a big jump ~8.14s later has a completely FLAT trace (no transition anywhere) - meaning the jump had already fully reverted to baseline before the correction fired, and the correction is really just the slow (tau=2s) EMA catching up to a reversion the fast EMA had already completed; the near-exact ~8.14s timing (vs. `FREQ_EMA_WARMUP_MS=8000`) points to this being a side effect of the auto-dump's own instant resync-at-rearm now happening mid-relaxation instead of whenever a human used to eventually poll 'K'. (2) The underlying jump-cycles themselves repeat with an extremely regular ~153.6-second period (<0.3% spread across 9 consecutive cycles) - a dedicated code search found nothing in this firmware (no software timers exist in this project at all) that could produce that cadence. Since timestamps come from the ESP32's own clock, this rules out a PC-side artifact - something on/coupled to the device itself is running on a ~153.6s cycle, and it isn't already-known code. Flagged as the top open lead for next session. Full writeup, including the per-event table, in `null_bias_investigation.md`.

**2026-09-15, later same day: second segment of the same hands-off run (`K_out_2.txt`, same boot, picks up right where the first file's timestamps left off) - strongly reconfirms both findings from the first segment.** User reported seeing "jumps and holds at maybe up to 20Hz" - that number matches the printed `delta=` field exactly (this file's deltas run -5 to +23Hz), but same as last time, `delta` is only the modest at-trigger amount, not the real swing: 5 of this file's 6 cycles are again big, `after` landing 300-370Hz above the ~700Hz baseline, same magnitude as the first file. Every correction event is again a completely flat trace with `fast` sitting dead-flat while `slow` visibly decays toward it - and the ~153.6s cadence held rock-steady across all 5 cycles in this file too (153589-153758ms), now 14 consecutive cycles across both files/~38.6 minutes total, all in the same tight band. Combined total this run: 30/30 events auto-captured, zero missed, zero manual 'K' needed. One of this file's triggers is a repeat of the atypical +1200Hz single-bin jump type (first seen in the twenty-sixth capture) - confirms it's a real recurring member of the repertoire. Full table and detail in `null_bias_investigation.md`.

**2026-09-15, later still: user reports a live SDR-observed "-22Hz, stuck for a while" and asks if it matches the K log (third segment, `K_out_3.txt`, same run continues) - honest answer is "not obviously, and here's why that might not be a contradiction."** Their guessed timestamp (`t=2923625`) is a real event, but on its own it's just a modest ~5Hz dip, not a clear match for a sustained -22Hz shift. Two non-exclusive ways to reconcile: the printed `after=` value is only a 50ms-post-trigger snapshot (already established earlier this session as NOT necessarily the final settled value - this capture's own drift hadn't leveled off yet when the window closed), or the "-22Hz, stuck" state could be a slower, separate drift where fast and slow EMAs moved down TOGETHER without ever re-diverging by 5Hz - which the auto-dump system (a pure divergence detector) would never print anything for, no matter how long it lasted. That second possibility is a real blind spot worth keeping in mind. Cadence/correction-gap pattern otherwise holds exactly as the last two files. User is watching live and will send a follow-up once it reverts (or reports if it's still stuck) - that pairing would be the most direct data yet on why a selected value persists. Flagged as in-progress. Full detail in `null_bias_investigation.md`.

**2026-09-15, later still: added 'H', a second independent frequency-watchdog aimed at the case 'K' structurally can't see - a jump that STICKS.** User pointed out that what they're watching on the SDR is the real output sitting off-frequency for a long time (their example: "-22Hz, been there a while"), and asked how to detect that directly. Root cause of the gap: 'K' only fires on fast-vs-slow EMA divergence, and deliberately resyncs slow to fast every time it's read - so if fast and slow ever drift together slowly enough to never re-diverge from EACH OTHER, 'K' goes silent even while genuinely far from where the run started. Fix: a new, PERMANENT anchor (snapped once at boot-settle, never resynced) that the fast EMA is compared against instead. Fires `CONFIRMED STUCK` (with a coarse, always-on 60-second rolling trace of the actual raw `tx_freq` attached - deliberately un-smoothed, so it can't be confused with an SDR/FFT display's own exponential-average "trail") once a deviation has been continuous for 15+ seconds - chosen specifically to sit above how long this session's own data shows an ordinary reverting 'K'-style jump actually lasts, so it doesn't just duplicate what 'K' already catches - plus a periodic "still stuck" heartbeat and a `RECOVERED` print with total duration once it clears. New manual command `'H'` mirrors 'K's on-demand status read. Not bench-tested. Full design rationale in `null_bias_investigation.md`.

**2026-09-15, later still: real-world feedback on 'H' arrived before it was even bench-tested, and it caught a real calibration miss.** User reported the 700Hz tone drifting from -13Hz to +8Hz off nominal and still sitting there - exactly what 'H' was built to catch, but at only ~8Hz off nominal it would have missed the original 10Hz threshold entirely. Lowered `HELD_TRIGGER_HZ` to 5Hz (matching `SLOW_JUMP_TRIGGER_HZ`, the user's own already-established visual-read perceptual floor) - the 15-second minimum duration is what actually filters out ordinary reverting jumps, not the magnitude threshold, so there was never a good reason for it to be higher. The three K events sent alongside all decode within the already-established repertoire (a flat correction, a repeat of the +1200Hz-jump cascade, another flat correction) - nothing structurally new there, but the last correction event is a good candidate for being the actual transition into the "-13 to +8, still sitting" state, extending the "after= is just a 50ms snapshot" finding with a concrete real-world example. Full writeup in `null_bias_investigation.md`.

## 2026-09-15, later still: considered and ruled out - USB polling from the PC as the source of the ~153.6s cycle

User asked whether USB polling from the PC could explain the ~153.6s periodicity. Checked directly: a full-codebase grep for every USB/CDC-related symbol found only a one-time boot-enumeration delay, the already-known packet-boundary padding fix, TX-buffer backpressure throttling, and a reboot-comment note about USB CDC teardown - nothing with any periodic character. Combined with the earlier finding that no software timer subsystem exists in this firmware at all, that K/H timestamps come from the ESP32's own internal clock (independent of USB), that the two-tone signal path is entirely internal/tick-driven with no host-data dependency, and that no standard USB/OS polling interval is known to sit near 153.6s at the observed <0.3% precision - ruled out as a likely cause. The strongest single argument: the user has independently seen the same shift on the SDR (real transmitted RF, not just a log artifact), and USB here has no causal path into the actual DDS/phase output, only into diagnostics printing. Offered a concrete, definitive test if wanted: run the board on a separate supply with USB data disconnected for a controlled window, then reconnect and check whether the latched 'K'/'H' state shows the cycle continued through the blackout. Not run yet. Full reasoning in `null_bias_investigation.md`. The ~153.6s mechanism itself remains the top open question.

## 2026-09-15, later still: fresh-run capture decoded - why "-6Hz to +3Hz" didn't trigger 'H', plus a possible diagnostic data-race glitch and a new escalated +1200Hz-jump cascade variant

New boot/run, 6 K auto-captures + 2 'H' status checks over ~625s. All within the already-established repertoire: two more instances of the atypical +1200Hz single-bin jump (400->1600Hz), each followed by its usual ~8.14s flat correction, and the ~153.6s periodicity reconfirmed across 3 more up-to-up gaps (153655/153672/153672ms) - notably on a brand-new boot, meaning the cadence re-establishes from scratch each run rather than being tied to some absolute time-since-power-on.

**Directly answering "why didn't H trigger for -6 to +3Hz"**: every `fast` EMA value actually recorded in this log - both 'H' status snapshots and every K correction's settled value - stayed within about -3.1Hz to +3.1Hz of the anchor. It never reached the 5Hz bar, so 'H' correctly had nothing to fire on. The "+3Hz" half matches a real measured event (+3.08Hz); there's no clean match for "-6Hz" in the internal data, which leaves two honest possibilities: the visual SDR read overstated the dip, or 'H' (built on the same `fast` EMA as 'K') inherits the same open "is freq_dev_hz measuring what the SDR actually shows" concern raised earlier this session - not resolved here.

**Two new items flagged, not yet investigated further**: (1) one 'H' trace showed a single isolated 0.5s sample spiking to ~719Hz sandwiched between two flat plateaus - too fast to be a real transition by every other pattern seen this session, more likely a torn/non-atomic cross-core read of the `fast` EMA float; worth checking whether that variable needs volatile/atomic-safe access. (2) two of the +1200Hz-type jumps this run showed cascade tails climbing past the usual 1600Hz ceiling into the 1900Hz range - the highest values seen yet; not enough data to call it a distinct new type. Full detail in `null_bias_investigation.md`.

## 2026-09-15, later still: first direct SDR-vs-'fast' comparison in two-tone mode, at rest - checks out

User confirmed the SDR read "exactly on freq" while `'H'` showed `fast=703.41Hz` - the first real two-tone-mode comparison (as opposed to the earlier `'s'`-tone calibration check, which doesn't exercise the beat-null-crossing conditions this session's null-bias work is about). A ~3.4Hz reading counting as "on freq" fits the already-established 5Hz trigger bars nicely and confirms `fast` isn't obviously biased while sitting quiet at nominal. Doesn't yet answer whether `fast` stays trustworthy during/right after an actual jump - that's the harder, still-open half of the question, and the natural next data point if the user can catch an `'H'`-vs-SDR read during a live jump rather than only at rest. Full detail in `null_bias_investigation.md`.

## 2026-09-15, later still: cross-core race audit of the real DSP path (not just diagnostics) - two new hazards found, one old suspicion reopened

User asked whether any variables the actual DSP/filter code relies on (not just the diagnostic ring buffer) could suffer the same kind of cross-core race. Delegated a full audit: `dsp_task` (all per-sample processing) runs IRAM_ATTR on Core 0, `loop()`/serial commands/diagnostics run on Core 1, and there are no locks anywhere in the project - just bare volatile scalars. Found two genuinely new hazards sitting directly in the transmitted-signal path: `envelope_output.cpp`'s PWM offset/scale pair (read together every sample, written as two separate unsynchronized stores, including back-to-back during preset load) and `test_signals.cpp`'s tone-frequency/amplitude pairs (same pattern, read every sample by the two-tone generator that feeds straight into the freq_dev_hz chain this whole investigation is about). Also reopened a suspicion the project already had: the `envelope_gdeq`/`ssb_dsp` EQ coefficient-reinit pattern was flagged back on 2026-09-09 as a possible source of preset-switch weirdness, and a canary was added to check it - but that canary only checks `isfinite()`, which can't catch a real-but-torn (mismatched coefficient + stale filter state) value, so the earlier "no mismatch found" result doesn't actually clear this hypothesis. Caveat: all of these produce at most a single bad DSP tick (sub-millisecond) if they fire, which doesn't obviously explain the multi-second SUSTAINED jumps 'K'/'H' are built to catch - better candidates for an occasional brief glitch than for a held state. Full detail and file/line references in `null_bias_investigation.md`.

## 2026-09-15, later still: biggest SDR-vs-diagnostic mismatch yet - "-35Hz, still there" but 'H' shows +1.85Hz for the prior 60 seconds

User reported a sustained -35Hz jump, ran 'H' right after seeing it - and both 'H's live status (dev=-3.74Hz) and its 60-second rolling trace (flat at +1.85Hz the whole way back) show nothing remotely close to -35Hz. Every previous "didn't quite match" case this session was single-digit Hz; this one is off by an order of magnitude, and it's the first time BOTH of H's independent readouts (live status and trace) agree there's nothing there. Two possibilities, not yet resolved: either there was enough of a timing gap between seeing the jump and actually sending 'H' that the real excursion had already reverted and scrolled out of the 60s trace window (plausible, given how fast other transitions this session revert), or there's a genuine, previously undemonstrated gap between what freq_dev_hz measures and what's actually being transmitted during a real held jump - which would mean 'K'/'H' can't be trusted to catch every real event. The decisive test: next time, send 'H' within a couple seconds of seeing a jump and note the gap. If it still shows nothing, this becomes the top-priority open thread - bigger than the periodicity question, since it would mean this session's whole diagnostic apparatus has a real blind spot. Full writeup in `null_bias_investigation.md`.

## 2026-09-15, later still: -35Hz mystery sharpens (no K auto-capture either), and 'H' output now carries timestamps

User clarified two things about the -35Hz report: the K event right before the 'H' read (the correction at t=1880773, interleaved mid-print with the H command) means the true H-vs-event gap was provably tiny, not a long unaccounted delay - and no separate K auto-capture fired for the -35Hz event at all, which K's design shouldn't be able to miss silently (auto-dump fires every loop() iteration once a trigger latches). Both facts weaken the "just read too late" explanation and strengthen the possibility of a real, demonstrated disagreement between what freq_dev_hz measures and what the SDR shows at the same moment - though it's still not fully pinned down which exact K print corresponds to what the user saw.

**Fixed the actual complaint**: added explicit `t=%ums` timestamps to every 'H'-related print (the on-demand status read and all three auto-fired CONFIRMED STUCK/still-stuck/RECOVERED lines), directly comparable to K's own timestamps. Next time a jump and an H read happen close together, the exact gap between them will be computable from the log directly instead of inferred from how the serial output happened to interleave. Not bench-tested yet. Full reasoning in `null_bias_investigation.md`.

## 2026-09-15, later still: strongest lead yet - envelope-domain filter memory, raced cross-core, invisible to K/H by construction

User pushed back hard on the accumulating hedging: they've reported sustained held states multiple times and the internal data has never shown it - "we are not looking at the same reality" - and argued the recurring jump magnitudes look mechanistic/numeric, not random noise. They proposed a cross-core race corrupting something with real MEMORY (not freshly recomputed each tick), which could explain a state that stays wrong rather than self-healing in one sample.

Checked this against the code directly. The phase/frequency path itself (Hilbert delay line, phase accumulators, freq_dev slew-limiter) turns out to be single-core-owned and not raced - that part of the theory isn't supported. But the envelope-domain IIR filters (`envelope_gdeq`'s allpass memory, `envelope_ampeq`'s shelf biquad memory) ARE raced in exactly the shape described: Core 1 non-atomically reinitializes multi-field filter state while Core 0 keeps running the filter live every tick. Unlike the earlier races, this state genuinely carries forward as feedback, so a bad value doesn't automatically self-heal. Critically, because it's envelope-side, it would never show up in `freq_dev_hz`/K/H at all - and this project's own design notes already anticipated a path from envelope corruption to something that LOOKS like a frequency artifact on a receiver (AM-to-PM crosstalk, explicitly named in a settings.h comment as the explanation if FM-looking sidebands ever appeared with freq_dev_hz staying clean).

Whether the corrupted state is merely stale-but-finite (which would slowly decay via the filter's own math, over seconds) or truly poisoned (NaN/Inf, which never recovers on its own) matters a lot, and there's already a canary check in this codebase (added 9/11, runs every loop iteration) that would print a MISMATCH the first time the filter state goes non-finite - I don't recall ever seeing that line in a shared log, which argues against the sharpest version of this theory but not the milder one. Proposed concrete test: run `'c'` right after/during a live stuck event to check directly. This is now the strongest, most concrete lead in the whole investigation - full writeup in `null_bias_investigation.md`.

## 2026-09-15, later still: the theory completes - envelope masking/unmasking of the already-known near-null instability

User confirmed never seeing a bad canary, and asked a sharp question: is the RF output frequency essentially a power/energy-weighted function of freq(t) and envelope(t), such that an amplitude error alone (no real frequency error) could produce an apparent frequency shift? Answer: yes, and it completes today's theory. An SDR/receiver's frequency-domain view inherently weights each instant by its envelope-carried energy; freq_dev_hz/fast/K/H are a plain, envelope-blind average with no such weighting. This session already established freq_dev_hz is genuinely unstable near beat-envelope nulls, but normally invisible because envelope is naturally near-zero right when that happens - so it contributes almost no energy to what's actually transmitted. If the envelope-EQ filter state gets corrupted (the cross-core race flagged in the previous entry) and stays elevated instead of dipping during a null, that already-existing near-null excursion suddenly gets real energy weight in the output - explaining a real SDR-visible frequency shift with freq_dev_hz never actually being wrong, why the canary (isfinite-only) would never catch a merely wrong-but-finite envelope value, why the same quantized magnitudes keep recurring (unmasking an already-characterized deterministic pattern, not fresh noise), and why it can persist for seconds (filter state, not an instantaneous glitch). This is now the leading, most complete theory in the investigation - not yet directly confirmed, but consistent with everything found today. Full writeup in `null_bias_investigation.md`.

## 2026-09-15, later still: traced whether the null-region code preserves the joint freq+ampl relationship - it doesn't, and that's confirmed by the project's own history

User asked directly how the null "correction" code ensures the joint integral of frequency and amplitude corrections is right, given today's energy-weighting theory. Traced the actual pipeline: freq_dev_hz and envelope are generated together, then envelope alone passes through three independent envelope-only stages (envelope_floor, gdeq, ampeq) with zero corresponding adjustment to freq_dev_hz - only afterward does relative_delay bring them back together, purely for timing alignment, not amplitude/phase consistency. Nothing anywhere checks that the pair stays physically sane near a null.

This isn't an oversight - envelope_floor.cpp's own NOTE 1/NOTE 2 history shows the original design DID try correcting both sides together, found the frequency-side fix was actively wrong (removed it), and kept only the envelope-side fix. Per today's theory, envelope_floor/gdeq/ampeq - each independently, whenever engaged - can break the pairing between near-zero envelope and the phase's genuinely-required fast rotation at a null, which is exactly the shape of mismatch that would inject real spectral energy at an otherwise-masked frequency.

Scope caveat: all three (envelope_floor, gdeq, ampeq) default to off/no-op, and gdeq/ampeq fully no-op when disabled (so the earlier-flagged cross-core race in their reinit can't matter unless one is actually toggled on). Asked the user directly whether 'g'/'a'/'A'/'x' were active during the sessions where jumps were observed - this determines whether this mechanism is currently live or whether the search should stay on the phase/timing side instead. Full writeup in `null_bias_investigation.md`.

## 2026-09-15, later still: null depth varies and correlates with K triggers (user's direct scope observation) - checked a drift theory numerically, found and fixed a real print-precision blind spot

User directly watched the envelope waveform and reports the null's minimum level visibly varies over time and is "pretty sure" the deepest nulls are what trigger 'K' - which makes physical sense (a deeper null requires a more extreme phase swing to represent the zero-crossing, producing a bigger freq_dev_hz spike). They also confirmed all three envelope filters ('g'/'a'/'A') are ON and suspect filter-induced sample jitter explains why the null doesn't land at a fixed sample position cycle to cycle.

Tested one candidate explanation numerically: float32 rounding drift in the two-tone oscillators' own phase accumulators (a real, deterministic, per-boot-reproducible phenomenon that would fit the "resets identically each boot" fact already established). Simulated it directly - the actual drift rate implies a full-cycle period of ~2,460-6,300 seconds depending on which quantity you look at, 15-40x too slow to explain the observed ~153.6s cadence. Reported this honestly rather than force-fitting it. Since the beat period is an exact 16-sample integer, an unfiltered/undelayed signal would show zero cycle-to-cycle null jitter at all - so the jitter the user sees is itself evidence pointing at the same envelope filters already flagged today for a cross-core race and for having no mechanism to keep them consistent with the phase path near a null.

Also found and fixed a genuine, months-old blind spot: env_min has been printed at 3 decimal places this entire session, and since real null depths sit well below 0.001, every env_min value in every log ever shared has printed as an uninformative flat 0.000/0.001 - the print statement was hiding exactly the data needed to test the user's hypothesis. Bumped both print sites (K trace bins, jump log) to scientific notation. Not bench-tested. Next capture should finally show real env_min variation. Full writeup in `null_bias_investigation.md`.

## 2026-09-15, later still: K_out_4.txt (54 events, 72 minutes) - env_min fix confirmed working, null-depth hypothesis proven with hard numbers, and the active-filter list for this capture ruled out almost entirely

First capture taken since yesterday's env_min print-precision fix, answering "anything new?" directly: yes, a lot. The 54 K auto-captures split into two tight, reproducible clusters - 25 "deep" events (env_min~0.0016, always a big +15 to +24Hz jump toward a ~1000-1080Hz band) and 26 "shallow" events (env_min~0.090, always a smaller -5 to -14Hz dip back toward ~700Hz), plus 3 "solo" events where the null came up shallower than usual and only just cleared K's minimum threshold. This directly confirms, with numbers instead of a scope-side impression, the user's own hypothesis: deeper null -> bigger, positive-going K event; shallower null -> smaller, negative-going one.

The ~153.6s periodicity is now measured to within 0.03% (153,633-153,684ms across 24 gaps) and turns out to be a two-phase cycle, not a single event: every "deep" event is followed 8136-8154ms later by its paired "shallow" event. That level of clockwork regularity over more than an hour points at a deterministic numeric/timing mechanism rather than aperiodic race-condition corruption.

Checked which envelope filters were actually engaged during this specific capture, from the log's own command trace: gdeq, both ampeq stages, the pre-Hilbert EQ, and the compressor were all confirmed OFF - directly contradicting "all on" and ruling out, for this data, every filter-race hypothesis built up over the last several entries. What WAS active - envelope pre-distortion (`'D'`) - turned out to be a stateless lookup table (can't itself cause a slow drift) but a crucial interpretation lens: `env_min` is measured AFTER this table, inside its steepest bin, and inverting it shows the real raw-envelope null depths are ~0.005% ("deep") vs ~0.79% ("shallow") - a genuine ~150x difference, not a display artifact. This significantly narrows what's left to investigate: the two-tone generator itself and `ssb_dsp_process_sample()`'s own Hilbert/DC-block math are now the two remaining candidates for the ~153.6s pattern's root cause. Full numbers, LUT inversion math, and file/line references in `null_bias_investigation.md`.

## 2026-09-15, later still: user confirms filters-off was deliberate; traced the exact phase-to-freq_dev math and confirmed near-null noise is currently unmitigated

Confirmed the previous entry's filters-off state in `K_out_4.txt` was an intentional A/B test, not an accident. User then asked exactly how phase discontinuities get resolved and how phase converts to frequency. Traced `ssb_dsp_process_sample()` directly: `wrap_pi()` does standard phase unwrapping on the atan2-derived phase difference (picks the smallest-magnitude equivalent step, valid as long as real content stays under Nyquist/2), then `freq_dev = dphi * sample_rate_hz / (2*pi)` is a plain scaled phase-difference calculation.

The real finding is in the uncertainty question: near a null, tiny I/Q values make the phase angle hypersensitive to ordinary signal noise, and the code does nothing in the phase math itself to correct for that. Two downstream mechanisms could limit it - a slew-rate limiter on freq_dev (its own doc comment cites ~60Hz/sample for real content vs ~8000Hz/sample at a genuine null) and a magnitude clamp (20000Hz) - but the slew limiter is off by default in every preset checked so far, and the magnitude clamp is confirmed mathematically unable to ever bind on a single-sample event (wrap_pi's own pi bound already caps any single-sample freq_dev at 8000Hz at 16kHz, under the 20000Hz clamp). So near-null phase noise currently reaches freq_dev completely unmitigated - deliberate, per this project's own established history of trying and reverting a freeze-based fix. Concrete next experiment identified: deliberately engaging the slew limiter and checking whether it reduces K/jump-log magnitudes. Full trace and numbers in `null_bias_investigation.md`.

## 2026-09-15, later still: is there a resolution bias on I/Q near zero? Yes - quantified, and it's float32 cancellation noise, not ADC quantization, at nearly the same scale as the "deep" null cluster

User asked whether I/Q suffer a resolution bias approaching zero. Classic ADC-style quantization doesn't apply to any capture examined so far - two-tone test mode bypasses the ADC entirely (pure float32 synthetic generation). What does apply is floating-point catastrophic cancellation: both I (2-term sinf sum) and Q (65-tap Hilbert FIR sum) are computed as sums of much-larger terms that nearly cancel right at a null. Simulated both directly against float64 ground truth using the real filter coefficients: I's rounding floor is negligible (~1e-8), but Q's is not - median ~2.8e-5, 95th percentile ~1e-4, and in ~5% of near-null instants the rounding error actually exceeds the true value.

This lands in the same order of magnitude as the "deep" null cluster's back-computed raw envelope (~5e-5) from the K_out_4.txt decode two entries ago - meaning at the very deepest nulls actually observed, env_min may partly be measuring float32 arithmetic noise rather than a clean physical null depth, which is a plausible contributor to that cluster's ~15% cycle-to-cycle spread. Doesn't apply to the "shallow" cluster (2 orders of magnitude above this floor - a real physical value). Sharpens, rather than replaces, the still-open periodicity/null-depth-variation question. Full simulation numbers and methodology in `null_bias_investigation.md`.

## 2026-09-15, later still: is the Hilbert transform IIR (can errors lock in)? No - confirmed FIR, errors are self-limiting to a two-sample doublet

Checked whether the Hilbert transform is IIR (where the just-quantified cancellation rounding noise could persist). It's pure FIR - 65 fixed taps, plain weighted sum over raw input history, no feedback term anywhere - so every Q value is recomputed fresh each sample and a rounding-noise flicker vanishes completely once the relevant samples age out of the 65-tap window. Contrasts sharply with the already-flagged `envelope_gdeq`/`envelope_ampeq` IIR filters, whose real feedback state is exactly why THOSE can "stay corrupted" if raced. One nuance: `prev_phase` carries one sample forward, so a single bad phase value leaks into two consecutive freq_dev outputs as an equal-and-opposite doublet - ordinary derivative-estimator behavior, not feedback, and self-vanishing after that. Net: the earlier resolution-bias floor is real but transient, not a sustained corruption. Full reasoning in `null_bias_investigation.md`.

## 2026-09-16: decisive result - relative_delay-induced 20Hz jump is PROVABLY invisible to H/K; energy-weighting theory now fully proven, not just consistent

Two observations. First, watching both tones independently on the SDR, their separation has stayed a constant 1kHz - never the "upper up/lower down" spread the user expected from a jump. This is fully consistent with theory: a genuine sustained frequency bias is a pure spectral translation (Fourier shift theorem), shifting both tones by the same amount together - good corroborating evidence.

Second, and decisive: the user can reproducibly induce a real, sustained ~20Hz SDR-visible shift via the `'['`/`']'` relative_delay scan, hold it indefinitely, and `'H'` always reports ~700Hz nominal regardless - a controlled, repeatable experiment, not an ambiguous spontaneous event. Traced the full mechanism and it's airtight: H/K's fast/slow EMAs are fed by `delayed_freq_dev_hz`, which `relative_delay_apply()` produces via plain linear interpolation between two adjacent entries of the SAME raw freq_dev_hz ring buffer. Since that raw signal is periodic/stationary, blending its own nearby samples at any lag cannot change its own long-run average - proving H/K are structurally, permanently incapable of reflecting a relative_delay-induced shift, no matter how large or how long held. Meanwhile envelope is read at a DIFFERENT lag than freq_dev, so what `'['`/`']'` actually does is mis-time the envelope/freq_dev pairing in the real transmitted signal - a genuine energy-weighting effect that never touches freq_dev_hz's own statistics.

This completes (not just supports) the energy-weighting theory from several entries back with a full, code-verified proof from a controlled experiment. Implication: any real spontaneous jump whose root cause is a timing/pairing mismatch between envelope and freq_dev - i.e. every mechanism floated this session - would be exactly as invisible to H/K as this deliberately-induced one. Resolves the earlier "we are not looking at the same reality" disagreement with a concrete mechanism rather than an argument. Next step identified: build a real energy-weighted diagnostic from the already-present but never-read-back `env2_dphi_sum`/`env2_sum` accumulators in `ssb_dsp.c`. Full trace in `null_bias_investigation.md`.

## 2026-09-16: found a distinct candidate for the hands-off jumps (AD9851 bit-bang timing margin, currently disabled) and implemented the post-delay energy-weighted 'H' fix

User correctly cautioned against assuming the `relative_delay` blind-spot proof explains the spontaneous hands-off jumps too - `relative_delay` is a fixed setting hands-off, so a real spontaneous cause needs its own mechanism. Found one already sitting in `AD9851.c`'s history: `AD9851_BITBANG_EDGE_DELAY_ENABLED` (the settle-margin protecting against data-dependent bit errors during large single-tick FTW jumps - the code's own comment already names this "the leading theory for the two-tone-specific 'sticks' symptom") is currently DISABLED, reverted 2026-09-11 pending a re-scope that never happened. The exact comparison test needed (restore the margin, run a hands-off comparison) was planned in a 2026-09-11 comment and never completed - and that same comment records a prior hands-off "-45Hz jump... digital chain otherwise clean" event this mechanism would explain. This is a genuinely different failure mode from yesterday's finding: a physical bit-level SPI transmission error at the AD9851 itself, invisible to every software diagnostic (including a corrected energy-weighted one) since none of them read back what the chip actually latched. Concrete next step identified (needs the user's bench, not firmware): re-enable the edge delay and re-run the abandoned hands-off comparison.

Also implemented the diagnostic fix the user agreed to: a proper post-delay, envelope^2-weighted EMA added to `'H'`'s output, fed from `delayed_freq_dev_hz` and the correctly time-matched `envelope_at_freq_time` (not `delayed_envelope`, a trap the project already caught once before on 2026-09-12). This completes a fix proposed back on 2026-09-12 and never built, and directly addresses why the existing `weighted_bias` diagnostic never tracked real jumps (it was accumulating pre-delay, blind to relative_delay pairing exactly like plain `fast`/`slow` are). Not bench-tested. Full implementation details and reasoning in `null_bias_investigation.md`.

## 2026-09-16, later still: first bench test surfaces a stale config.h AND catches a real, sustained, hands-off jump with numbers attached

First bench report on the new energy-weighted `'H'` line: "SDR sitting at -22Hz and H reports 696." Chasing down which field "696" referred to turned up something bigger: this repo's `config.h` had `TWOTONE_F2_HZ` at `1900.0f` - the legacy, non-Fs-commensurate pair the 2026-09-09 entry already recommended retiring - not `1700.0f`. Git history traced it to commit `8fd8349` (2026-09-12, "Add dither on tone2... current 2tone set to 700/1900"), which silently reverted `de32ffd`'s earlier fix. The user confirmed their actual bench config is 700/1700; fixed `config.h` to match and documented the history so it can't drift again unnoticed. Also found `test_signals.cpp`'s `'T'` band-preset table has no 700/1700 entry at all - that pair is only reachable via the compile-time default, before any `'T'` press.

With the tone pair confirmed, a second, genuinely new finding fell out of the same bench session: two "always-on" diagnostics turned out to be aliasing artifacts against the exactly-periodic two-tone signal, and a real, sustained, hands-off jump got caught with full before/after numbers in a freshly-uploaded capture (`K_out_5.txt`). `held_trace`'s `tx_freq` column looked "stuck" near a single value for 60 seconds straight in two separate captures - not a frozen DDS, but its 500ms sample interval (8000 ticks) being an exact multiple of the 700/1700Hz pair's 16-tick period, so every snapshot lands on the identical phase of the repeating waveform. The fine `slow_trace` bins (20-tick, 1.25ms) show the same effect at a different scale - a suspiciously clean period-4 alternation between exactly 400.0Hz and 800.0Hz, explained by `LCM(16,20)/20 = 4` distinct bin-phase alignments.

The real find: `K_out_5.txt` contains a slow-jump auto-capture (`t=769436ms`, `before=700.44Hz after=1063.38Hz delta=+17.74Hz`) with `relative_delay` unchanged for the preceding 731 seconds - ruling out yesterday's relative_delay-pairing mechanism for this specific event. Working the fast EMA's own known time constant backward (the 40-post-bin capture window is exactly one fast tau) implies `freq_dev` sat near a ~1264Hz plateau for at least 50ms - a sustained excursion, not a single bad phase sample (a maximal single-tick spike, capped by `wrap_pi()` at 8000Hz, could only move `fast` by ~9Hz in one tick; this moved it by ~345Hz). This also explains two earlier, smaller "glitch" values the user separately spotted in `held_trace` snapshots (722.55Hz and 759.61Hz, each ~30-60Hz above baseline in a single 500ms-spaced sample) - too large for a single tick by the same arithmetic, so almost certainly smaller/shorter instances of the same underlying phenomenon that this coarse trace only occasionally catches mid-excursion. Full derivation, numbers, and the two competing explanations (AD9851 bit-bang latch vs. a DSP-level cause) in `null_bias_investigation.md`.

Also fixed a design flaw in the energy-weighted `'H'` line itself, caught on this same first bench read: its `dev` was being measured against the plain EMA's anchor, producing a large, static, meaningless offset (+410Hz) with no connection to any real event. Gave it its own anchor, seeded the same way at boot-settle.

## 2026-09-16, later still: first real-world hit on the energy-weighted 'H' fix (-38.37Hz vs SDR's -38Hz), a harder second data point, and the ~153.6s cycle reconfirmed with new numbers

`K_out_6.txt`'s first manual `'H'` read matched the user's independently-read SDR value almost exactly: energy_weighted `dev=-38.37Hz` against an SDR reading of -38Hz, while the plain line showed essentially nothing (+1.23Hz) at the same instant - the first real-world confirmation that yesterday's energy-weighted fix tracks what the SDR actually sees, not just an internally-consistent number. A second read 23 seconds later (after an untimed coffee-break gap) showed energy_weighted `dev=+63.65Hz` against an SDR reading of only +8Hz - a real mismatch, but not an unexplained one: pulling every auto-captured slow-jump event in the file showed a full rise/recovery pair happened in the gap between the two reads, and the second read landed 12 seconds after that recovery had already completed by the plain fast/slow measure. Since the energy-weighted EMA shares the plain one's exact time constant, a 63Hz gap surviving 12 seconds (240 tau's) after the plain signal fully settled can't be the same event just decaying slower - either a separate pairing-type deviation was still active, or the SDR was glanced at a few seconds off from the keystroke. Recommended follow-up: read `'H'` and the SDR together (seconds apart, not a coffee break) at several points across one full cycle, to get a clean correlation table.

Separately, the timeline of auto-captured events reproduces the ~153.6s/~8.15s two-phase periodic cycle originally found in `K_out_4.txt`, independently, with even tighter precision this time (rise-to-rise intervals of 153671ms and 153673ms; recovery lags of 8141/8143/8147ms) - strong cross-validation that whatever drives this cycle is still active and highly regular. Full timeline, numbers, and reasoning in `null_bias_investigation.md`.

## 2026-09-16, later still: user spots 8Hz-spaced SDR sidebands (elevated at 40Hz) - found an exact 40Hz match in firmware, and it may be the diagnostics themselves

User reported 8Hz-spaced sidebands around the 700Hz tone on the SDR, with the 40Hz (5th harmonic) elevated, and noted recent shift readings like -38Hz sit close to that 40Hz line given their own measurement uncertainty. A full codebase search found no 8Hz source anywhere, but a very clean match at 40Hz: `EMA_TREND_SAMPLE_TICKS`=400 ticks = exactly 25ms = 40Hz - the trend-ring bookkeeping added earlier today for `'K'`'s trend printout, which runs unconditionally every tick inside the hot DSP path, after that tick's own PWM/AD9851 writes but potentially eating into the NEXT tick's timing budget once every 40th-of-a-second. If real, this means the diagnostic instrumentation built to investigate hands-off jumps may itself be introducing a small periodic artifact into the transmitted signal - ironic, but a legitimate, traceable mechanism, not a stretch. Scoped to the smaller ~20-60Hz-class wobbles only - nowhere near big enough to explain the large auto-captured jumps already logged. No 8Hz firmware source found; one inactive-by-default candidate (`'Q'` dither's 4Hz update, 2nd harmonic at 8Hz) worth ruling out at the bench. No firmware change made yet - two concrete, cheap tests proposed (check dither state; try gating the trend-ring write off the hot path) pending the user's go-ahead. Full trace and reasoning in `null_bias_investigation.md`.

## 2026-09-16, later still: correction - the recurring ~153.6s "rise/recovery" auto-capture is very likely a null-noise artifact of the plain EMA, not a real transmitted jump

`K_out_7.txt` was `K_out_6.txt` continued (same un-cleared log, confirmed byte-identical prefix). The user watched the SDR through the extra ~23 minutes and saw no more than +/-4Hz of real movement, despite 12 more occurrences of the exact same ~153.6s rise/recovery pattern already logged several times this session - every single one showing `env_min` pinned at a genuine near-total null. This walks back the `K_out_5.txt` entry's conclusion that this pattern represents a "genuine, sustained, hands-off jump" needing a hardware explanation: comparing the one occurrence that DID coincide with real SDR movement (the -38Hz/+8Hz episode) against the 12 that didn't, the numbers are statistically indistinguishable - nothing in the auto-capture's own before/after/delta marks the "real" one as different. Most consistent explanation: near-null moments can swing the plain (unweighted) instantaneous-frequency average by hundreds of Hz while carrying almost no real transmitted power, exactly the failure mode the energy-weighting theory already predicts, and an SDR (reading actual radiated power) correctly doesn't see it. This de-prioritizes the AD9851 bit-bang theory as an explanation for THIS specific recurring pattern (a hardware bit-latch fault would affect the real output every time, not 1 time in 13) - the bit-bang theory and the still-unexplained rarer real events (the original -22Hz report, and whatever actually caused -38/+8 if not this pattern) remain open separately. Proposed next step: have a future auto-capture also print the energy-weighted reading alongside the plain one, so a real event can be told from an artifact without a coincidental manual `'H'` press. Full numbers and reasoning in `null_bias_investigation.md`.

## 2026-09-16, later still: built the proposed fix - auto-captures now print an energy-weighted before/after, and a new independent detector auto-logs energy-weighted holds/jumps

User confirmed both parts of the previous entry's proposal. Implemented in `diagnostics.cpp`, not yet bench-tested:

**Auto-capture printout**: every `AUTO-CAPTURED`/`TRIGGERED` slow-jump event (from `'K'` or automatic) now also prints an `energy_weighted before=... after=... anchor=... anchor_dev=...` line, computed at the same trigger-arm and post-capture-close instants as the existing plain before/after. This means a real event vs. a null-noise artifact (the distinction the previous entry needed 13 events and a statistical comparison to establish by hand) can now be read straight off a single future capture.

**New independent detector**: a full second copy of the existing `'H'` held-freq detector (`s_energy_held_*`, mirroring `s_held_*`'s state machine exactly), but keyed off the energy-weighted `dev` (`energy_weighted_fast_hz() - s_freq_energy_anchor_hz`) instead of the plain fast/anchor pair. Reuses the same tuned thresholds (`HELD_TRIGGER_HZ`=5Hz, `HELD_MIN_DURATION_MS`=15s, `HELD_REANNOUNCE_MS`=30s) rather than inventing new ones. Prints three new, clearly-labeled messages: `ENERGY-WEIGHTED CONFIRMED STUCK` (plus a `held_trace` dump), `ENERGY-WEIGHTED still stuck` (30s heartbeat), and `ENERGY-WEIGHTED RECOVERED`. Since this is keyed off the same anchor/function the corrected `'H'` line uses, a confirmed hold here should mean real transmitted power, not a plain-EMA null-noise wobble.

Brace/paren balance re-verified (155/155, 1133/1133) after all edits. Full code and reasoning in `null_bias_investigation.md`. Next bench capture should exercise both new printouts alongside a simultaneous SDR reading.

## 2026-09-16, later still: log_20260916_111946.txt has no diagnostic output - just a boot config dump that decodes to preset 1, not preset 3 as intended, but the mismatch itself is a useful data point

The uploaded file is one line - a `'P'`-style config dump, no `'H'`/`'K'`/held/auto-capture output at all - so there's nothing internal to cross-check this session's SDR readings (+3Hz, -7Hz, +2Hz) against. Decoding the dumped struct against `settings.h` confirms the user's own suspicion: it's byte-for-byte `settingsPresets[1]` ("TwoTone Base"), not preset 3 ("Shelf2 Baseline gdeq adj#4") - printed under the name "Live" since no preset had been loaded yet this boot.

The fields that differ between preset 1 (this run) and preset 3 (normal) are exactly the envelope-shaping/timing chain this session has repeatedly implicated in envelope/freq_dev pairing mismatches: `relative_delay_samples` (0.00 vs 2.00), `env_gdeq_enable` (off vs on), `env_predistort_enable` (off vs on), `env_ampeq_enable`/`env_ampeq_shelf2_enable` (both off vs on), and `master_gain_db` (-2.0 vs -1.4, unrelated to timing). With all of those off, the SDR shifts reported (+3/-7/+2Hz) were much smaller than prior preset-3 sessions' -38Hz/+8Hz reads or the 700->1000+Hz auto-captured jumps.

Treated as a data point, not a conclusion (three numbers, no trace, could easily be a shorter session or one that missed the ~153.6s cycle's rise phase by chance) - but it's the first natural A/B this session has seen on the exact mechanism the pairing-mismatch theory depends on. Recommended follow-up logged in `null_bias_investigation.md`: repeat this same preset-1/default config for a longer session with the log actually capturing `'H'`/`'K'`/auto-capture output, and compare directly against an equal-length preset-3 run.

## 2026-09-16, later still: the real version of that capture arrives - the new ENERGY-WEIGHTED detector shows the ~153.6s cycle is a smooth ~145s climb, not a sudden event (biggest finding of the session so far)

Same filename re-uploaded with its actual content this time (1365 lines). First real bench test of the two diagnostic pieces built earlier today. Three complete ~153.6s rise/recovery cycles are captured, matching the known cycle length almost exactly (145.0-145.1s "held" + ~8.6s "quiet" each time) - nothing new there. What's new: the independent energy-weighted held detector shows that between one "recovery" and the next "rise," the energy-weighted deviation from its own anchor doesn't sit flat waiting for a sudden event - it **climbs smoothly and continuously the entire ~145 seconds** (roughly +34Hz at 45s in, +50Hz at 75s, +64Hz at 105s, +78Hz at 135s, peaking at +120-133Hz right at the next rise), while the plain fast/slow EMA pair sits completely flat the whole time. That means whatever drives the well-known recurring cycle isn't a single instantaneous event - it's a slow, continuous process spanning almost the entire gap between events, invisible to the plain (unweighted) diagnostics and only now visible through the energy-weighted lens.

No SDR reading was taken during this capture, which is now the single most important missing piece: if the SDR shows a matching smooth climb over the same ~145s, this is a genuine and previously-invisible real transmitted-frequency drift (a major finding). If the SDR stays flat (as it did for `K_out_7.txt`'s null-noise events), the climb is a diagnostic-side artifact and the new detector's thresholds (currently reused from the plain detector, and effectively "triggered" 94% of the time under this config) need retuning. Proposed a concrete hypothesis for the real-drift case (a slow few-PPM beat between two independently-clocked periodic processes, with ~153.6s as its own beat period) - clearly flagged as unconfirmed pending that SDR check.

Also traced, with code (not guesswork this time), why `env_min` pinned at ~0.2 instead of near-zero under this preset: `envelope_at_freq_time` is captured *after* the active preset's envelope-shaping chain, and preset 1's linear offset/scale mapping (`envelope*0.90+0.20`) floors even a true DSP null at 0.20 - a structural property of the diagnostic, not a bug, but a reason energy-weighted numbers aren't directly comparable across presets with different shaping chains. Full numbers, table, and reasoning in `null_bias_investigation.md`. No firmware change made - next step is a bench SDR check, not a code change.

## 2026-09-16, later still: the SDR check comes back - the smooth ~145s climb was NOT real. Hardware-clock-beat theory de-prioritized; the preset-1 offset floor is the leading explanation instead

User confirms the SDR moved no more than a few Hz ("+3 -7 +2") across the entire session just logged - the three cycles where the energy-weighted metric climbed smoothly to +120-133Hz each time. So that climb has no real-world counterpart: it's a diagnostic-side phenomenon, not a genuine transmitted-frequency drift. This rules out the previous entry's "slow hardware clock beat" hypothesis as the main explanation (a real clock beat would show up on the SDR) and points back at the offset-floor finding from the same log: under preset 1, the envelope value used to WEIGHT the energy-weighted average never drops below 0.20 even at a genuine DSP null, while the underlying `dev` (frequency deviation) math still has its usual large null-driven excursions regardless of preset - so near-null noise that's supposed to get suppressed by near-zero weight instead leaks in at ~4% weight. Contrasted against `K_out_6.txt`'s clean -38.37Hz-vs-SDR's-38Hz match (very likely captured on preset 3, where predistort allows a true near-zero weight), this suggests the energy-weighted diagnostics are only trustworthy on presets without a nonzero linear PWM offset floor.

Practical takeaway: don't trust `ENERGY-WEIGHTED CONFIRMED STUCK`/`still stuck` messages on preset 1 (or similar offset-without-predistort presets) as real-event indicators - they fire almost continuously regardless of what the SDR shows. Recommended next step: repeat the same test on preset 3 with a running SDR check, to confirm the detector behaves properly there. Still unexplained: why the leak produces a smooth 145-second climb rather than settling immediately - flagged as open rather than guessed at. No firmware change made. Full reasoning in `null_bias_investigation.md`.

## 2026-09-16, later still: the requested preset-3 comparison arrives with manually-logged SDR readings - and turns up a completely new, striking finding: `held_trace`'s own `tx_freq` shows a discrete ~8000Hz "phantom" shift lasting one full ~153.6s cycle, confirmed fake by three independent checks

User ran the requested preset-3 test with the SDR watched live, logging a reading whenever it moved by roughly 4Hz or more ("if SDR is not logged, it didn't change"). The SDR's whole-session range was tiny - only 24Hz total across ~18 minutes and 7 cycles - consistent with everything else logged about this transmitter's real stability.

While checking the energy-weighted comparison, a bigger and completely unrelated finding turned up: `held_trace`'s `tx_freq` column - the literal value sent to the AD9851, previously assumed to be unimpeachable "ground truth" - sits at a fixed **~8000Hz below normal** for one entire, exact ~153.6-second cycle (three consecutive trace dumps line up end-to-end into one continuous episode, boundary-aligned with the known cycle). Three independent checks all say this never actually happened: the 2026-09-09 corruption canary stays silent the whole file (rules out a corrupted `s_carrier_hz`), `fast`/`slow` stay completely flat the entire time (rules out a real, sustained `freq_dev` change), and the SDR shows nothing at all during or around the window (rules out a real RF event).

Leading explanation: `held_trace` samples at an exact multiple of the two-tone's own period (a fact already known from way back this session - hence its normally "stuck-looking" values), so it always snapshots the identical relative phase of the periodic waveform. A single skipped or double-counted DSP tick could shift that sampling phase onto a different, more extreme point of the cycle (near where a genuine phase discontinuity produces large instantaneous swings) without touching the real, continuously-transmitted signal at all - explaining why nothing else noticed. The episode's start/end line up almost exactly with the well-known recurring cycle's own boundaries, hinting this and the classic rise/recovery pattern might share a root cause (an occasional missed tick), but that's unconfirmed - the log doesn't include the `[timing]` overrun/late-tick counters that would settle it directly.

Also closed the loop on the actual requested comparison: preset 3's energy-weighted readings are smaller than preset 1's (max swing ~75Hz vs ~130Hz) but still show a large, highly consistent, cycle-phase-locked bias unrelated to the real event size (rise events cluster at ~1026-1030Hz, recoveries at ~1145Hz, regardless of that cycle's actual SDR movement) - so the offset-floor theory is probably part of the story, not the whole story. Full numbers and reasoning in `null_bias_investigation.md`. No firmware change made yet.

## 2026-09-16, later still: added overrun/late-tick/max-busy_us data to held_trace, so the next capture can directly test the tick-slip theory

User asked whether the overrun data could be added to the logging - exactly the missing piece needed to confirm or rule out the previous entry's tick-slip theory for the `tx_freq` phantom shift. Added three new columns to `held_trace`'s existing 500ms rolling trace: `overruns`, `late` (late-tick count), and `max_busy_us`, all simple snapshots of counters that already exist and are already exercised every tick - deliberately not new tracking logic, to avoid adding a second novel mechanism right where a bug is being hunted for. All three only ever increase (they reset on a manual `'r'` command, same as before), so a step between one printed line and the next pinpoints exactly which 500ms bin saw a real DSP-timing hiccup. Next time the `tx_freq` phantom shift (or the classic rise/recovery boundary) recurs, lining it up against these three columns will either confirm the tick-slip theory directly or rule it out. Not yet bench-tested. Full details in `null_bias_investigation.md`.

## 2026-09-16, later still: the overrun test comes back clean - tick-slip theory is dead, but the phantom `tx_freq` shift turns out to be deterministic to single-digit milliseconds across two separate boots

Second preset-3 run with the new columns live: `overruns=0` and `late=0` on every one of 1727 samples, no exceptions, including right through the recurring ~8000Hz `tx_freq` phantom episode itself. `max_busy_us` stays comfortably under the 62.5us tick budget throughout (46-59us) and reads identically at the phantom's onset/recovery instants as everywhere else. The missed-DSP-tick theory is now cleanly ruled out.

The much bigger surprise: the phantom recurred with the same ~8000Hz magnitude, and its onset/recovery timestamps matched the previous capture to single-digit milliseconds (`t=615687ms` exactly in both files for the onset side, `t=769365ms` exactly in both for the recovery side) - as did every other cycle boundary across both ~18-minute sessions. That precision rules out a random cause (RFI, a cosmic-ray-style bit flip, a marginal SPI glitch) - it points at something deterministic and keyed to elapsed time since boot, not yet identified (no matching constant found in the codebase). Recommended next step: a much longer capture to see if it recurs again at further multiples of ~615s, and to stop looking at DSP-tick timing (now cleared) and start looking at other independently-timed processes in the firmware or the ESP-IDF/FreeRTOS runtime underneath it. Full reasoning in `null_bias_investigation.md`. No firmware change made this time.

## 2026-09-16, later still: `log_20260916_193030.txt` - new external audio-tone counter strongly confirms the energy-weighted swing isn't real, but its own baseline unexpectedly steps by large amounts every cycle; `tx_freq` phantom recurs a third time, still clean, still boot-locked to the same ms

User's new automated audio-frequency counter (measures the 700Hz tone's offset directly, triggered off the diagnostic log messages) gives the clearest confirmation yet: across all 7 cycles in this ~939s capture, the audio tone stays flat to ~1Hz while `energy_weighted` `dev` swings by 40-85Hz in the same window, and the plain fast/slow `held_freq` detector never fired once in the whole session. Strong new evidence the energy-weighted climb is a weighting-scheme artifact, not a real transmitted event.

New open puzzle, not yet explained: the audio tone's own flat baseline steps to a different value at every `~153.6s` cycle boundary - a sequence of -13.7, +2.0, -75.6, +81.5, -6.5(ish), 0, -9.7 Hz across the 7 cycles - with step sizes up to ~157Hz that don't match any confirmed-real event size and that the plain fast/slow EMA (silent throughout) would have caught if real. Leading candidate: the new counter may have its own version of the same fixed-phase-sampling susceptibility that produces the `tx_freq` phantom below, since it's triggered off the same cycle-boundary log messages - but this isn't confirmed, since there's no visibility yet into how the counter itself works. Recommended: either a description of the counter's own design, or a longer repeated capture to see if the plateau *sequence* itself repeats across boots (would point at the counter) or doesn't (would argue for something real).

`tx_freq` phantom shift recurred a third time (same 3-run signature, 241 lines, same magnitude) with `overruns=0`/`late=0` throughout on this third independent test, and its onset/recovery timestamps again match both previous captures to the millisecond (`t=615687ms`/`t=769365ms`). Tick-slip theory stays dead; boot-time determinism now confirmed across three separate boots. No firmware change made this turn. Full numbers and the plateau table in `null_bias_investigation.md`.

## 2026-09-16, later still: audio counter's own design (8192-pt FFT peak search @ 48kHz) explains the plateau-stepping puzzle - it's almost certainly being fooled by the already-known 8Hz/40Hz sideband comb, not measuring a real frequency shift

User described the counter: 8192 samples @ 48kHz (5.86Hz bins, 170.7ms window), peak search + bin interpolation, ~0.3Hz accurate "for a clean tone." Two numbers line up badly for this signal specifically: 700Hz itself sits 0.467 of a bin off-center (worst case is 0.5) so an unwindowed FFT leaks its own energy substantially into neighboring bins, and the already-documented 8Hz-spaced sideband comb (elevated at 40Hz) sits right in that same neighborhood (40Hz = 6.8 bins away) and also doesn't land on clean bin centers. A peak-search-and-interpolate algorithm built for a single clean tone has no guard against a nearby real comb of harmonic energy competing with it - exactly the kind of thing that would bias the reported frequency by tens of Hz in a way that steps once per measurement rather than drifting smoothly, matching what was observed. This ties the plateau puzzle directly to the already-open 8Hz/40Hz sideband item rather than pointing at anything new or real in the transmit chain. Not confirmed, no firmware change made - cheap next tests (windowing the FFT, narrowing the peak search, logging the winning bin) proposed in `null_bias_investigation.md`.

## 2026-09-16, later still: sideband-leakage theory for the audio-counter plateau puzzle is DEAD (proven by simulation) - real sidebands are ~100x too weak to matter; leading candidate moves to the SDR's own receive chain

User correctly flagged the real sideband levels (30-40dB down) and the 150Hz SDR filter. Simulated the counter's exact algorithm (8192-pt rectangular FFT + parabolic interpolation) against a -30/-40dB interferer at 8-40Hz offset, worst-case phase: bias stays within +/-0.05Hz of the no-interferer baseline (~0.4Hz), nowhere near the observed 15-157Hz plateau steps. Leakage theory falsified; also shows the counter's own math is already accurate enough that a bigger FFT block (which the user was weighing, but was rightly wary of - risk of smearing a real mid-block transition) wouldn't help and isn't recommended. With the counter's math and the transmit-side DSP both now cleared, the leading candidate shifts to the SDR receiver itself (LO/tracking/AGC stability) rather than anything in this firmware - consistent with the user's own much-earlier note that SDR tracking isn't reliable. Genuinely unresolved; doesn't change any transmit-side conclusion, since none of those rest on the SDR. Full simulation numbers in `null_bias_investigation.md`.

## 2026-09-16, later still: `log_20260916_200756.txt` ends in a "very noisy mode" the user compares to the known x4-interp burble - completely invisible to every diagnostic currently in place, and that's itself the finding

User reported the run ended in a noisy mode resembling the previously root-caused x4-interp burble (`ENVELOPE_INTERP_FACTOR=4` was confirmed twice on real hardware to cause audible noise via `dsp_task`'s wake rate, not per-tick compute time). Checked this ~1690s mid-session capture exhaustively - `overruns=0`/`late=0` on all 2640 samples, `max_busy_us` normal throughout, no canary hits, fast/slow flat, the usual `~153.6s` cadence runs cleanly right to the file's last, completely ordinary line. Nothing shows the reported noise at all.

That's informative rather than a dead end: every counter this project has (`overruns`/`late`/`max_busy_us`) measures per-tick busy duration, not how often `dsp_task` gets woken - exactly the dimension the x4-interp case's real root cause lived in. A transient wake-rate/ISR hiccup could produce the same audible symptom while leaving all of today's counters clean, simply because none of them watch that. One coincidence flagged, not explained: the user's own estimated ~155s recovery time sits close to this project's well-documented ~153.6s cycle, though a rough human estimate and a precise DSP-measured period aren't really comparable numbers. No firmware change made - would need either a description of what the noise actually sounds like (to check against the two already-known, distinct noise mechanisms in this project's history) or new instrumentation that watches wake-to-wake timing directly, which nothing currently does. Full reasoning in `null_bias_investigation.md`.

## 2026-09-16, later still: `log_20260916_205209.txt` - ADC disable ruled out as the noisy-mode cause; a second, fully-captured noisy episode (went noisy, snapped back on its own) again leaves zero trace anywhere in the diagnostics

User disabled the ADC as a candidate cause - no difference, so ADC is ruled out. This capture also contains a full noisy episode (onset and self-recovery both inside the ~550.7s window, ~847 `held_trace` samples) rather than cutting off mid-episode like last time. Same result as before: `overruns=0`/`late=0` throughout, `max_busy_us` normal (45-52us), no canary hits, no `tx_freq` phantom (expected - outside its usual boot-relative window), and the usual `~153.6s` cadence runs with completely normal timing straight through wherever the noise fell. Nothing marks it at all.

Two independent noisy episodes now, one candidate cause eliminated, both showing total silence from every current diagnostic - a real, audible symptom that current instrumentation structurally cannot see. This continues to point at either the envelope/PWM/analog chain (already-established theory: corruption there is invisible to every `freq_dev_hz`-based diagnostic, which is everything built so far) or a wake-rate/ISR-timing effect like the x4-interp precedent, rather than anything the frequency-side counters measure. No firmware change made. Full reasoning in `null_bias_investigation.md`.

## 2026-09-16, later still: rethink on why computed avg diverges from measured - this session's own new fast energy-weighted EMA likely uses the wrong envelope, and there's already a validated reference sitting unused to check it against

Stepped back from measurement-side theories to re-derive this from first principles, and found the answer was already sitting in this codebase's own history: `ssb_dsp.c` has a lifetime energy-weighted accumulator (`ssb_dsp_get_null_bias_stats()`), built and hardware-validated back on 2026-09-09/12, that pairs envelope² with the phase delta at the exact same raw tick using the RAW envelope - confirmed to match SDR readings to a few Hz across 6/6 tone pairs, and described as giving one STABLE number for a fixed configuration, not something that cycles through plateaus. This session's own new fast EMA (`diagnostics.cpp`, built from scratch this session) instead weights by a downstream, SHAPED envelope (post-ampeq/gdeq/predistort) - chosen earlier for being correctly time-aligned across the delay ring, but never checked for whether shaping also changes the envelope's VALUE right at the null, which is exactly the failure mode that would explain wild plateau-cycling on an unchanging bench setup.

Added a direct, same-instant cross-check (no bench-side action needed, no unmuting): every `ENERGY-WEIGHTED CONFIRMED STUCK` print now also prints the OLD, validated, raw-envelope lifetime average right below it, so the next capture settles this outright - if the validated reference stays small/stable while the new fast EMA claims tens of Hz, that confirms the fast EMA's envelope choice as the actual source of this whole session's central puzzle, not anything physical. Full reasoning in `null_bias_investigation.md`. Committed, not yet bench-tested.

## 2026-09-16, later still: cross-check confirms it on the first try - the shaped envelope is the source of the whole session's "computed vs measured" puzzle; added a second, correctly-paired fast EMA to nail it down further

The cross-check added last entry paid off immediately. Over a 45.7s window between two `ENERGY-WEIGHTED CONFIRMED STUCK` events, `ssb_dsp.c`'s validated, raw-envelope-paired lifetime average moved 3.2Hz (1184.87Hz -> 1188.07Hz - ordinary slow settling) while this session's own shaped-envelope fast EMA moved 83.0Hz (1056.37Hz -> 1139.40Hz) over the identical interval. The validated reference sees nothing; the new fast EMA reports a big swing anyway - confirms the shaped envelope (ampeq/gdeq/predistort/PWM mapping, all downstream of the raw DSP envelope) as the actual source of this whole session's puzzle, not the transmitted signal. Side note: even the fast EMA's own one-time boot anchor (1070.13Hz) sits 114.7Hz away from where the validated reference has read all session - the baseline itself was never trustworthy, not just the swings on top of it.

Added a second, correctly-paired fast EMA (same 50ms tau, but weighted by this tick's own raw envelope/freq_dev captured immediately after `ssb_dsp_process_sample()`, before any shaping) so the next capture gets a live-speed three-way comparison rather than relying on the slow lifetime accumulator alone. Two known simplifications flagged (pre-delay vs. the shaped EMA's post-delay; post-clamp vs. the validated accumulator's pre-clamp dphi) - likely minor, not yet proven so. If this new one also stays stable where the old one swings, the fix is clear: retire the shaped-envelope fast EMA. Full numbers and code changes in `null_bias_investigation.md`. Not yet bench-tested.

## 2026-09-16, later still: three-way cross-check comes back nuanced - shaping confirmed as ~65% of the swing, but a smaller residual remains even when correctly paired, most likely ordinary sampling variance rather than a second bug

First live data from the new raw-envelope-paired fast EMA (`cross-check2`): over the same 45.7s window, the old shaped-envelope EMA swung 83.07Hz, the validated lifetime average swung 4.02Hz, and the new correctly-paired fast EMA swung 29.31Hz - a real ~65% reduction, confirming shaping as the majority cause, but not a clean pass. Leading explanation for the remaining ~29Hz: ordinary sampling variance from running a fast (50ms) EMA on a quantity a lifetime accumulator needs millions of samples to average cleanly - this project already established exactly this reasoning back on 2026-09-12 for a different discrepancy, so it's a well-precedented, not ad-hoc, explanation.

Bigger-picture implication if this holds: a live, fast, 5Hz-threshold "stuck" detector may not be a workable instrument for this quantity at all, regardless of envelope pairing - the fix path is either a much longer EMA tau (trading responsiveness for the depth needed to tame the variance) or dropping the live detector in favor of periodic on-demand lifetime-accumulator reads (`'V'`), the way the original 2026-09-09/12 work already did it successfully. Not actioned yet - recommended a longer, unchanging-setup dwell test next, watching only `cross-check2`, to see whether its spread narrows with time (variance) or stays put (points back at the two remaining caveats: pre-delay vs. the active relative_delay, and post-clamp vs. pre-clamp dphi). Full numbers in `null_bias_investigation.md`.

## 2026-09-17: fresh `'J'` capture at delay=+2.00 reproduces the known null-crossing churn almost number-for-number, confirms a 5-day-old unconfirmed hypothesis about integer delay, and gives the still-open cross-check2 residual a concrete mechanism

New `'J'` jump-log dump at `relative_delay=+2.00` (this project's own established best two-tone IMD compromise, not an extreme test value): 1782960 qualifying (>300Hz) events since last reset, 40% near-null-blended, 45% near-null-either, an 8-entry ring showing a locked 2-state cycle alternating ~14201360Hz <-> ~14193359Hz. Raw near/far values (`near=-6802.0Hz far=1198.2Hz raw_delta=8000.1Hz`) are essentially identical to the 2026-09-12 capture at a completely different delay (+1.75) - same already-root-caused two-tone polar-null mechanism, not anything new, and `raw_delta` matching `step_hz` again confirms it's a real raw discontinuity, not a `relative_delay` interpolation artifact.

Two things worth flagging. First, every entry shows blended envelope identical to the min envelope (`env=X/X`) - the first-ever direct confirmation of a 2026-09-12 hypothesis that was proposed but never tested: at an exact integer sample delay (+2.00), `interp_ring()`'s blend weight collapses to 100%/0%, so there's no second raw sample to blend with. Second, and more relevant to the live investigation: roughly 55-60% of these jumps sit at a MODERATE envelope (~0.205, 4-5x the 0.05 null threshold) while still showing the full ~8000Hz raw discontinuity - meaning "envelope near null" is a narrower predictor than the current threshold assumes; the phase computation stays badly behaved over a wider envelope range around each beat minimum than the classification currently catches.

That last point gives the still-open `cross-check2` ~29Hz residual (previous entry) a concrete mechanism instead of a generic "sampling variance" label: `cross-check2` weights by `env2`, and at `env=0.205` that weight (`≈0.042`) isn't negligible, so this large, high-repetition-rate (hundreds-to-thousands/sec), moderate-envelope churn very plausibly isn't fully averaging out inside a 50ms-tau EMA's ~50-beat-cycle memory the way it does over the lifetime accumulator's millions of cycles. Reframes, doesn't contradict, the sampling-variance theory - and strengthens the case for lengthening the EMA tau substantially or dropping the live detector in favor of the periodic lifetime-accumulator read, since this is a structured, repeating signal component, not unstructured noise. Also confirmed the trailing `held_freq: ... dev=+40.00Hz from anchor=1070.13Hz` in this same capture is from the already-known-untrustworthy OLD shaped-envelope `'H'` detector (matches its known-bad boot anchor) - no new concern there. Not actioned - analysis only, no firmware change. Full derivation and two proposed next steps in `null_bias_investigation.md`.

## 2026-09-17, later still: the long-deferred hardware test finally happens - push/pull drivers fitted on the AD9851 lines, directly testing `AD9851.c`'s own leading "sticks" theory; hands-on impression is no change, hands-off run pending

The hardware test flagged weeks ago and never done: push/pull drivers now fitted on the AD9851 interface lines, speeding up the edges (especially the rising one). This directly targets the theory already written into `AD9851.c`'s own comments - that the old BS170-based inverting level shift gave a slow, passive, pull-up-charged LOW-to-HIGH edge (affecting both a fresh DATA 0-to-1 bit and, via the inversion, W_CLK's AD9851-side rising/sampling edge), and that a large one-tick FTW jump from two-tone near-null atan2 noise (flipping many bits at once) was "the leading theory for the two-tone-specific 'sticks' symptom."

First (hands-on) impression: no visible change to RF output. User's own added observation: there was already plenty of margin between the data change and the rising clock edge before this change, ahead of it independently. Consistent with `AD9851_BITBANG_EDGE_DELAY_ENABLED` having sat at `0` (no explicit settle delay) since 2026-09-11.

**Confirmed by the user**: the new drivers preserve the same inverting logic as before, just actively driven both directions now - `AD9851_INVERTING_LEVEL_SHIFT` correctly stays `=1`, no firmware/hardware mismatch. No firmware change made or needed.

Next: a hands-off run for a clean before/after comparison against the historical "sticks"/near-null-jump baseline - either the first real confirmation of a theory that's been open for weeks, or a clean way to rule out edge speed and redirect attention back to the phase/atan2 computation itself. Full reasoning in `null_bias_investigation.md`. Not actioned further, awaiting that capture.

## 2026-09-17, later still: `log_20260917_112714.txt` - the push/pull-driver hands-off run overturns "ordinary sampling variance" as cross-check2's residual explanation (it's aliasing of the well-documented ~153.6s cycle instead), shows no visible hardware-test improvement, and surfaces a real ~50Hz audio-tone excursion invisible to cross-check2

First hands-off capture on the new push/pull AD9851 drivers, ~14 minutes, 11 full `ENERGY-WEIGHTED CONFIRMED STUCK`/`RECOVERED` cycles with `cross-check`/`cross-check2` live throughout, plus the audio-tone counter running. Three findings.

**The STUCK/RECOVER cycle is confirmed as an exact, precisely repeating ~153.6-153.7s oscillation**: `STUCK+` (dev ~+68Hz) always lasts 89.7-89.8s, an ~18.2s gap, `STUCK-` (dev ~-16.5Hz) always lasts 45.7-45.8s, then immediately back to `STUCK+` - matching to within 0.1s across 5-6 repeats. This is the project's long-recurring `~153.6s` period, now pinned down as the already-known-buggy shaped-EMA detector's own deterministic hysteresis cycle. `relative_delay` stayed untouched at `+2.00` throughout.

**Big correction: cross-check2's "residual ~29Hz swing" is NOT sampling variance - it's aliasing.** Its 11 readings split into two near-identical clusters depending on which phase of the cycle they were sampled at: ~1168.6-1168.8Hz during every `STUCK+` (6/6, smoothly drifting down by hundredths of a Hz per cycle - a real slow trend, not noise) and ~1198.46-1198.48Hz during every `STUCK-` (5/5, same smooth drift). A value reproducible to within 0.02Hz across 11 independent events 14 minutes apart is not statistical variance - it's `cross-check2` only ever being sampled at ONE fixed phase point of the exact cycle just characterized (it's printed at the `'H'` detector's own trigger instant, always ~15s into a stuck period). That's textbook stroboscopic aliasing: sample a periodic process at a fixed phase and it always reads back the same value, regardless of how much it varies elsewhere in the cycle. Recommended fix path changes accordingly - lengthening the EMA's tau won't help (the problem was never insufficient averaging); the useful next step is printing `cross-check`/`cross-check2` on a fixed TIME cadence, decoupled from `'H'`'s trigger, across a full cycle to see the real waveform.

**The hardware test itself shows no visible improvement** - `overruns=0`/`late=0` throughout as always, and both the shaped-EMA cycle's signature and the audio-tone counter's plateau/drift character are indistinguishable from before the driver swap, matching the user's own hands-on impression. One wrinkle, now upgraded from "unexplained" to "confirmed real and significant": one `STUCK+` segment shows the audio-tone counter drifting to a sustained ~-50Hz, and the user directly confirms this was a real jump, not a counter artifact. `cross-check2`, sampled at the very same instant, reads its completely ordinary value - i.e. a genuine, user-confirmed ~50Hz shift left zero trace in EVERY frequency-side diagnostic this project has (`cross-check`, `cross-check2`, `'H'`, plain fast/slow, `'J'`). **Correction, same day**: the user also confirms the signal during this event was CLEAN, not noisy - so this is NOT the same mechanism as the "very noisy mode" symptom (that speculation is retracted); a clean-but-wrong-frequency signal points more specifically at something computing/encoding the wrong frequency cleanly (e.g. the AD9851 FTW itself) than at generic analog-chain corruption. Full numbers and next steps in `null_bias_investigation.md`. Not actioned - analysis only, no firmware change.

## 2026-09-17, later still: a new, distinct jump class spotted hands-on - frequency can be "walked back" toward nominal by stepping `relative_delay` (~2-3Hz per coarse step), but only in a particular off-nominal state, and doing so costs IMD - strong circumstantial evidence it's the same delay-interpolation mechanism already characterized, not a new bug

User's own hands-on finding: in a particular state, nudging `relative_delay` with `'['`/`']'` measurably shifts the measured frequency (~2-3Hz per 0.05-sample coarse step) - but this delay-dependence disappears once back on nominal frequency. Unlike the swings this investigation has tracked so far (which change the frequency reading while IMD stays reasonable), pulling this mode's reading back toward nominal by moving delay away from `2.00` (Preset 3's established best-IMD point) visibly degrades IMD - the "fix" trades a reading symptom for a real physical cost rather than correcting anything.

Very likely the same mechanism already characterized in `null_bias_investigation.md` since 2026-09-12 (and reconfirmed there this same day via the fresh `'J'` capture): `relative_delay`'s ring interpolation linearly blends two adjacent raw `freq_dev_hz` samples, and a 2-3Hz shift per 0.05-sample step implies a local raw slope of ~40-60Hz/sample at that point in the cycle - far smaller than a full ~8000Hz null-crossing jump, but a real, non-zero slope, consistent with sampling a moderately-varying (not flat, not at a null) part of the signal. Explains the IMD cost too: `2.00-2.10` is the independently-established true physical optimum, so moving off it to chase a reading that's itself just an interpolation artifact gives up real IMD for no real correction.

**Sharpened same day, prompted by a direct user challenge**: "point in the cycle" needed unpacking, since 700/1700Hz are exactly locked to the 16000Hz sample rate and the ADC is disabled - there's no real-world jitter anywhere in this loop, and the raw DSP math must be exactly periodic every 10ms (the two-tone waveform's own repeat period) forever. Two different cycles were being conflated: the FAST, exactly-periodic 1ms/10ms two-tone beat (locked, no ambiguity - the user's point stands) and the SLOW ~153.6s macro cycle pinned down earlier today, which - since it can't come from a purely periodic, memoryless raw computation - must actually originate downstream, in the shaped-envelope IIR filters or the `'H'` detector's own hysteresis logic, not upstream. The delay-walk effect itself re-points at the FAST cycle: a human keypress isn't synced to a 1ms/10ms clock, so each attempt lands at an effectively arbitrary phase of the exactly-repeating beat (sometimes near a null, sometimes not) - explaining why it's only sometimes observed, with zero real randomness involved. Falsifiable refinement: triggering the delay-walk test at a controlled phase offset relative to the two-tone generator, rather than an arbitrary keypress, should make it fully predictable. Full reasoning in `null_bias_investigation.md`. Not actioned, no firmware change.

## 2026-09-17, later still: a rigorous user challenge (LTI stages + locked tones + no ADC = no room for variability) leads to a real bug - the two-tone generator's naive float32 phase accumulator - confirmed by simulation, but its drift rate is ~16x too slow to be THE ~153.6s mechanism

User correctly pointed out that the two-tone generator, Hilbert FIR, and shaping IIR filters are all deterministic LTI stages fed an exactly-periodic (10ms) input with no ADC involved - so an idealized version of this chain has no business containing anything at the ~153.6s timescale. Checked the actual generator code (`test_signals.cpp`): it's a naive floating-point NCO (`phase += increment; if (phase > two_pi) phase -= two_pi;`, all in 32-bit `float`) - a well-known class of implementation that accumulates rounding error over time rather than recomputing exactly each tick.

Simulated the exact recurrence to check whether this actually matters - and caught a mistake in my own first attempt before trusting it. A quick "count wraps over one window" method first suggested an almost-too-good ~162.6s match; re-running properly (tracking true unwrapped phase against an exact double-precision reference across 8 million ticks) showed the first result was a boundary-quantization artifact of that quick method, not real. The corrected measurement shows a genuine, linear drift: the 700Hz tone runs +1.8e-4 Hz fast, the 1700Hz tone runs -2.0e-4 Hz slow, making the true beat frequency off by ~-3.8e-4 Hz - a real precession, but with a ~2500s (~41-44 minute) period, about 16x too slow to be the ~153.6s cycle this investigation has been chasing.

Net result: a genuine, low-risk, worth-fixing floating-point bug in the test-tone generator (fix: exact modulo-based or integer phase accumulation instead of accumulate-and-subtract), but NOT the explanation for the exact 153.6s period - that likely still lives downstream (the shaped-envelope IIR filters or the `'H'` detector's hysteresis), with the ESP32's actual hardware timer rate (vs the assumed exact 16000.000000Hz) newly flagged as another candidate worth checking. Full simulation numbers and the self-caught methodology error in `null_bias_investigation.md`. Not actioned - analysis only, no firmware change.

## 2026-09-17, later still: does the NCO drift matter more AT the null? Yes - refined math lands within ~7% of the observed 153.6s cycle; the user's own follow-up insight names the correct fix directly, now implemented and verified by simulation to give exactly zero drift

Answered the natural follow-up to the last entry: the small NCO drift found there is far more consequential AT the null than its "average beat frequency" effect suggested. With exactly 16 samples per 1kHz beat cycle, the earlier ~2639s full beat-phase-drift period, divided by those 16 discrete sample slots, gives ~165s - within ~7% of the precisely-measured 153.6-153.7s macro cycle. Not exact, but a much stronger, more mechanistically-grounded candidate than the raw beat-drift number, since it's the discrete "which sample lands closest to the true null" granularity that downstream near-null-sensitive computation actually sees.

The user's own follow-up nailed the fix directly: since the whole chain is locked and deterministic, every two-tone cycle should be reproduced bit-for-bit identically, not just "drift slowly." Implemented in `test_signals.cpp`: the old accumulate-and-subtract phase generator is replaced with an exact recompute from a wrapping integer sample index each tick (tone1 always, tone2 whenever dither is off) - verified by simulation to give exactly zero drift (0.0 deviation at every boundary across an 8M-tick run, vs. the old scheme's confirmed growing error). Dither keeps its own accumulator since its frequency is intentionally time-varying. Not yet bench-tested (no toolchain here). The decisive test still pending: whether the real ~153.6s cycle changes at all once every cycle is bit-exact on real hardware. Full reasoning and code details in `null_bias_investigation.md`.

## 2026-09-17, later still: `log_20260917_122337.txt` - first hardware data with the NCO fix flashed. The macro cycle didn't disappear, but its period changed dramatically (~153.6s -> ~8.15s, ~19x faster); the separately-reported "twice a second changing tone cycle" is not confirmed or explained by this file

First real capture since the `test_signals.cpp` phase-generator fix was flashed, same 700/1700 test setup. The `slow_trace: AUTO-CAPTURED` event's period is now **8148.5ms +/- 3.6ms** across 53 consecutive intervals - tight and clock-like, but about **19x faster** than the ~153.6-153.7s cycle characterized on every pre-fix capture. Not "the fix didn't work" - a real, substantial change in the mechanism's operating period on the same config, strong (if indirect) evidence the old NCO drift was feeding whatever triggers this hysteresis, even though the hysteresis logic itself (still most likely the shaped-envelope IIR filters or the `'H'` detector) evidently lives downstream and didn't disappear.

Separately: the user reported "a regular freq shift about twice a second," clarified as a changing tone CYCLE (character/timbre) rather than a discrete jump. Nothing in this file confirms, explains, or rules this out - the only fine-grained per-tick data is a ~17ms burst, far too short to see a ~500ms-scale pattern, and no other timestamped structure lands near that rate. Needs a continuous multi-second capture, or a marker press at the moment it's heard. Full reasoning in `null_bias_investigation.md`. Not actioned - analysis only, no firmware change this turn.

## 2026-09-17, later still: `ToneWarble1.wav` - the "twice a second changing tone cycle" measures out to almost exactly a 1.000s period, not 2Hz, and the leading candidate is `diagnostics_service()`'s own 1000ms-gated Serial print block on Core 1 - a mechanism this project has already partially characterized as capable of disturbing dsp_task on Core 0

A short-time FFT analysis of the user's own audio recording of the symptom found a real, tightly repeating pattern throughout the whole 4.93s clip: a long (~430ms) dominant-tone dwell, a shorter (~265ms) dwell at a nearby frequency, then a busier flickering tail, repeating with measured inter-cycle gaps of 1000.0/994.6/997.4/997.3ms - essentially exactly 1 second, not the "twice a second" impression, likely because each 1Hz cycle contains two salient tone-character states that a listener would naturally count as two events.

Grepped the whole codebase for anything unconditionally periodic at ~1000ms. Found exactly one strong candidate: `diagnostics_service()` (Core 1, `diagnostics.cpp:2536`) fires an unconditional (whenever diagnostics aren't muted) `Serial.printf()` burst - `print_timing_and_adc_block()` + `print_null_bias_block()` - once per second, every second, for as long as the unit runs. This isn't a new theory being invented cold: this project has already measured and documented (same file's header comments) that this exact print block's cost is watched, and an earlier "Fs jitter hunt" already found that blocking `dsp_task` (Core 0) stretches its gptimer wake-up cost via cross-core IPI - i.e. this class of Core-1-disturbs-Core-0 interference is already a known, characterized risk in this codebase, just not previously tied to an audible, once-a-second symptom. The other 1000ms-gated code path found (`envelope_output.cpp`'s DAC I2C error log) was ruled out - it only fires on an actual bus failure, not as a steady background event.

**Open confound**: whether Serial/diagnostics output (`'v'`) was actually unmuted during this recording - if muted, this print block never runs and the candidate is eliminated. Cheap decisive test proposed: record with `'v'` toggled between clips and see whether the ~1.000s pattern tracks the mute state. If it does, this would be the first mechanism in the whole investigation shown to correlate with something audible in real time, rather than only with internal detector/diagnostic state. Full reasoning in `null_bias_investigation.md`. Not actioned - analysis only, no firmware change.

## 2026-09-17, later still: the `diagnostics_service()` theory for the ~1.000s `ToneWarble1.wav` cycle is ruled out - `'v'` was already off during the recording and turning it on makes no difference; an exhaustive re-search finds no other unconditional ~1Hz timer anywhere in the firmware, pointing suspicion at the monitoring/recording chain instead

User confirms `'v'` (diagnostics) was already muted when the warble was recorded, and switching it on made no audible difference - this rules out the previous entry's leading candidate cleanly, since that whole print block (and `print_status_line()`) sit behind the same mute gate and literally never execute when `'v'` is off.

Re-searched the whole codebase for any other unconditional ~1Hz mechanism (no mute gate involved) and found none: no other hardware/software timer, no WiFi/BT/OTA code at all, `loop()` itself has no counter that could produce a ~1s cadence independent of `diagnostics_service()`'s own gated timer, and there's no preset auto-cycle anywhere (presets are static, keypress-selected only). This is a genuinely exhausted firmware-side search for this specific signature.

Balance of evidence now points away from this project's ESP32 firmware and toward the monitoring/recording chain itself (receiver AGC, SDR software's own AGC/averaging rate, USB audio buffering, etc.) as the source of the ~1.000s cycle. Next step: find out exactly how the recording was made (what's on the receiving end) before searching the TX firmware further for this. Full reasoning in `null_bias_investigation.md`. Not actioned - analysis only, no firmware change.

## 2026-09-17, later still: recording-chain details point the `ToneWarble1.wav` ~1.000s cycle at the SDR receiver's own AGC, not the TX firmware - visible directly on the spectrum display's baseline noise (not just the audio), and only present on two-tone (sine/AM are clean)

Two key facts from the user: the recording is Audacity capturing an SDR's audio output, and the ~1.000s pattern is visible directly on the SDR's own spectrum display as modulating baseline noise - not just in the demodulated audio - which rules out an Audacity/USB-audio-buffering artifact. Critically, sine tone and AM test signals are clean; only two-tone shows it.

That's a strong pointer at classic receiver "AGC pumping": two-tone has a much higher peak-to-average ratio and genuine envelope nulls compared to a constant-envelope sine or gentle AM signal, and AGC loops are known to oscillate visibly/audibly on exactly that class of signal, at a rate set mostly by the AGC's own attack/decay time constants (which would also explain why the measured period is so tight - a control loop's self-timed oscillation is often more regular than the input driving it). This means the already-well-characterized two-tone envelope irregularities this investigation has spent weeks on may be sufficient to trigger this via the SDR's own AGC, with no new firmware bug required.

Cheap decisive test proposed, no firmware involved: switch the SDR to manual/fixed gain (AGC off) and see if the pumping disappears. If it does, this symptom moves out of the firmware investigation entirely. Full reasoning in `null_bias_investigation.md`. Not actioned - analysis only, no firmware change.

## 2026-09-17, later still: retracting the AGC theory - user has already checked it directly and is confident the ~1.000s cycle is coming from the ESP - made a temporary, clearly-marked causal-test change (1000ms -> 1700ms) to the one concrete "~1 second internal loop" this codebase has, per the user's direct request

The user has already checked the SDR's AGC and is confident this originates on the ESP32 side - retracting the previous AGC-pumping theory; direct hands-on verification overrides that inference.

Confirmed first that `diagnostics.cpp`'s fast/slow EMA detector (2s tau, backing the `'H'`/held-frequency detector) is read-only - it only ever watches `tx_freq`, never writes back into the live `freq_dev_hz`/`tx_freq` computation, so it has no path to affect the actual transmitted signal regardless of its tau value.

The one thing that IS both unconditional-when-unmuted and concretely ~1 second is `diagnostics_service()`'s own print-block gate (`diagnostics.cpp`) - the same one the earlier mute-toggle test seemed to rule out. But mute on/off only tests presence/absence, not the period itself. Per the user's direct request, retuned it from 1000ms to 1700ms (temporary, clearly commented, easy to revert) as a genuine causal test: if the audible/spectrum cycle shifts to ~1.7s with `'v'` ON and this build flashed, that proves this block (or something keyed to its exact timing) is the mechanism; if the cycle stays at ~1.000s regardless, that rules it out via direct parameter variation rather than just on/off, redirecting the search to the two-tone-specific DSP path instead. Note: this test REQUIRES `'v'` on to have any effect at all (unlike the original muted recording). Not yet bench-tested - ready to flash. Full reasoning in `null_bias_investigation.md`.

## 2026-09-17, later still: the 1700ms causal test also comes back negative - `diagnostics_service()` conclusively cleared by two independent tests, reverted back to 1000ms; answered the user's direct question on 700/1700's interference periodicities, which turned up a striking exact-100-cycles-per-second coincidence shared by every preset in this codebase - rigorously checked against (and cleared) today's own NCO-fix wrap boundary by direct float32 simulation

`'v'` on/off still makes no difference with the print gate at 1700ms - same result as the original mute test on the 1000ms build. Combined with the earlier finding that this whole detector module only ever watches `tx_freq` and never feeds back into the live signal, `diagnostics_service()` is now cleared by two independent tests, not just reasoning. Reverted the gate back to 1000ms.

Answered the user's direct question on 700Hz/1700Hz's interference periodicities in full (difference/sum/3rd- and 5th-order IMD products, individual tone periods, and the GCD-based 10ms fundamental repeat, already established this session). None of these individually is near 1 second, but a striking exact relationship stands out: 100 repetitions of the 10ms fundamental, and separately all 16000 raw samples, both equal exactly 1.000000 second - and this isn't unique to 700/1700: every preset in `TWOTONE_BAND_PRESETS[]` shares the same 100Hz GCD by design. Also caught and flagged a small, separate, harmless bug while checking this: the "700/1700 legacy default" preset's own display string is mislabeled "700/1900."

Because today's own `test_signals.cpp` NCO fix wraps its sample index at exactly `SAMPLE_RATE_HZ` (exactly once per second), this exact coincidence made it the obvious thing to re-check rigorously rather than assume clean. Built a bit-accurate float32 simulation of the real phase-generation arithmetic and measured the actual discontinuity at each 1-second wrap vs. ordinary 10ms boundaries and arbitrary non-boundary samples: the wrap-boundary jump (~5e-08) is smaller than ordinary floating-point jitter elsewhere in the run (1e-05 to 3e-04) - the fix is cleared, confirmed by simulation rather than just the hand-argument in its own comment.

Mechanism remains unidentified. Proposed next test: since every built-in preset shares the same 100Hz-GCD grid, try a deliberately non-100Hz-spaced pair (e.g. 733/1717Hz) to see if breaking that exact alignment changes or removes the ~1.000s pattern - a real causal test of whether the "100 cycles per second" coincidence matters at all. Full reasoning and simulation numbers in `null_bias_investigation.md`. No firmware change this turn beyond the 1700->1000 revert.

## 2026-09-17, later still: answered directly - the 1-second wrap always lands exactly at the envelope PEAK (structurally, by construction), never near a null, and its 0.5ms distance to the nearest real null is no different from any ordinary peak's; `Warbletone3-300-500.wav` confirms the ~1s pattern also shows up on the 300/500 preset

Checked rigorously rather than assumed: at every wrap, both tones' phase resets to exactly 0 (by the fix's own recompute-from-index formula), which is precisely the two-tone envelope's maximum - verified numerically at exactly 2.000000 (the theoretical max) for both 700/1700 and 300/500. The nearest real null sits 8 samples (0.5ms) either side for 700/1700, which is completely unremarkable - nulls recur every 16 samples throughout the ENTIRE waveform regardless of the wrap, so the wrap is no closer to a null than any other ordinary peak. Combined with the earlier discontinuity-size result, today's NCO fix is now cleared twice over.

`Warbletone3-300-500.wav` (a different preset, same 100Hz-GCD family) shows the same ~1s pattern (autocorrelation peak at 0.979s), confirming this isn't unique to 700/1700. The proposed non-100Hz-spaced test pair is still the cleanest next step. Full reasoning and numbers in `null_bias_investigation.md`. Analysis only, no firmware change.

## 2026-09-17, later still: `warble733-1717.wav` (+ its `'v'`-on companion) still shows ~1.000s, but a math check caught a mistake in the test pair itself - 733/1717 are coprime, so their own TRUE fundamental period is exactly 1.000s by definition, and (bigger realization) EVERY integer-Hz two-tone pair is mathematically guaranteed to repeat a whole number of times per second, so no integer-Hz pair can ever be a clean "broken alignment" control. Added a temporary non-integer-Hz test preset to `test_signals.cpp`

Both the muted and unmuted `733/1717` recordings show the same ~1.000s autocorrelation peak (0.384/0.997s) as every prior capture. But checking the math first: `gcd(733,1717)=1`, so this pair's own correct, expected fundamental period IS exactly 1 second - it isn't a "broken alignment" control as intended, it's if anything MORE tied to 1 second than 700/1700 or 300/500. Bigger realization: since the GCD of any two integers is itself an integer, EVERY possible integer-Hz two-tone pair mathematically must repeat a whole number of times per second - this was never a special property of any specific preset, so it can't distinguish a real bug from normal behavior using integer-Hz tones at all.

The only way to actually break this alignment is a non-integer-Hz pair. No serial command exists to dial in custom frequencies, so added a temporary preset to `TWOTONE_BAND_PRESETS[]`: `700.37/1700.61 (TEMP non-integer-Hz control)`, reachable via repeated `'T'` presses, clearly marked for removal once tested. If the ~1.000s pattern still shows up with this pair (whose own natural repeat period doesn't land on any whole-second boundary), that's decisive proof the mechanism is something else entirely, not the tone generator's own periodic structure. Full reasoning in `null_bias_investigation.md`. Ready to flash and test.

## 2026-09-17, later still: the decisive non-integer-Hz test (`700.37/1700.61Hz`) is a genuine positive result - the clean single ~1.000s peak seen on every integer-Hz pair so far is gone, replaced by a smeared cluster of 8 comparably-strong candidates across 0.3-1.0s. The exact-whole-cycles-per-second alignment really does matter; leading theory shifts to a live in-DSP resonance/entrainment mechanism (same class as the already-known 153.6s/8.15s macro cycles, just not yet located) now that every specific timer candidate is cleared

Ran the decisive test: `700.37Hz/1700.61Hz`, `'v'` off. Confirmed with two independent autocorrelation methods (binary bin-match and continuous peak-frequency value) - both agree: no single dominant ~1.000s peak anymore. Every integer-Hz recording so far showed one clearly dominant peak at ~0.98-1.00s; this one shows eight comparably-strong candidates (0.312-1.000s, correlations 0.549-0.624) with no clear winner.

This is meaningful, not a null result - the underlying beat/null-crossing rate barely changed (1000.24Hz vs 1000Hz, a 0.024% difference), nowhere near enough to explain going from one sharp line to eight smeared candidates on its own. So it's specifically EXACT commensurability with 1 second that matters, not just approximate tone spacing.

With every specific timer/counter candidate now cleared (diagnostics_service() twice over, today's NCO fix twice over), this looks like a classic signature of a nonlinear/hysteresis mechanism being driven by a periodic input - forced into one clean resonant line when the input repeats exactly every cycle, smeared across nearby periods when it doesn't. That's the same CLASS of behavior already established this session for the 153.6s/8.15s macro cycles, just not yet located in the live DSP/envelope path itself (as opposed to diagnostics.cpp's already-cleared read-only copy). Next step: look at ssb_dsp.cpp's Hilbert/atan2 chain and envelope_gdeq.h's shaping IIR filters for anything with its own feedback/threshold behavior near a 1-second timescale. Full reasoning in `null_bias_investigation.md`. Analysis only, no firmware change this turn.

## 2026-09-17, later still: `733-1700_Preset1.wav` (every optional shaping/filter/compressor/EQ/predistortion/interpolation/ampeq stage OFF) - the pattern gets CLEANER and STRONGER, not weaker, revealing its true fundamental at ~0.485s (~2.06Hz) - almost exactly "twice a second," the user's original description from the start of this whole thread. Core `ssb_dsp_process_sample()` pipeline itself checked directly and has no candidate timer - leading theory is now the already-known near-null "sticks" mechanism recurring deterministically because the input is exactly periodic

Parsed the pasted preset line against `PersistentSettings`' field order to confirm "all filters off" really does disable every optional stage (gdeq shaping, ADC LPF, EQ, compressor, predistortion, null floor, slew limiter, envelope interpolation, ampeq). With all of that off, the autocorrelation signal got CLEANER (0.822 correlation, the strongest yet) and revealed harmonic structure matching a true ~0.485s (~2.06Hz) fundamental, with the previously-measured "1.000s" being its own 2nd harmonic. This is the first time the numbers land almost exactly on "twice a second" - the user's very first description of this symptom, weeks/entries ago - rather than needing the "two salient states per 1s cycle" hand-wave used earlier today.

This simultaneously rules out every disabled optional stage as the source, since the effect survived and strengthened. Read `ssb_dsp_process_sample()` end to end (the one pipeline stage that's always on, unconditionally) and found nothing with a ~0.5s-scale time constant - just a short (~8ms) Hilbert FIR, memoryless atan2, unconditional lifetime accumulators (no periodic reset), and a static clamp. Leading theory: this is the already-well-characterized near-null "sticks" phenomenon (atan2/Hilbert fragility at envelope zero-crossings) occurring deterministically and repeatably because the two-tone input itself is exactly periodic (100Hz GCD) - if only some fraction of the ~100 null-crossings/second hit the specific floating-point corner case that triggers a stick, and that subset recurs every ~48-49 cycles, you'd get exactly this signature, with no separate timer or filter needed. Also explains why the non-integer-Hz test smeared the pattern instead of removing it.

Proposed next step: capture a 'J' jump-log and an audio recording of the same run simultaneously (Preset 1, 700/1700) to directly check whether the jump-log's own near-null event timestamps recur at ~0.485s/~0.97s intervals - would confirm this theory outright rather than leave it plausible-but-unverified. Full reasoning in `null_bias_investigation.md`. Analysis only, no firmware change.

## 2026-09-17, later still: the requested simultaneous `'J'`-log + audio capture arrives (`log_20260917_142917.txt` + `700-1700_Preset1.wav`) - it complicates rather than confirms the "0.485s is the true fundamental" theory from the previous entry, and the overreach gets corrected

The log turned out to be a single `slow_trace: AUTO-CAPTURED` event plus one manually-triggered `'J'` snapshot (8 entries within a 5ms window) - a one-shot capture, not a continuous multi-trigger series, so it can't actually test a recurrence-interval theory (that needs many repeated, timestamped trigger events to measure an interval from). Noted for next time.

The paired `700-1700_Preset1.wav` (same Preset 1/all-filters-off condition as the previous entry, but 700/1700 instead of 733/1700) gave a clean single ~1.003s autocorrelation peak with NO 0.485s component - checked explicitly, correlation at 0.485s came back negative (-0.111). This directly contradicts the previous entry's "0.485s is the universal true fundamental, 1.000s is just its 2nd harmonic" claim.

Correcting that overreach rather than letting it stand: that claim was drawn from a single recording (733/1700) and shouldn't have been generalized. The pattern now looks like every 700/1700 (100Hz-GCD) test done today shows one clean ~1.000s peak with no 0.485s substructure, while both 733-based (coprime, GCD=1) tests have shown the extra ~0.485s component - suggesting it may be specific to the 733Hz/coprime-pair class rather than a universal property. Genuinely still open, just narrowed and honestly corrected.

This gap - a single log snapshot can't test a recurrence-interval theory - is exactly what a full-rate per-sample capture feature would solve; see the next entry for its design and implementation.

## 2026-09-17, later still: implemented the requested full-rate freq/env capture - new `'F'` serial command

Built the feature the user asked about ("would it be worth doing a dump of all freq/env values every sample for a second or so"). `'F'` arms a capture that lazily allocates a ~125KB buffer (16000 samples, ~1.0s at 16kHz - matching the sample rate and today's own NCO-fix wrap period) for `raw_freq_dev_current`/`raw_envelope_current`, fills it from the existing per-tick diagnostic hook (`diagnostics_set_tx_info()`), then dumps it as CSV in small chunks from `diagnostics_service()` (same TX-buffer-safety pattern as every other print in this file) once full, freeing the buffer afterward. Not a permanent static buffer - the board (ESP32-S3 Super Mini) is assumed to have no PSRAM, so it's allocated only while actually capturing. AD9851-only, same as `'J'`/`'K'`/`'H'`. Gives direct per-tick ground truth to check the still-open "does a near-null event recur at ~0.485s/~0.97s" theory against, instead of inferring it from SDR audio. Verified brace/paren/`#if` balance across all three edited files (`diagnostics.h`, `diagnostics.cpp`, `serial_commands.cpp`) - no ESP32 toolchain available here to do a real build, so this is unverified beyond that; not yet bench-tested, ready to flash. Full design in `null_bias_investigation.md`.

## 2026-09-17, later still: `'F'` works first try on real hardware (`log_20260917_153446.txt`, 700/1700 Preset 1) - it overturns one part of the leading theory and surfaces a new, more precise open question

Near-null "sticks" turn out to happen at essentially EVERY null crossing (1000 events across the 16000-sample/1.0s buffer, one every ~1ms) - not a rare subset recurring every ~48-49 cycles as the previous entry theorized; that specific claim is retracted. What actually varies is the glitch VALUE at each null: it mostly locks into an exact, bit-for-bit repeating 5-event (~5ms) template (70+ perfect repetitions seen in the first 0.346s of this capture), but 16 of the 1000 events (1.6%) briefly take on a different value instead, clustered into 6 short groups spread unevenly across the remaining ~0.65s of the capture - none in the first third, then a denser run later on. One second of data isn't enough to responsibly claim these 6 "slip" clusters recur at a specific interval (loosely near-matching either the ~0.485s or ~0.97s audio-derived numbers, but flagged explicitly as "worth watching for," not a finding, given how the last two entries had to walk back similar small-sample overreach). Next step: a longer single `'F'` capture (would need `FREQENV_CAPTURE_LEN` increased past 16000, RAM-budget permitting) to actually get enough slip events in one continuous window to measure their own recurrence directly. Full numbers in `null_bias_investigation.md`. Analysis only, no firmware change this turn.

## 2026-09-17, later still: independent from-scratch simulation of `ssb_dsp.c`'s real algorithm reproduces `log_20260917_153446.txt`'s exact glitch values, sub-Hz precision - the near-null "sticks" mechanism is now CONFIRMED math, not board noise

Built a Python port of `ssb_dsp_process_sample()`'s actual pipeline (real 65-tap Hilbert FIR, `fast_atan2`/`fast_sqrt`'s real approximations, float32 throughout) driven by the same deterministic 700/1700Hz two-tone generator, with nothing tuned to match the capture - every constant taken directly from the source. Result: every value in the capture's exact repeating template (`-6802.0, 6003.4, 6169.4, 6171.2, 6004.1` Hz) appears in the simulation's own glitch-value set to within ~1Hz, the rarer "slip" values match too (-7705, -7891, 7600 clusters all present), event rate matches (~1 stick per 16 samples in both), and even the slip RATE matches closely (1.4% sim vs 1.6% real). This confirms the near-null sticks - ubiquity and magnitude both - are expected, deterministic math (`fast_atan2`/`fast_sqrt` phase noise at envelope zero-crossings, exactly what the existing clamp comment already names), not board-specific noise or a hardware defect. Still open: what determines which specific slip value occurs when, and whether that connects to the ~0.485s/~0.97s audible warble - this explains the raw mechanism, not its higher-level timing. Simulation script kept out of the repo (ad hoc analysis, same as this session's other Python work) and delivered directly. Full numbers in `null_bias_investigation.md`.

## 2026-09-17, later still: `'F'` capture length doubled (1.0s -> 2.0s, ~125KB -> ~250KB) after the user reported this board's real free-RAM headroom from an actual build

User reported the board's build output directly: ~292KB free at link time against a 320KB chip (no PSRAM, as assumed), comfortably more than the original 1s capture needed. Bumped `FREQENV_CAPTURE_LEN` 16000 -> 32000, updated every place that quoted the old size (`diagnostics.h`, `diagnostics.cpp`, `serial_commands.cpp`), and deliberately stopped at 2.0s rather than spending the whole reported headroom, since that pool is shared with task stacks and other runtime heap use the link-time report can't see - the existing malloc-failure fallback protects against guessing wrong. Gives room to catch 2-4 repetitions of the still-open ~0.485s-0.97s slip-timing question in one continuous window. Not yet bench-tested at the new size. Full reasoning in `null_bias_investigation.md`.

## 2026-09-17, later still: `'F'` no longer interleaves other Serial output with its CSV - user's external logger was choking on it

The first capture parsed cleanly by hand but only because ~192 stray non-CSV lines (blank lines, one `slow_trace: AUTO-CAPTURED` print) had to be filtered out first - the user's own logger can't do that and flagged it as a real problem. Fixed by having `diagnostics_service()` skip everything else it normally does (status line, timing block, canary check, slow-trace/held-freq auto-dumps) for as long as a freqenv capture is armed, filling, or dumping - a hard early-return covering the capture's whole lifecycle, not just the CSV-printing phase. Tradeoff accepted: those background monitors go quiet for up to a couple seconds during a capture, same as this feature already being mute-exempt for the same "explicit, one-shot, user-requested" reasoning. Not yet bench-tested. Full reasoning in `null_bias_investigation.md`.

## 2026-09-17, later still: MAJOR finding - the raw freq_dev/envelope signal is PROVABLY exactly periodic at 1.000s for any tone frequency (proven by simulation, confirmed on real hardware to 40-microsecond precision), which explains today's whole ~1.000s thread but puts the earlier 733-1700 "~0.485s" finding in tension

Second `'F'` capture (`log_20260917_160237.txt`) arrived truncated (user's logger kept only the last 0.54s of the 2.0s buffer - flagged, suggested a dedicated file-logging tool instead of terminal scrollback next time), but the CSV itself had zero interleaved lines - the suppression fix worked. That partial data's slip-cluster timing turned out identical (to 40 microseconds, across 5 independent points) to a segment of the FIRST capture, offset by a constant ~1.019s. Checked why via simulation: `ssb_dsp_process_sample()`'s output is exactly periodic at 16000 samples/1.000s to zero float32 difference, for EVERY tone-frequency pair tried (700/1700, 733/1700, and the non-integer-Hz control) - a direct, provable consequence of the tone generator's own NCO-fix recomputing phase from a sample index that always wraps at exactly 16000, regardless of f1/f2. This is a complete, hardware-confirmed explanation for why the raw digital signal repeats every ~1.000s - not a bug, a structural certainty of the current tone-generator design.

The catch: this same proof means the digital signal CANNOT produce the ~0.485s component recorded earlier today for `733-1700_Preset1.wav` (733/1700 is now confirmed exactly-16000-sample-periodic too) - that earlier finding isn't wrong exactly, but needs re-examination: either something downstream of the core DSP that Preset 1 doesn't fully silence, or the SDR/audio side, not this mechanism. Flagged as the concrete next step rather than resolved. Full numbers and reasoning in `null_bias_investigation.md`.

## 2026-09-17, later still: requested 733/1700 `'F'` capture comes back full and clean (`log_20260917_162817.txt`) - it's ALSO exactly periodic at 1.000s on real hardware (zero difference, whole 2.0s buffer), which settles the previous entry's open tension outright

Full 32000-sample capture this time, properly logged to a file (no truncation). Splitting the buffer at its midpoint and comparing the two halves directly: max difference = 0.0, bit-for-bit identical, across the entire 1-second period - for contrast, a non-period lag on the same data showed an 8.2kHz difference, so this isn't a degenerate/trivial result. This definitively rules out `ssb_dsp_process_sample()`'s own output as the source of the earlier ~0.485s finding in `733-1700_Preset1.wav` - that audio correlation was real, but it cannot come from the core DSP loop, now proven exactly periodic on the very same tone-pair/preset that produced it. Narrows the remaining mystery to two candidates, neither tested yet: something downstream (PWM mapping, relative_delay, envelope_interp, or the AD9851 chip's own analog behavior) that "Preset 1, all filters off" doesn't actually fully disable, or the SDR/audio-capture chain itself. Next step proposed: revisit the SDR-side test (manual/fixed gain) that was floated early today but never actually run, now that the ESP's own core DSP is doubly proven clean. Full numbers in `null_bias_investigation.md`.

## 2026-09-17, later still: `'Q'` dither simulation looked like a fix (destroys the whole-second periodicity, 14677Hz cycle-to-cycle diff) but a real-hardware test came back NOISIER, not cleaner - dither now suspected to be the wrong tool, existing freq_dev slew-rate limiter proposed as the next candidate instead

Simulated the existing `'Q'` two-tone dither (unchanged, 0.05Hz/4Hz) against the newly-proven whole-second periodicity and confirmed it decisively breaks cycle-to-cycle repetition (max diff 0.0Hz -> ~14677.5Hz). Looked like confirmation `'Q'` already solves the problem with no retuning needed - user had asked for "test generator only" scope, which this respects.

Then the user actually tried `'Q'` on the bench and reported a NOISIER result, not an improvement - contradicting the simulation. Working theory: dither only shuffles WHEN/how correlated the ~5000-9000Hz near-null `freq_dev` spikes are, it doesn't shrink them - so a single coherent periodic buzz became the same total spike energy smeared randomly across the run, which likely sounds worse even though the coherent tone is gone. User then asked whether `fast_atan2`/`fast_sqrt` themselves should be randomized to kill "characteristic output patterns" - reviewed both functions and found neither is a lookup table/quantizer, both are smooth deterministic polynomial approximations, so the repeating glitch "template" seen earlier is a symptom of the tone generator's proven exact periodicity revisiting the same (I,Q) pairs, not something separately baked into the trig functions - randomizing them would very likely hit the same "redistributes but doesn't reduce" problem, and would also touch the always-on live-TX DSP path, a bigger scope change than agreed.

Proposed a different, already-built candidate instead: the freq_dev slew-rate limiter (`ssb_dsp_set_freq_dev_slew_limit_hz()`, `'{'`/`'}'`, off by default), which caps the raw SIZE of each sample-to-sample jump rather than trying to decorrelate timing - its own doc comment already suggests it was built with exactly this ~8000Hz-scale glitch in mind. Not yet tested against this specific problem. Documentation-only update so far (`test_signals.h`'s `'Q'` doc comment now carries this full story); no firmware behavior changed. Next step: an `'F'` capture with the slew limiter enabled, and clarifying with the user exactly what "noisier" meant in their bench test. Full reasoning in `null_bias_investigation.md`.

## 2026-09-17, later still: decided to bench-test the existing freq_dev slew limiter next (not trig randomization); "noisier" clarified as audible on the demodulated signal

User confirmed: test the freq_dev slew-rate limiter (`'{'`/`'}'`) next rather than pursuing fast_atan2/fast_sqrt randomization, and clarified the earlier `'Q'` result was noisier specifically as HEARD on the demodulated/received audio (not a waterfall observation, not the null_bias diagnostic numbers). No firmware change needed - the limiter and its runtime toggle already exist and are already wired into the live DSP path. Wrote out a concrete bench procedure in `null_bias_investigation.md`: `'Q'` off for a clean comparison, one `'{'` press (0 -> 2000Hz limit, inside the range its own doc comment says should catch an ~8000Hz-scale null event), run the same 700/1700 or 733/1700 two-tone Preset-1 test, listen for whether the periodic buzz shrinks without new broadband noise appearing, and ideally take an `'F'` capture with the limiter on to directly confirm spike magnitude is actually being clamped. Documentation/planning only - no code changed this turn. Full procedure in `null_bias_investigation.md`.

## 2026-09-17, later still: simulation explains the bench result on the slew limiter - it really does shift the center frequency (a genuine, steady-state side effect), and doesn't remove the recurring warble pattern either

User reported from the bench: the slew limiter measurably shifts the perceived/measured center frequency depending on its setting, but the warble sounds about the same regardless of setting. Built a simulation (`sim_slew_limiter_test.py`, scratchpad, exact port of `ssb_dsp.c`'s real slew-limiter algorithm in its real pipeline position) to check both claims quantitatively on 700/1700 two-tone. Confirmed both, and explained the mechanism for each: (1) the frequency shift is real and steady-state (not a toggle glitch) - up to +16.75Hz at the loosest engaged setting (2000Hz), shrinking toward the raw baseline as the limit tightens (+0.98Hz at the 100Hz floor) - because forcing a near-null spike's excess deviation to ramp gradually spreads it into neighboring samples where envelope has already recovered, which DO count in the physically-correct envelope-weighted average frequency (unlike the single suppressed near-zero-envelope spike sample itself); (2) the warble stays similarly present because slew limiting doesn't change WHEN or how OFTEN the ~1000/second near-null events happen, only how big each one is allowed to get - if the audible character comes from the event RATE rather than peak size, no slew setting removes it. Peak magnitude does drop a lot with tighter settings (7912Hz raw down to 1403Hz at the tightest step) but that alone doesn't appear to fix the audible problem. Net: like `'Q'` dither, this is a real, testable, but ultimately unconvincing fix - both attack a proxy, not the null-crossing event itself. Makes the two already-drafted (never implemented) fix directions in `null_bias_investigation.md`'s open items - directly reworking the near-null phase/frequency computation - look like the more promising next step. Documentation/analysis only, no firmware changed. Full numbers in `null_bias_investigation.md`.

## 2026-09-17, later still: CORRECTION - the slew limiter's real problem is intermodulation distortion of the two tones themselves, not a small center-frequency side effect; both tried mitigations (dither, slew) now ruled out on real hardware

User's actual bench readings (~500Hz shift at a very tight clamp, ~250Hz at a 4000Hz clamp) didn't match the previous entry's simulated ~17Hz weighted-average estimate, in size OR direction - a sign the earlier metric was measuring the wrong thing. Rebuilt the analysis properly: reconstructed the actual transmitted complex baseband signal from freq_dev/envelope and took its FFT to see exactly where the two tones land spectrally (sanity-checked against the known-clean raw case, which reproduces exact 700.0/1700.0 Hz peaks). Result: the slew limiter doesn't shift the two tones as a pair - at any setting tight enough to meaningfully cut the ~8000Hz null spikes, it smears them into a broad cluster of intermodulation products with no clean tone to even measure a "shift" against, because a 700/1700 two-tone's own 1000Hz beat frequency already needs real slew rates comparable to the very limits meant to only catch null glitches. Only very loose settings (6000-8000Hz) stay clean, but those barely touch the actual spikes.

**Net status after three real-hardware-tested angles**: both `'Q'` dither and the freq_dev slew limiter have now been tried and both made things measurably worse, not better - neither can tell a null-crossing glitch apart from legitimate signal content, so both process every sample identically. This points hard at the two never-implemented, envelope-gated fix directions already sitting in `null_bias_investigation.md`'s open items as the next real thing to try, since they're the only proposals that touch only the samples actually flagged as near-null. Documentation/analysis only, no firmware changed. Full spectral numbers in `null_bias_investigation.md`.

## 2026-09-17, later still: user fixed a separate two-tone freq-gen bug on the bench; post-fix the ~1s periodicity reads as a "wobble"/raised noise floor rather than a narrow spur; pushed back on the proposed phase/freq dither fix and flagged that the "noise" may actually be the already-proven exact comb, just unmasked

User fixed an unrelated small bug in two-tone frequency generation on the bench (details not yet seen here). Post-fix, DSP math still confirmed correct, and the proven ~1.000s periodicity now shows up as a "wobble" with a much higher wideband SDR noise floor, instead of the previously-seen narrow jumping spur - user notes this resembles the `'Q'` dither result and proposes adding a small dither directly to phase/freq_dev near the fast-trig computation to break the periodicity.

Pushed back with two points: (1) this is mechanistically the same trade as `'Q'`, which already made real measured results worse, not better - dither redistributes glitch energy rather than reducing it, regardless of how small the perturbation is (the "small = safe" reasoning addresses a different risk than the one already observed); (2) the reported "noise floor" may not be genuine noise at all - since the raw signal is proven bit-for-bit periodic at exactly 1.000s, its true spectrum is a comb of discrete 1Hz-spaced lines, which most SDRs' resolution bandwidth can't resolve and would display as a raised noise floor even though it's fully deterministic. Proposed a cheap test (narrow the SDR's RBW / take a long high-res FFT) to check before any code change, and suggested the fixed freq-gen bug may have been inadvertently dithering things before today, which would explain the spur-character change. Recommended moving to the two already-drafted, envelope-gated near-null fix directions instead of another dither variant. Documentation/discussion only, no firmware changed. Still waiting on the freq-gen fix's details and the RBW test result. Full reasoning in `null_bias_investigation.md`.

## 2026-09-17, later still: the "unresolved 1Hz comb" hypothesis is FALSIFIED at 0.05Hz real RF resolution - genuinely broadband, not a comb - which reopens the never-tested "downstream of the core DSP" candidates from earlier today

Two clarifications resolve open questions from the last entry. First, "today's freq-gen fix" is the already-known NCO fix (accumulate-based phase generator -> exact wrapping-index recompute-every-tick), not a separate bug - the user pasted its doc comment directly. This confirms the old generator's float32 accumulation drift was itself acting as an unintentional, tiny dither source. Second, and more importantly: the user CAN resolve the real RF spectrum to 0.05Hz (20x finer than a 1.000s-periodic comb's 1Hz line spacing) and it does NOT show a comb - genuinely broadband content dominates. Also confirmed the OLD, less-exact tone generator gave a measurably PURER tone (lower noise floor) despite jumping around, than the mathematically-exact NEW one does - the opposite of what "the core DSP's periodicity is the audible cause" would predict.

Since the digital freq_dev/envelope core is now proven exactly periodic three separate ways, an exactly-periodic input through any fixed deterministic chain must produce an exactly-periodic (comb) output - no exception, by basic Fourier theory. Real broadband content instead means the actual transmitted signal is not exactly periodic, and the gap can only be introduced somewhere downstream of `ssb_dsp_process_sample()` - reopening the "candidate 2" list flagged (and never tested) much earlier today: `relative_delay_apply()`, `envelope_interp_on_full_tick()`, the envelope PWM output mapping, or the AD9851 chip's own analog/SPI/PLL behavior. Recommendation now supersedes the previous entry's "design the near-null DSP fix next" - that DSP output is unusually well-verified at this point; the downstream chain is not, and is now the more promising target. Requested the user's actual capture/recording to verify directly with the same FFT tooling used earlier today, rather than relying on a description. No firmware changed. Full reasoning in `null_bias_investigation.md`.

## 2026-09-17, later still: analyzed the real SDRuno IQ captures directly - it IS real comb structure, not smooth noise, but ~25-30dB stronger than the verified core DSP alone predicts, pointing the dominant cause downstream

Analyzed both uploaded SDRuno IQ captures (Preset 1 slew-off, Preset 3) directly as complex I/Q baseband signals. Confirmed the genuine two-tone signal in both (exact 1000Hz tone spacing), measured Preset 1's skirt (-3dB width ~5Hz growing to -30dB width ~204-214Hz - a real, large skirt matching "~150Hz-ish" by eye), and found that at native 0.054Hz resolution it resolves into a genuine discrete comb, not smooth noise - refining, not reversing, the earlier falsification of the "unresolved 1Hz comb" idea (there IS comb structure, just coarser than 1Hz spacing).

Decisive step: simulated the already-proven-exact core DSP signal alone (no downstream stages) over one exact 1.000s period and compared its own spectrum at the same kind of resolution. It shows a real but much lower (-53 to -60dB) broadband-looking floor from the near-null glitches - a genuine, now-directly-demonstrated contribution - but does NOT reproduce the sharp ~100-200Hz-spaced comb teeth real hardware shows reaching -25 to -30dB, a ~25-30dB gap. This means the core DSP contributes a real but minor floor; the DOMINANT structure seen on the bench must come from downstream (`relative_delay_apply()`, `envelope_interp_on_full_tick()`, PWM output mapping, or AD9851 SPI/PLL behavior) - the same candidate list from earlier today, now on a quantitative footing rather than a Fourier-theory argument alone. Preset 3's capture showed a visually smoother floor with no obvious discrete comb teeth, unlike Preset 1 - possibly its extra processing (EQ/compressor/AGC) decorrelating whatever produces Preset 1's sharper comb. No firmware changed - analysis only. Full numbers and plots in `null_bias_investigation.md`.

## 2026-09-17, later still: settings.h shows Preset 1 zeroes/disables relative_delay, gdeq, ampeq, and envelope_interp - removing them as comb candidates; user's AM/D-correction evidence narrows further; two new AD9851-path candidates raised (FQ_UD latch margin, known Fs/wakeup jitter)

Re-checked an assumption before handing it to the user as the leading candidate: `relative_delay_apply()` is NOT "always active" under Preset 1 - `settingsPresets[1]` stores `relative_delay_samples=0.00` (an identity pass-through at exactly zero), plus `env_gdeq_enable`, `envelope_interp_enable`, `env_predistort_enable`, `env_ampeq_enable`, and `env_ampeq_shelf2_enable` all false/zero-filled. Every envelope/phase-conditioning stage on the last two entries' downstream list is off or a no-op under Preset 1 - correcting, not extending, the previous entry.

User's own evidence points the same way: the comb-adjacent noise isn't present on AM (rules out generic mains/PSU coupling), and disabling predistort with zero PWM scale/mid offset (exercising the one live envelope-path lever hard) left the tone pair and its noise "very similar to before." Envelope/correction chain and analog PWM path both substantially ruled out; what's left live under Preset 1 is the bare PWM offset/scale mapping and the AD9851 SPI/DDS path.

Checked project history before re-offering the AD9851 BS170/level-shifter theory as fresh: already tested today via the push/pull driver fit, "no visible improvement" for closely related symptoms. Confirmed today's Preset 1 capture was taken WITH those drivers fitted, so that negative result applies directly here - not re-proposing that specific mechanism without new justification.

User raised two more specific, distinct candidates: (1) **FQ_UD's own latch-edge margin** - the AD9851 only actually latches new data on FQ_UD's edge, and that edge currently has zero settling margin (`AD9851_BITBANG_EDGE_DELAY_ENABLED=0` disables it along with the per-bit delays). History check: FQ_UD's own delay was once kept in place as a separately-justified "cheap" margin when the per-bit delay was trimmed back (2026-09-10), but got swept into the blanket disable the next day and never tested in isolation, and never at all since the push/pull driver fit. (2) **This project's already-documented Fs/wakeup jitter** (`max_gap_us` ~71-78us against the 62.5us tick budget, cross-core scheduling contention, per 2026-09-10/11 entries) - worked through why this plausibly matters here: the AD9851 free-runs its DDS core between updates, so jitter in WRITE timing distorts things in proportion to how fast `freq_dev` is changing at that moment (same delta(t)*rate-of-change mechanism already used on the AM side of this project) - and `freq_dev` changes fastest right at nulls, exactly where the comb is seen. The two candidates could stack (signal-integrity margin vs. firmware write timing).

No firmware changed. Next tests identified, not yet run: re-enable only the FQ_UD-latch delay and re-test hands-off; scope FQ_UD vs. the last W_CLK edge now that push/pull drivers are fitted; correlate `max_gap_us` timing against a comb-showing capture to see if comb timing tracks wakeup jitter rather than the two-tone signal itself. Documentation/discussion only. Full reasoning in `null_bias_investigation.md`.

## 2026-09-17, later still: added a firmware toggle ('O') to revert to the pre-NCO-fix "pure but jumping" two-tone phase generator on demand, for a sanity-check A/B without a reflash

User asked whether the old accumulate-based two-tone phase generator was left in place after today's NCO fix (`a32c50c`), wanting to revert to it as a sanity check and confirm nothing else relevant changed since. Checked: it was NOT kept as a fallback - fully replaced for tone1 (always) and tone2 (dither off); the only surviving trace is tone2's own dither branch, a separate code path. Checked every commit since the fix: `ssb_dsp.c`, `AD9851.c`, `relative_delay`, the envelope/gdeq/ampeq/interp files, and `settings.h` are all untouched - everything else that changed is docs or diagnostics/capture instrumentation, none of it touching the signal/RF path. One caveat: `TWOTONE_BAND_PRESETS` gained one extra temporary test entry since then (doesn't change any existing pair, just adds one more `'T'` cycle stop).

Added the requested toggle: **`'O'`** (mirrors `'Q'`'s pattern) in `test_signals.cpp`/`.h` and `serial_commands.cpp`, plus a boot-banner line. Off by default (today's exact-recompute generator is unaffected); when ON, tone1 always reverts to the old accumulate-and-subtract phase update, and tone2 does too unless `'Q'` dither is on (dither's own tone2 path already used the accumulator regardless). `s_tone_sample_index` keeps advancing either way so toggling back off resumes cleanly. A small one-time phase discontinuity on the switching tick is expected/accepted, same as `'Q'`'s and `'T'`'s own transitions. Committed; not yet bench-tested. Full reasoning in `null_bias_investigation.md`.

## 2026-09-17, later still: 'O' does not reset the tone generator; new Preset 3/ADC-off capture shows a dramatically cleaner floor - strong support for the Fs/wakeup-jitter candidate

Confirmed from the code: `test_signals_set_twotone_legacy_phase_enabled()` is a plain flag flip - it does NOT reset phase/sample-index state on either transition, by design (so the exact-recompute path resumes cleanly off the still-advancing index when 'O' goes back off). Same one-time-discontinuity tradeoff already documented for 'O'/'Q'/'T'.

Analyzed a new capture, "Preset 3 with ADC off" (`SDRuno_20260917_204619Z_14175kHz.wav`), against the two 19:2x captures from earlier today:

| capture | -30dB width | far floor | near-tone floor | near-far delta |
|---|---|---|---|---|
| Preset 1, slew off | 3.2 Hz | -89.6 dB | -62.1 dB | +27.5 dB |
| Preset 3 | 10.5 Hz | -84.5 dB | -49.9 dB | +34.7 dB |
| **Preset 3, ADC off** | **0.7 Hz** | **-100.6 dB** | **-88.1 dB** | **+12.4 dB** |

A large, unambiguous improvement with the ADC off: near-tone floor down 38dB vs. Preset 3/ADC-on (~6300x power), far floor down 16dB, skirt width down from 10.5Hz to 0.7Hz - close to the core-DSP-only prediction from several entries ago. This lines up directly with the Fs/wakeup-jitter candidate raised last entry: this project's own diagnostics already attribute part of `max_gap_us` scheduling jitter to ADC-ISR-priority contention, so removing the ADC removes that contention source - exactly the kind of broad floor/skirt improvement a write-timing-jitter mechanism would predict, not a narrow spectral change. First piece of evidence in this investigation that moves a candidate from "plausible mechanism" to "matches a real, controlled A/B."

Not yet established: exactly how "ADC off" was done on the bench (current mainline firmware's own comments say the ADC "always runs, no longer conditional," so this wasn't a stock toggle) - matters for interpreting the result precisely. Recommended next step: pull `[timing] max_gap_us`/`overruns` for both ADC-on and ADC-off conditions on the same preset to confirm jitter actually dropped, closing the loop causally. Documentation/analysis only, no firmware changed. Full numbers in `null_bias_investigation.md`.

## 2026-09-17, later still: CORRECTION - ADC was already disabled in ALL recent captures, including the two "noisy" ones; the previous entry's ADC-off/wakeup-jitter causal story is retracted

User clarified: every recent capture with the new exact-recompute tone generator - both the "noisy" 19:2x captures AND the 20:46 "low noise benchmark" - was built with `ADC_CAPTURE_ENABLED=0` (config.h:142, a compile-time flag gating whether the ADC's continuous DMA driver starts at all - confirmed real by a 2026-09-02 hardware finding that this flag kills a measurable 5kHz DMA/ISR-driven noise pulse elsewhere in the system). ADC state was constant across all three captures, not the variable distinguishing clean from noisy - so **the previous entry's central claim (ADC-off explains the ~26-38dB improvement, confirming the wakeup-jitter candidate) is retracted.** That candidate returns to "plausible, untested" status, same as FQ_UD-latch-margin.

This reopens the real question: what DOES differ between the 19:31 Preset 3 capture (-49.9dB near-tone floor) and the 20:46 one (-88.1dB), a ~38dB gap between two nominally-identical preset selections? Since preset reload should set every DSP-side lever identically, something else must have changed between the two sessions - a runtime toggle a preset reload wouldn't undo (relative_delay, gain, a gdeq/ampeq/predistort state left on), or something receiver-side (SDR gain/attenuation, AGC, antenna/dummy-load connection). Asked the user which. A genuine ADC-on-vs-off A/B remains a worthwhile separate test given the flag's own confirmed real effect elsewhere - it just isn't what today's captures already show. Documentation only, no firmware changed. Full reasoning in `null_bias_investigation.md`.

## 2026-09-17, later still: RESOLVED - the 19:31/20:46 Preset 3 captures differed ONLY in tone generator; old generator's own float rounding is a much smaller, more surgical decorrelator than 'Q' dither

User confirmed the two captures were identical except for which tone generator was active - old (pre-NCO-fix) vs. new (exact-recompute) - and that the new `'O'` toggle reproduces this exact ~38dB A/B live, no other change needed. A genuinely clean, single-variable result: the OLD, less-exact generator measurably cleans up the near-tone floor and comb/skirt, confirming the earlier qualitative "old generator sounded purer" observation with hard numbers.

Working theory (not yet confirmed by simulation): the old accumulator's already-quantified drift (~+/-2e-4Hz/500s) is far too slow to explain an 18s-scale difference by itself - but its `phase += increment; phase -= two_pi` update carries forward history-dependent float32 rounding noise every tick, unlike the exact generator's fresh-every-tick recompute (no memory at all). Near-nulls recur every ~16 samples and `fast_atan2` is extremely sensitive there, so even a rounding-noise perturbation far smaller than `'Q'`'s deliberate +/-0.05Hz wobble could be enough to decorrelate cycle-to-cycle null glitches - while the exact generator reproduces the identical glitch at the identical null every single cycle, which is exactly what makes it a coherent comb (the "exact periodicity -> exact comb" argument from several entries ago, now with a concrete "why it's audible" mechanism attached). This reconciles rather than contradicts the earlier negative `'Q'` result: both decorrelate periodicity, but `'Q'` is a large, deliberately audible-scale wobble with its own footprint, while the old generator's rounding noise is apparently a much smaller, more surgical perturbation.

If this holds, the right fix is neither reverting to the buggy accumulator (reintroduces a real drift bug) nor `'Q'` at its current magnitude - it's finding the smallest perturbation that reproduces the old generator's benefit without either liability. Next steps, none yet run: more `'O'` A/B runs across bands to build confidence; simulate the old accumulator's actual per-tick rounding-noise magnitude (distinct from its slow drift); test `'Q'` at a much smaller magnitude as a direct prediction of this theory. Documentation only, no firmware changed. Full reasoning in `null_bias_investigation.md`.

## 2026-09-17, later still: simulation FALSIFIES the "old generator's rounding noise decorrelates glitches" theory; redirects to Preset 3's active gdeq/ampeq/relative_delay/predistort stack instead

Built and ran the direct test: exact old-accumulator generator vs. the verified exact-recompute generator, both through the full verified core-DSP simulation over 20 cycles. Two findings kill the previous entry's theory as stated: (1) the OLD generator's cycle-to-cycle `freq_dev` difference is a smooth, monotonically DECAYING sequence, not noise - the signature of the already-known slow systematic drift, not new per-cycle rounding noise; (2) the simulated near-tone floor for OLD vs. EXACT generator is -184dB vs. -226dB - OLD is 42dB WORSE in simulation (wrong sign vs. the real 38dB-better hardware result), and both are ~100dB below anything physically meaningful, meaning the core DSP itself is functionally identical between the two generators.

Redirected theory: the real effect must be downstream, and Preset 3 (unlike Preset 1) has its entire downstream stack live - gdeq (candidate B), both ampeq shelves, predistort, and a real 2.00-sample (exact-integer) relative_delay. Plausible mechanism: these are LTI filters whose resonant response builds up more energy the longer an exciting frequency dwells exactly on it - the EXACT generator's glitches recur at bit-identical instants forever (a rigid comb that can sit on a resonance indefinitely), while the OLD generator's glitches slowly slip cycle to cycle (never dwelling exactly on one frequency), potentially starving any downstream resonance of sustained excitation. Speculative, not yet simulated with the real downstream stages.

User is independently running the decisive discriminating test: `'O'` A/B on Preset 1 (nothing downstream active) instead of Preset 3. Little/no difference there would confirm the downstream stack as the mechanism; a comparably large difference would contradict this and point back at the core DSP/AD9851 path. Awaiting result. Documentation/simulation only, no firmware changed. Full reasoning and numbers in `null_bias_investigation.md`.

## 2026-09-17, later still: Preset 1 'O' test comes back "basically the same" - FALSIFIES the downstream-stack theory, redirects to whatever's active on every preset

User's decisive test: `'O'` A/B on Preset 1 shows the same old-vs-new gap as Preset 3, just with a different settled "off" frequency (-10Hz vs -18Hz) - expected, since `relative_delay`'s value is already known to tune that offset (Preset 1=0.00 vs. Preset 3=2.00). Since Preset 1 has zero live downstream conditioning - confirmed by re-reading `relative_delay.cpp`: at `delay=0.00`, `interp_ring()` returns an exact bit-identical pass-through, no blending at all - the previous entry's "LTI resonance in gdeq/ampeq" theory is falsified. It predicted little/no gap on Preset 1; that's not what happened.

Redirects to mechanisms active on EVERY preset: the AD9851 SPI/FQ_UD write path (FQ_UD-latch-margin candidate, not yet tested), Fs/wakeup jitter (also not yet actually tested - ADC has been off in every capture so far), or the basic PWM/analog envelope path (partially discounted earlier, but not against this exact comparison). Recommending splitting FQ_UD's own edge-delay out of `AD9851_BITBANG_EDGE_DELAY_ENABLED` into its own flag, so it can be A/B'd via `'O'` without paying the per-bit delays' separate real-time cost - proposed, not yet implemented. Documentation only, no firmware changed. Full reasoning in `null_bias_investigation.md`.

## 2026-09-17, later still: DECISIVE - the two generators' raw freq_dev output statistics genuinely differ in exactly the way that stresses the AD9851 write path

Redid the comparison the right way, per direct pushback: compared the actual tick-to-tick `|freq_dev[n]-freq_dev[n-1]|` step (the same quantity as this project's own `max_freq_dev_step` diagnostic) over 60 cycles for both generators, instead of an idealized FFT reconstruction. Result is decisive: count of steps >8500Hz is 332/cycle for the EXACT generator vs. only 2.7/cycle for OLD (~124x); >9000Hz is 203/cycle vs. 0.83/cycle (~244x). The EXACT generator's per-cycle worst-case step is EXACTLY 10226.39Hz on every single cycle (std=0.0), always at the identical sample index (2968) - perfectly reproducible by construction. OLD's worst case averages only 8049Hz, varies cycle to cycle (std=289Hz), and its location wanders across a 768-sample span.

This reopens the AD9851/level-shifter family of theories on solid new footing: this project's own history already documents that jumps in the ~8562Hz-class range can flip enough DATA bits at once to outrun a level shifter's settling time. The EXACT generator routinely and reliably exceeds that zone every single cycle at fixed positions; OLD mostly doesn't. If AD9851 signal integrity degrades above that threshold, EXACT would trigger it with total reproducibility (a strong discrete comb) while OLD would trigger it rarely and unpredictably (lower, less coherent noise) - matching the real hardware result. Sample index 2968 is now a known, precisely reproducible trigger point for a targeted scope/`'J'` capture. Strengthens the case for the already-proposed FQ_UD-latch-margin flag split. Documentation/simulation only, no firmware changed. Full numbers in `null_bias_investigation.md`.

## 2026-09-17, later still: split FQ_UD's own latch-edge delay into an independent flag (`AD9851_FQUD_EDGE_DELAY_ENABLED`, starts at 1) for isolated bench testing

Implemented the split proposed last entry: `AD9851.c` gets a new, independent `AD9851_FQUD_EDGE_DELAY_ENABLED` flag guarding just the FQ_UD-latch `ad9851_edge_delay()` call (end of `ad9851_set_frequency()`'s bit-bang tail), separate from `AD9851_BITBANG_EDGE_DELAY_ENABLED`'s two per-bit DATA-settle delays and their own real, measured real-time-budget cost. Either flag alone now enables the FQ_UD site; the original flag still also covers the per-bit sites unchanged. `half_period_cycles` (the delay's duration) is already computed unconditionally whenever bit-bang mode is active, so no other change was needed - fully isolated, one-file edit.

Set to `1` (not the usual off-by-default) since the user wants to test it now rather than needing a second edit/reflash. Motivated directly by today's `'O'` A/B finding: the EXACT tone generator reliably drives freq_dev jumps past ~8500-9000Hz hundreds of times per cycle at fixed positions (squarely in this project's own documented AD9851-corruption range), while the OLD generator mostly doesn't. If enabling just this one delay narrows the `'O'` A/B gap, that confirms FQ_UD margin as a real contributor; if not, this candidate is cleared. Not yet bench-tested. Full reasoning in `null_bias_investigation.md`.

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
