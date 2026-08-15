#pragma once

#include "gti/c_abi.h"

#include <stdint.h>

#define GTI_FAILURE_ABI_VERSION_V1 UINT32_C(1)
#define GTI_FAILURE_EXIT_STATUS 70
#define GTI_FAILURE_ARTIFACT_IDENTITY_SIZE UINT32_C(32)

typedef uint16_t gti_failure_code_v1;

enum {
  GTI_FAILURE_CODE_NONE_V1 = 0,
  GTI_FAILURE_CODE_INTEGER_OVERFLOW_V1 = 1,
  GTI_FAILURE_CODE_DIVISION_BY_ZERO_V1 = 2,
  GTI_FAILURE_CODE_MODULO_BY_ZERO_V1 = 3,
  GTI_FAILURE_CODE_NEGATIVE_SHIFT_COUNT_V1 = 4,
  GTI_FAILURE_CODE_SHIFT_COUNT_OUT_OF_RANGE_V1 = 5,
  GTI_FAILURE_CODE_NUMERIC_CONVERSION_OUT_OF_RANGE_V1 = 6,
  GTI_FAILURE_CODE_INDEX_OUT_OF_BOUNDS_V1 = 7,
  GTI_FAILURE_CODE_EMPTY_OWNER_ACCESS_V1 = 8,
  GTI_FAILURE_CODE_INVALID_EXPECTED_ACCESS_V1 = 9,
  GTI_FAILURE_CODE_INVALID_STORAGE_STATE_V1 = 10,
  GTI_FAILURE_CODE_ALLOCATION_FAILURE_V1 = 11,
  GTI_FAILURE_CODE_INFALLIBLE_HOST_OPERATION_FAILED_V1 = 12,
  GTI_FAILURE_CODE_HOSTED_RUNTIME_CONTRACT_FAILURE_V1 = 13,
};

typedef uint16_t gti_failure_detail_v1;

enum {
  GTI_FAILURE_DETAIL_NONE_V1 = 0,
  GTI_FAILURE_DETAIL_ADDITION_V1 = 1,
  GTI_FAILURE_DETAIL_SUBTRACTION_V1 = 2,
  GTI_FAILURE_DETAIL_MULTIPLICATION_V1 = 3,
  GTI_FAILURE_DETAIL_DIVISION_V1 = 4,
  GTI_FAILURE_DETAIL_NEGATION_V1 = 5,
  GTI_FAILURE_DETAIL_INTEGER_DIVISION_V1 = 6,
  GTI_FAILURE_DETAIL_INTEGER_MODULO_V1 = 7,
  GTI_FAILURE_DETAIL_LEFT_SHIFT_V1 = 8,
  GTI_FAILURE_DETAIL_RIGHT_SHIFT_V1 = 9,
  GTI_FAILURE_DETAIL_NUMERIC_CAST_V1 = 10,
  GTI_FAILURE_DETAIL_HOSTED_ARGUMENT_COUNT_V1 = 11,
  GTI_FAILURE_DETAIL_FIXED_ARRAY_V1 = 12,
  GTI_FAILURE_DETAIL_STRING_VIEW_V1 = 13,
  GTI_FAILURE_DETAIL_VECTOR_V1 = 14,
  GTI_FAILURE_DETAIL_STRING_V1 = 15,
  GTI_FAILURE_DETAIL_PRIVATE_STORAGE_V1 = 16,
  GTI_FAILURE_DETAIL_DEREFERENCE_V1 = 17,
  GTI_FAILURE_DETAIL_MEMBER_ACCESS_V1 = 18,
  GTI_FAILURE_DETAIL_VALUE_ON_ERROR_V1 = 19,
  GTI_FAILURE_DETAIL_ERROR_ON_VALUE_V1 = 20,
  GTI_FAILURE_DETAIL_DUPLICATE_CONSTRUCTION_V1 = 21,
  GTI_FAILURE_DETAIL_UNINITIALIZED_ACCESS_V1 = 22,
  GTI_FAILURE_DETAIL_RELOCATION_CAPACITY_V1 = 23,
  GTI_FAILURE_DETAIL_INVALID_RELOCATION_SOURCE_V1 = 24,
  GTI_FAILURE_DETAIL_OCCUPIED_RELOCATION_DESTINATION_V1 = 25,
  GTI_FAILURE_DETAIL_UNIQUE_OWNER_V1 = 26,
  GTI_FAILURE_DETAIL_ELEMENT_CONSTRUCTION_V1 = 27,
  GTI_FAILURE_DETAIL_HOSTED_ARGUMENTS_V1 = 28,
  GTI_FAILURE_DETAIL_STDOUT_WRITE_V1 = 29,
  GTI_FAILURE_DETAIL_AUTOMATIC_JOIN_V1 = 30,
  GTI_FAILURE_DETAIL_NEGATIVE_ARGUMENT_COUNT_V1 = 31,
  GTI_FAILURE_DETAIL_RECURSIVE_THREAD_LOCAL_INITIALIZATION_V1 = 32,
};

typedef struct gti_failure_record_v1 {
  uint32_t abi_version;
  gti_failure_code_v1 code;
  gti_failure_detail_v1 detail;
  uint32_t site_index;
  uint32_t reserved;
  uint8_t artifact_identity[32];
} gti_failure_record_v1;

typedef struct gti_failure_outcome_descriptor_v1 {
  gti_failure_code_v1 code;
  gti_failure_detail_v1 detail;
  uint32_t reserved;
} gti_failure_outcome_descriptor_v1;

typedef struct gti_failure_site_descriptor_v1 {
  gti_c_string_view logical_source;
  uint64_t line;
  uint64_t start;
  uint64_t end;
  const gti_failure_outcome_descriptor_v1 *outcomes;
  uint32_t outcome_count;
  uint32_t reserved;
} gti_failure_site_descriptor_v1;

typedef struct gti_failure_artifact_descriptor_v1 {
  uint32_t abi_version;
  uint32_t reserved;
  uint8_t artifact_identity[32];
  const gti_failure_site_descriptor_v1 *sites;
  uint32_t site_count;
  uint32_t sites_reserved;
  const uint8_t *canonical_descriptor;
  uint64_t canonical_descriptor_size;
} gti_failure_artifact_descriptor_v1;

typedef struct gti_failure_emergency_v1 {
  uint32_t abi_version;
  uint32_t reserved;
  gti_failure_record_v1 primary;
  gti_failure_record_v1 secondary;
} gti_failure_emergency_v1;

typedef void (*gti_failure_observer_v1)(const gti_failure_record_v1 *record,
                                        void *context);

typedef uint32_t gti_failure_report_result_v1;

enum {
  GTI_FAILURE_REPORT_OK_V1 = 0,
  GTI_FAILURE_REPORT_INVALID_ARGUMENT_V1 = 1,
  GTI_FAILURE_REPORT_IO_ERROR_V1 = 2,
};

#if defined(__cplusplus)
#define GTI_FAILURE_NORETURN [[noreturn]]
#elif defined(_MSC_VER)
#define GTI_FAILURE_NORETURN __declspec(noreturn)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define GTI_FAILURE_NORETURN _Noreturn
#else
#define GTI_FAILURE_NORETURN
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Descriptors and every pointer they contain are immutable and pinned for the
 * complete call. A nonzero record uses a one-based site_index and must match
 * the artifact identity and that site's allowed outcome set. The only
 * descriptor-free record has an all-zero identity and site_index zero.
 * Reporting performs no allocation and writes exactly one LF-terminated line.
 */
gti_failure_report_result_v1 gti_rt_failure_write_report_v1(
    const gti_failure_record_v1 *record,
    const gti_failure_artifact_descriptor_v1 *artifact);

gti_failure_report_result_v1 gti_rt_failure_write_cleanup_report_v1(
    const gti_failure_emergency_v1 *failure,
    const gti_failure_artifact_descriptor_v1 *primary_artifact,
    const gti_failure_artifact_descriptor_v1 *secondary_artifact);

/*
 * Generated containment code calls this only after GTI cleanup completes.
 * The optional observer is invoked once over an immutable copy before the
 * report. Observer re-entry, mutation, or an escaping C++ exception reports
 * the protected original record. The process terminates immediately with
 * GTI_FAILURE_EXIT_STATUS; report-I/O failure never changes that status.
 */
GTI_FAILURE_NORETURN void
gti_rt_failure_terminate_v1(const gti_failure_record_v1 *record,
                            const gti_failure_artifact_descriptor_v1 *artifact,
                            gti_failure_observer_v1 observer,
                            void *observer_context);

GTI_FAILURE_NORETURN void gti_rt_failure_terminate_cleanup_v1(
    const gti_failure_emergency_v1 *failure,
    const gti_failure_artifact_descriptor_v1 *primary_artifact,
    const gti_failure_artifact_descriptor_v1 *secondary_artifact);

#ifdef __cplusplus
}
#endif

#undef GTI_FAILURE_NORETURN
