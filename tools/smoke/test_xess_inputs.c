#include "xess_inputs.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void check(const char* name, int condition)
{
    printf("[%s] %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) {
        failures++;
    }
}

int main(void)
{
    kz3_xess_draw_info draw;
    memset(&draw, 0, sizeof(draw));
    draw.frame = 42;
    draw.target_offset = 0x02200000u;
    draw.target_width = 1280;
    draw.target_height = 720;
    draw.mrt_count = 4;
    draw.depth_slot = 7;
    draw.target_is_local = 1;
    draw.full_viewport = 1;
    draw.depth_test = 1;

    check(
        "full-resolution four-target depth pass selects deferred depth",
        kz3_xess_is_deferred_primary(&draw, 1280, 720));
    draw.mrt_count = 1;
    check(
        "single-target depth pass is not the deferred primary",
        !kz3_xess_is_deferred_primary(&draw, 1280, 720));

    draw.target_offset = KZ3_XESS_SCENE_TARGET_OFFSET;
    check(
        "depth-tested scene draw reuses deferred jitter",
        kz3_xess_should_jitter(&draw, 1280, 720, 42, 7));
    check(
        "scene draw rejects a stale deferred-depth frame",
        !kz3_xess_should_jitter(&draw, 1280, 720, 41, 7));

    draw.depth_test = 0;
    draw.blend = 0;
    draw.sampled_full_size_render_target = 1;
    draw.sampled_render_target_offset =
        KZ3_XESS_DEFERRED_LIGHTING_OFFSET;
    check(
        "validated lighting resolve selects opaque capture",
        kz3_xess_is_opaque_resolve(&draw, 1280, 720));
    draw.blend = 1;
    check(
        "blended scene draw cannot replace opaque capture",
        !kz3_xess_is_opaque_resolve(&draw, 1280, 720));

    kz3_xess_capture_key opaque = {
        42, 3, 9, 1280, 720, (uintptr_t)0x1234u};
    kz3_xess_capture_key pre = opaque;
    check(
        "same-frame scene captures form a responsive pair",
        kz3_xess_capture_pair_matches(&opaque, &pre));
    pre.frame++;
    check(
        "capture pair rejects a different frame",
        !kz3_xess_capture_pair_matches(&opaque, &pre));
    pre = opaque;
    pre.resource++;
    check(
        "capture pair rejects a different resource",
        !kz3_xess_capture_pair_matches(&opaque, &pre));
    pre = opaque;
    pre.context_generation++;
    check(
        "capture pair rejects a recreated XeSS context",
        !kz3_xess_capture_pair_matches(&opaque, &pre));

    printf("[test_xess_inputs] %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
