#include <stdio.h>
#include <unistd.h>

#include "util.h"

static void test_volume_math(void) {
    printf("=== volume math ===\n");
    float levels[] = { 0.0f, 0.25f, 0.5f, 0.72f, 1.0f };
    for (int i = 0; i < 5; i++) {
        float p = levels[i];
        float lin = perceptual_to_linear(p);
        float back = linear_to_perceptual(lin);
        printf("perceptual=%.2f  linear=%.6f  roundtrip=%.6f  diff=%.8f\n",
               p, lin, back, back - p);
    }
}

static void test_volume_step(void) {
    printf("\n=== volume_step ===\n");
    float cases[] = { 0.0f, 0.03f, 0.50f, 0.97f, 1.0f };
    for (int i = 0; i < 5; i++) {
        float up   = volume_step(cases[i], +5);
        float down = volume_step(cases[i], -5);
        printf("level=%.2f  +5->%.2f  -5->%.2f\n", cases[i], up, down);
    }
}

static void test_icon(void) {
    printf("\n=== volume_icon ===\n");
    printf("0.0  muted=false : %s\n", volume_icon(0.0f, false));
    printf("0.2  muted=false : %s\n", volume_icon(0.2f, false));
    printf("0.5  muted=false : %s\n", volume_icon(0.5f, false));
    printf("0.8  muted=false : %s\n", volume_icon(0.8f, false));
    printf("0.8  muted=true  : %s\n", volume_icon(0.8f, true));
}

static void test_parse_name_json(void) {
    printf("\n=== parse_name_json ===\n");
    const char *cases[] = {
        "{\"name\":\"alsa_output.usb-HP__Inc_HyperX_Cloud_III_000000000000-00.analog-stereo\"}",
        "{\"name\":\"simple-sink\"}",
        "{\"name\":\"\"}",
        "not json at all",
        NULL
    };
    for (int i = 0; cases[i]; i++) {
        char buf[128] = {0};
        int r = parse_name_json(cases[i], buf, sizeof(buf));
        printf("input: %-40s  r=%d  name='%s'\n", cases[i], r, buf);
    }
}

static void test_emit(void) {
    printf("\n=== emit (to stdout) ===\n");
    emit_sink(STDOUT_FILENO, 0.72f, false);
    emit_sink(STDOUT_FILENO, 0.0f, true);
    emit_player(STDOUT_FILENO, "spotify",
                "Roslyn", "Bon Iver",
                "Playing", 0.8);
    emit_player(STDOUT_FILENO, "firefox",
                "Track with \"quotes\" inside", "Artist",
                "Paused", 0.5);
}

int main(void) {
    test_volume_math();
    test_volume_step();
    test_icon();
    test_parse_name_json();
    test_emit();
    return 0;
}
