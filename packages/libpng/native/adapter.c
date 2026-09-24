#include "bindings.h"
#include <png.h>
#include <string.h>

static GtiPngResult result_for(png_image *image) {
  GtiPngResult result = {GTI_PNG_INVALID, image->width, image->height, 0};
  // Bound the row stride to libpng's signed stride domain before
  // multiplication.
  if (image->width == 0 || image->height == 0 || image->width > INT32_MAX / 4 ||
      (uint64_t)image->width * 4 > UINT64_MAX / image->height) {
    return result;
  }
  result.bytes = (uint64_t)image->width * image->height * 4;
  result.status = GTI_PNG_OK;
  return result;
}

uint32_t gti_png_version(void) { return png_access_version_number(); }

GtiPngResult gti_png_info(const uint8_t *data, uint64_t length) {
  png_image image;
  memset(&image, 0, sizeof(image));
  image.version = PNG_IMAGE_VERSION;
  GtiPngResult result = {GTI_PNG_INVALID, 0, 0, 0};
  if (data != NULL && length != 0 &&
      png_image_begin_read_from_memory(&image, data, length)) {
    result = result_for(&image);
  }
  png_image_free(&image);
  return result;
}

GtiPngResult gti_png_decode_rgba(const uint8_t *data, uint64_t length,
                                 uint8_t *output, uint64_t capacity) {
  png_image image;
  memset(&image, 0, sizeof(image));
  image.version = PNG_IMAGE_VERSION;
  GtiPngResult result = {GTI_PNG_INVALID, 0, 0, 0};
  if (data == NULL || length == 0 ||
      !png_image_begin_read_from_memory(&image, data, length)) {
    png_image_free(&image);
    return result;
  }
  result = result_for(&image);
  image.format = PNG_FORMAT_RGBA;
  if (result.status == GTI_PNG_OK) {
    if (output == NULL || capacity < result.bytes) {
      result.status = GTI_PNG_BUFFER_SMALL;
    } else if (!png_image_finish_read(&image, NULL, output, 0, NULL)) {
      result.status = GTI_PNG_INVALID;
    }
  }
  png_image_free(&image);
  return result;
}

GtiPngResult gti_png_encode_rgba(const uint8_t *pixels, uint64_t length,
                                 uint32_t width, uint32_t height,
                                 uint8_t *output, uint64_t capacity) {
  png_image image;
  memset(&image, 0, sizeof(image));
  image.version = PNG_IMAGE_VERSION;
  image.width = width;
  image.height = height;
  image.format = PNG_FORMAT_RGBA;
  GtiPngResult result = result_for(&image);
  if (result.status != GTI_PNG_OK || pixels == NULL || length < result.bytes) {
    result.status = GTI_PNG_INVALID;
    return result;
  }
  png_alloc_size_t required = 0;
  if (!png_image_write_to_memory(&image, NULL, &required, 0, pixels, 0, NULL)) {
    result.status = GTI_PNG_INVALID;
  } else {
    result.bytes = required;
    if (output != NULL) {
      if (capacity < required) {
        result.status = GTI_PNG_BUFFER_SMALL;
      } else if (!png_image_write_to_memory(&image, output, &required, 0,
                                            pixels, 0, NULL)) {
        result.status = GTI_PNG_INVALID;
      } else {
        result.bytes = required;
      }
    }
  }
  png_image_free(&image);
  return result;
}
