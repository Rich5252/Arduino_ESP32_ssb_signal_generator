/**
 * serial_commands.cpp - see serial_commands.h.
 */

#include "serial_commands.h"
#include "config.h"
#include "dsp_state.h"
#include "adc_capture.h"
#include "envelope_gdeq.h"
#include "envelope_ampeq.h"
#include "envelope_predistort.h"
#include "envelope_floor.h"
#include "envelope_output.h"
#include "envelope_interp.h"
#include "diagnostics.h"
#include "settings.h"
#include "ssb_dsp.h"
#include "test_signals.h"
#if AD9851_ATTACHED
#include "relative_delay.h"
#include "carrier_output.h"
#endif
#include <Arduino.h>
#include <cstdarg>
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"   // vTaskList() - see the 'L' handler below

// Every command reply in this file is sent through serial_reply() below,
// rather than calling Serial.print()/println()/printf() directly - this
// is what fixes a real, reproducible bug found on real hardware: the 'I'
// command would sometimes not reply when switching interpolation ON, and
// then send TWO replies together on the next 'I' (switching off).
//
// Root cause: "-> 4x envelope output interpolation ON (see
// envelope_interp.h)\r\n" is exactly 64 bytes long - and 64 bytes is the
// USB full-speed CDC bulk endpoint's max packet size on the ESP32-S3's
// native USB. USB CDC marks the end of a transfer with either a "short"
// packet (fewer bytes than the endpoint's max packet size) or an
// explicit trailing zero-length packet (ZLP). When a transfer's total
// length is an EXACT multiple of 64, neither happens on its own - the
// host's CDC-ACM driver can't tell whether that 64-byte chunk is the
// whole message or just the first packet of a longer one, so it holds
// the data back rather than handing it to the terminal application
// (Arduino IDE Monitor, PuTTY, or a custom logger - all three showed the
// identical symptom, which is what pointed away from anything
// app-specific and toward this transport-layer explanation). Only once
// MORE bytes are queued behind it - i.e. the NEXT reply - does the total
// stop being a clean 64-byte multiple, and both replies get delivered
// together. This is a widely-reported, generic USB CDC-ACM behavior, not
// anything specific to this board (see e.g. hathach/tinyusb#2041,
// STMicroelectronics/STM32CubeF3#2). Serial.flush() does NOT fix it: by
// the time flush() runs, the exact-64-byte packet has already gone out
// the door - flush() just waits for the TX queue to drain, it can't
// retroactively add a terminator to a transfer the device already
// considers complete.
//
// The 'I' "off" reply happened to be 65 bytes (not a multiple of 64), so
// it always flushed cleanly on its own - which is why the bug looked
// like it only affected switching ON. The actual trigger was just that
// one reply's coincidental byte count, not anything about ON vs off.
//
// Rather than pad that one string, every reply in this file is routed
// through here: it formats like printf, then pads the transmitted length
// by one harmless trailing space whenever it would otherwise land
// exactly on a 64-byte boundary. That means no future wording change,
// digit-width change (e.g. a different ENVELOPE_INTERP_FACTOR), or any
// other command's variable-length numeric field can silently
// reintroduce this same bug somewhere else in this file.
static void serial_reply(const char *fmt, ...)
{
    // 2026-09-02: bumped 256 -> 512 - the 'w' reply (serial_commands.cpp's
    // AUDIO_SRC_CHIRP handler) grew to ~500 bytes once it started reporting
    // 'g' state too, real hardware confirmed this silently TRUNCATED the
    // vsnprintf() output below (missing trailing \r\n - the exact symptom:
    // "I see first response but no \r\n"). The follow-up "second w gives no
    // response" is unrelated and NOT a bug: every source-switch command in
    // this file (t/s/m/p/y/h/w) guards on `src != TARGET`, so re-sending
    // 'w' while already in AUDIO_SRC_CHIRP is a deliberate, silent no-op -
    // same behavior 't' or 'm' would show if sent twice in a row. 512 gives
    // real headroom over every current reply in this file; the truncation
    // guard below still fires and the debug print makes it visible if any
    // future reply ever needs more.
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (len < 0) {
        return;   // formatting error - nothing sane to send
    }
    if ((size_t)len >= sizeof(buf)) {
        // Was a SILENT truncation until 2026-09-02 (see this function's own
        // comment above for how that manifested) - now at least visible.
        Serial.printf("[serial_reply] WARNING: reply truncated, needed %d bytes, buf is %u\r\n",
                      len, (unsigned)sizeof(buf));
        len = (int)sizeof(buf) - 1;   // truncated - still send what fit
    }

    if (len > 0 && (len % 64) == 0 && (size_t)len < sizeof(buf) - 1) {
        // Insert the padding space just before the trailing "\r\n" every
        // reply in this file ends with (rather than appending after it),
        // so it never leaks onto the front of whatever gets printed next.
        int insert_at = (len >= 2 && buf[len - 2] == '\r' && buf[len - 1] == '\n')
                         ? len - 2 : len;
        memmove(&buf[insert_at + 1], &buf[insert_at], (size_t)(len - insert_at) + 1);
        buf[insert_at] = ' ';
        len++;
    }

    Serial.write((const uint8_t *)buf, (size_t)len);
}

// Returns the C symbol name (e.g. "AUDIO_SRC_MIC") for a given
// audio_source_t, for the 'P' settings-dump command below - distinct from
// dsp_state.h's audio_source_name(), which returns a human-readable label
// ("mic", "TWO-TONE TEST", ...) that isn't valid C and can't be pasted
// into a PersistentSettings initializer.
static const char *audio_source_enum_name(audio_source_t src)
{
    switch (src) {
        case AUDIO_SRC_MIC:        return "AUDIO_SRC_MIC";
        case AUDIO_SRC_TWOTONE:    return "AUDIO_SRC_TWOTONE";
        case AUDIO_SRC_SINGLETONE: return "AUDIO_SRC_SINGLETONE";
        case AUDIO_SRC_ENVSTEP:    return "AUDIO_SRC_ENVSTEP";
        case AUDIO_SRC_FMTEST:     return "AUDIO_SRC_FMTEST";
        case AUDIO_SRC_AMTEST:     return "AUDIO_SRC_AMTEST";
        case AUDIO_SRC_CHIRP:      return "AUDIO_SRC_CHIRP";
        default:                   return "AUDIO_SRC_MIC";
    }
}

// Working duty value for 'd'/'>'/'<'/'N'/'B' (direct duty override, see
// envelope_output.h) - lives here, not in envelope_output.cpp, since it's
// only ever touched from this file's own single-threaded command
// handling (loop()/Core 1); envelope_output.cpp only needs to know the
// override's on/off state, not the stepping value itself.
static uint32_t s_duty_override_value = 0;

void handle_serial_commands(void)
{
    // Runtime source switch: 't' -> two-tone, 's' -> single-tone, 'm' ->
    // live mic, 'p'/'y'/'h' -> isolation tests. dsp_task reads the audio
    // source fresh every tick, so this takes effect on the very next
    // sample - no glitch/restart needed. Unrecognized bytes (e.g. line
    // endings from some serial monitors) are silently ignored rather than
    // echoed as an error.
    while (Serial.available()) {
        char c = Serial.read();
        audio_source_t src = dsp_state_get_audio_source();

        if (c == 't' && src != AUDIO_SRC_TWOTONE) {
            dsp_state_set_audio_source(AUDIO_SRC_TWOTONE);
            serial_reply("-> two-tone test signal\r\n");
        } else if (c == 's' && src != AUDIO_SRC_SINGLETONE) {
            dsp_state_set_audio_source(AUDIO_SRC_SINGLETONE);
            serial_reply("-> single-tone test signal (%.0fHz)\r\n", SINGLETONE_HZ);
        } else if (c == 'm' && src != AUDIO_SRC_MIC) {
            dsp_state_set_audio_source(AUDIO_SRC_MIC);
            serial_reply("-> live mic input\r\n");
        } else if (c == 'p' && src != AUDIO_SRC_ENVSTEP) {
            dsp_state_set_audio_source(AUDIO_SRC_ENVSTEP);
            serial_reply("-> envelope step test (%.1fHz square wave, carrier fixed - "
                          "measure the RSET node's rise/settling time against this edge)\r\n", ENVSTEP_HZ);
        } else if (c == 'y' && src != AUDIO_SRC_FMTEST) {
            dsp_state_set_audio_source(AUDIO_SRC_FMTEST);
            serial_reply("-> FM isolation test (%.0fHz sine mod, %.0fHz peak deviation, beta=%.2f - "
                          "expect FM sidebands at fc+/-n*%.0fHz, no envelope content - "
                          "isolates AD9851/SPI chain from Hilbert/DSP math)\r\n",
                          FM_TEST_MOD_HZ, FM_TEST_DEV_HZ, FM_TEST_DEV_HZ/FM_TEST_MOD_HZ, FM_TEST_MOD_HZ);
        } else if (c == 'h' && src != AUDIO_SRC_AMTEST) {
            dsp_state_set_audio_source(AUDIO_SRC_AMTEST);
            serial_reply("-> AM isolation test (%.0fHz sine mod, %.0f%% depth, carrier fixed - "
                          "expect ONLY fc+/-%.0fHz sideband pair, no FM content - "
                          "isolates RSET/PWM/filter path from AD9851/DSP)\r\n",
                          AM_TEST_MOD_HZ, AM_TEST_DEPTH * 200.0f, AM_TEST_MOD_HZ);
        } else if (c == 'w') {
            // Sine-chirp test mode - see test_signals.h/config.h's CHIRP_*
            // constants and the .ino's dsp_task early-intercept block.
            // Resets the sweep's phase/elapsed-time state on every entry
            // so 'w' always starts a clean sweep from t=0 (mute period
            // first), never resuming mid-sweep from a previous session.
            //
            // 2026-09-02: deliberately NOT gated on `src != AUDIO_SRC_CHIRP`
            // the way every other source-switch command above is (t/s/m/p/
            // y/h all no-op if already in their target mode) - real
            // hardware use turned up a genuine reason 'w' needs to differ:
            // the 'g' group-delay-equalizer A/B workflow (toggle 'g', then
            // re-run 'w' to compare compensated vs raw TF) NEEDS a fresh,
            // synced restart every time, even while already mid-sweep -
            // otherwise you're stuck waiting up to the full CHIRP_SWEEP_SEC
            // (5s) for the next natural mute/sync marker after toggling
            // 'g', rather than getting an immediate clean trigger point for
            // the TF rig. Re-sending 'w' while already sweeping is now a
            // real, useful "restart now" action, not a redundant re-select.
            // NOTE: CMD_DEBUG_PIN_ENABLED (config.h) must be 0 for the
            // chirp's square-wave reference on pin39 to be glitch-free -
            // it shares that physical pin with TIMING_DEBUG_GPIO_CMD.
            test_signals_chirp_reset();
            dsp_state_set_audio_source(AUDIO_SRC_CHIRP);
            serial_reply("-> sine chirp test (%.0fHz-%.0fHz, %.1fs sweep, %.0fms mute/sync, ref pin%d, "
                          "%dx fast-tick); DC mapping ('u'/'j'/'i'/'k'/'D') still applies, gdeq ('g') "
                          "currently %s, ampeq shelf 1 ('a') currently %s, ampeq shelf 2 ('A') currently "
                          "%s - toggle 'g'/'a'/'A' + re-run 'w' to A/B compensated vs raw TF (phase "
                          "channel for 'g', amplitude channel for 'a'/'A')\r\n",
                          CHIRP_F0_HZ, CHIRP_F1_HZ, CHIRP_SWEEP_SEC, CHIRP_MUTE_SEC * 1000.0f,
                          CHIRP_REF_GPIO, ENVELOPE_INTERP_FACTOR,
                          envelope_gdeq_get_enabled() ? "ON" : "OFF",
                          envelope_ampeq_get_enabled() ? "ON" : "OFF",
                          envelope_ampeq_shelf2_get_enabled() ? "ON" : "OFF");
        } else if (c == 'T') {
            // Steps the two-tone pair through TWOTONE_BAND_PRESETS
            // (test_signals.cpp) - lets you sweep the pair across the
            // audio band without a recompile, to empirically map how much
            // relative-delay retuning each band needs (see the
            // group-delay-equalizer refit discussion). Switches into
            // two-tone mode too if not already there, so 'T' alone is
            // enough to start a sweep.
            bool was_twotone = (src == AUDIO_SRC_TWOTONE);
            const char *band_name = test_signals_next_twotone_band();
            if (!was_twotone) {
                dsp_state_set_audio_source(AUDIO_SRC_TWOTONE);
            }
            serial_reply("-> two-tone band: %s (f1=%.0fHz f2=%.0fHz)%s - "
                          "re-tune relative delay ('['/']') for this band before capturing\r\n",
                          band_name, test_signals_get_twotone_f1_hz(), test_signals_get_twotone_f2_hz(),
                          was_twotone ? "" : ", two-tone mode enabled");
#if AD9851_ATTACHED
        } else if (c == ']') {
            relative_delay_increase();
            float d = relative_delay_get_samples();
            serial_reply("-> relative delay %+.2f samples (~%+.0fus) - %s\r\n",
                          d, d * 1000000.0f / SAMPLE_RATE_HZ,
                          d > 0.0f ? "phase held back" :
                          d < 0.0f ? "envelope held back" : "aligned");
        } else if (c == '[') {
            relative_delay_decrease();
            float d = relative_delay_get_samples();
            serial_reply("-> relative delay %+.2f samples (~%+.0fus) - %s\r\n",
                          d, d * 1000000.0f / SAMPLE_RATE_HZ,
                          d > 0.0f ? "phase held back" :
                          d < 0.0f ? "envelope held back" : "aligned");
#endif
        } else if (c == 'u') {
            envelope_output_raise_pwm_offset();
            float off = envelope_output_get_pwm_offset(), scale = envelope_output_get_pwm_scale();
            serial_reply("-> PWM duty range %.0f%%-%.0f%% (offset=%.2f, scale=%.2f)\r\n",
                          off * 100.0f, (off + scale > 1.0f ? 1.0f : off + scale) * 100.0f, off, scale);
        } else if (c == 'j') {
            envelope_output_lower_pwm_offset();
            float off = envelope_output_get_pwm_offset(), scale = envelope_output_get_pwm_scale();
            serial_reply("-> PWM duty range %.0f%%-%.0f%% (offset=%.2f, scale=%.2f)\r\n",
                          off * 100.0f, (off + scale > 1.0f ? 1.0f : off + scale) * 100.0f, off, scale);
        } else if (c == 'i') {
            envelope_output_widen_pwm_scale();
            float off = envelope_output_get_pwm_offset(), scale = envelope_output_get_pwm_scale();
            serial_reply("-> PWM duty range %.0f%%-%.0f%% (offset=%.2f, scale=%.2f)\r\n",
                          off * 100.0f, (off + scale > 1.0f ? 1.0f : off + scale) * 100.0f, off, scale);
        } else if (c == 'k') {
            envelope_output_narrow_pwm_scale();
            float off = envelope_output_get_pwm_offset(), scale = envelope_output_get_pwm_scale();
            serial_reply("-> PWM duty range %.0f%%-%.0f%% (offset=%.2f, scale=%.2f)\r\n",
                          off * 100.0f, (off + scale > 1.0f ? 1.0f : off + scale) * 100.0f, off, scale);
        } else if (c == 'g') {
            bool now_on = !envelope_gdeq_get_enabled();
            envelope_gdeq_set_enabled(now_on);   // internally resets state on an off->on transition
            serial_reply("-> envelope group-delay equalizer %s%s\r\n", now_on ? "ON" : "off",
                          now_on ? " - re-tune relative delay ('['/']') from scratch, "
                                   "theoretical starting point ~+2.65 samples (see envelope_gdeq.h)" : "");
        } else if (c == 'a') {
            // Mirrors the 'g' handler above exactly - see envelope_ampeq.h.
            // Shelf 1 (2500Hz/+6dB) ONLY - shelf 2 has its own independent
            // 'A' toggle below (split 2026-09-04 after real-hardware
            // testing showed shelf1-only vs shelf1+shelf2 needed to be
            // A/B-able without a reflash). VALIDATED on real hardware
            // 2026-09-03: magnitude tracked prediction closely; real IMD
            // benefit confirmed; does reintroduce measured group-delay
            // dispersion because gdeq hasn't been refit against it (see
            // envelope_ampeq.h). Off by default - currently this
            // project's recommended config (shelf 1 on, shelf 2 off).
            bool now_on = !envelope_ampeq_get_enabled();
            envelope_ampeq_set_enabled(now_on);   // internally resets shelf 1's state on an off->on transition
            serial_reply("-> envelope magnitude (insertion-loss) equalizer, shelf 1 (2500Hz/+6dB) %s%s\r\n", now_on ? "ON" : "off",
                          now_on ? " - validated on real hardware 2026-09-03 (magnitude tracks "
                                   "prediction, real IMD benefit, reintroduces some group-delay "
                                   "dispersion - see envelope_ampeq.h). Shelf 2 ('A') is separate." : "");
        } else if (c == 'A') {
            // Shelf 2 (6000Hz/+10dB), independent of 'a' above - see
            // envelope_ampeq.h's 2026-09-04 entries. VALIDATED on real
            // hardware 2026-09-04 (`ga_Trial2_TF.txt`): real magnitude
            // gain at 8000Hz (+6.7dB, short of the +10dB predicted), but
            // group-delay dispersion roughly DOUBLED again on top of
            // shelf 1's own increase, and real two-tone IMD came back
            // marginally WORSE with both stages on than shelf 1 alone -
            // in this still-unrefit-gdeq state. Off by default; current
            // recommendation is to leave this off until gdeq is refit
            // against the combined analog+shelf1+shelf2 phase response.
            bool now_on = !envelope_ampeq_shelf2_get_enabled();
            envelope_ampeq_shelf2_set_enabled(now_on);   // internally resets shelf 2's state on an off->on transition
            serial_reply("-> envelope magnitude (insertion-loss) equalizer, shelf 2 (6000Hz/+10dB) %s%s\r\n", now_on ? "ON" : "off",
                          now_on ? " - real hardware 2026-09-04: more magnitude recovery toward "
                                   "8000Hz, but dispersion roughly doubles again and two-tone IMD "
                                   "came back marginally WORSE combined with shelf 1 in this "
                                   "unrefit-gdeq state (see envelope_ampeq.h / group_delay_fit_notes.md) "
                                   "- re-run 'w' chirp/TFA to check the current combined response" : "");
        } else if (c == 'D') {
            bool now_on = !envelope_predistort_get_enabled();
            envelope_predistort_set_enabled(now_on);
            serial_reply("-> envelope pre-distortion %s%s\r\n", now_on ? "ON" : "off",
                          now_on ? " - REPLACES the 'u'/'j'/'i'/'k' linear offset/scale mapping "
                                   "while on (see envelope_predistort.h); those knobs have no "
                                   "effect until this is toggled off again" : "");
        } else if (c == 'x') {
            envelope_floor_raise();
            serial_reply("-> envelope-null floor raised to %.2f (see envelope_floor.h)\r\n",
                          envelope_floor_get());
        } else if (c == 'z') {
            envelope_floor_lower();
            serial_reply("-> envelope-null floor lowered to %.2f (see envelope_floor.h)\r\n",
                          envelope_floor_get());
        } else if (c == '}') {
            ssb_dsp_raise_freq_dev_slew_limit(dsp_state_get_ssb());
            float limit = ssb_dsp_get_freq_dev_slew_limit_hz(dsp_state_get_ssb());
            if (limit >= SSB_DSP_FREQ_DEV_SLEW_UNLIMITED_HZ) {
                serial_reply("-> freq_dev slew-rate limit: off (see ssb_dsp.h)\r\n");
            } else {
                serial_reply("-> freq_dev slew-rate limit loosened to %.0fHz/sample (see ssb_dsp.h)\r\n", limit);
            }
        } else if (c == '{') {
            ssb_dsp_lower_freq_dev_slew_limit(dsp_state_get_ssb());
            float limit = ssb_dsp_get_freq_dev_slew_limit_hz(dsp_state_get_ssb());
            serial_reply("-> freq_dev slew-rate limit tightened to %.0fHz/sample (see ssb_dsp.h)\r\n", limit);
        } else if (c == 'I') {
            bool now_on = !envelope_interp_get_enabled();
            envelope_interp_set_enabled(now_on);   // internally seeds prev/target on an off->on transition
            serial_reply("-> %dx envelope output interpolation %s (see envelope_interp.h)\r\n",
                          ENVELOPE_INTERP_FACTOR, now_on ? "ON" : "off");
        } else if (c == 'C') {
            // v4.3/v4.4: direct A/B between the three interp curves,
            // independent of 'I' itself - see envelope_interp.h's "v4.3"
            // and "v4.4" header notes. Only affects rendered output while
            // 'I' is ON; harmless (and remembered) to cycle with 'I' off.
            // Cycles CATMULL_ROM(0) -> LINEAR(1) -> HOLD(2) -> CATMULL_ROM,
            // same modulo-cycle convention as 'f' (adc_lpf_mode).
            envelope_interp_curve_t cur = envelope_interp_get_curve();
            envelope_interp_curve_t next = (envelope_interp_curve_t)((cur + 1) % 3);
            envelope_interp_set_curve(next);
            static const char *k_curve_desc[3] = {
                "Catmull-Rom (v4.2 cubic Hermite, default)",
                "LINEAR (v4 straight-line ramp)",
                "HOLD (v4.4 plain 64kHz ZOH, no ramp)"
            };
            serial_reply("-> envelope interp curve: %s%s (see envelope_interp.h v4.3/v4.4 notes)\r\n",
                          k_curve_desc[next],
                          envelope_interp_get_enabled() ? "" : " - no effect until 'I' is ON");
        } else if (c == 'f') {
            // Cycles off -> Butterworth -> Chebyshev -> off. See
            // adc_capture.h's adc_lpf_mode_t / ADC_LPF_CUTOFF_HZ /
            // ADC_LPF_CHEBYSHEV_RIPPLE_DB for what each mode actually does.
            adc_lpf_mode_t mode = adc_capture_get_lpf_mode();
            mode = (adc_lpf_mode_t)((mode + 1) % 3);
            adc_capture_set_lpf_mode(mode);
            serial_reply("-> ADC LPF: %s\r\n", adc_capture_lpf_mode_name(mode));
        } else if (c == 'n') {
            // Cycles the null-bias diagnostic's envelope threshold - see
            // ssb_dsp_set_null_bias_threshold() in ssb_dsp.h and the
            // '[dsp] null_bias' line in the periodic diagnostics block.
            // Tune this if near_null_samples% comes back 0 (threshold too
            // tight for this signal's actual peak envelope) or looks like
            // it's catching ordinary low-envelope content, not just
            // genuine two-tone nulls. Default (0.05f) is index 1 here, so
            // this starts by loosening it on the first press.
            static const float k_null_thresholds[] = {0.02f, 0.05f, 0.10f, 0.20f};
            static int s_null_threshold_idx = 1;
            s_null_threshold_idx = (s_null_threshold_idx + 1) % 4;
            float thr = k_null_thresholds[s_null_threshold_idx];
            ssb_dsp_set_null_bias_threshold(dsp_state_get_ssb(), thr);
            serial_reply("-> null_bias threshold=%.2f (see '[dsp] null_bias' diagnostic line)\r\n", thr);
        } else if (c == 'v') {
            diagnostics_toggle_muted();
        } else if (c == 'r') {
            // Resets every diagnostic counter/watermark for a clean
            // measurement window, without needing a full reflash. Useful
            // after switching modes (e.g. 't' then 'm') so counters like
            // drop_total/starve_ticks_total reflect only what happens
            // AFTER the reset, not history carried over from a different
            // mode or an earlier test run in the same boot. Touches every
            // module that keeps its own diagnostic counters, same as the
            // original inline handler touched every counter directly.
            diagnostics_reset();
            adc_capture_reset_diag();
            ssb_dsp_reset_freq_dev_stats(dsp_state_get_ssb());
            serial_reply("-> diagnostics reset, clean window starting now\r\n");
        } else if (c == 'e') {
            bool now_on = !ssb_dsp_get_eq_enabled(dsp_state_get_ssb());
            ssb_dsp_set_eq_enabled(dsp_state_get_ssb(), now_on);
            serial_reply("-> EQ (HPF+presence) %s\r\n", now_on ? "ON" : "off");
        } else if (c == 'c') {
            bool now_on = !ssb_dsp_get_compressor_enabled(dsp_state_get_ssb());
            ssb_dsp_set_compressor_enabled(dsp_state_get_ssb(), now_on);
            serial_reply("-> compressor %s\r\n", now_on ? "ON" : "off");
        } else if (c == '+') {
            float new_gain = ssb_dsp_get_master_gain_db(dsp_state_get_ssb()) + MASTER_GAIN_STEP_DB;
            dsp_state_set_master_gain_db(new_gain);
            serial_reply("-> master gain %+.1f dB\r\n", new_gain);
        } else if (c == '-') {
            float new_gain = ssb_dsp_get_master_gain_db(dsp_state_get_ssb()) - MASTER_GAIN_STEP_DB;
            dsp_state_set_master_gain_db(new_gain);
            serial_reply("-> master gain %+.1f dB\r\n", new_gain);
        } else if (c == '.') {
            float new_gain = ssb_dsp_get_master_gain_db(dsp_state_get_ssb()) + MASTER_GAIN_FINE_STEP_DB;
            dsp_state_set_master_gain_db(new_gain);
            serial_reply("-> master gain %+.2f dB\r\n", new_gain);
        } else if (c == ',') {
            float new_gain = ssb_dsp_get_master_gain_db(dsp_state_get_ssb()) - MASTER_GAIN_FINE_STEP_DB;
            dsp_state_set_master_gain_db(new_gain);
            serial_reply("-> master gain %+.2f dB\r\n", new_gain);
        } else if (c == 'd') {
            // Direct duty override - see envelope_output.h's header
            // comment. Bypasses master gain/envelope/offset-scale/
            // predistort entirely so the RSET/PWM/filter/AD9851 chain can
            // be characterized against a KNOWN, exact commanded duty
            // count instead of one inferred from a gate-voltage reading -
            // see envelope_predistort.h's REVISION 3 notes for why this
            // was added. The carrier/phase path is untouched - select a
            // steady source separately (e.g. 's', single-tone, phase
            // rock-steady - the same choice REVISION 1/2's own
            // characterization used).
            bool now_on = !envelope_output_duty_override_get_enabled();
            envelope_output_duty_override_set_enabled(now_on);
            uint32_t max_duty = envelope_output_get_max_duty();
            if (now_on) {
                s_duty_override_value = 0;
                envelope_output_write_duty_raw(s_duty_override_value);
                serial_reply("-> duty override ON, duty=%lu/%lu ('>'/'<'=+-1, 'N'/'B'=+-16; "
                              "dsp_task's normal envelope pipeline is now locked out of the "
                              "RSET output until 'd' again)\r\n",
                              (unsigned long)s_duty_override_value, (unsigned long)max_duty);
            } else {
                serial_reply("-> duty override off (dsp_task's normal envelope pipeline back in control)\r\n");
            }
        } else if (c == '>' && envelope_output_duty_override_get_enabled()) {
            uint32_t max_duty = envelope_output_get_max_duty();
            if (s_duty_override_value < max_duty) s_duty_override_value++;
            envelope_output_write_duty_raw(s_duty_override_value);
            serial_reply("-> duty %lu/%lu\r\n", (unsigned long)s_duty_override_value, (unsigned long)max_duty);
        } else if (c == '<' && envelope_output_duty_override_get_enabled()) {
            uint32_t max_duty = envelope_output_get_max_duty();
            if (s_duty_override_value > 0) s_duty_override_value--;
            envelope_output_write_duty_raw(s_duty_override_value);
            serial_reply("-> duty %lu/%lu\r\n", (unsigned long)s_duty_override_value, (unsigned long)max_duty);
        } else if (c == 'N' && envelope_output_duty_override_get_enabled()) {
            uint32_t max_duty = envelope_output_get_max_duty();
            s_duty_override_value = (s_duty_override_value + 16 > max_duty) ? max_duty : s_duty_override_value + 16;
            envelope_output_write_duty_raw(s_duty_override_value);
            serial_reply("-> duty %lu/%lu\r\n", (unsigned long)s_duty_override_value, (unsigned long)max_duty);
        } else if (c == 'B' && envelope_output_duty_override_get_enabled()) {
            uint32_t max_duty = envelope_output_get_max_duty();
            s_duty_override_value = (s_duty_override_value < 16) ? 0 : s_duty_override_value - 16;
            envelope_output_write_duty_raw(s_duty_override_value);
            serial_reply("-> duty %lu/%lu\r\n", (unsigned long)s_duty_override_value, (unsigned long)max_duty);
#if AD9851_ATTACHED
        } else if (c == 'o') {
            bool now_on = !carrier_output_get_rf_enabled();
            carrier_output_set_rf_enabled(now_on);
            serial_reply("-> AD9851 RF output %s\r\n", now_on ? "ON" : "off (powered down)");
#endif
        } else if (c == 'L') {
            // One-shot FreeRTOS task list - added to directly settle the
            // "Events"/"Arduino" IDE core-affinity question (both showing
            // Core 1 in the board menu) instead of continuing to reason
            // from uncertain memory of what Arduino-ESP32's boot glue
            // actually creates. See the [core1] busy breakdown's 96.2%
            // unexplained "other" bucket - this either shows an
            // unaccounted-for task (e.g. an "arduino_events" task) sitting
            // on Core 1, or shows only the tasks we already know about, in
            // which case the "other" bucket points back at the ADC ISR (or
            // this module's own idle-hook threshold) instead.
            //
            // vTaskList() requires configUSE_TRACE_FACILITY=1 AND
            // configUSE_STATS_FORMATTING_FUNCTIONS=1 in this board's
            // FreeRTOSConfig.h - NOT verified from here, since this
            // environment has no ESP-IDF/Arduino-ESP32 checkout to check
            // against (only this project's own source files are available
            // to read). If either is off, this line simply won't compile -
            // same situation as the esp_freertos_idle_cb_t signature
            // mismatch earlier this session, where the real compiler error
            // was more reliable than guessing from memory. If that
            // happens, the fix is to switch to uxTaskGetSystemState()
            // instead (needs only configUSE_TRACE_FACILITY, not the
            // formatting half) and format each TaskStatus_t field by hand.
            //
            // The exact column layout (whether a per-task core/affinity
            // column is present) is also an ESP-IDF FreeRTOS-port
            // extension whose default here isn't verified - but even
            // without it, the task NAME list alone is enough to confirm or
            // rule out an extra Arduino-created task existing at all.
            //
            // Buffer budgeted per FreeRTOS docs (~40 bytes/task) for up to
            // ~20 tasks - this project's known tasks are dsp_task,
            // dac_task, loopTask, IDLE0, IDLE1, ipc0, ipc1, Tmr Svc, plus
            // headroom for whatever Arduino-ESP32's boot glue adds (the
            // very thing this command exists to check for).
            static char task_list_buf[900];
            serial_reply("-> FreeRTOS task list (columns per vTaskList(): name, state "
                          "B/R/D/S/X, priority, stack high-water mark, task number, then "
                          "core/affinity IF this board's FreeRTOSConfig.h includes it):\r\n");
            vTaskList(task_list_buf);
            Serial.write((const uint8_t *)task_list_buf, strlen(task_list_buf));
            serial_reply("-> end task list\r\n");
        } else if (c == 'P') {
            // Prints every current lever as a single comma-separated line,
            // in exactly PersistentSettings's field order (name,
            // audio_source, relative_delay_samples, env_pwm_offset,
            // env_pwm_scale, env_gdeq_enable, adc_lpf_mode, eq_enable,
            // compressor_enable, master_gain_db, ad9851_output_enable,
            // env_predistort_enable, env_floor, freq_dev_slew_limit_hz,
            // envelope_interp_enable, envelope_interp_curve,
            // env_ampeq_enable, env_ampeq_shelf2_enable) -
            // wrapped in braces with a trailing comma so the whole line
            // can be pasted directly into settingsPresets[] in settings.h
            // as a new preset entry.
            // Rename "Live" (and add a numbered comment above it, matching
            // the existing presets' style) after pasting - and remember
            // settings.h's static_assert ties the array size to the
            // '0'-'9' range in this file, so adding an 11th preset needs
            // that range widened too (see the static_assert's own comment
            // in settings.h).
            //
            // env_predistort_enable ('D', envelope_predistort.h) IS
            // included now, folded into the pasteable line - worth knowing
            // before pasting this into a preset: if 'D' is ON right now,
            // the env_pwm_offset/env_pwm_scale values in the line below
            // reflect the mapping that's currently INACTIVE (pre-distortion
            // is overriding them) - they'll only take effect again on
            // whichever of the two (env_predistort_enable=false, or 'D'
            // toggled off live) happens first.
            //
            // envelope_interp_curve ('C', envelope_interp.h v4.3/v4.4) -
            // prints as the enum constant name
            // (ENVELOPE_INTERP_CURVE_CATMULL_ROM/_LINEAR/_HOLD), same
            // convention as adc_lpf_mode below, so the pasted line compiles
            // directly.
            //
            // env_ampeq_enable ('a', envelope_ampeq.h, shelf 1) - a plain
            // bool, same as env_gdeq_enable. VALIDATED on real hardware
            // 2026-09-03 (magnitude tracks prediction, real IMD benefit,
            // reintroduces some group-delay dispersion pending a gdeq
            // refit).
            //
            // env_ampeq_shelf2_enable ('A', envelope_ampeq.h, shelf 2) is
            // the newest trailing field, added 2026-09-04 when shelf 1/2
            // were split into independent flags. VALIDATED on real
            // hardware the same day: more magnitude recovery toward
            // 8000Hz, but dispersion roughly doubles again and two-tone
            // IMD came back marginally WORSE combined with shelf 1 in
            // this still-unrefit-gdeq state - pasting "true" here means
            // that preset starts up with shelf 2 active; current project
            // recommendation is "false" (shelf 1 only) until gdeq is
            // refit.
#if AD9851_ATTACHED
            float rel_delay = relative_delay_get_samples();
            bool rf_enabled = carrier_output_get_rf_enabled();
#else
            // No relative-delay line or AD9851 output to read without
            // AD9851_ATTACHED - placeholders matching every existing
            // preset's own default (0 delay wasn't any preset's default,
            // but it's the only sane placeholder absent a real value;
            // true for ad9851_output_enable matches all 5 current presets).
            float rel_delay = 0.0f;
            bool rf_enabled = true;
#endif
            // adc_lpf_mode prints as the enum constant name (ADC_LPF_MODE_OFF
            // etc.), not a string literal - it's a valid C identifier, so
            // the pasted line compiles directly into settingsPresets[].
            static const char *k_adc_lpf_mode_enum_name[3] = {
                "ADC_LPF_MODE_OFF", "ADC_LPF_MODE_BUTTERWORTH", "ADC_LPF_MODE_CHEBYSHEV"
            };
            // freq_dev_slew_limit_hz prints as the sentinel constant's own
            // name when off, same reasoning as adc_lpf_mode above - so the
            // pasted line compiles AND stays meaningful (a bare huge float
            // literal would compile fine too, but wouldn't self-document
            // as "off" the way the constant name does).
            char slew_str[40];
            float slew_limit = ssb_dsp_get_freq_dev_slew_limit_hz(dsp_state_get_ssb());
            if (slew_limit >= SSB_DSP_FREQ_DEV_SLEW_UNLIMITED_HZ) {
                snprintf(slew_str, sizeof(slew_str), "SSB_DSP_FREQ_DEV_SLEW_UNLIMITED_HZ");
            } else {
                snprintf(slew_str, sizeof(slew_str), "%.0ff", slew_limit);
            }
            static const char *k_interp_curve_enum_name[3] = {
                "ENVELOPE_INTERP_CURVE_CATMULL_ROM", "ENVELOPE_INTERP_CURVE_LINEAR",
                "ENVELOPE_INTERP_CURVE_HOLD"
            };
            serial_reply("-> settings line (paste into settingsPresets[] in settings.h, then rename \"Live\"):\r\n");
            serial_reply("    { \"Live\", %s, %.2ff, %.2ff, %.2ff, %s, %s, %s, %s, %.1ff, %s, %s, %.2ff, %s, %s, %s, %s, %s },\r\n",
                          audio_source_enum_name(dsp_state_get_audio_source()),
                          rel_delay,
                          envelope_output_get_pwm_offset(),
                          envelope_output_get_pwm_scale(),
                          envelope_gdeq_get_enabled() ? "true" : "false",
                          k_adc_lpf_mode_enum_name[adc_capture_get_lpf_mode()],
                          ssb_dsp_get_eq_enabled(dsp_state_get_ssb()) ? "true" : "false",
                          ssb_dsp_get_compressor_enabled(dsp_state_get_ssb()) ? "true" : "false",
                          ssb_dsp_get_master_gain_db(dsp_state_get_ssb()),
                          rf_enabled ? "true" : "false",
                          envelope_predistort_get_enabled() ? "true" : "false",
                          envelope_floor_get(),
                          slew_str,
                          envelope_interp_get_enabled() ? "true" : "false",
                          k_interp_curve_enum_name[envelope_interp_get_curve()],
                          envelope_ampeq_get_enabled() ? "true" : "false",
                          envelope_ampeq_shelf2_get_enabled() ? "true" : "false");
        } else if (c >= '0' && c <= '9') {
            int preset = c - '0';
            const PersistentSettings& p = settingsPresets[preset];

            dsp_state_set_audio_source(p.audio_source);
#if AD9851_ATTACHED
            // relative_delay only exists under AD9851_ATTACHED (see
            // relative_delay.h) - guarded the same way here, so this
            // still compiles standalone (mic->DSP->DAC, no AD9851 board
            // yet) per the AD9851_ATTACHED toggle documented in config.h,
            // not just in the currently-built configuration.
            relative_delay_set_samples(p.relative_delay_samples);
#endif
            envelope_output_set_pwm_offset(p.env_pwm_offset);
            envelope_output_set_pwm_scale(p.env_pwm_scale);
            // Same reset-on-enable reasoning as the 'g' handler - now
            // shared via envelope_gdeq_set_enabled() itself, so it only
            // has to be correct in one place: only reset state on an
            // off->on transition, not on every preset load (all 5 presets
            // currently have gdeq on, so switching between them keeps the
            // filter running continuously, not jumping every time).
            envelope_gdeq_set_enabled(p.env_gdeq_enable);
            adc_capture_set_lpf_mode(p.adc_lpf_mode);

            ssb_dsp_set_eq_enabled(dsp_state_get_ssb(), p.eq_enable);
            ssb_dsp_set_compressor_enabled(dsp_state_get_ssb(), p.compressor_enable);
            dsp_state_set_master_gain_db(p.master_gain_db);

#if AD9851_ATTACHED
            carrier_output_set_rf_enabled(p.ad9851_output_enable);
#endif

            // Both stateless/pure functions of their input (see
            // envelope_predistort_set_enabled()'s and
            // envelope_floor_set()'s own comments) - no reset-on-transition
            // concern the way envelope_gdeq_set_enabled() has, so a plain
            // set on every preset load is correct as-is.
            envelope_predistort_set_enabled(p.env_predistort_enable);
            envelope_floor_set(p.env_floor);

            // Stateless setter (a plain clamp-and-store on the handle,
            // same as master_gain_db) - no reset-on-transition concern,
            // safe to call unconditionally on every preset load.
            ssb_dsp_set_freq_dev_slew_limit_hz(dsp_state_get_ssb(), p.freq_dev_slew_limit_hz);

            // Same reset-on-enable reasoning as the 'g'/'I' handlers -
            // shared via envelope_interp_set_enabled() itself, so an
            // off->on transition on preset load seeds prev/target cleanly
            // too, not just when toggled live.
            envelope_interp_set_enabled(p.envelope_interp_enable);

            // v4.3: plain store, no reset-on-transition concern (see
            // envelope_interp_set_curve()'s own comment) - safe to call
            // unconditionally on every preset load, same as adc_lpf_mode
            // above.
            envelope_interp_set_curve(p.envelope_interp_curve);

            // Same reset-on-enable reasoning as the 'g'/'a'/'A' handlers -
            // shared via each shelf's own _set_enabled(), so an off->on
            // transition on preset load resets that shelf's state cleanly
            // too, not just when toggled live. Independent as of
            // 2026-09-04 (see envelope_ampeq.h) - each preset now controls
            // shelf 1 and shelf 2 separately.
            envelope_ampeq_set_enabled(p.env_ampeq_enable);
            envelope_ampeq_shelf2_set_enabled(p.env_ampeq_shelf2_enable);

            serial_reply("-> preset %d: %s\r\n", preset, p.name);
        }
    }
}
