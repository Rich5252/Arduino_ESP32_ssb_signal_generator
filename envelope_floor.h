#pragma once

/**
 * envelope_floor.h
 *
 * ---- Envelope-null floor ----
 * Two-tone (and real speech) envelopes dip toward zero at destructive-
 * interference nulls. The lower the envelope gets, the further down into
 * the measured BS170/RSET transfer curve it drives the commanded duty -
 * straight into the steepest, most sparsely-characterized part of
 * envelope_predistort.h's table (36-55% duty, 60-137dB/V measured). Small
 * LUT interpolation error there gets amplified by that slope.
 * envelope_floor_apply() smoothly compresses the WHOLE [0,1] envelope
 * range into [floor,1] (peak still maps to 1, only the bottom is raised)
 * so it can't be driven that low - raising it via 'x' trades some
 * true-null carrier suppression (see envelope_predistort.h's "true zero"
 * discussion - the deepest measured nulls only bought ~58dB anyway) for
 * keeping the envelope out of that worst region.
 *
 * This ONLY touches envelope, and does so with a continuous affine remap,
 * not a hard clamp. Two earlier versions were tried and removed after
 * real-hardware testing - see envelope_floor.cpp's header comment for
 * both postmortems:
 *  1. Also froze freq_dev_hz (phase rate) while below the floor, on the
 *     theory that atan2() near a null is untrustworthy noise. Wrong for a
 *     two-tone signal - the fast phase rotation at a null is genuine
 *     required signal content, not noise - and it built up a phase debt
 *     that snapped back as a hard discontinuity once the floor got large
 *     enough ("does nothing, then products jump to 0dB at a threshold").
 *  2. A hard clamp (min(envelope, floor)) on the envelope side alone.
 *     Left a derivative discontinuity at every crossing, which a two-tone
 *     signal hits often (near every null) - showed up as nearly every
 *     product rising gently with each floor increment, a new distortion
 *     source confounding the actual question being tested.
 * If phase-side null artifacts still need addressing later, it'll need
 * an approach that actually tracks/pre-warps the phase trajectory through
 * the null, not one that freezes its rate.
 *
 * Call with the RAW envelope straight out of ssb_dsp_process_sample() (or
 * the ENVSTEP/FMTEST/AMTEST equivalents), BEFORE
 * envelope_gdeq_process()/envelope_predistort_process() - the returned
 * value replaces what's passed downstream from there.
 *
 * Floor starts at 0.0 (off - raw envelope passes through unchanged, since
 * envelope is never negative) so existing tuning isn't disturbed until
 * deliberately opted into, same convention as 'g'/'D'.
 */

#include "esp_attr.h"

// Smoothly compresses envelope's [0,1] range into [floor,1]. Returns the
// remapped envelope to use downstream.
float IRAM_ATTR envelope_floor_apply(float raw_envelope);

float envelope_floor_get(void);
void envelope_floor_set(float floor);
void envelope_floor_raise(void);   // 'x' - step up by ENV_FLOOR_STEP
void envelope_floor_lower(void);   // 'z' - step down by ENV_FLOOR_STEP
