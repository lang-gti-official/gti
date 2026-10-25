#include "../src/compiler/llvm/simd.h"

#include <llvm/IR/Verifier.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>

#include <array>
#include <iostream>
#include <stdexcept>

using namespace lang;
using namespace lang::native_llvm;

int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  llvm::LLVMContext context;
  llvm::Module module("gti.simd.component", context);
  unsigned positives = 0, negatives = 0;
  const std::array operations{
      IntrinsicKind::SimdAdd,      IntrinsicKind::SimdSubtract,
      IntrinsicKind::SimdMultiply, IntrinsicKind::SimdDivide,
      IntrinsicKind::SimdEqual,    IntrinsicKind::SimdLess};
  for (bool wide : {false, true}) {
    for (unsigned lanes : {1, 2, 3, 4, 5, 7, 8, 31, 64}) {
      for (unsigned operation = 0; operation < operations.size(); ++operation) {
        const auto element = wide ? SemanticType::Double : SemanticType::Float;
        const auto outputElement =
            operation >= 4 ? SemanticType::Bool : element;
        MirInstruction instruction;
        instruction.kind = MirInstructionKind::Call;
        instruction.intrinsic = operations[operation];
        instruction.info.type = SemanticType::Void;
        std::array<TypeLayout, 3> layouts;
        for (unsigned index = 0; index != 3; ++index) {
          const auto array = SemanticType::arrayOf(
              index == 2 ? outputElement : element, lanes);
          const auto reference = SemanticType::referenceTo(
              array, index == 2 ? AccessMode::Mutable : AccessMode::ReadOnly);
          instruction.parameterTypes.push_back(reference);
          MirOperand operand;
          operand.type = reference;
          operand.kind = index == 2 ? MirOperandKind::BorrowWrite
                                    : MirOperandKind::BorrowRead;
          instruction.operands.push_back(operand);
          auto *lane = index == 2 && operation >= 4
                           ? llvm::Type::getInt8Ty(context)
                       : wide ? llvm::Type::getDoubleTy(context)
                              : llvm::Type::getFloatTy(context);
          layouts[index].storage = llvm::ArrayType::get(lane, lanes);
        }
        auto *pointer = llvm::PointerType::get(context, 0);
        const auto name = std::string("probe_") + (wide ? "d_" : "f_") +
                          std::to_string(lanes) + "_" +
                          std::to_string(operation);
        auto *function = llvm::Function::Create(
            llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                                    {pointer, pointer, pointer}, false),
            llvm::GlobalValue::ExternalLinkage, name, module);
        llvm::IRBuilder<> ir(
            llvm::BasicBlock::Create(context, "entry", function));
        const std::array<llvm::Value *, 3> addresses{
            function->getArg(0), function->getArg(1), function->getArg(2)};
        // Ambient builder flags must not leak into a strict GTI operation.
        ir.setFastMathFlags(llvm::FastMathFlags::getFast());
        emitSimd(ir, instruction, addresses, layouts);
        if (!ir.getFastMathFlags().isFast())
          throw std::runtime_error("builder flags not restored");
        ir.CreateRetVoid();
        for (const auto &block : *function)
          for (const auto &row : block)
            if (const auto *floating =
                    llvm::dyn_cast<llvm::FPMathOperator>(&row))
              if (floating->getFastMathFlags().any())
                throw std::runtime_error("SIMD fast math leaked");
        ++positives;
        if (wide || lanes != 1 || operation != 0)
          continue;
        const auto reject = [&](auto mutate) {
          auto invalid = instruction;
          auto badLayouts = layouts;
          auto badAddresses = addresses;
          mutate(invalid, badLayouts, badAddresses);
          bool rejected = false;
          try {
            emitSimd(ir, invalid, badAddresses, badLayouts);
          } catch (const std::logic_error &) {
            rejected = true;
          }
          if (!rejected)
            throw std::runtime_error("malformed SIMD contract accepted");
          ++negatives;
        };
        reject(
            [](auto &i, auto &, auto &) { i.intrinsic = IntrinsicKind::None; });
        reject([](auto &i, auto &, auto &) { i.parameterTypes.clear(); });
        reject([](auto &i, auto &, auto &) { i.operands.clear(); });
        reject([](auto &, auto &, auto &a) { a[0] = nullptr; });
        reject([](auto &i, auto &, auto &) {
          i.parameterTypes[2].referenceAccess = AccessMode::ReadOnly;
        });
        reject([](auto &i, auto &, auto &) {
          i.parameterTypes[0].arguments[0].arguments[0] = SemanticType::Int32;
        });
        reject([&](auto &, auto &l, auto &) {
          l[2].storage = llvm::ArrayType::get(ir.getInt1Ty(), 1);
        });
        reject([&](auto &, auto &l, auto &) {
          l[0].storage = llvm::ArrayType::get(ir.getDoubleTy(), 1);
        });
      }
    }
  }
  if (llvm::verifyModule(module, &llvm::errs()))
    return 1;
  std::error_code error;
  llvm::raw_fd_ostream output(argv[1], error, llvm::sys::fs::OF_None);
  if (error)
    throw std::runtime_error(error.message());
  module.print(output, nullptr);
  std::cout << positives << " SIMD profiles and " << negatives
            << " rejection controls passed\n";
}
