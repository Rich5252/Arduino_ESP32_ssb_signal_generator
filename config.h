#pragma once

/**
 * config.h
 *
 * Shared compile-time configuration for the ssb_mic_test sketch. Split out
 * of ssb_mic_test.ino during the module-split refactor (the .ino was
 * getting hard to navigate at ~1900 lines) purely to collect the #defines
 * that more than one .cpp module needs in a single place - none of the
 * *values* below changed as part of that refactor. If you're hunting for a
 * define that used to live directly in the .ino, it's almost certainly
 * here now; a handful of genuinely module-private defines (ADC/PWM/DAC
 * pin & peripheral config, isolation-test-specific tuning that only one
 * .cpp touches) moved into that module's own header instead - see
 * adc_capture.h, envelope_output.h, carrier_output.h, relative_delay.h,
 * envelope_gdeq.h.
 *
 * IMPORTANT: include this before any other project header in every
 * translation unit that references AD9851_ATTACHED, PWM_COMPARISON_ENABLED,
 * etc. - several of those gate #if blocks in OTHER headers (in particular,
 * PWM_COMPARISON_ENABLED must be defined before "driver/ledc.h" is
 * included anywhere, same constraint the original .ino had).
 */

#include <stdint.h>

// ---- Set to 1 once the AD9851 board is wired up and its driver calls are
// filled in. Until then this runs mic->DSP->DAC standalone. ----
#define AD9851_ATTACHED 1

// ---- Two-tone test mode: bypass the mic ADC with a synthesized signal.
// Zero-hardware smoke test of the DSP chain. ----
#define TWOTONE_TEST_MODE   1  // testing default - two-tone on at boot
#define TWOTONE_F1_HZ        700.0f
#define TWOTONE_F2_HZ       1900.0f
#define TWOTONE_AMPLITUDE    0.45f   // keep below 0.5 so peaks don't clip when summed
#define SINGLETONE_HZ        1000.0f  // a clean, unambiguous default - see generate_singletone_sample()
#define SINGLETONE_AMPLITUDE 0.7f     // single tone alone - more headroom available than the
                                      // two-tone sum needs, comparable to a moderately hot mic level

// ---- Pre-Hilbert audio conditioning (HPF + presence EQ + compressor) ----
// See ssb_dsp.h's ssb_audio_fx_config_t for the individual parameters, set
// in dsp_cfg.audio_fx in setup(). Leave off with TWOTONE_TEST_MODE if you
// want to look at the raw DSP chain's spurious performance without any
// conditioning in the signal path.
#define AUDIO_FX_ENABLED 1  // was 0 (pending re-tuning) - now toggleable live per-stage via serial
                             // 'e' (EQ) / 'c' (compressor) commands, see serial_commands.cpp - flip
                             // back to 0 only if you want the whole subsystem compiled out entirely
#define MASTER_GAIN_STEP_DB 1.0f  // per '+'/'-' keypress - see ssb_dsp_set_master_gain_db()
#define MASTER_GAIN_FINE_STEP_DB 0.1f  // per '.'/',' keypress - same idea, finer resolution for
                                        // dialing in precise single-tone gain-sweep measurement
                                        // points (e.g. envelope_predistort.h's calibration table)
                                        // without needing 10 presses of '+'/'-' to move 1dB

// Mic-path DC-blocking single-pole filter's time constant, in seconds -
// dsp_task computes dc_alpha = expf(-1.0f / (SAMPLE_RATE_HZ *
// DC_BLOCK_TIME_CONSTANT_S)) from this at startup, instead of hardcoding
// the per-sample coefficient directly, so it keeps the same real-world
// cutoff regardless of SAMPLE_RATE_HZ. Value derived from the original
// hardcoded dc_alpha=0.995 @ 10000Hz: tau = -1/(Fs*ln(alpha)) =
// -1/(10000*ln(0.995)) ~= 0.019950s.
#define DC_BLOCK_TIME_CONSTANT_S 0.019950f

// ---- PWM comparison path enable flag. Defined here (before any header
// that depends on it) rather than down in envelope_output.h - #if needs
// this to already be known wherever "driver/ledc.h" gets included. ----
#define PWM_COMPARISON_ENABLED 1

#define dac_task_enabled 0

// ---- Timing debug pin: toggled high at the start of dsp_task's real work
// and low at the end, so a scope on this pin directly measures the actual
// loop iteration time on real hardware - much more trustworthy than
// estimating it. Set to 0 to remove once you've got your measurement. ----
#define TIMING_DEBUG_ENABLED 1
#define TIMING_DEBUG_GPIO     4   // within the board's easy-access GPIO1-13 range; not otherwise used

#define SAMPLE_RATE_HZ     16000u  // was 9600 (see below), then 10000 for a long stretch, now
                                    // raised to 16000 once the AD9851 write path (bit-bang +
                                    // fast register writes, see AD9851.c) freed up enough
                                    // real-time budget: at 10000 write_us alone was ~50-52us of
                                    // the 100us period; the bit-bang rewrite cut that to ~25us,
                                    // and with adc+dsp added (~45us total, measured via
                                    // [timing]) there was real headroom to spend. 16000 is the
                                    // next value up from 10000 that still divides both 80000
                                    // (ADC) and 2000000 (gptimer resolution_hz) evenly - the
                                    // only other clean option below 20000 - see below for why
                                    // that divisibility matters, and why 20000 itself was ruled
                                    // out (period drops to 50us, leaving too little margin
                                    // against the ~12-20us of wakeup jitter already measured).
                                    //
                                    // IMPORTANT: SAMPLE_RATE_HZ isn't just this #define - see
                                    // envelope_gdeq.h's ENV_GDEQ_A1/A2 (fitted separately per
                                    // Fs, #error's out at compile time for any value other than
                                    // 10000/16000), settings.h's relative_delay_samples presets
                                    // (rescaled by the Fs ratio when this last changed, but only
                                    // an approximation - see that file's header note), and
                                    // dsp_task's dc_alpha (now derived from DC_BLOCK_TIME_CONSTANT_S
                                    // below rather than hardcoded, so it doesn't need touching).
                                    //
                                    // Original 9600->10000 change: 9600 didn't divide either
                                    // 80000 (ADC) or 2000000 (gptimer resolution_hz) evenly,
                                    // causing a genuine ~4.8% ADC_SAMPLES_PER_TICK mismatch
                                    // (true_ratio measured 8.387 vs nominal 8) plus imprecise
                                    // gptimer alarm timing. 10000 divided both cleanly
                                    // (80000/10000=8, 2000000/10000=200) - restored the FIFO's
                                    // exact-ratio assumption the catch-up logic depends on for
                                    // smooth operation; 16000 keeps that same property
                                    // (80000/16000=5, 2000000/16000=125).
                                    //
                                    // A drop to ADC_CONT_SAMPLE_FREQ_HZ=48000 (adc_capture.h) was
                                    // tried, to free Core-0 headroom for the wakeup-jitter work
                                    // below - reverted after real hardware showed no clear CPU
                                    // win and a real noise regression (see that #define's own
                                    // "TRIED, REVERTED" comment for the full story). Rate is back
                                    // to 80000 - the divisibility numbers above are live again as
                                    // written, not just historical.
#define HILBERT_TAPS       65   // was briefly tested at 129 to check whether Hilbert filter
                                 // approximation accuracy was the source of the IMD floor that
                                 // tracks 1:1 with signal level below -6dB - real hardware A/B
                                 // showed no significant difference, ruling that hypothesis out
                                 // cleanly. Reverted to 65 since 129 bought nothing but extra FIR
                                 // cost. The floor's more likely explanation is now the sub-sample
                                 // timing residual - see relative_delay.h's fractional delay
                                 // line, added specifically to test that instead. Must stay ODD
                                 // if changed again.
#define MAX_FREQ_DEV_HZ    8000.0f  // Originally raised from 2800.0f for a diagnostic A/B test -
                                     // real hardware showed a consistent ~+100Hz offset on BOTH
                                     // tones of a 700/1900Hz two-tone test (landed at 800/1999Hz)
                                     // while a single 1000Hz tone was exactly on frequency;
                                     // fast_atan2/fast_sqrt already ruled out via direct A/B
                                     // (SSB_DSP_FAST_TRIG=0 test, no change). Raising the clamp
                                     // let ssb_dsp.c's max_unclamped_freq_dev_hz diagnostic (see
                                     // diagnostics.cpp's '[dsp] max_unclamped=' line) measure the
                                     // TRUE peak deviation without the clamp masking it - came
                                     // back ~4800-4900Hz for real two-tone signals (see
                                     // FM_TEST_DEV_HZ below), well above the old 2800Hz ceiling,
                                     // meaning that clamp was routinely engaging on real peaks,
                                     // not just as a rare edge-case safety limit. Whether that
                                     // clamping was actually the CAUSE of the +100Hz offset was
                                     // never confirmed either way - that thread was left open.
                                     //
                                     // Separately, real hardware mic white-noise testing showed
                                     // the freq-dev clamp performs better set higher - so 8000Hz
                                     // is being KEPT for now rather than reverted to 2800.0f, but
                                     // still isn't a deliberately-chosen final value (no attempt
                                     // yet to find where between ~4900Hz and 8000Hz is actually
                                     // optimal, vs. just "high enough to stop hurting").

// ---- Isolation test signal parameters (ENVSTEP / FMTEST / AMTEST) - see
// audio_source_t in settings.h for what each mode does. ----
#define ENVSTEP_HZ   4.0f   // square wave rate - slow enough for easy scope triggering/viewing,
                             // fast enough not to be tedious to observe
#define FM_TEST_MOD_HZ  1200.0f  // matches the 700/1900Hz two-tone pair's beat frequency, for
                                  // direct comparability - spurs at n*1200Hz offsets would mean
                                  // something in AD9851 chain itself, not the DSP/Hilbert path
#define FM_TEST_DEV_HZ  3000.0f  // peak deviation - comparable order to real two-tone peak
                                  // deviations seen in testing (up to ~4800-4900Hz observed).
                                  // Gives modulation index beta=3000/1200=2.5 - a reasonably rich
                                  // sideband spectrum, good for comparing against the predicted
                                  // Bessel-function pattern
#define AM_TEST_MOD_HZ  1200.0f  // same rate as FM_TEST_MOD_HZ, for direct comparability between
                                  // the two isolation tests
#define AM_TEST_DEPTH   0.5f     // envelope swings the FULL available [0,1] range (0.5 +/- 0.5) -
                                  // 100% depth AM, deliberately stressing the RSET path's linearity
                                  // across its whole normalized range, same as real two-tone peaks
                                  // routinely reach in practice

// Worst-case timing diagnostics period, shared by diagnostics.cpp - one
// sample period in microseconds.
#define SSB_SAMPLE_PERIOD_US (1000000UL / SAMPLE_RATE_HZ)
