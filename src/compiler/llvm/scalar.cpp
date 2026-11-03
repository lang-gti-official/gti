#include "scalar.h"

#include <algorithm>
#include <array>
#include <llvm/ADT/APFloat.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Intrinsics.h>

namespace lang::native_llvm {

llvm::ConstantInt *integerConstant(llvm::IntegerType *type, bool negative,
                                   std::uint64_t magnitude) {
  auto bits = llvm::APInt(64, magnitude).zextOrTrunc(type->getBitWidth());
  if (negative)
    bits.negate();
  return llvm::ConstantInt::get(type->getContext(), bits);
}
namespace {
bool floating(const SemanticType &type) {
  return type == SemanticType::Float || type == SemanticType::Double;
}
MirOperation baseOperation(MirOperation operation) {
  switch (operation) {
  case MirOperation::AddAssign:
    return MirOperation::Add;
  case MirOperation::SubtractAssign:
    return MirOperation::Subtract;
  case MirOperation::MultiplyAssign:
    return MirOperation::Multiply;
  case MirOperation::DivideAssign:
    return MirOperation::Divide;
  case MirOperation::RemainderAssign:
    return MirOperation::Remainder;
  case MirOperation::BitwiseAndAssign:
    return MirOperation::BitwiseAnd;
  case MirOperation::BitwiseOrAssign:
    return MirOperation::BitwiseOr;
  case MirOperation::BitwiseXorAssign:
    return MirOperation::BitwiseXor;
  case MirOperation::ShiftLeftAssign:
    return MirOperation::ShiftLeft;
  case MirOperation::ShiftRightAssign:
    return MirOperation::ShiftRight;
  default:
    return operation;
  }
}
} // namespace

Scalars::Scalars(Session &session, llvm::IRBuilder<> &ir, Guard guard)
    : session(session), ir(ir), guard(std::move(guard)) {}

llvm::Constant *Scalars::constantStorage(
    const TypeLayout &layout,
    std::span<const std::pair<std::uint64_t, llvm::Constant *>> fragments) {
  std::vector<llvm::Constant *> fields;
  std::uint64_t end = 0;
  const auto padding = [&](std::uint64_t size) {
    if (size)
      fields.push_back(llvm::Constant::getNullValue(
          llvm::ArrayType::get(ir.getInt8Ty(), size)));
  };
  for (const auto &[offset, field] : fragments) {
    const auto size =
        session.dataLayout().getTypeAllocSize(field->getType()).getFixedValue();
    if (offset < end || offset > layout.size || size > layout.size - offset)
      throw std::logic_error(
          "LLVM static fragments differ from their exact storage layout");
    padding(offset - end);
    fields.push_back(field);
    end = offset + size;
  }
  padding(layout.size - end);
  // Typed fragments preserve relocations. Opaque-pointer users still address
  // this byte-congruent initializer through the one canonical layout plan.
  return llvm::ConstantStruct::getAnon(session.context(), fields, true);
}

llvm::Constant *Scalars::constant(const ConstantValue &value,
                                  const SemanticType &type) {
  auto *storage = session.layouts().get(type).storage;
  if (std::holds_alternative<NullConstant>(value))
    return llvm::ConstantPointerNull::get(session.layouts().pointer());
  if (const auto *text = std::get_if<std::string>(&value))
    return llvm::cast<llvm::Constant>(literal(*text, type));
  if (const auto *integer = std::get_if<ConstantInteger>(&value))
    return integerConstant(llvm::cast<llvm::IntegerType>(storage),
                           integer->negative, integer->magnitude);
  if (const auto *enumerator = std::get_if<ConstantEnum>(&value)) {
    if (const auto *payload = session.program().mir().findPayloadEnum(type)) {
      const auto variant = enumerator->value.magnitude;
      if (enumerator->value.negative || variant >= payload->variants.size() ||
          !payload->variants[variant].payloadTypes.empty())
        throw std::logic_error(
            "LLVM static enum constant is not an exact unit alternative");
      const auto &layout = session.layouts().payload(type);
      const std::array fragments{std::pair<std::uint64_t, llvm::Constant *>{
          layout.type.fieldOffsets.at(layout.tagIndex), ir.getInt32(variant)}};
      return constantStorage(layout.type, fragments);
    }
    return integerConstant(llvm::cast<llvm::IntegerType>(storage),
                           enumerator->value.negative,
                           enumerator->value.magnitude);
  }
  if (const auto *floating = std::get_if<BinaryFloat>(&value))
    return llvm::cast<llvm::Constant>(literal(*floating, type));
  if (const auto *character = std::get_if<CharacterLiteral>(&value))
    return llvm::ConstantInt::get(storage, character->value);
  if (const auto *boolean = std::get_if<bool>(&value))
    return llvm::ConstantInt::get(storage, *boolean);
  if (const auto *aggregate = std::get_if<ConstantAggregateValue>(&value);
      aggregate && aggregate->value &&
      aggregate->value->kind == ConstantAggregateKind::Array &&
      type.kind == SemanticType::Array &&
      aggregate->value->elements.size() == type.arrayLength) {
    std::vector<llvm::Constant *> elements;
    for (const auto &element : aggregate->value->elements)
      elements.push_back(constant(element, type.arguments.at(0)));
    if (std::any_of(elements.begin(), elements.end(), [&](auto *element) {
          return element->getType() !=
                 session.layouts().get(type.arguments[0]).storage;
        })) {
      const auto layout = session.layouts().get(type);
      std::vector<std::pair<std::uint64_t, llvm::Constant *>> fragments;
      for (std::size_t index = 0; index < elements.size(); ++index)
        fragments.emplace_back(index * layout.stride, elements[index]);
      return constantStorage(layout, fragments);
    }
    return llvm::ConstantArray::get(llvm::cast<llvm::ArrayType>(storage),
                                    elements);
  }
  if (const auto *aggregate = std::get_if<ConstantAggregateValue>(&value);
      aggregate && aggregate->value &&
      aggregate->value->kind == ConstantAggregateKind::Object &&
      type.kind == SemanticType::Class) {
    const auto &object = *aggregate->value;
    const auto &owner = session.layouts().instance(type);
    if (!object.exactType || *object.exactType != type ||
        object.identity != owner.declaration || !owner.bases.empty() ||
        owner.requiresActiveCleanup ||
        object.fields.size() != owner.declaredFields.size())
      throw BackendUnsupportedError("LLVM static object requires its complete "
                                    "passive concrete constant");
    if (owner.unionLayout) {
      for (const auto &field : owner.declaredFields) {
        const auto selected = std::find_if(
            object.fields.begin(), object.fields.end(),
            [&](const auto &value) { return value.symbol == field.symbol; });
        if (selected == object.fields.end() ||
            !constant(selected->value, field.type)->isNullValue())
          throw BackendUnsupportedError(
              "LLVM union static datum requires exact zero initialization");
      }
      return llvm::Constant::getNullValue(storage);
    }
    auto *record = llvm::cast<llvm::StructType>(storage);
    std::vector<llvm::Constant *> fields;
    for (auto *element : record->elements())
      fields.push_back(llvm::Constant::getNullValue(element));
    for (const auto &field : owner.declaredFields) {
      const auto selected = std::find_if(
          object.fields.begin(), object.fields.end(),
          [&](const auto &value) { return value.symbol == field.symbol; });
      if (selected == object.fields.end())
        throw std::logic_error("LLVM static object lost a sealed field");
      fields.at(session.layouts().fieldIndex(type, field.symbol)) =
          constant(selected->value, field.type);
    }
    bool canonical = true;
    for (unsigned index = 0; index < fields.size(); ++index)
      canonical &= fields[index]->getType() == record->getElementType(index);
    if (canonical)
      return llvm::ConstantStruct::get(record, fields);
    const auto layout = session.layouts().get(type);
    std::vector<std::pair<std::uint64_t, llvm::Constant *>> fragments;
    for (std::size_t index = 0; index < fields.size(); ++index)
      fragments.emplace_back(layout.fieldOffsets.at(index), fields[index]);
    return constantStorage(layout, fragments);
  }
  if (const auto *aggregate = std::get_if<ConstantAggregateValue>(&value);
      aggregate && aggregate->value &&
      aggregate->value->kind == ConstantAggregateKind::PayloadEnum &&
      type.kind == SemanticType::Enum) {
    const auto &node = *aggregate->value;
    const auto *payload = session.program().mir().findPayloadEnum(type);
    if (!payload || !node.exactType || *node.exactType != type ||
        node.identity != payload->declaration ||
        node.variant >= payload->variants.size() ||
        node.elements.size() !=
            payload->variants[node.variant].payloadTypes.size())
      throw std::logic_error(
          "LLVM static payload lost its exact alternative fields");
    const auto &layout = session.layouts().payload(type);
    std::vector<std::pair<std::uint64_t, llvm::Constant *>> fragments{
        {layout.type.fieldOffsets.at(layout.tagIndex),
         ir.getInt32(node.variant)}};
    const auto *variant =
        session.dataLayout().getStructLayout(layout.variants[node.variant]);
    for (std::size_t index = 0; index < node.elements.size(); ++index)
      fragments.emplace_back(
          layout.type.fieldOffsets.at(layout.payloadIndex) +
              variant->getElementOffset(index),
          constant(node.elements[index],
                   payload->variants[node.variant].payloadTypes[index]));
    return constantStorage(layout.type, fragments);
  }
  throw BackendUnsupportedError("LLVM static datum requires an admitted scalar "
                                "or aggregate constant");
}

SemanticType Scalars::domain(const SemanticType &type) const {
  if (type.kind == SemanticType::Enum) {
    for (const auto &enumeration : session.program().mir().integralEnums())
      if (enumeration.type == type)
        return enumeration.underlyingType;
    throw BackendUnsupportedError(
        "LLVM scalar enum requires a sealed integral domain");
  }
  return type;
}
bool Scalars::signedType(const SemanticType &type) const {
  auto integer = constantIntegerDomain(domain(type));
  return integer && integer->signedValue;
}
SemanticType Scalars::promoted(const SemanticType &type) const {
  auto integer = constantIntegerDomain(type);
  return integer && integer->width < 32 ? SemanticType::Int32 : type;
}
SemanticType Scalars::common(const SemanticType &left,
                             const SemanticType &right) const {
  auto a = domain(left), b = domain(right);
  if (a == b && (a == SemanticType::Bool || a == SemanticType::Char))
    return a;
  if (a == SemanticType::Double || b == SemanticType::Double)
    return SemanticType::Double;
  if (a == SemanticType::Float || b == SemanticType::Float)
    return SemanticType::Float;
  a = promoted(a);
  b = promoted(b);
  auto x = constantIntegerDomain(a), y = constantIntegerDomain(b);
  if (!x || !y)
    throw BackendUnsupportedError(
        "LLVM numeric operation requires concrete scalar domains");
  if (x->width != y->width)
    return x->width > y->width ? a : b;
  return x->signedValue ? b : a;
}

llvm::Value *Scalars::literal(const Literal &literal,
                              const SemanticType &type) {
  auto *output = session.layouts().get(type).value;
  if (std::holds_alternative<std::nullptr_t>(literal))
    return llvm::ConstantPointerNull::get(session.layouts().pointer());
  if (const auto *integer = std::get_if<uint64_t>(&literal)) {
    if (auto *integral = llvm::dyn_cast<llvm::IntegerType>(output))
      return integerConstant(integral, false, *integer);
    return llvm::ConstantInt::get(output, *integer);
  }
  if (const auto *boolean = std::get_if<bool>(&literal))
    return llvm::ConstantInt::get(output, *boolean);
  if (const auto *character = std::get_if<CharacterLiteral>(&literal))
    return llvm::ConstantInt::get(output, character->value);
  if (const auto *number = std::get_if<BinaryFloat>(&literal)) {
    const bool wide = number->format == BinaryFloatFormat::Binary64;
    return llvm::ConstantFP::get(
        session.context(),
        llvm::APFloat(wide ? llvm::APFloat::IEEEdouble()
                           : llvm::APFloat::IEEEsingle(),
                      llvm::APInt(wide ? 64 : 32, number->bits)));
  }
  if (const auto *text = std::get_if<std::string>(&literal)) {
    auto *storage = session.textLiteral(*text);
    if (type.kind == SemanticType::CString)
      return storage;
    return llvm::ConstantStruct::get(session.abi().textView(),
                                     {storage, ir.getInt64(text->size())});
  }
  throw BackendUnsupportedError("LLVM scalar literal shape is not implemented");
}

llvm::Value *Scalars::convert(llvm::Value *value, const SemanticType &from,
                              const SemanticType &to, bool checked) {
  if (from == to)
    return value;
  auto source = domain(from), target = domain(to);
  auto *output = session.layouts().get(target).value;
  if (value->getType()->isPointerTy() && output->isPointerTy())
    return value;
  if (value->getType()->isPointerTy() && to == SemanticType::Bool)
    return ir.CreateIsNotNull(value);
  if (source == target)
    return value;
  const DefinedFailureOutcome failure{
      DefinedFailureCode::NumericConversionOutOfRange,
      DefinedFailureDetail::NumericCast};
  if (value->getType()->isIntegerTy() && output->isIntegerTy()) {
    if (checked) {
      auto *wide =
          ir.CreateIntCast(value, ir.getInt128Ty(), signedType(source));
      const auto width = output->getIntegerBitWidth();
      const auto minimum = signedType(target)
                               ? llvm::APInt::getSignedMinValue(width).sext(128)
                               : llvm::APInt(128, 0);
      const auto maximum = signedType(target)
                               ? llvm::APInt::getSignedMaxValue(width).zext(128)
                               : llvm::APInt::getMaxValue(width).zext(128);
      guard(ir.CreateOr(ir.CreateICmpSLT(wide, llvm::ConstantInt::get(
                                                   ir.getInt128Ty(), minimum)),
                        ir.CreateICmpSGT(wide, llvm::ConstantInt::get(
                                                   ir.getInt128Ty(), maximum))),
            failure);
    }
    return ir.CreateIntCast(value, output, signedType(source));
  }
  if (value->getType()->isIntegerTy() && output->isFloatingPointTy())
    return signedType(source) ? ir.CreateSIToFP(value, output)
                              : ir.CreateUIToFP(value, output);
  if (value->getType()->isFloatingPointTy() && output->isFloatingPointTy())
    return ir.CreateFPCast(value, output);
  if (value->getType()->isFloatingPointTy() && output->isIntegerTy()) {
    const auto width = output->getIntegerBitWidth();
    auto *truncated = ir.CreateUnaryIntrinsic(llvm::Intrinsic::trunc, value);
    const auto boundary =
        llvm::APInt(128, 1).shl(signedType(target) ? width - 1 : width);
    llvm::APFloat upper(value->getType()->isDoubleTy()
                            ? llvm::APFloat::IEEEdouble()
                            : llvm::APFloat::IEEEsingle());
    upper.convertFromAPInt(boundary, false, llvm::APFloat::rmNearestTiesToEven);
    auto lower = upper;
    if (signedType(target))
      lower.changeSign();
    else
      lower = llvm::APFloat::getZero(upper.getSemantics());
    auto *valid = ir.CreateAnd(
        ir.CreateFCmpOGE(truncated,
                         llvm::ConstantFP::get(session.context(), lower)),
        ir.CreateFCmpOLT(truncated,
                         llvm::ConstantFP::get(session.context(), upper)));
    guard(ir.CreateNot(valid), failure);
    return signedType(target) ? ir.CreateFPToSI(value, output)
                              : ir.CreateFPToUI(value, output);
  }
  throw BackendUnsupportedError(
      "LLVM scalar conversion shape is not implemented");
}

llvm::Value *Scalars::arithmetic(MirOperation operation,
                                 std::span<llvm::Value *const> inputs,
                                 std::span<const SemanticType> types,
                                 const SemanticType &result) {
  const bool comparison = operation >= MirOperation::Equal &&
                          operation <= MirOperation::GreaterEqual;
  const bool shift = operation == MirOperation::ShiftLeft ||
                     operation == MirOperation::ShiftRight;
  const auto type = comparison ? common(types[0], types[1])
                    : shift    ? promoted(domain(types[0]))
                               : domain(result);
  auto *a = convert(inputs[0], types[0], type, false);
  auto *b = inputs.size() > 1
                ? convert(inputs[1], types[1], shift ? types[1] : type, false)
                : nullptr;
  if (comparison) {
    if (floating(type)) {
      switch (operation) {
      case MirOperation::Equal:
        return ir.CreateFCmpOEQ(a, b);
      case MirOperation::NotEqual:
        return ir.CreateFCmpUNE(a, b);
      case MirOperation::Less:
        return ir.CreateFCmpOLT(a, b);
      case MirOperation::LessEqual:
        return ir.CreateFCmpOLE(a, b);
      case MirOperation::Greater:
        return ir.CreateFCmpOGT(a, b);
      case MirOperation::GreaterEqual:
        return ir.CreateFCmpOGE(a, b);
      default:
        break;
      }
    } else {
      switch (operation) {
      case MirOperation::Equal:
        return ir.CreateICmpEQ(a, b);
      case MirOperation::NotEqual:
        return ir.CreateICmpNE(a, b);
      case MirOperation::Less:
        return signedType(type) ? ir.CreateICmpSLT(a, b)
                                : ir.CreateICmpULT(a, b);
      case MirOperation::LessEqual:
        return signedType(type) ? ir.CreateICmpSLE(a, b)
                                : ir.CreateICmpULE(a, b);
      case MirOperation::Greater:
        return signedType(type) ? ir.CreateICmpSGT(a, b)
                                : ir.CreateICmpUGT(a, b);
      case MirOperation::GreaterEqual:
        return signedType(type) ? ir.CreateICmpSGE(a, b)
                                : ir.CreateICmpUGE(a, b);
      default:
        break;
      }
    }
  }
  if (floating(type)) {
    switch (operation) {
    case MirOperation::Add:
      return ir.CreateFAdd(a, b);
    case MirOperation::Subtract:
      return ir.CreateFSub(a, b);
    case MirOperation::Multiply:
      return ir.CreateFMul(a, b);
    case MirOperation::Divide:
      return ir.CreateFDiv(a, b);
    case MirOperation::Positive:
      return a;
    case MirOperation::Negate:
      return ir.CreateFNeg(a);
    default:
      throw BackendUnsupportedError(
          "LLVM floating operation is not implemented");
    }
  }
  auto *integer = llvm::cast<llvm::IntegerType>(a->getType());
  auto zero = llvm::ConstantInt::get(integer, 0);
  const bool signedValue = signedType(type);
  const auto overflow = [&](llvm::Value *left, llvm::Value *right,
                            llvm::Intrinsic::ID signedId,
                            llvm::Intrinsic::ID unsignedId,
                            DefinedFailureDetail detail) -> llvm::Value * {
    auto *pair = ir.CreateBinaryIntrinsic(signedValue ? signedId : unsignedId,
                                          left, right);
    // Ordinary GTI arithmetic cannot wrap on its successful continuation.
    // Keep the overflow detector non-poisoning. Compute its flagged result
    // alongside the detector so instruction selection can share arithmetic,
    // but expose it only after the guard (or verified discharge). The result
    // can be poison on failure; no failure-path operation consumes it.
    // Extracting the intrinsic's
    // wrapping lane instead loses this invariant before LLVM loop/range
    // optimization. Explicit wrapping/saturating intrinsics use compute()'s
    // separate path and must never receive these flags.
    llvm::Value *result = nullptr;
    switch (signedId) {
    case llvm::Intrinsic::sadd_with_overflow:
      result =
          ir.CreateAdd(left, right, "checked.add", !signedValue, signedValue);
      break;
    case llvm::Intrinsic::ssub_with_overflow:
      result =
          ir.CreateSub(left, right, "checked.sub", !signedValue, signedValue);
      break;
    case llvm::Intrinsic::smul_with_overflow:
      result =
          ir.CreateMul(left, right, "checked.mul", !signedValue, signedValue);
      break;
    default:
      throw std::logic_error("LLVM checked arithmetic lost its operation");
    }
    guard(ir.CreateExtractValue(pair, 1),
          {DefinedFailureCode::IntegerOverflow, detail});
    return result;
  };
  switch (operation) {
  case MirOperation::Positive:
    return a;
  case MirOperation::LogicalNot:
    return ir.CreateNot(a);
  case MirOperation::BitwiseNot:
    return ir.CreateNot(a);
  case MirOperation::Negate:
    return overflow(zero, a, llvm::Intrinsic::ssub_with_overflow,
                    llvm::Intrinsic::usub_with_overflow,
                    DefinedFailureDetail::Negation);
  case MirOperation::Add:
    return overflow(a, b, llvm::Intrinsic::sadd_with_overflow,
                    llvm::Intrinsic::uadd_with_overflow,
                    DefinedFailureDetail::Addition);
  case MirOperation::Subtract:
    return overflow(a, b, llvm::Intrinsic::ssub_with_overflow,
                    llvm::Intrinsic::usub_with_overflow,
                    DefinedFailureDetail::Subtraction);
  case MirOperation::Multiply:
    return overflow(a, b, llvm::Intrinsic::smul_with_overflow,
                    llvm::Intrinsic::umul_with_overflow,
                    DefinedFailureDetail::Multiplication);
  case MirOperation::BitwiseAnd:
    return ir.CreateAnd(a, b);
  case MirOperation::BitwiseOr:
    return ir.CreateOr(a, b);
  case MirOperation::BitwiseXor:
    return ir.CreateXor(a, b);
  case MirOperation::ShiftLeft:
  case MirOperation::ShiftRight: {
    const auto detail = operation == MirOperation::ShiftLeft
                            ? DefinedFailureDetail::LeftShift
                            : DefinedFailureDetail::RightShift;
    if (signedType(types[1]))
      guard(ir.CreateICmpSLT(b, llvm::ConstantInt::get(b->getType(), 0)),
            {DefinedFailureCode::NegativeShiftCount, detail});
    auto *wide = ir.CreateIntCast(b, ir.getInt128Ty(), signedType(types[1]));
    guard(ir.CreateICmpUGE(wide, llvm::ConstantInt::get(
                                     ir.getInt128Ty(), integer->getBitWidth())),
          {DefinedFailureCode::ShiftCountOutOfRange, detail});
    b = ir.CreateIntCast(b, integer, false);
    return operation == MirOperation::ShiftLeft ? ir.CreateShl(a, b)
           : signedValue                        ? ir.CreateAShr(a, b)
                                                : ir.CreateLShr(a, b);
  }
  case MirOperation::Divide:
  case MirOperation::Remainder: {
    const bool remainder = operation == MirOperation::Remainder;
    guard(ir.CreateICmpEQ(b, zero),
          {remainder ? DefinedFailureCode::ModuloByZero
                     : DefinedFailureCode::DivisionByZero,
           remainder ? DefinedFailureDetail::IntegerModulo
                     : DefinedFailureDetail::IntegerDivision});
    if (signedValue) {
      auto *exceptional = ir.CreateAnd(
          ir.CreateICmpEQ(
              a, llvm::ConstantInt::get(integer, llvm::APInt::getSignedMinValue(
                                                     integer->getBitWidth()))),
          ir.CreateICmpEQ(b, llvm::ConstantInt::getSigned(integer, -1)));
      if (!remainder)
        guard(exceptional, {DefinedFailureCode::IntegerOverflow,
                            DefinedFailureDetail::Division});
      else
        b = ir.CreateSelect(exceptional, llvm::ConstantInt::get(integer, 1), b);
      return remainder ? ir.CreateSRem(a, b) : ir.CreateSDiv(a, b);
    }
    return remainder ? ir.CreateURem(a, b) : ir.CreateUDiv(a, b);
  }
  default:
    throw BackendUnsupportedError(
        "LLVM scalar arithmetic operation is not implemented");
  }
}

llvm::Value *Scalars::compute(const MirInstruction &instruction,
                              std::span<llvm::Value *const> inputs) {
  if (instruction.literal)
    return literal(*instruction.literal, instruction.info.type);
  if (instruction.operation == MirOperation::EnumConstant &&
      instruction.enumValue) {
    auto *type = session.layouts().get(instruction.info.type).value;
    return integerConstant(llvm::cast<llvm::IntegerType>(type),
                           instruction.enumValue->negative,
                           instruction.enumValue->magnitude);
  }
  if (instruction.operation == MirOperation::Identity ||
      instruction.operation == MirOperation::Comma)
    return inputs.back();
  if (instruction.operation == MirOperation::Convert)
    return convert(inputs.front(), instruction.operands.front().type,
                   instruction.info.type);
  if (const auto intrinsic =
          integerArithmeticIntrinsic(instruction.intrinsic)) {
    const auto type = instruction.operands[0].type;
    const auto width = inputs[0]->getType()->getIntegerBitWidth();
    const bool sign = signedType(type);
    const bool add = intrinsic->operation == CheckedIntegerOperation::Add;
    const bool sub = intrinsic->operation == CheckedIntegerOperation::Subtract;
    const auto id = sign ? (add   ? llvm::Intrinsic::sadd_with_overflow
                            : sub ? llvm::Intrinsic::ssub_with_overflow
                                  : llvm::Intrinsic::smul_with_overflow)
                         : (add   ? llvm::Intrinsic::uadd_with_overflow
                            : sub ? llvm::Intrinsic::usub_with_overflow
                                  : llvm::Intrinsic::umul_with_overflow);
    auto *pair = ir.CreateBinaryIntrinsic(id, inputs[0], inputs[1]);
    auto *value = ir.CreateExtractValue(pair, 0);
    auto *over = ir.CreateExtractValue(pair, 1);
    if (intrinsic->mode == IntegerArithmeticMode::Wrapping)
      return value;
    if (intrinsic->mode == IntegerArithmeticMode::OverflowPredicate)
      return over;
    auto *integer = llvm::cast<llvm::IntegerType>(inputs[0]->getType());
    llvm::Value *limit = llvm::ConstantInt::get(
        integer, sub ? llvm::APInt(width, 0) : llvm::APInt::getMaxValue(width));
    if (sign) {
      auto *negative =
          ir.CreateICmpSLT(inputs[0], llvm::ConstantInt::get(integer, 0));
      if (!add && !sub)
        negative = ir.CreateXor(
            negative,
            ir.CreateICmpSLT(inputs[1], llvm::ConstantInt::get(integer, 0)));
      limit =
          ir.CreateSelect(negative,
                          llvm::ConstantInt::get(
                              integer, llvm::APInt::getSignedMinValue(width)),
                          llvm::ConstantInt::get(
                              integer, llvm::APInt::getSignedMaxValue(width)));
    }
    return ir.CreateSelect(over, limit, value);
  }
  std::vector<SemanticType> types;
  for (const auto &operand : instruction.operands)
    types.push_back(operand.type);
  const auto operation = baseOperation(instruction.operation);
  return arithmetic(operation, inputs, types, instruction.info.type);
}

llvm::Value *Scalars::mutate(const MirInstruction &instruction,
                             const SemanticType &target,
                             std::span<llvm::Value *const> inputs) {
  if (instruction.operands.size() != 1 || inputs.size() != 2)
    throw std::logic_error("LLVM compound assignment lost its evaluated input");
  const auto operation = baseOperation(instruction.operation);
  const auto &right = instruction.operands.front().type;
  const auto computation = operation == MirOperation::ShiftLeft ||
                                   operation == MirOperation::ShiftRight
                               ? promoted(domain(target))
                               : common(target, right);
  const std::array types{target, right};
  auto *value = arithmetic(operation, inputs, types, computation);
  return convert(value, computation, target);
}

} // namespace lang::native_llvm
