#include "../src/compiler/llvm/body.h"
#include "cpp_backend_test_support.h"

#include <iostream>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>

namespace {
using namespace lang;
using namespace lang::native_llvm;

void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

void check(const std::filesystem::path &root) {
  FrontendOptions options;
  options.requireEntryPoint = true;
  options.requireFunctionDefinitions = true;
  options.lowerReachableOnly = true;
  options.lowerLibraryReachableOnly = true;
  const auto frontend = Frontend(options).analyze(
      root / "tests/fixtures/llvm_tls_cleanup_context.gti", std::nullopt,
      {root / "stdlib/prelude.gti"}, {}, {root / "stdlib"});
  require(frontend.canGenerateCode(), "TLS source reaches MIR");
  std::vector<LoweredProgramIssue> issues;
  const auto input = verifiedLoweredProgram(
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir,
                             options.target),
      issues);
  require(input.has_value(), "TLS source has a verified snapshot");
  const auto admission = planLlvmAdmission(*input);
  require(admission.admitted(), "TLS source is independently admitted");
  Session session(*input, {});
  lowerScalarProgram(session, admission);
  session.verify();
  std::size_t callbacks = 0;
  for (const auto &function : session.module()) {
    if (!function.getName().starts_with("__gti_native_tls_cleanup_"))
      continue;
    ++callbacks;
    require(function.arg_size() == 4 &&
                function.getReturnType()->isIntegerTy(1) &&
                function.hasRetAttribute(llvm::Attribute::ZExt),
            "TLS callback has the exact C bool/four-pointer ABI");
    const llvm::CallBase *enter = nullptr, *leave = nullptr;
    for (const auto &block : function)
      for (const auto &instruction : block) {
        const auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
        if (!call || !call->getCalledFunction())
          continue;
        const auto name = call->getCalledFunction()->getName();
        if (name == "gti_rt_failure_cleanup_enter_v1") {
          require(!enter, "one incoming context acquisition per callback");
          enter = call;
        }
        if (name == "gti_rt_failure_cleanup_leave_v1") {
          require(!leave, "one incoming context release per callback");
          leave = call;
        }
      }
    require(enter && leave, "active TLS callback scopes its incoming context");
    require(
        enter->arg_size() == 2 &&
            enter->getArgOperand(0) == function.getArg(2) &&
            enter->getArgOperand(1) == function.getArg(3),
        "primary record and distinct artifact are forwarded without copies");
    require(leave->arg_size() == 1,
            "context release accepts exactly its token");
    const auto *token = llvm::dyn_cast<llvm::PHINode>(leave->getArgOperand(0));
    require(token && token->getNumIncomingValues() == 2,
            "only incoming-primary and no-primary paths merge into release");
    bool acquired = false, empty = false;
    for (const auto &value : token->incoming_values()) {
      acquired |= value.get() == enter;
      empty |= llvm::isa<llvm::ConstantPointerNull>(value.get());
    }
    require(acquired && empty,
            "release uses the exact acquired token or an inert null token");
  }
  require(callbacks == 2, "both source TLS cleanup callbacks were checked");
}
} // namespace

int main(int argc, char **argv) {
  try {
    if (argc != 2)
      throw std::runtime_error("expected repository root");
    check(argv[1]);
    std::cout << "LLVM TLS callback contracts passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
