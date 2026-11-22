#ifndef MD_OPENH264_FETCH_H
#define MD_OPENH264_FETCH_H
#include <stddef.h>
#include <stdbool.h>
/* Only the standalone, explicit host CLI calls this. Never encoder startup. */
int md_openh264_fetch(void);
/* Verification primitive; digest is the pinned SHA-256 of decompressed bytes.
 * Exposed for deterministic offline fixtures; caller owns the returned buffer. */
bool md_openh264_unpack(const void *input, size_t size, const char *digest,
                         unsigned char **output, size_t *output_size);
#endif
