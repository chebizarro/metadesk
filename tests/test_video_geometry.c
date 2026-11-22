#include "video_geometry.h"
#include "md_test.h"
#include <limits.h>

static void rect(uint32_t fw, uint32_t fh, int ow, int oh,
                 int x, int y, int w, int h) {
    MdVideoRect r = md_video_fit(fw, fh, ow, oh);
    MD_CHECK(r.x == x && r.y == y && r.w == w && r.h == h);
}

int main(void) {
    rect(1200, 720, 1200, 720, 0, 0, 1200, 720);
    rect(1200, 720, 1600, 900, 50, 0, 1500, 900);
    rect(1200, 720, 900, 1600, 0, 530, 900, 540);
    rect(1920, 1080, 1000, 1000, 0, 219, 1000, 562);
    rect(1200, 720, 2400, 1440, 0, 0, 2400, 1440);
    rect(0, 720, 800, 600, 0, 0, 0, 0);
    rect(1200, 0, 800, 600, 0, 0, 0, 0);
    rect(1200, 720, 0, 600, 0, 0, 0, 0);
    rect(UINT32_MAX, 1, 512, 512, 0, 255, 512, 1);
    uint32_t x, y;
    MD_CHECK(md_video_map(1200, 720, 800, 450, 1600, 900, 400, 225, &x, &y));
    MD_CHECK(x == 600 && y == 360);
    MD_CHECK(md_video_map(1200, 720, 800, 450, 1600, 900, 25, 0, &x, &y));
    MD_CHECK(x == 0 && y == 0);
    MD_CHECK(md_video_map(1200, 720, 800, 450, 1600, 900, 774, 449, &x, &y));
    MD_CHECK(x == 1198 && y == 718);
    MD_CHECK(!md_video_map(1200, 720, 800, 450, 1600, 900, 24, 225, &x, &y));
    MD_CHECK(!md_video_map(1200, 720, 800, 450, 1600, 900, 775, 225, &x, &y));
    MD_CHECK(!md_video_map(1200, 720, 900, 1600, 900, 1600, 450, 529, &x, &y));
    MD_CHECK(!md_video_map(1200, 720, 900, 1600, 900, 1600, 450, 1070, &x, &y));
    MD_CHECK(md_video_map(1200, 720, 900, 1600, 900, 1600, 450, 800, &x, &y));
    MD_CHECK(x == 600 && y == 360);
    MD_CHECK(md_video_map(1200, 720, 600, 1066, 900, 1599, 300, 533, &x, &y));
    MD_CHECK(x == 600 && y == 360);
    MD_CHECK(!md_video_map(1200, 720, 800, 450, 1600, 900, -1, 1, &x, &y));
    MD_CHECK(!md_video_map(1200, 720, 800, 450, 1600, 900, 800, 1, &x, &y));
    MD_CHECK(!md_video_map(1200, 720, 0, 450, 1600, 900, 0, 1, &x, &y));
    MD_CHECK(!md_video_map(1200, 720, 800, 450, 1600, 900, 25, 0, NULL, &y));
    return md_test_report();
}
