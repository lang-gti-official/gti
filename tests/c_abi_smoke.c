#include "gti/runtime.h"
#include "gti/runtime_thread.h"

#include <stddef.h>
#include <stdint.h>

_Static_assert(_Generic(&gti_rt_once_invoke_v1,
                   void (*)(gti_once_initializer_v1): 1,
                   default: 0),
               "registered once invocation ABI");
_Static_assert(_Generic(&gti_rt_once_enter_v1,
                   void (*)(gti_once_initializer_v1): 1,
                   default: 0),
               "exact once entry ABI");
_Static_assert(_Generic(&gti_rt_once_leave_v1,
                   void (*)(gti_once_initializer_v1): 1,
                   default: 0),
               "exact once leave ABI");

_Static_assert(_Generic(&gti_rt_cthread_current_v1,
                   uint64_t (*)(void): 1,
                   default: 0),
               "C-thread identity ABI");

_Static_assert(_Generic(&gti_rt_cthread_sleep_v1,
                   int32_t (*)(int64_t, int64_t, int64_t *, int64_t *): 1,
                   default: 0),
               "C-thread sleep scalar ABI");

_Static_assert(_Generic(&gti_rt_cthread_yield_v1,
                   void (*)(void): 1,
                   default: 0),
               "the C-style yield entry must retain its void C prototype");
_Static_assert(_Generic(&gti_rt_mtx_init_v1,
                   int32_t (*)(uint64_t *, int32_t): 1,
                   default: 0),
               "manual mutex init ABI");
_Static_assert(_Generic(&gti_rt_cnd_init_v1,
                   int32_t (*)(uint64_t *): 1,
                   default: 0),
               "condition init ABI");
_Static_assert(_Generic(&gti_rt_cnd_destroy_v1,
                   void (*)(uint64_t): 1,
                   default: 0),
               "condition destroy ABI");
_Static_assert(_Generic(&gti_rt_cnd_signal_v1,
                   int32_t (*)(uint64_t): 1,
                   default: 0),
               "condition signal ABI");
_Static_assert(_Generic(&gti_rt_cnd_broadcast_v1,
                   int32_t (*)(uint64_t): 1,
                   default: 0),
               "condition broadcast ABI");
_Static_assert(_Generic(&gti_rt_cnd_wait_v1,
                   int32_t (*)(uint64_t, uint64_t): 1,
                   default: 0),
               "condition wait ABI");
_Static_assert(_Generic(&gti_rt_cnd_timedwait_v1,
                   int32_t (*)(uint64_t, uint64_t, int64_t, int64_t): 1,
                   default: 0),
               "condition deadline ABI");
_Static_assert(_Generic(&gti_rt_mtx_destroy_v1,
                   void (*)(uint64_t): 1,
                   default: 0),
               "manual mutex destruction ABI");
_Static_assert(_Generic(&gti_rt_mtx_lock_v1,
                   int32_t (*)(uint64_t): 1,
                   default: 0),
               "manual mutex acquire ABI");
_Static_assert(_Generic(&gti_rt_mtx_trylock_v1,
                   int32_t (*)(uint64_t): 1,
                   default: 0),
               "manual mutex try-acquire ABI");
_Static_assert(_Generic(&gti_rt_mtx_timedlock_v1,
                   int32_t (*)(uint64_t, int64_t, int64_t): 1,
                   default: 0),
               "manual mutex deadline ABI");
_Static_assert(_Generic(&gti_rt_mtx_unlock_v1,
                   int32_t (*)(uint64_t): 1,
                   default: 0),
               "manual mutex release ABI");
_Static_assert(_Generic(&gti_rt_cthread_create_v1,
                   int32_t (*)(uint64_t *, gti_cthread_entry_v1, void *): 1,
                   default: 0),
               "C-thread create ABI");
_Static_assert(_Generic(&gti_rt_cthread_join_v1,
                   int32_t (*)(uint64_t, int32_t *): 1,
                   default: 0),
               "C-thread join ABI");
_Static_assert(_Generic(&gti_rt_cthread_enter_v1,
                   void (*)(gti_cthread_entry_v1): 1,
                   default: 0),
               "C-thread exact-entry ABI");

#define GTI_CHECK_UNARY_MATH(name)                                             \
  _Static_assert(                                                              \
      _Generic(&gti_rt_math_##name##_f32_v1, float (*)(float): 1, default: 0), \
      "binary32 math must retain its scalar C prototype")
GTI_CHECK_UNARY_MATH(floor);
GTI_CHECK_UNARY_MATH(ceil);
GTI_CHECK_UNARY_MATH(round);
GTI_CHECK_UNARY_MATH(trunc);
GTI_CHECK_UNARY_MATH(sqrt);
GTI_CHECK_UNARY_MATH(cbrt);
GTI_CHECK_UNARY_MATH(exp);
GTI_CHECK_UNARY_MATH(log);
GTI_CHECK_UNARY_MATH(log10);
GTI_CHECK_UNARY_MATH(sin);
GTI_CHECK_UNARY_MATH(cos);
GTI_CHECK_UNARY_MATH(tan);
GTI_CHECK_UNARY_MATH(asin);
GTI_CHECK_UNARY_MATH(acos);
GTI_CHECK_UNARY_MATH(atan);
#undef GTI_CHECK_UNARY_MATH
_Static_assert(_Generic(&gti_rt_math_pow_f32_v1,
                       float (*)(float, float): 1,
                       default: 0) &&
                   _Generic(&gti_rt_math_fmod_f32_v1,
                       float (*)(float, float): 1,
                       default: 0) &&
                   _Generic(&gti_rt_math_atan2_f32_v1,
                       float (*)(float, float): 1,
                       default: 0),
               "binary32 two-argument math must retain its scalar C prototype");

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
_Static_assert(GTI_FILE_OPEN_READ_V1 == 0 && GTI_FILE_OPEN_READ_APPEND_V1 == 5,
               "the v1 file-open mode ordinals must remain stable");
_Static_assert(GTI_FILE_SEEK_BEGIN_V1 == 0 && GTI_FILE_SEEK_END_V1 == 2,
               "the v1 file-seek origin ordinals must remain stable");
_Static_assert(
    _Generic(&gti_rt_chrono_steady_now_v1,
            gti_chrono_status_v1 (*)(int64_t *): 1,
            default: 0) &&
        _Generic(&gti_rt_chrono_system_now_v1,
            gti_chrono_status_v1 (*)(int64_t *): 1,
            default: 0),
    "the v1 clock entries must retain their scalar output prototypes");
_Static_assert(GTI_CHRONO_OK_V1 == 0 && GTI_CHRONO_OUT_OF_RANGE_V1 == 2,
               "the v1 clock status ordinals must remain stable");
_Static_assert(
    _Generic(&gti_rt_process_run_v1,
        gti_process_error_v1 (*)(gti_c_string_view, int32_t *, int32_t *,
                                 int32_t *): 1,
        default: 0),
    "the v1 process entry must retain counted argv and scalar outputs");
_Static_assert(GTI_PROCESS_OK_V1 == 0 && GTI_PROCESS_WAIT_FAILED_V1 == 5 &&
                   GTI_PROCESS_TERMINATION_EXITED_V1 == 0 &&
                   GTI_PROCESS_TERMINATION_SIGNALED_V1 == 1,
               "the v1 process status ordinals must remain stable");
_Static_assert(_Generic(&gti_rt_write_stdout_bytes_v1,
                   int64_t (*)(gti_c_string_view): 1,
                   default: 0),
               "the v1 stdout entry must consume the counted-text record");
_Static_assert(_Generic(&gti_rt_write_stderr_bytes_v1,
                   int64_t (*)(gti_c_string_view): 1,
                   default: 0),
               "the v1 stderr entry must consume the counted-text record");
_Static_assert(
    _Generic(&gti_rt_flush_stdout_v1, int32_t (*)(void): 1, default: 0) &&
        _Generic(&gti_rt_flush_stderr_v1, int32_t (*)(void): 1, default: 0),
    "the v1 standard-stream flush entries must remain exact");
_Static_assert(_Generic(&gti_rt_read_stdin_bytes_v1,
                   int64_t (*)(uint8_t *, uint64_t): 1,
                   default: 0),
               "the v1 stdin chunk entry must retain its C prototype");
_Static_assert(_Generic(&gti_rt_open_file_v1,
                   int64_t (*)(gti_c_string_view, gti_file_open_mode_v1): 1,
                   default: 0),
               "the v1 file-open entry must retain its C prototype");
_Static_assert(_Generic(&gti_rt_read_file_v1,
                   int64_t (*)(int64_t, uint8_t *, uint64_t): 1,
                   default: 0),
               "the v1 file-read entry must retain its C prototype");
_Static_assert(_Generic(&gti_rt_write_file_v1,
                   int64_t (*)(int64_t, gti_c_string_view): 1,
                   default: 0),
               "the v1 file-write entry must retain its C prototype");
_Static_assert(_Generic(&gti_rt_seek_file_v1,
                   int64_t (*)(int64_t, int64_t, gti_file_seek_origin_v1): 1,
                   default: 0),
               "the v1 file-seek entry must retain its C prototype");

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

_Static_assert(GTI_THREAD_ABI_VERSION_V1 == UINT32_C(1),
               "the initial managed-thread ABI version must remain one");
_Static_assert(GTI_THREAD_SPAWN_OK_V1 == 0 &&
                   GTI_THREAD_SPAWN_OPERATION_NOT_PERMITTED_V1 == 2 &&
                   GTI_THREAD_JOIN_OK_V1 == 0 &&
                   GTI_THREAD_JOIN_WORKER_FAILURE_V1 == 3 &&
                   GTI_THREAD_WORKER_FAILURE_NONE_V1 == 0 &&
                   GTI_THREAD_WORKER_FAILURE_CLEANUP_V1 == 2,
               "managed-thread status ordinals must remain stable");
_Static_assert(sizeof(gti_thread_spawn_result_v1) == 16 &&
                   offsetof(gti_thread_spawn_result_v1, state) == 8,
               "the v1 managed-thread spawn result layout must remain fixed");
_Static_assert(sizeof(gti_thread_join_result_v1) == 184 &&
                   offsetof(gti_thread_join_result_v1, failure_kind) == 8 &&
                   offsetof(gti_thread_join_result_v1, failure) == 16 &&
                   offsetof(gti_thread_join_result_v1, cleanup_failure) == 64 &&
                   offsetof(gti_thread_join_result_v1, primary_artifact) ==
                       168 &&
                   offsetof(gti_thread_join_result_v1, secondary_artifact) ==
                       176,
               "the v1 managed-thread join result layout must remain fixed");
_Static_assert(
    sizeof(gti_thread_value_join_result_v1) == 208 &&
        offsetof(gti_thread_value_join_result_v1, join) == 0 &&
        offsetof(gti_thread_value_join_result_v1, result_context) == 184 &&
        offsetof(gti_thread_value_join_result_v1, take_result) == 192 &&
        offsetof(gti_thread_value_join_result_v1, abandon_result) == 200,
    "the v1 managed-thread value join result layout must remain fixed");
_Static_assert(
    _Generic(&gti_rt_thread_spawn_v1,
        gti_thread_spawn_result_v1 (*)(gti_thread_task_entry_v1, void *,
                                       gti_thread_task_destroy_v1): 1,
        default: 0),
    "the managed-thread spawn entry must retain its C prototype");
_Static_assert(
    _Generic(&gti_rt_thread_spawn_value_v1,
        gti_thread_spawn_result_v1 (*)(gti_thread_task_entry_v1, void *,
                                       gti_thread_task_complete_v1,
                                       gti_thread_task_abandon_v1,
                                       gti_thread_result_take_v1,
                                       gti_thread_result_abandon_v1): 1,
        default: 0),
    "the managed-thread value spawn entry must retain its C prototype");
_Static_assert(_Generic(&gti_rt_thread_join_v1,
                   gti_thread_join_result_v1 (*)(gti_thread_state_v1 **): 1,
                   default: 0),
               "the managed-thread join entry must retain its C prototype");
_Static_assert(
    _Generic(&gti_rt_thread_join_value_v1,
        gti_thread_value_join_result_v1 (*)(gti_thread_state_v1 **): 1,
        default: 0),
    "the managed-thread value join entry must retain its C prototype");

int main(void) {
  static const char text[] = "gti";
  const gti_c_string_view view = {.data = text, .length = 3};
  return view.data == text && view.length == UINT64_C(3) ? 0 : 1;
}
