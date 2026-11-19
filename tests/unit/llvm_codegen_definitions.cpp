#include "../../src/compiler/llvm/session.h"
#include "gti/target.h"
#include "unit_test.h"

#include <iostream>
#include <llvm/IR/IRBuilder.h>
#include <stdexcept>
#include <string_view>

namespace {

void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

void define(llvm::Module &module, llvm::StringRef name,
            llvm::GlobalValue::LinkageTypes linkage) {
  auto &context = module.getContext();
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), false);
  auto *function = llvm::Function::Create(type, linkage, name, module);
  llvm::IRBuilder<> builder(
      llvm::BasicBlock::Create(context, "entry", function));
  builder.CreateRetVoid();
}

} // namespace

GTI_UNIT_TEST(llvm_codegen_definition_inventory) {
  using namespace lang::native_llvm;
  llvm::LLVMContext context;
  llvm::Module unit("unit", context);
  define(unit, "present", llvm::GlobalValue::ExternalLinkage);
  CodegenDefinitionInventory expected;
  expected.try_emplace("present", llvm::GlobalValue::ExternalLinkage);
  expected.try_emplace("missing", llvm::GlobalValue::ExternalLinkage);
  consumeCodegenUnitDefinitions(unit, expected);
  const auto rejected = [](auto check) {
    try {
      check();
      return false;
    } catch (const std::logic_error &) {
      return true;
    }
  };
  require(rejected([&] { requireCompleteCodegenDefinitions(expected); }),
          "validation accepted a missing definition");
  require(rejected([&] { consumeCodegenUnitDefinitions(unit, expected); }),
          "validation accepted a duplicate definition");
  expected.try_emplace("present", llvm::GlobalValue::InternalLinkage);
  require(rejected([&] { consumeCodegenUnitDefinitions(unit, expected); }),
          "validation accepted changed linkage");
  return 0;
}

GTI_UNIT_TEST(llvm_codegen_unit_diagnostic) {
  using namespace lang;
  using namespace lang::native_llvm;
  llvm::LLVMContext context;
  llvm::Module module("unit-failure", context);
  const auto target = TargetInfo::host();
  LlvmBackendOptions options{.codegen = {.units = 2, .threads = 2}};
  const auto machine = makeMachine(target, options);
  module.setDataLayout(machine->createDataLayout());
  module.setTargetTriple(machine->getTargetTriple());
  define(module, "valid", llvm::GlobalValue::ExternalLinkage);
  try {
    (void)emitObjectUnits(
        module, target, options, [](unsigned unit, llvm::LLVMContext &context) {
          context.emitError(unit == 0 ? "first forced unit diagnostic"
                                      : "second forced unit diagnostic");
        });
  } catch (const std::runtime_error &error) {
    const std::string_view message = error.what();
    const auto first = message.find("LLVM object unit 0");
    const auto second = message.find("LLVM object unit 1");
    require(first < second && second < message.size() &&
                message.find("first forced unit diagnostic") < second &&
                message.find("second forced unit diagnostic") > second &&
                message.find("second forced unit diagnostic") < message.size(),
            "emission did not report the LLVM diagnostics in unit order");
    return 0;
  }
  std::cerr << "emission returned a partial artifact after a unit error\n";
  return 1;
}
