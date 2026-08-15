#include "gti/failure.h"
#include "gti/runtime.h"
#include "gti/runtime_failure.h"

#include <cstdint>
#include <type_traits>

static_assert(
    static_cast<std::uint16_t>(lang::DefinedFailureCode::IntegerOverflow) ==
    GTI_FAILURE_CODE_INTEGER_OVERFLOW_V1);
static_assert(
    static_cast<std::uint16_t>(
        lang::DefinedFailureDetail::RecursiveThreadLocalInitialization) ==
    GTI_FAILURE_DETAIL_RECURSIVE_THREAD_LOCAL_INITIALIZATION_V1);
static_assert(sizeof(gti_failure_record_v1) == 48);
static_assert(std::is_trivially_copyable_v<gti_failure_record_v1>);

int main() {
  const lang::DefinedFailureOutcome compilerOutcome{
      .code = lang::DefinedFailureCode::IntegerOverflow,
      .detail = lang::DefinedFailureDetail::Addition,
  };
  if (!lang::validDefinedFailureOutcome(compilerOutcome)) {
    return 1;
  }
  return gti_rt_failure_write_report_v1(nullptr, nullptr) ==
                 GTI_FAILURE_REPORT_INVALID_ARGUMENT_V1
             ? 0
             : 2;
}
