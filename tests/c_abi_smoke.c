#include "gti/runtime.h"

#include <stddef.h>
#include <stdint.h>

_Static_assert(sizeof(((gti_c_string_view *)0)->length) == sizeof(uint64_t),
               "the GTI C ABI string-view length must remain uint64_t");
_Static_assert(
    _Generic(&gti_rt_write_stdout,
        int32_t (*)(gti_c_string_view): 1,
        default: 0),
    "the stdout runtime entry must consume the public counted-text record");
_Static_assert(_Generic(&gti_rt_write_stdout_byte,
                   int32_t (*)(uint8_t): 1,
                   default: 0),
               "the stdout-byte runtime entry must retain its C prototype");
_Static_assert(
    _Generic(&gti_rt_open_file_read,
        int64_t (*)(gti_c_string_view): 1,
        default: 0),
    "the file-open runtime entry must consume the public counted-text record");
_Static_assert(_Generic(&gti_rt_read_stdin_byte,
                   int32_t (*)(void): 1,
                   default: 0),
               "the stdin runtime entry must retain its C prototype");
_Static_assert(_Generic(&gti_rt_read_file_byte,
                   int32_t (*)(int64_t): 1,
                   default: 0),
               "the file-read runtime entry must retain its C prototype");
_Static_assert(_Generic(&gti_rt_close_file,
                   int32_t (*)(int64_t): 1,
                   default: 0),
               "the file-close runtime entry must retain its C prototype");

_Static_assert(UINTPTR_MAX == UINT64_MAX,
               "the shipped GTI runtime ABI requires a 64-bit pointer");
_Static_assert(GTI_FAILURE_ABI_VERSION_V1 == UINT32_C(1),
               "the initial failure ABI version must remain one");
_Static_assert(GTI_FAILURE_EXIT_STATUS == 70,
               "defined failure must retain status 70");
_Static_assert(GTI_FAILURE_CODE_INTEGER_OVERFLOW_V1 == 1 &&
                   GTI_FAILURE_CODE_HOSTED_RUNTIME_CONTRACT_FAILURE_V1 == 13,
               "failure code ordinals must remain stable");
_Static_assert(
    GTI_FAILURE_DETAIL_ADDITION_V1 == 1 &&
        GTI_FAILURE_DETAIL_RECURSIVE_THREAD_LOCAL_INITIALIZATION_V1 == 32,
    "failure detail ordinals must remain stable");

_Static_assert(sizeof(gti_failure_record_v1) == 48,
               "the v1 failure record ABI must remain 48 bytes");
_Static_assert(_Alignof(gti_failure_record_v1) == 4,
               "the v1 failure record alignment must remain four");
_Static_assert(offsetof(gti_failure_record_v1, abi_version) == 0 &&
                   offsetof(gti_failure_record_v1, code) == 4 &&
                   offsetof(gti_failure_record_v1, detail) == 6 &&
                   offsetof(gti_failure_record_v1, site_index) == 8 &&
                   offsetof(gti_failure_record_v1, reserved) == 12 &&
                   offsetof(gti_failure_record_v1, artifact_identity) == 16,
               "the v1 failure record field offsets must remain fixed");

_Static_assert(sizeof(gti_failure_outcome_descriptor_v1) == 8,
               "the v1 failure outcome descriptor must remain eight bytes");
_Static_assert(offsetof(gti_failure_outcome_descriptor_v1, code) == 0 &&
                   offsetof(gti_failure_outcome_descriptor_v1, detail) == 2 &&
                   offsetof(gti_failure_outcome_descriptor_v1, reserved) == 4,
               "the v1 failure outcome offsets must remain fixed");

_Static_assert(sizeof(gti_failure_site_descriptor_v1) == 56,
               "the v1 failure site descriptor must remain 56 bytes");
_Static_assert(offsetof(gti_failure_site_descriptor_v1, logical_source) == 0 &&
                   offsetof(gti_failure_site_descriptor_v1, line) == 16 &&
                   offsetof(gti_failure_site_descriptor_v1, start) == 24 &&
                   offsetof(gti_failure_site_descriptor_v1, end) == 32 &&
                   offsetof(gti_failure_site_descriptor_v1, outcomes) == 40 &&
                   offsetof(gti_failure_site_descriptor_v1, outcome_count) ==
                       48 &&
                   offsetof(gti_failure_site_descriptor_v1, reserved) == 52,
               "the v1 failure site offsets must remain fixed");

_Static_assert(sizeof(gti_failure_artifact_descriptor_v1) == 72,
               "the v1 artifact descriptor must remain 72 bytes");
_Static_assert(
    offsetof(gti_failure_artifact_descriptor_v1, abi_version) == 0 &&
        offsetof(gti_failure_artifact_descriptor_v1, reserved) == 4 &&
        offsetof(gti_failure_artifact_descriptor_v1, artifact_identity) == 8 &&
        offsetof(gti_failure_artifact_descriptor_v1, sites) == 40 &&
        offsetof(gti_failure_artifact_descriptor_v1, site_count) == 48 &&
        offsetof(gti_failure_artifact_descriptor_v1, sites_reserved) == 52 &&
        offsetof(gti_failure_artifact_descriptor_v1, canonical_descriptor) ==
            56 &&
        offsetof(gti_failure_artifact_descriptor_v1,
                 canonical_descriptor_size) == 64,
    "the v1 artifact descriptor offsets must remain fixed");

_Static_assert(sizeof(gti_failure_emergency_v1) == 104,
               "the v1 emergency envelope must remain 104 bytes");
_Static_assert(offsetof(gti_failure_emergency_v1, primary) == 8 &&
                   offsetof(gti_failure_emergency_v1, secondary) == 56,
               "the v1 emergency envelope offsets must remain fixed");

_Static_assert(_Generic(&gti_rt_failure_write_report_v1,
                   gti_failure_report_result_v1 (*)(
                       const gti_failure_record_v1 *,
                       const gti_failure_artifact_descriptor_v1 *): 1,
                   default: 0),
               "the ordinary failure report entry must retain its C prototype");
_Static_assert(_Generic(&gti_rt_failure_write_cleanup_report_v1,
                   gti_failure_report_result_v1 (*)(
                       const gti_failure_emergency_v1 *,
                       const gti_failure_artifact_descriptor_v1 *,
                       const gti_failure_artifact_descriptor_v1 *): 1,
                   default: 0),
               "the cleanup failure report entry must retain its C prototype");
_Static_assert(_Generic(&gti_rt_failure_terminate_v1,
                   void (*)(const gti_failure_record_v1 *,
                            const gti_failure_artifact_descriptor_v1 *,
                            gti_failure_observer_v1, void *): 1,
                   default: 0),
               "the hosted failure terminal entry must retain its C prototype");
_Static_assert(
    _Generic(&gti_rt_failure_terminate_cleanup_v1,
        void (*)(const gti_failure_emergency_v1 *,
                 const gti_failure_artifact_descriptor_v1 *,
                 const gti_failure_artifact_descriptor_v1 *): 1,
        default: 0),
    "the cleanup failure terminal entry must retain its C prototype");

int main(void) {
  static const char text[] = "gti";
  const gti_c_string_view view = {.data = text, .length = 3};
  return view.data == text && view.length == UINT64_C(3) ? 0 : 1;
}
