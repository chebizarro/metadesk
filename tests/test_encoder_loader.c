#include "encode.h"
#include "md_test.h"
int main(int argc, char **argv) {
    MdEncoderConfig cfg = {.width = 640, .height = 480, .fps = 30,
        .encoder_name = argc > 2 ? argv[2] : "openh264", .openh264_path = argc > 1 ? argv[1] : "/missing/openh264"};
    MD_CHECK(md_encoder_create(&cfg) == NULL);
    return md_test_report();
}
