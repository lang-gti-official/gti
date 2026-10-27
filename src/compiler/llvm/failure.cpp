#include "failure.h"
#include "runtime_abi.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Intrinsics.h>

namespace lang::native_llvm {
namespace {
llvm::Constant *global(Session &session, llvm::Constant *value,
                       llvm::StringRef name) {
  return new llvm::GlobalVariable(session.module(), value->getType(), true,
                                  llvm::GlobalValue::PrivateLinkage, value,
                                  name);
}
} // namespace

Failures::Failures(Session &session) : session(session) {
  auto &c = session.context();
  auto *i8 = llvm::Type::getInt8Ty(c);
  auto *i16 = llvm::Type::getInt16Ty(c);
  auto *i32 = llvm::Type::getInt32Ty(c);
  auto *i64 = llvm::Type::getInt64Ty(c);
  auto u32 = [&](uint64_t x) { return llvm::ConstantInt::get(i32, x); };
  auto u64 = [&](uint64_t x) { return llvm::ConstantInt::get(i64, x); };
  auto bytes = [&](std::string_view text) {
    return global(session, llvm::ConstantDataArray::getString(c, text, false),
                  "gti.failure.bytes");
  };
  const auto &metadata = session.program().mir().failureMetadata();
  std::vector<llvm::Constant *> sites;
  for (const auto &site : metadata.sites()) {
    std::vector<llvm::Constant *> outcomes;
    for (auto outcome : site.outcomes)
      outcomes.push_back(llvm::ConstantStruct::get(
          session.abi().failureOutcome(),
          {llvm::ConstantInt::get(i16, static_cast<unsigned>(outcome.code)),
           llvm::ConstantInt::get(i16, static_cast<unsigned>(outcome.detail)),
           u32(0)}));
    auto *outcomeArray = llvm::ConstantArray::get(
        llvm::ArrayType::get(session.abi().failureOutcome(), outcomes.size()),
        outcomes);
    auto *text = llvm::ConstantStruct::get(
        session.abi().textView(),
        {bytes(site.logicalSource), u64(site.logicalSource.size())});
    sites.push_back(llvm::ConstantStruct::get(
        session.abi().failureSite(),
        {text, u64(site.line), u64(site.start), u64(site.end),
         global(session, outcomeArray, "gti.failure.outcomes"),
         u32(outcomes.size()), u32(0)}));
  }
  auto *siteArray = llvm::ConstantArray::get(
      llvm::ArrayType::get(session.abi().failureSite(), sites.size()), sites);
  const auto &identity = metadata.artifactIdentity().bytes;
  const auto &canonical = metadata.descriptorBytes();
  artifact_ =
      global(session,
             llvm::ConstantStruct::get(
                 session.abi().failureArtifact(),
                 {u32(1), u32(0), llvm::ConstantDataArray::get(c, identity),
                  global(session, siteArray, "gti.failure.sites"),
                  u32(sites.size()), u32(0),
                  global(session, llvm::ConstantDataArray::get(c, canonical),
                         "gti.failure.canonical"),
                  u64(canonical.size())}),
             "gti.failure.artifact");
  static_cast<void>(i8);
  containment_ = !session.program().mir().threadTaskAdapters().empty();
  if (containment_) {
    auto *pointer = session.layouts().pointer();
    incomingRecord_ = threadState("__gti_native_cleanup_record_v1", pointer);
    incomingArtifact_ =
        threadState("__gti_native_cleanup_artifact_v1", pointer);
    worker_ = threadState("__gti_native_managed_worker_v1", i8);
    abandon_ = threadState("__gti_native_worker_abandon_v1", i8);
  }
}

llvm::GlobalVariable *Failures::threadState(const char *name,
                                            llvm::Type *type) {
  auto *global = new llvm::GlobalVariable(
      session.module(), type, false, llvm::GlobalValue::InternalLinkage,
      llvm::Constant::getNullValue(type), name, nullptr,
      llvm::GlobalVariable::GeneralDynamicTLSModel);
  return global;
}

llvm::Value *Failures::slot(llvm::IRBuilder<> &ir,
                            llvm::GlobalVariable *global) {
  if (!containment_ || !global)
    throw std::logic_error(
        "LLVM worker containment state requires managed task adapters");
  return ir.CreateThreadLocalAddress(global);
}

llvm::StructType *Failures::failureView() {
  if (!failureView_) {
    auto *pointer = session.layouts().pointer();
    auto *i32 = llvm::Type::getInt32Ty(session.context());
    // gti_thread_failure_view_v1: abi, kind, ordinary, cleanup, artifacts.
    failureView_ = llvm::StructType::create(
        session.context(), {i32, i32, pointer, pointer, pointer, pointer},
        "gti.c.thread_failure_view.v1");
  }
  return failureView_;
}

llvm::Value *Failures::abandoning(llvm::IRBuilder<> &ir) {
  return ir.CreateICmpNE(ir.CreateLoad(ir.getInt8Ty(), slot(ir, abandon_)),
                         ir.getInt8(0));
}

void Failures::clearAbandoning(llvm::IRBuilder<> &ir) {
  ir.CreateStore(ir.getInt8(0), slot(ir, abandon_));
}

void Failures::markWorker(llvm::IRBuilder<> &ir) {
  ir.CreateStore(ir.getInt8(1), slot(ir, worker_));
}

llvm::Value *Failures::onWorker(llvm::IRBuilder<> &ir) {
  // Without a verified task adapter no native managed worker can exist.
  if (!containment_)
    return ir.getFalse();
  return ir.CreateICmpNE(ir.CreateLoad(ir.getInt8Ty(), slot(ir, worker_)),
                         ir.getInt8(0));
}

llvm::Value *Failures::suspendWorker(llvm::IRBuilder<> &ir) {
  if (!containment_)
    return nullptr;
  auto *address = slot(ir, worker_);
  auto *previous = ir.CreateLoad(ir.getInt8Ty(), address);
  ir.CreateStore(ir.getInt8(0), address);
  return previous;
}

void Failures::restoreWorker(llvm::IRBuilder<> &ir, llvm::Value *previous) {
  if (containment_ && previous)
    ir.CreateStore(previous, slot(ir, worker_));
}

void Failures::publishOrdinary(llvm::IRBuilder<> &ir, llvm::Value *channel) {
  auto *function = ir.GetInsertBlock()->getParent();
  llvm::IRBuilder<> entry(&function->getEntryBlock(),
                          function->getEntryBlock().begin());
  auto *view = entry.CreateAlloca(failureView());
  auto *type = failureView();
  auto *null = llvm::ConstantPointerNull::get(session.layouts().pointer());
  ir.CreateStore(ir.getInt32(1), ir.CreateStructGEP(type, view, 0));
  ir.CreateStore(ir.getInt32(1), ir.CreateStructGEP(type, view, 1));
  ir.CreateStore(record(ir, channel), ir.CreateStructGEP(type, view, 2));
  ir.CreateStore(null, ir.CreateStructGEP(type, view, 3));
  ir.CreateStore(origin(ir, channel), ir.CreateStructGEP(type, view, 4));
  ir.CreateStore(null, ir.CreateStructGEP(type, view, 5));
  ir.CreateCall(
      declareRuntimeService(session, RuntimeService::PublishThreadFailure),
      {view});
}

llvm::Value *Failures::record(llvm::IRBuilder<> &ir, llvm::Value *channel) {
  return ir.CreateStructGEP(session.abi().failureChannel(), channel, 0);
}
llvm::Value *Failures::origin(llvm::IRBuilder<> &ir, llvm::Value *channel) {
  return ir.CreateLoad(
      session.layouts().pointer(),
      ir.CreateStructGEP(session.abi().failureChannel(), channel, 1));
}

void Failures::copyChannel(llvm::IRBuilder<> &ir, llvm::Value *destination,
                           llvm::Value *source) {
  // Failure channels are plain ABI storage, not source-language owners. Keep
  // their inline message buffer out of aggregate SSA scalarization; memmove
  // also preserves the prior load-before-store behavior for aliased channels.
  auto *type = session.abi().failureChannel();
  const auto alignment = session.dataLayout().getABITypeAlign(type);
  ir.CreateMemMove(destination, alignment, source, alignment,
                   session.dataLayout().getTypeAllocSize(type).getFixedValue());
}

void Failures::originate(llvm::IRBuilder<> &ir, llvm::Value *channel,
                         const MirInstruction &instruction,
                         DefinedFailureOutcome outcome, llvm::Value *callerSite,
                         llvm::Value *callerArtifact, llvm::Value *message) {
  auto *type = session.abi().failureRecord();
  llvm::Value *value = llvm::Constant::getNullValue(type);
  value = ir.CreateInsertValue(value, ir.getInt32(2), 0);
  value = ir.CreateInsertValue(
      value, ir.getInt16(static_cast<unsigned>(outcome.code)), 1);
  value = ir.CreateInsertValue(
      value, ir.getInt16(static_cast<unsigned>(outcome.detail)), 2);
  FailureSiteId site = 0;
  for (std::size_t index = 0;
       index < instruction.definedFailure.localOrigins.size(); ++index)
    for (auto candidate :
         instruction.definedFailure.localOrigins[index].outcomes)
      if (candidate == outcome)
        site = instruction.localFailureSites.at(index);
  if (site == 0)
    throw std::logic_error("LLVM failure has no authorized source outcome");
  llvm::Value *origin = artifact_;
  llvm::Value *siteValue = ir.getInt32(site);
  if (instruction.tracksCaller) {
    siteValue = instruction.forwardsCallerLocation
                    ? callerSite
                    : ir.getInt32(instruction.callerLocationSite);
    origin = instruction.forwardsCallerLocation ? callerArtifact : artifact_;
    if (!siteValue || !origin)
      throw std::logic_error("LLVM tracked failure lost caller attribution");
  }
  value = ir.CreateInsertValue(value, siteValue, 3);
  auto *identityType = type->getElementType(5);
  auto *identity = ir.CreateLoad(
      identityType,
      ir.CreateStructGEP(session.abi().failureArtifact(), origin, 2));
  value = ir.CreateInsertValue(value, identity, 5);
  ir.CreateStore(value, record(ir, channel));
  ir.CreateStore(
      origin, ir.CreateStructGEP(session.abi().failureChannel(), channel, 1));
  if (message) {
    ir.CreateCall(declareRuntimeService(session, RuntimeService::CapturePanic),
                  {record(ir, channel), ir.CreateExtractValue(message, 0),
                   ir.CreateExtractValue(message, 1)});
  }
}

void Failures::terminate(llvm::IRBuilder<> &ir, llvm::Value *channel) {
  auto *null = llvm::ConstantPointerNull::get(session.layouts().pointer());
  ir.CreateCall(
      declareRuntimeService(session, RuntimeService::TerminateFailure),
      {record(ir, channel), origin(ir, channel), null, null});
  ir.CreateUnreachable();
}
llvm::Value *Failures::enterCleanup(llvm::IRBuilder<> &ir,
                                    llvm::Value *primary) {
  return enterCleanupRecord(ir, record(ir, primary), origin(ir, primary));
}
llvm::Value *Failures::enterCleanupRecord(llvm::IRBuilder<> &ir,
                                          llvm::Value *primary,
                                          llvm::Value *artifact) {
  if (!containment_)
    return ir.CreateCall(
        declareRuntimeService(session, RuntimeService::EnterFailureCleanup),
        {primary, artifact});
  // Same contract as the runtime context: the first scope borrows its record
  // and returns it as the token; nested scopes cannot replace it.
  auto *recordSlot = slot(ir, incomingRecord_);
  auto *current = ir.CreateLoad(session.layouts().pointer(), recordSlot);
  auto *first = ir.CreateIsNull(current);
  ir.CreateStore(ir.CreateSelect(first, primary, current), recordSlot);
  auto *artifactSlot = slot(ir, incomingArtifact_);
  ir.CreateStore(
      ir.CreateSelect(first, artifact,
                      ir.CreateLoad(session.layouts().pointer(), artifactSlot)),
      artifactSlot);
  return ir.CreateSelect(
      first, primary,
      llvm::ConstantPointerNull::get(session.layouts().pointer()));
}
void Failures::leaveCleanup(llvm::IRBuilder<> &ir, llvm::Value *token) {
  if (!token)
    return;
  if (!containment_) {
    ir.CreateCall(
        declareRuntimeService(session, RuntimeService::LeaveFailureCleanup),
        {token});
    return;
  }
  auto *function = ir.GetInsertBlock()->getParent();
  auto *leave =
      llvm::BasicBlock::Create(session.context(), "cleanup.leave", function);
  auto *done =
      llvm::BasicBlock::Create(session.context(), "cleanup.left", function);
  ir.CreateCondBr(ir.CreateIsNotNull(token), leave, done);
  ir.SetInsertPoint(leave);
  auto *recordSlot = slot(ir, incomingRecord_);
  auto *invalid = llvm::BasicBlock::Create(session.context(),
                                           "cleanup.leave.invalid", function);
  auto *valid = llvm::BasicBlock::Create(session.context(),
                                         "cleanup.leave.valid", function);
  ir.CreateCondBr(
      ir.CreateICmpEQ(ir.CreateLoad(session.layouts().pointer(), recordSlot),
                      token),
      valid, invalid);
  ir.SetInsertPoint(invalid);
  ir.CreateIntrinsic(llvm::Intrinsic::trap, llvm::ArrayRef<llvm::Type *>{}, {});
  ir.CreateUnreachable();
  ir.SetInsertPoint(valid);
  auto *null = llvm::ConstantPointerNull::get(session.layouts().pointer());
  ir.CreateStore(null, recordSlot);
  ir.CreateStore(null, slot(ir, incomingArtifact_));
  ir.CreateBr(done);
  ir.SetInsertPoint(done);
}
void Failures::checkIncomingCleanup(llvm::IRBuilder<> &ir,
                                    llvm::Value *secondary) {
  if (!containment_) {
    ir.CreateCall(
        declareRuntimeService(session, RuntimeService::CheckFailureCleanup),
        {record(ir, secondary), origin(ir, secondary)});
    return;
  }
  auto *function = ir.GetInsertBlock()->getParent();
  auto *inherited =
      llvm::BasicBlock::Create(session.context(), "cleanup.inherited", function);
  auto *done =
      llvm::BasicBlock::Create(session.context(), "cleanup.checked", function);
  auto *primary =
      ir.CreateLoad(session.layouts().pointer(), slot(ir, incomingRecord_));
  ir.CreateCondBr(ir.CreateIsNotNull(primary), inherited, done);
  ir.SetInsertPoint(inherited);
  ir.CreateCall(emergency(),
                {primary,
                 ir.CreateLoad(session.layouts().pointer(),
                               slot(ir, incomingArtifact_)),
                 record(ir, secondary), origin(ir, secondary)});
  ir.CreateBr(done);
  ir.SetInsertPoint(done);
}

llvm::Function *Failures::emergency() {
  if (emergency_)
    return emergency_;
  auto *pointer = session.layouts().pointer();
  auto *function = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(session.context()),
                              {pointer, pointer, pointer, pointer}, false),
      llvm::GlobalValue::InternalLinkage, "__gti_failure_worker_emergency_v1",
      session.module());
  function->addFnAttr(llvm::Attribute::Cold);
  function->addFnAttr(llvm::Attribute::NoInline);
  emergency_ = function;
  llvm::IRBuilder<> ir(
      llvm::BasicBlock::Create(session.context(), "entry", function));
  auto *primary = function->getArg(0);
  auto *primaryArtifact = function->getArg(1);
  auto *secondary = function->getArg(2);
  auto *secondaryArtifact = function->getArg(3);
  auto *type = session.abi().failureEmergency();
  auto *envelope = ir.CreateAlloca(type);
  ir.CreateStore(ir.getInt32(2), ir.CreateStructGEP(type, envelope, 0));
  ir.CreateStore(ir.getInt32(0), ir.CreateStructGEP(type, envelope, 1));
  auto *recordType = session.abi().failureRecord();
  const auto alignment = session.dataLayout().getABITypeAlign(recordType);
  const auto bytes =
      session.dataLayout().getTypeAllocSize(recordType).getFixedValue();
  ir.CreateMemMove(ir.CreateStructGEP(type, envelope, 2), alignment, primary,
                   alignment, bytes);
  ir.CreateMemMove(ir.CreateStructGEP(type, envelope, 3), alignment,
                   secondary, alignment, bytes);
  auto *publish =
      llvm::BasicBlock::Create(session.context(), "worker.publish", function);
  auto *terminal =
      llvm::BasicBlock::Create(session.context(), "terminal", function);
  ir.CreateCondBr(
      ir.CreateICmpNE(ir.CreateLoad(ir.getInt8Ty(), slot(ir, worker_)),
                      ir.getInt8(0)),
      publish, terminal);
  ir.SetInsertPoint(publish);
  // The worker sink copies the exact envelope synchronously. Every generated
  // frame between here and the task boundary is abandoned by status, so the
  // inherited context it borrowed from those frames is retired now.
  auto *view = ir.CreateAlloca(failureView());
  auto *viewType = failureView();
  auto *null = llvm::ConstantPointerNull::get(pointer);
  ir.CreateStore(ir.getInt32(1), ir.CreateStructGEP(viewType, view, 0));
  ir.CreateStore(ir.getInt32(2), ir.CreateStructGEP(viewType, view, 1));
  ir.CreateStore(null, ir.CreateStructGEP(viewType, view, 2));
  ir.CreateStore(envelope, ir.CreateStructGEP(viewType, view, 3));
  ir.CreateStore(primaryArtifact, ir.CreateStructGEP(viewType, view, 4));
  ir.CreateStore(secondaryArtifact, ir.CreateStructGEP(viewType, view, 5));
  ir.CreateCall(
      declareRuntimeService(session, RuntimeService::PublishThreadFailure),
      {view});
  ir.CreateStore(null, slot(ir, incomingRecord_));
  ir.CreateStore(null, slot(ir, incomingArtifact_));
  ir.CreateStore(ir.getInt8(1), slot(ir, abandon_));
  ir.CreateRetVoid();
  ir.SetInsertPoint(terminal);
  ir.CreateCall(
      declareRuntimeService(session, RuntimeService::TerminateCleanupFailure),
      {envelope, primaryArtifact, secondaryArtifact});
  ir.CreateUnreachable();
  return function;
}
void Failures::terminateCleanup(llvm::IRBuilder<> &ir, llvm::Value *primary,
                                llvm::Value *secondary) {
  if (containment_) {
    // A native worker publishes and returns; any other thread terminates in
    // the helper. The caller follows the abandonment request.
    ir.CreateCall(emergency(), {record(ir, primary), origin(ir, primary),
                                record(ir, secondary), origin(ir, secondary)});
    return;
  }
  ir.CreateCall(cleanupTerminator(), {primary, secondary});
  ir.CreateUnreachable();
}

llvm::Function *Failures::cleanupTerminator() {
  if (cleanupTerminator_)
    return cleanupTerminator_;
  auto *pointer = session.layouts().pointer();
  auto *function = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(session.context()),
                              {pointer, pointer}, false),
      llvm::GlobalValue::InternalLinkage, "__gti_failure_terminate_cleanup",
      session.module());
  // The emergency envelope is needed only at terminal double failure. Keep
  // its stack storage and copies out of every caller's ordinary cleanup frame.
  function->addFnAttr(llvm::Attribute::Cold);
  function->addFnAttr(llvm::Attribute::NoInline);
  function->addFnAttr(llvm::Attribute::NoReturn);
  cleanupTerminator_ = function;
  llvm::IRBuilder<> ir(
      llvm::BasicBlock::Create(session.context(), "entry", function));
  auto *primary = function->getArg(0);
  auto *secondary = function->getArg(1);
  auto *type = session.abi().failureEmergency();
  auto *envelope = ir.CreateAlloca(type);
  ir.CreateStore(ir.getInt32(2), ir.CreateStructGEP(type, envelope, 0));
  ir.CreateStore(ir.getInt32(0), ir.CreateStructGEP(type, envelope, 1));
  auto *recordType = session.abi().failureRecord();
  const auto alignment = session.dataLayout().getABITypeAlign(recordType);
  const auto bytes =
      session.dataLayout().getTypeAllocSize(recordType).getFixedValue();
  ir.CreateMemMove(ir.CreateStructGEP(type, envelope, 2), alignment,
                   record(ir, primary), alignment, bytes);
  ir.CreateMemMove(ir.CreateStructGEP(type, envelope, 3), alignment,
                   record(ir, secondary), alignment, bytes);
  ir.CreateCall(
      declareRuntimeService(session, RuntimeService::TerminateCleanupFailure),
      {envelope, origin(ir, primary), origin(ir, secondary)});
  ir.CreateUnreachable();
  return function;
}

} // namespace lang::native_llvm
