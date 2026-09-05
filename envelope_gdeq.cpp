/**
 * envelope_gdeq.cpp - see envelope_gdeq.h.
 */

#include "envelope_gdeq.h"

static ssb_allpass1_t s_env_gdeq_1;
static ssb_allpass1_t s_env_gdeq_2;
static volatile bool s_env_gdeq_enable = false;

// Added 2026-09-05 alongside the runtime 'G' toggle - see envelope_gdeq.h's
// header comment and envelope_gdeq_set_use_aa_candidate()'s doc comment.
// Always false (and can't be set true) when ENV_GDEQ_HAS_AA_CANDIDATE is 0.
static volatile bool s_env_gdeq_use_aa_candidate = false;

void envelope_gdeq_init(void)
{
    float a1 = s_env_gdeq_use_aa_candidate ? ENV_GDEQ_A1_AA_CANDIDATE : ENV_GDEQ_A1;
    float a2 = s_env_gdeq_use_aa_candidate ? ENV_GDEQ_A2_AA_CANDIDATE : ENV_GDEQ_A2;
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

bool envelope_gdeq_get_use_aa_candidate(void)
{
    return s_env_gdeq_use_aa_candidate;
}

void envelope_gdeq_set_use_aa_candidate(bool use_candidate)
{
#if !ENV_GDEQ_HAS_AA_CANDIDATE
    // No candidate fitted for this filter/Fs - stay on the default pair
    // regardless of what's requested (see the header comment). Silently
    // ignoring rather than asserting/erroring here because this can be
    // reached from a pasted-preset PersistentSettings field
    // (env_gdeq_use_aa_candidate) built on a DIFFERENT filter/Fs
    // combination - same "don't brick playback over a stale preset field,
    // just don't apply the part that doesn't make sense here" approach
    // this project uses elsewhere for cross-build preset fields.
    use_candidate = false;
#endif
    if (use_candidate == s_env_gdeq_use_aa_candidate) {
        return;  // no-op transition - don't reset state (glitch) for nothing
    }
    s_env_gdeq_use_aa_candidate = use_candidate;
    // Re-init (not just reset) - the coefficient itself changed, and
    // ssb_allpass1_init() is documented cheap/task-context-safe. This also
    // zeroes both sections' state as a side effect, same no-stale-x1/y1
    // reasoning as the off->on case in envelope_gdeq_set_enabled() above -
    // switching live never feeds a mismatched coefficient/state pair into
    // the very next sample.
    float a1 = s_env_gdeq_use_aa_candidate ? ENV_GDEQ_A1_AA_CANDIDATE : ENV_GDEQ_A1;
    float a2 = s_env_gdeq_use_aa_candidate ? ENV_GDEQ_A2_AA_CANDIDATE : ENV_GDEQ_A2;
    ssb_allpass1_init(&s_env_gdeq_1, a1);
    ssb_allpass1_init(&s_env_gdeq_2, a2);
}
