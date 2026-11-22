#ifndef MD_ENCODER_CHAIN_H
#define MD_ENCODER_CHAIN_H
#include <stdbool.h>
typedef enum { MD_ENCODER_MACOS, MD_ENCODER_LINUX, MD_ENCODER_WINDOWS } MdEncoderPlatform;
typedef bool (*MdEncoderTry)(void *userdata, const char *name);
/* Returns the selected name, or NULL. The callback owns failed-open cleanup. */
const char *md_encoder_select(MdEncoderPlatform platform, bool nvenc,
                              const char *only, MdEncoderTry attempt, void *userdata);
#endif
