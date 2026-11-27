#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CURSOR_MAX_PX (256 * 256)
static pthread_mutex_t g_cursor_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_cursor_hx, g_cursor_hy;
static uint32_t g_cursor_px[CURSOR_MAX_PX];
static int g_cursor_w, g_cursor_h;
#include "cursor_state.inc"
#include "cursor_publish.inc"

static int snapshot[6 + 4];
static int failures;

static void check(int ok, const char *what) {
    if (!ok) {
        printf("FAIL: %s (serial %d, hidden %d)\n", what, snapshot[0], snapshot[1]);
        failures++;
    }
}

static void take(void) {
    memset(snapshot, 0, sizeof snapshot);
    if (droiddeck_cursor_snapshot(snapshot, (int)(sizeof snapshot / sizeof snapshot[0])) < 6)
        printf("FAIL: snapshot refused\n"), failures++;
}

int main(void) {
    take();
    check(snapshot[0] == 0 && snapshot[1] == 1, "a new compositor has no cursor state and reports none");

    cursor_publish_hidden();
    take();
    check(snapshot[0] != 0 && snapshot[1] == 1, "a first set_cursor(NULL) is reported as hidden");
    int first = snapshot[0];
    cursor_publish_hidden();
    take();
    check(snapshot[0] == first, "hiding a hidden cursor again changes nothing");

    const uint32_t arrow[4] = {1, 2, 3, 4};
    cursor_publish_pixels((const uint8_t *)arrow, 2, 2, 8, 1, 1);
    take();
    check(snapshot[0] > first && snapshot[1] == 0 && snapshot[2] == 2 && snapshot[3] == 2 && snapshot[6] == 1,
          "a cursor image is reported as shown");
    int shown = snapshot[0];
    cursor_publish_hidden();
    take();
    check(snapshot[0] > shown && snapshot[1] == 1, "hiding a shown cursor is reported");
    int hidden = snapshot[0];
    cursor_publish_shown();
    take();
    check(snapshot[0] > hidden && snapshot[1] == 0, "showing the last image again is reported");
    check(droiddeck_cursor_serial() == snapshot[0], "the serial read alone matches the snapshot");

    if (failures) return 1;
    puts("PASS: cursor state publishing");
    return 0;
}
