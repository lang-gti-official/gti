#include "failure.h"
#include "runtime_abi.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>

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
  return ir.CreateCall(
      declareRuntimeService(session, RuntimeService::EnterFailureCleanup),
      {record(ir, primary), origin(ir, primary)});
}
void Failures::leaveCleanup(llvm::IRBuilder<> &ir, llvm::Value *token) {
  if (token)
    ir.CreateCall(
        declareRuntimeService(session, RuntimeService::LeaveFailureCleanup),
        {token});
}
void Failures::checkIncomingCleanup(llvm::IRBuilder<> &ir,
                                    llvm::Value *secondary) {
  ir.CreateCall(
      declareRuntimeService(session, RuntimeService::CheckFailureCleanup),
      {record(ir, secondary), origin(ir, secondary)});
}
void Failures::terminateCleanup(llvm::IRBuilder<> &ir, llvm::Value *primary,
                                llvm::Value *secondary) {
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
