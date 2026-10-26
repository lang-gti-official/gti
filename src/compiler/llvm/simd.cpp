#include "simd.h"

#include <array>
#include <stdexcept>

namespace lang::native_llvm {

void emitSimd(llvm::IRBuilder<> &ir, const MirInstruction &instruction,
              std::span<llvm::Value *const> addresses,
              std::span<const TypeLayout> arrays) {
  if (!simdIntrinsic(instruction.intrinsic) || addresses.size() != 3 ||
      arrays.size() != 3 || instruction.parameterTypes.size() != 3 ||
      instruction.operands.size() != 3 || instruction.result ||
      instruction.info.type != SemanticType::Void)
    throw std::logic_error("LLVM SIMD lost its verified three-array contract");
  std::array<llvm::ArrayType *, 3> storage;
  std::array<SemanticType, 3> types;
  for (unsigned index = 0; index != 3; ++index) {
    const auto &parameter = instruction.parameterTypes[index];
    if (parameter.kind != SemanticType::Reference ||
        parameter.arguments.size() != 1 ||
        parameter.referenceAccess !=
            (index == 2 ? AccessMode::Mutable : AccessMode::ReadOnly) ||
        !addresses[index] || !addresses[index]->getType()->isPointerTy())
      throw std::logic_error("LLVM SIMD lost an exact evaluated array address");
    types[index] = parameter.arguments[0];
    storage[index] = llvm::dyn_cast<llvm::ArrayType>(arrays[index].storage);
    if (types[index].kind != SemanticType::Array ||
        types[index].arguments.size() != 1 ||
        types[index].arrayLengthParameterId || !storage[index] ||
        storage[index]->getNumElements() != types[index].arrayLength)
      throw std::logic_error(
          "LLVM SIMD storage disagrees with the concrete array type");
  }
  const auto lanes = types[0].arrayLength;
  const bool comparison = simdComparisonIntrinsic(instruction.intrinsic);
  const auto &element = types[0].arguments[0];
  if (lanes < 1 || lanes > 64 || types[0] != types[1] ||
      (element != SemanticType::Float && element != SemanticType::Double) ||
      types[2].arrayLength != lanes ||
      types[2].arguments[0] != (comparison ? SemanticType::Bool : element) ||
      storage[0]->getElementType() != storage[1]->getElementType() ||
      !(element == SemanticType::Float
            ? storage[0]->getElementType()->isFloatTy()
            : storage[0]->getElementType()->isDoubleTy()) ||
      (comparison
           ? !storage[2]->getElementType()->isIntegerTy(8)
           : storage[2]->getElementType() != storage[0]->getElementType()))
    throw std::logic_error(
        "LLVM SIMD lane/mask storage violates the verified profile");

  llvm::IRBuilderBase::FastMathFlagGuard flags(ir);
  ir.clearFastMathFlags();
  auto *vectorType =
      llvm::FixedVectorType::get(storage[0]->getElementType(), lanes);
  std::array<llvm::Value *, 2> inputs;
  // Load complete inputs before publishing any output. No vector alignment,
  // over-read, alias promise or host SIMD feature is required by this lowering.
  for (unsigned input = 0; input != 2; ++input) {
    inputs[input] = llvm::PoisonValue::get(vectorType);
    for (unsigned lane = 0; lane != lanes; ++lane) {
      auto *address = ir.CreateGEP(storage[input], addresses[input],
                                   {ir.getInt64(0), ir.getInt64(lane)});
      auto *value = ir.CreateLoad(storage[input]->getElementType(), address);
      inputs[input] =
          ir.CreateInsertElement(inputs[input], value, ir.getInt32(lane));
    }
  }
  llvm::Value *result = nullptr;
  switch (instruction.intrinsic) {
  case IntrinsicKind::SimdAdd:
    result = ir.CreateFAdd(inputs[0], inputs[1]);
    break;
  case IntrinsicKind::SimdSubtract:
    result = ir.CreateFSub(inputs[0], inputs[1]);
    break;
  case IntrinsicKind::SimdMultiply:
    result = ir.CreateFMul(inputs[0], inputs[1]);
    break;
  case IntrinsicKind::SimdDivide:
    result = ir.CreateFDiv(inputs[0], inputs[1]);
    break;
  case IntrinsicKind::SimdEqual:
    result = ir.CreateFCmpOEQ(inputs[0], inputs[1]);
    break;
  case IntrinsicKind::SimdLess:
    result = ir.CreateFCmpOLT(inputs[0], inputs[1]);
    break;
  default:
    throw std::logic_error("LLVM SIMD received a non-SIMD intrinsic");
  }
  for (unsigned lane = 0; lane != lanes; ++lane) {
    auto *value = ir.CreateExtractElement(result, ir.getInt32(lane));
    if (comparison)
      value = ir.CreateZExt(value, storage[2]->getElementType());
    auto *address = ir.CreateGEP(storage[2], addresses[2],
                                 {ir.getInt64(0), ir.getInt64(lane)});
    ir.CreateStore(value, address);
  }
}

} // namespace lang::native_llvm
