#include <assert.h>
#include <stdbool.h>
#include "../modules/webos/utils/max_res.c"

static int mode;
static bool fake_mrc(int codec, int *w, int *h, int *fps) {
    assert(codec == 4);
    if (mode == 1) return false;
    if (mode == 2) return true; /* success without writing outputs */
    *w = 4096; *h = 2304; *fps = 120;
    return true;
}
int main(void) {
    unsigned int w = 0, h = 0, fps = 0;
    MRCGetMaxVideoResolution = fake_mrc;
    assert(SS4S_webOS_GetMaxVideoResolution(SS4S_VIDEO_H265, &w, &h, &fps) == 0);
    assert(w == 4096 && h == 2304 && fps == 120);
    for (mode = 1; mode <= 2; ++mode) {
        w = h = fps = 17;
        assert(SS4S_webOS_GetMaxVideoResolution(SS4S_VIDEO_H265, &w, &h, &fps) == -1);
        assert(w == 17 && h == 17 && fps == 17);
    }
    assert(SS4S_webOS_GetMaxVideoResolution(SS4S_VIDEO_AV1, &w, &h, &fps) == -1);
    return 0;
}
