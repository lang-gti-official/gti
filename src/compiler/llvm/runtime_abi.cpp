#include "runtime_abi.h"

namespace lang::native_llvm {

llvm::Function *declareRuntimeService(Session &session,
                                      RuntimeService service) {
  auto &context = session.context();
  auto *ptr = llvm::PointerType::get(context, 0);
  auto *result = llvm::Type::getVoidTy(context);
  std::vector<llvm::Type *> parameters;
  std::string name;
  bool neverReturns = false;
  bool noUnwind = false;
  bool cold = false;
  switch (service) {
  case RuntimeService::AtomicNotifyRegistered:
    noUnwind = true;
    name = "gti_rt_atomic_notify_registered_v1";
    parameters = {ptr, llvm::Type::getInt32Ty(context),
                  llvm::Type::getInt32Ty(context)};
    break;
  case RuntimeService::QuarantineThread:
    noUnwind = true;
    name = "gti_rt_thread_quarantine_v1";
    parameters = {ptr};
    break;
  case RuntimeService::ProgramStorageRetirementReady:
    noUnwind = true;
    name = "gti_rt_thread_program_storage_retirement_ready_v1";
    result = llvm::Type::getInt32Ty(context);
    break;
  case RuntimeService::EnterThreadFailureSink:
    noUnwind = true;
    name = "gti_rt_thread_failure_sink_enter_v1";
    parameters = {ptr, ptr, ptr};
    break;
  case RuntimeService::LeaveThreadFailureSink:
    noUnwind = true;
    name = "gti_rt_thread_failure_sink_leave_v1";
    parameters = {ptr};
    break;
  case RuntimeService::PublishThreadFailure:
    noUnwind = true;
    name = "gti_rt_thread_failure_publish_v1";
    parameters = {ptr};
    break;
  case RuntimeService::ThreadCleanupFailureKind:
    noUnwind = true;
    name = "gti_rt_thread_cleanup_failure_kind_v1";
    result = llvm::Type::getInt32Ty(context);
    break;
  case RuntimeService::CopyThreadCleanupPrimary:
    noUnwind = true;
    name = "gti_rt_thread_cleanup_primary_copy_v2";
    result = llvm::Type::getInt32Ty(context);
    parameters = {ptr, ptr};
    break;
  case RuntimeService::BeginThreadLocals:
    name = "gti_rt_thread_locals_begin_v1";
    break;
  case RuntimeService::DrainThreadLocals:
    name = "gti_rt_thread_locals_drain_v2";
    parameters = {ptr, ptr};
    break;
  case RuntimeService::ThreadLocalsAttached:
  case RuntimeService::ThreadLocalsClosing:
    name = service == RuntimeService::ThreadLocalsAttached
               ? "gti_rt_thread_locals_attached_v1"
               : "gti_rt_thread_locals_closing_v1";
    result = llvm::Type::getInt32Ty(context);
    break;
  case RuntimeService::RegisterThreadLocal:
    name = "gti_rt_thread_locals_register_v1";
    parameters = {ptr};
    break;
  case RuntimeService::TerminateFailure:
    name = "gti_rt_failure_terminate_v2";
    parameters = {ptr, ptr, ptr, ptr};
    neverReturns = true;
    break;
  case RuntimeService::TerminateCleanupFailure:
    name = "gti_rt_failure_terminate_cleanup_v2";
    parameters = {ptr, ptr, ptr};
    neverReturns = true;
    break;
  case RuntimeService::AbortAllocation:
    name = "gti_rt_failure_abort_allocation_v2";
    parameters = {ptr, ptr};
    neverReturns = true;
    break;
  case RuntimeService::ContractTerminate:
    // The generated C++ representation ends these with std::terminate; the
    // runtime entry reaches the same host terminate handler.
    name = "gti_rt_contract_terminate_v1";
    neverReturns = true;
    noUnwind = true;
    cold = true;
    break;
  case RuntimeService::ValidateFailure:
    name = "gti_rt_failure_validate_v2";
    parameters = {ptr, ptr};
    result = llvm::Type::getInt32Ty(context);
    break;
  case RuntimeService::CapturePanic:
    name = "gti_rt_failure_capture_panic_bytes_v2";
    parameters = {ptr, ptr, llvm::Type::getInt64Ty(context)};
    result = llvm::Type::getInt32Ty(context);
    break;
  case RuntimeService::AtomicWait:
    noUnwind = true;
    name = "gti_rt_atomic_wait_v1";
    parameters = {ptr, llvm::Type::getInt64Ty(context),
                  llvm::Type::getInt32Ty(context),
                  llvm::Type::getInt32Ty(context)};
    break;
  case RuntimeService::BeginCThreadRuntime:
  case RuntimeService::EndCThreadRuntime:
    noUnwind = true;
    name = service == RuntimeService::BeginCThreadRuntime
               ? "gti_rt_cthread_begin_v1"
               : "gti_rt_cthread_end_v1";
    break;
  case RuntimeService::AtomicNotify:
    noUnwind = true;
    name = "gti_rt_atomic_notify_v1";
    parameters = {ptr, llvm::Type::getInt32Ty(context),
                  llvm::Type::getInt32Ty(context)};
    break;
  case RuntimeService::MutexInitialize:
    noUnwind = true;
    name = "gti_rt_mutex_initialize_v1";
    parameters = {ptr};
    break;
  case RuntimeService::MutexDestroy:
    noUnwind = true;
    name = "gti_rt_mutex_destroy_v1";
    parameters = {ptr};
    break;
  case RuntimeService::MutexLock:
    noUnwind = true;
    name = "gti_rt_mutex_lock_v1";
    parameters = {ptr};
    result = llvm::Type::getInt32Ty(context);
    break;
  case RuntimeService::MutexUnlock:
    noUnwind = true;
    name = "gti_rt_mutex_unlock_v1";
    parameters = {ptr};
    break;
  case RuntimeService::EnterFailureCleanup:
    cold = true;
    name = "gti_rt_failure_cleanup_enter_v1";
    parameters = {ptr, ptr};
    result = ptr;
    break;
  case RuntimeService::LeaveFailureCleanup:
    cold = true;
    name = "gti_rt_failure_cleanup_leave_v1";
    parameters = {ptr};
    break;
  case RuntimeService::CheckFailureCleanup:
    cold = true;
    name = "gti_rt_failure_cleanup_check_v1";
    parameters = {ptr, ptr};
    break;
  }
  if (name.empty())
    throw std::logic_error("unknown LLVM runtime service identity");
  auto *signature = llvm::FunctionType::get(result, parameters, false);
  if (auto *existing = session.module().getNamedValue(name)) {
    auto *function = llvm::dyn_cast<llvm::Function>(existing);
    if (!function || function->getFunctionType() != signature)
      throw std::logic_error("conflicting LLVM runtime service signature: " +
                             name);
    if (noUnwind)
      function->addFnAttr(llvm::Attribute::NoUnwind);
    return function;
  }
  auto *function = llvm::Function::Create(
      signature, llvm::GlobalValue::ExternalLinkage, name, session.module());
  if (neverReturns)
    function->addFnAttr(llvm::Attribute::NoReturn);
  if (noUnwind)
    function->addFnAttr(llvm::Attribute::NoUnwind);
  // These services run only while propagating a defined failure. This is a
  // cost/layout hint, not permission to remove cleanup or assume no unwind.
  if (cold)
    function->addFnAttr(llvm::Attribute::Cold);
  return function;
}

} // namespace lang::native_llvm
