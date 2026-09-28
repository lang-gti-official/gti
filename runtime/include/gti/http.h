#pragma once

#include "gti/c_abi.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Private, versioned transport boundary for <std/http>. Public GTI programs
// use owned std::http request/response values and never observe these handles.
typedef uint32_t gti_http_method_v1;
enum {
  GTI_HTTP_GET_V1 = 0,
  GTI_HTTP_POST_V1 = 1,
  GTI_HTTP_PUT_V1 = 2,
  GTI_HTTP_PATCH_V1 = 3,
  GTI_HTTP_DELETE_V1 = 4,
  GTI_HTTP_HEAD_V1 = 5,
};

typedef uint32_t gti_http_error_v1;
enum {
  GTI_HTTP_OK_V1 = 0,
  GTI_HTTP_INVALID_URL_V1 = 1,
  GTI_HTTP_INVALID_HEADER_V1 = 2,
  GTI_HTTP_UNSUPPORTED_SCHEME_V1 = 3,
  GTI_HTTP_NAME_RESOLUTION_FAILED_V1 = 4,
  GTI_HTTP_CONNECTION_FAILED_V1 = 5,
  GTI_HTTP_TLS_FAILED_V1 = 6,
  GTI_HTTP_TIMEOUT_V1 = 7,
  GTI_HTTP_REQUEST_FAILED_V1 = 8,
  GTI_HTTP_RESPONSE_TOO_LARGE_V1 = 9,
  GTI_HTTP_BACKEND_UNAVAILABLE_V1 = 10,
  GTI_HTTP_CLIENT_CREATION_FAILED_V1 = 11,
};

// All handles are process-local, positive pointer identities encoded as
// int64_t. Zero is the sole null handle. The implementation never exposes a
// libcurl type or ownership rule through this boundary.
gti_http_error_v1 gti_rt_http_client_create_v1(int64_t *output_client);
void gti_rt_http_client_destroy_v1(int64_t client);

// Headers are encoded as exactly header_count pairs of NUL-delimited name and
// value byte strings. The URL and CA bundle path are counted strings. An empty
// CA bundle path requests the platform/libcurl default trust store.
gti_http_error_v1 gti_rt_http_client_send_v1(
    int64_t client, gti_http_method_v1 method, gti_c_string_view url,
    gti_c_string_view encoded_headers, uint64_t header_count,
    gti_c_string_view body, uint64_t connect_timeout_ms,
    uint64_t request_timeout_ms, uint64_t max_response_bytes,
    gti_c_string_view ca_bundle_path, int64_t *output_response,
    int32_t *output_native_code);

uint64_t gti_rt_http_client_error_size_v1(int64_t client);
int64_t gti_rt_http_client_error_read_v1(int64_t client, uint64_t offset,
                                         uint8_t *output, uint64_t capacity);

void gti_rt_http_response_destroy_v1(int64_t response);
uint32_t gti_rt_http_response_status_v1(int64_t response);
uint64_t gti_rt_http_response_header_count_v1(int64_t response);
uint64_t gti_rt_http_response_header_name_size_v1(int64_t response,
                                                  uint64_t index);
int64_t gti_rt_http_response_header_name_read_v1(int64_t response,
                                                 uint64_t index,
                                                 uint64_t offset,
                                                 uint8_t *output,
                                                 uint64_t capacity);
uint64_t gti_rt_http_response_header_value_size_v1(int64_t response,
                                                   uint64_t index);
int64_t gti_rt_http_response_header_value_read_v1(int64_t response,
                                                  uint64_t index,
                                                  uint64_t offset,
                                                  uint8_t *output,
                                                  uint64_t capacity);
uint64_t gti_rt_http_response_body_size_v1(int64_t response);
int64_t gti_rt_http_response_body_read_v1(int64_t response, uint64_t offset,
                                          uint8_t *output, uint64_t capacity);

#ifdef __cplusplus
}
#endif
