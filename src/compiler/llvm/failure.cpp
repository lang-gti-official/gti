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
void Failures::terminateCleanup(llvm::IRBuilder<> &ir, llvm::Value *primary,
                                llvm::Value *secondary) {
  auto *type = session.abi().failureEmergency();
  auto *envelope = ir.CreateAlloca(type);
  llvm::Value *value = llvm::Constant::getNullValue(type);
  value = ir.CreateInsertValue(value, ir.getInt32(2), 0);
  value = ir.CreateInsertValue(
      value, ir.CreateLoad(session.abi().failureRecord(), record(ir, primary)),
      2);
  value = ir.CreateInsertValue(
      value,
      ir.CreateLoad(session.abi().failureRecord(), record(ir, secondary)), 3);
  ir.CreateStore(value, envelope);
  ir.CreateCall(
      declareRuntimeService(session, RuntimeService::TerminateCleanupFailure),
      {envelope, origin(ir, primary), origin(ir, secondary)});
  ir.CreateUnreachable();
}

} // namespace lang::native_llvm
