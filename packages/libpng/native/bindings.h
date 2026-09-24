#ifndef GTI_PACKAGE_LIBPNG_BINDINGS_H
#define GTI_PACKAGE_LIBPNG_BINDINGS_H
#include <stdint.h>

// All buffers belong to the caller. No setjmp/longjmp crosses into GTI.
#define GTI_PNG_OK 0
#define GTI_PNG_INVALID 1
#define GTI_PNG_BUFFER_SMALL 2
typedef struct GtiPngResult {
  int32_t status;
  uint32_t width;
  uint32_t height;
  uint64_t bytes;
} GtiPngResult;

uint32_t gti_png_version(void);
GtiPngResult gti_png_info(const uint8_t *png, uint64_t length);
GtiPngResult gti_png_decode_rgba(const uint8_t *png, uint64_t length,
                                 uint8_t *output, uint64_t capacity);
// A null output queries the encoded size. Input is tightly packed RGBA8.
GtiPngResult gti_png_encode_rgba(const uint8_t *pixels, uint64_t length,
                                 uint32_t width, uint32_t height,
                                 uint8_t *output, uint64_t capacity);
#endif
