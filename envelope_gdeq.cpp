/**
 * envelope_gdeq.cpp - see envelope_gdeq.h.
 */

#include "envelope_gdeq.h"

static ssb_allpass1_t s_env_gdeq_1;
static ssb_allpass1_t s_env_gdeq_2;
static volatile bool s_env_gdeq_enable = false;

// Added 2026-09-05 alongside the runtime 'G' toggle, extended 2026-09-06
// from a plain bool to this 3-state enum when "candidate B" made a second
// alternative coefficient set exist - see envelope_gdeq.h's header comment
// and envelope_gdeq_set_variant()'s doc comment. Can only ever hold a
// variant envelope_gdeq_variant_available() says is fitted for this
// build - envelope_gdeq_set_variant() enforces that on every write.
static volatile env_gdeq_variant_t s_env_gdeq_variant = ENV_GDEQ_VARIANT_DEFAULT;

// Shared by envelope_gdeq_init() and envelope_gdeq_set_variant() so the
// variant->coefficient mapping only has to be correct in one place.
static void env_gdeq_coefficients_for_variant(env_gdeq_variant_t variant, float *out_a1, float *out_a2)
{
    switch (variant) {
        case ENV_GDEQ_VARIANT_AA_CANDIDATE:
            *out_a1 = ENV_GDEQ_A1_AA_CANDIDATE;
            *out_a2 = ENV_GDEQ_A2_AA_CANDIDATE;
            break;
        case ENV_GDEQ_VARIANT_CANDIDATE_B:
            *out_a1 = ENV_GDEQ_A1_CANDIDATE_B;
            *out_a2 = ENV_GDEQ_A2_CANDIDATE_B;
            break;
        case ENV_GDEQ_VARIANT_DEFAULT:
        default:
            *out_a1 = ENV_GDEQ_A1;
            *out_a2 = ENV_GDEQ_A2;
            break;
    }
}

void envelope_gdeq_init(void)
{
    float a1, a2;
    env_gdeq_coefficients_for_variant(s_env_gdeq_variant, &a1, &a2);
    ssb_allpass1_init(&s_env_gdeq_1, a1);
    ssb_allpass1_init(&s_env_gdeq_2, a2);
}

float IRAM_ATTR envelope_gdeq_process(float envelope)
{
    if (s_env_gdeq_enable) {
        envelope = ssb_allpass1_process(&s_env_gdeq_1, envelope);
        envelope = ssb_allpass1_process(&s_env_gdeq_2, envelope);
    }
    return envelope;
}

bool envelope_gdeq_get_enabled(void)
{
    return s_env_gdeq_enable;
}

void envelope_gdeq_set_enabled(bool enable)
{
    bool was_on = s_env_gdeq_enable;
    s_env_gdeq_enable = enable;
    if (enable && !was_on) {
        ssb_allpass1_reset(&s_env_gdeq_1);
        ssb_allpass1_reset(&s_env_gdeq_2);
    }
}

env_gdeq_variant_t envelope_gdeq_get_variant(void)
{
    return s_env_gdeq_variant;
}

bool envelope_gdeq_variant_available(env_gdeq_variant_t variant)
{
    switch (variant) {
        case ENV_GDEQ_VARIANT_AA_CANDIDATE:
            return ENV_GDEQ_HAS_AA_CANDIDATE ? true : false;
        case ENV_GDEQ_VARIANT_CANDIDATE_B:
            return ENV_GDEQ_HAS_CANDIDATE_B ? true : false;
        case ENV_GDEQ_VARIANT_DEFAULT:
        default:
            return true;
    }
}

const char *envelope_gdeq_variant_name(env_gdeq_variant_t variant)
{
    switch (variant) {
        case ENV_GDEQ_VARIANT_AA_CANDIDATE:
            return "a+A candidate (a1=a2=-0.139115)";
        case ENV_GDEQ_VARIANT_CANDIDATE_B:
            // Shortened 2026-09-06 (same day, later) - the original name
            // baked in "NOT bench-validated", which (a) went stale within
            // hours once the HiRes_aAG4_TF.txt bench test came back, and
            // (b) combined with the 'G' handler's full caveat string to
            // overflow serial_reply()'s 512-byte buffer (607 bytes total,
            // silently... well, now visibly, since 2026-09-02, truncated
            // with a "[serial_reply] WARNING" - real hardware confirmed).
            // Status/caveat text now lives ONLY in the 'G' handler's
            // caveat string below (serial_commands.cpp) and the header
            // comment above, so it only has to be updated in one place as
            // the bench-test status evolves, instead of also being
            // baked into this name (which is also used by the much
            // shorter 'w'/boot-banner replies, where a long status clause
            // isn't wanted anyway).
            return "candidate B (a1=a2=+0.09)";
        case ENV_GDEQ_VARIANT_DEFAULT:
        default:
            return "default (a1=0.026173, a2=0.236810)";
    }
}

void envelope_gdeq_set_variant(env_gdeq_variant_t variant)
{
    if (!envelope_gdeq_variant_available(variant)) {
        // No fit exists for this variant on the active filter/Fs - stay on
        // the default pair regardless of what's requested (see the header
        // comment). Silently falling back rather than asserting/erroring
        // here because this can be reached from a pasted-preset
        // PersistentSettings field (env_gdeq_variant) built on a DIFFERENT
        // filter/Fs combination - same "don't brick playback over a stale
        // preset field, just don't apply the part that doesn't make sense
        // here" approach this project uses elsewhere for cross-build
        // preset fields.
        variant = ENV_GDEQ_VARIANT_DEFAULT;
    }
    if (variant == s_env_gdeq_variant) {
        return;  // no-op transition - don't reset state (glitch) for nothing
    }
    s_env_gdeq_variant = variant;
    // Re-init (not just reset) - the coefficients themselves changed, and
    // ssb_allpass1_init() is documented cheap/task-context-safe. This also
    // zeroes both sections' state as a side effect, same no-stale-x1/y1
    // reasoning as the off->on case in envelope_gdeq_set_enabled() above -
    // switching live never feeds a mismatched coefficient/state pair into
    // the very next sample.
    float a1, a2;
    env_gdeq_coefficients_for_variant(s_env_gdeq_variant, &a1, &a2);
    ssb_allpass1_init(&s_env_gdeq_1, a1);
    ssb_allpass1_init(&s_env_gdeq_2, a2);
}
