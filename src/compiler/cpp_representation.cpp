#include "cpp_representation.h"

#include "gti/lowered_program.h"

#include <llvm/ADT/ArrayRef.h>
#include <llvm/Support/SHA256.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lang {

namespace {

[[nodiscard]] std::string cppLoweredQualifiedName(std::string_view qualified,
                                                  bool global) {
  std::string result = global ? "::" : "::__gti_program::";
  std::size_t begin = 0;
  std::size_t index = 0;
  while (begin < qualified.size()) {
    const std::size_t end = qualified.find("::", begin);
    const std::string_view segment = qualified.substr(
        begin,
        end == std::string_view::npos ? std::string_view::npos : end - begin);
    if (segment.empty()) {
      return "void";
    }
    if (index != 0) {
      result += "::";
    }
    result += index == 0 && segment == "std"
                  ? std::string(cppEmittedStandardNamespace)
                  : std::string(segment);
    ++index;
    if (end == std::string_view::npos) {
      break;
    }
    begin = end + 2;
  }
  return index == 0 ? "void" : result;
}

void appendIdentityInteger(std::vector<std::uint8_t> &output,
                           std::uint64_t value) {
  for (unsigned index = 0; index < 8; ++index) {
    output.push_back(static_cast<std::uint8_t>(value >> (index * 8U)));
  }
}

void appendIdentityText(std::vector<std::uint8_t> &output,
                        std::string_view value) {
  appendIdentityInteger(output, value.size());
  output.insert(output.end(), value.begin(), value.end());
}

[[nodiscard]] bool
appendConcreteValueIdentity(std::vector<std::uint8_t> &output,
                            const CompileTimeValue &value) {
  if (value.kind != CompileTimeValue::UInt64) {
    return false;
  }
  appendIdentityText(output, "value-u64");
  appendIdentityInteger(output, value.value);
  return true;
}

[[nodiscard]] const LoweredDeclaration *
classDeclarationRow(const LoweredProgram &program, ClassId id) {
  for (const LoweredDeclaration &row : program.declarations()) {
    const auto *declaration =
        std::get_if<LoweredClassDeclaration>(&row.payload);
    if (declaration != nullptr && declaration->id == id) {
      return &row;
    }
  }
  return nullptr;
}

[[nodiscard]] const LoweredDeclaration *
enumDeclarationRow(const LoweredProgram &program, EnumId id) {
  for (const LoweredDeclaration &row : program.declarations()) {
    const auto *declaration = std::get_if<LoweredEnumDeclaration>(&row.payload);
    if (declaration != nullptr && declaration->id == id) {
      return &row;
    }
  }
  return nullptr;
}

[[nodiscard]] const LoweredClassDeclaration *
concreteClassPrimary(const LoweredProgram &program,
                     const LoweredClassDeclaration &declaration) {
  return declaration.exactSpecializationPrimary == 0
             ? &declaration
             : program.findClassDeclaration(
                   declaration.exactSpecializationPrimary);
}

[[nodiscard]] const LoweredDeclaration *
concreteClassPrimaryRow(const LoweredProgram &program,
                        const LoweredClassDeclaration &declaration) {
  const LoweredClassDeclaration *primary =
      concreteClassPrimary(program, declaration);
  return primary == nullptr ? nullptr
                            : classDeclarationRow(program, primary->id);
}

[[nodiscard]] bool concreteClassSupportsDirectRepresentation(
    const LoweredProgram &program, const LoweredClassDeclaration &declaration,
    const LoweredDeclaration &primaryRow) {
  if (declaration.compilerCapability != CompilerCapabilityTypeKind::None ||
      declaration.cOpaqueHandle || declaration.cAbiRecord) {
    return false;
  }
  const LoweredDeclaration *parent = program.findDeclaration(primaryRow.parent);
  if (parent != nullptr &&
      std::holds_alternative<LoweredClassDeclaration>(parent->payload)) {
    return false;
  }
  for (const LoweredDeclaration &candidate : program.declarations()) {
    if (candidate.parent != primaryRow.id) {
      continue;
    }
    if (const auto *storage =
            std::get_if<LoweredStorageDeclaration>(&candidate.payload)) {
      const auto containsUniqueOwner = [&](const auto &self,
                                           const SemanticType &type) -> bool {
        return type.kind == SemanticType::UniqueOwner ||
               std::any_of(type.arguments.begin(), type.arguments.end(),
                           [&](const SemanticType &argument) {
                             return self(self, argument);
                           });
      };
      if (containsUniqueOwner(containsUniqueOwner, storage->type)) {
        // C++ unique ownership deliberately supports an incomplete pointee.
        // A direct concrete wrapper would eagerly default its native special
        // members and instantiate std::unique_ptr<T>'s deletion contract
        // before T is complete. Keep these ownership carriers as source
        // templates so native instantiation retains the same delayed-complete
        // behavior, including recursive Node -> unique_ptr<Node> layouts.
        return false;
      }
    }
    if (std::holds_alternative<LoweredClassDeclaration>(candidate.payload) ||
        std::holds_alternative<LoweredEnumDeclaration>(candidate.payload) ||
        std::holds_alternative<LoweredTypeAliasDeclaration>(
            candidate.payload)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool
semanticTypeContainsLexicalClosure(const SemanticType &type) {
  return type.kind == SemanticType::Lambda ||
         std::any_of(type.arguments.begin(), type.arguments.end(),
                     semanticTypeContainsLexicalClosure);
}

[[nodiscard]] bool
concreteClassIsCallableTemplateCarrier(const LoweredProgram &program,
                                       const SemanticType &type) {
  static_cast<void>(type);
  for (const MirFunctionInstance &instance :
       program.mir().functionInstances()) {
    const bool lexicalClosure =
        semanticTypeContainsLexicalClosure(instance.returnType) ||
        std::any_of(instance.parameterTypes.begin(),
                    instance.parameterTypes.end(),
                    semanticTypeContainsLexicalClosure) ||
        std::any_of(instance.typeArguments.begin(),
                    instance.typeArguments.end(),
                    semanticTypeContainsLexicalClosure);
    if (!lexicalClosure) {
      continue;
    }
    // One lexical closure requires a source-template overlay whose dependent
    // class graph can transitively mention other generic classes (for example
    // vector<T>::iterator). Until that overlay records an explicit concrete
    // carrier closure, keep the program's class family on the one audited
    // native carrier path rather than mixing direct and dependent class ABIs.
    return true;
  }
  return false;
}

[[nodiscard]] bool appendConcreteTypeIdentity(std::vector<std::uint8_t> &output,
                                              const LoweredProgram &program,
                                              const SemanticType &type) {
  const auto appendArguments = [&]() {
    appendIdentityInteger(output, type.arguments.size());
    for (const SemanticType &argument : type.arguments) {
      if (!appendConcreteTypeIdentity(output, program, argument)) {
        return false;
      }
    }
    appendIdentityInteger(output, type.valueArguments.size());
    for (const CompileTimeValue &argument : type.valueArguments) {
      if (!appendConcreteValueIdentity(output, argument)) {
        return false;
      }
    }
    return true;
  };

  switch (type.kind) {
  case SemanticType::Void:
    appendIdentityText(output, "void");
    return true;
  case SemanticType::Int8:
    appendIdentityText(output, "i8");
    return true;
  case SemanticType::Int16:
    appendIdentityText(output, "i16");
    return true;
  case SemanticType::Int32:
    appendIdentityText(output, "i32");
    return true;
  case SemanticType::Int64:
    appendIdentityText(output, "i64");
    return true;
  case SemanticType::UInt8:
    appendIdentityText(output, "u8");
    return true;
  case SemanticType::UInt16:
    appendIdentityText(output, "u16");
    return true;
  case SemanticType::UInt32:
    appendIdentityText(output, "u32");
    return true;
  case SemanticType::UInt64:
    appendIdentityText(output, "u64");
    return true;
  case SemanticType::Float:
    appendIdentityText(output, "f32");
    return true;
  case SemanticType::Double:
    appendIdentityText(output, "f64");
    return true;
  case SemanticType::Bool:
    appendIdentityText(output, "bool");
    return true;
  case SemanticType::Char:
    appendIdentityText(output, "char");
    return true;
  case SemanticType::StringView:
    appendIdentityText(output, "string-view");
    return true;
  case SemanticType::CString:
    appendIdentityText(output, "c-string");
    return true;
  case SemanticType::NullPtr:
    appendIdentityText(output, "null-pointer");
    return true;
  case SemanticType::RawPointer:
    appendIdentityText(output, "raw-pointer");
    appendIdentityInteger(output,
                          static_cast<std::uint64_t>(type.pointerAccess));
    return appendArguments();
  case SemanticType::Array:
    if (type.arrayLengthParameterId != 0) {
      return false;
    }
    appendIdentityText(output, "array");
    appendIdentityInteger(output, type.arrayLength);
    return appendArguments();
  case SemanticType::Class: {
    const LoweredClassDeclaration *declaration =
        program.findClassDeclaration(type.classId);
    const LoweredDeclaration *row = classDeclarationRow(program, type.classId);
    if (declaration == nullptr || row == nullptr) {
      return false;
    }
    appendIdentityText(output, "class");
    appendIdentityText(output, declaration->qualifiedName);
    // A compiler-private or same-spelled nominal declaration in another
    // source unit remains distinct without importing snapshot-local IDs.
    appendIdentityText(output, row->source.source);
    const std::vector<SemanticType> &arguments =
        declaration->exactSpecializationPrimary == 0
            ? type.arguments
            : declaration->exactTypeArguments;
    const std::vector<CompileTimeValue> &values =
        declaration->exactSpecializationPrimary == 0
            ? type.valueArguments
            : declaration->exactValueArguments;
    appendIdentityInteger(output, arguments.size());
    for (const SemanticType &argument : arguments) {
      if (!appendConcreteTypeIdentity(output, program, argument)) {
        return false;
      }
    }
    appendIdentityInteger(output, values.size());
    for (const CompileTimeValue &value : values) {
      if (!appendConcreteValueIdentity(output, value)) {
        return false;
      }
    }
    return true;
  }
  case SemanticType::Enum: {
    const LoweredEnumDeclaration *declaration =
        program.findEnumDeclaration(type.enumId);
    const LoweredDeclaration *row = enumDeclarationRow(program, type.enumId);
    if (declaration == nullptr || row == nullptr) {
      return false;
    }
    appendIdentityText(output, "enum");
    appendIdentityText(output, declaration->qualifiedName);
    appendIdentityText(output, row->source.source);
    return true;
  }
  case SemanticType::Reference:
    appendIdentityText(output, "reference");
    appendIdentityInteger(output,
                          static_cast<std::uint64_t>(type.referenceAccess));
    return appendArguments();
  case SemanticType::UniqueOwner:
    appendIdentityText(output, "unique-owner");
    return appendArguments();
  case SemanticType::Storage:
    appendIdentityText(output, "storage");
    return appendArguments();
  case SemanticType::PrefixStorage:
    appendIdentityText(output, "prefix-storage");
    return appendArguments();
  case SemanticType::TypePack:
    if (!type.concretePack) {
      return false;
    }
    appendIdentityText(output, "concrete-type-pack");
    return appendArguments();
  case SemanticType::NativeFunction:
    if (!type.hasNativeFunctionShape()) {
      return false;
    }
    appendIdentityText(output, "native-function");
    return appendArguments();
  case SemanticType::FunctionItem: {
    const LoweredFunctionDeclaration *declaration =
        program.findFunctionDeclaration(type.functionId);
    if (!type.hasFunctionItemShape() || declaration == nullptr ||
        declaration->ownerClass != 0 ||
        declaration->linkage != LanguageLinkage::Gti) {
      return false;
    }
    appendIdentityText(output, "function-item");
    appendIdentityText(output, declaration->qualifiedName);
    appendIdentityInteger(output, declaration->sourceUnit);
    return appendArguments();
  }
  case SemanticType::Expected:
    appendIdentityText(output, "expected");
    return appendArguments();
  case SemanticType::Unknown:
  case SemanticType::SharedPointer:
  case SemanticType::TypeParameter:
  case SemanticType::TypeName:
  case SemanticType::Function:
  case SemanticType::Lambda:
  case SemanticType::Unexpected:
    return false;
  }
  return false;
}

[[nodiscard]] std::string
identityDigestHex(const std::vector<std::uint8_t> &identity) {
  static constexpr std::array<char, 16> digits = {'0', '1', '2', '3', '4', '5',
                                                  '6', '7', '8', '9', 'a', 'b',
                                                  'c', 'd', 'e', 'f'};
  const std::array<std::uint8_t, 32> digest = llvm::SHA256::hash(
      llvm::ArrayRef<std::uint8_t>(identity.data(), identity.size()));
  std::string result;
  result.reserve(digest.size() * 2);
  for (const std::uint8_t byte : digest) {
    result.push_back(digits[byte >> 4U]);
    result.push_back(digits[byte & 0x0FU]);
  }
  return result;
}

} // namespace

std::string cppSemanticTypeSpelling(const LoweredProgram &program,
                                    CppStandard standard,
                                    const SemanticType &type) {
  const auto spell = [&](const SemanticType &inner) {
    return cppSemanticTypeSpelling(program, standard, inner);
  };
  switch (type.kind) {
  case SemanticType::Void:
    return "void";
  case SemanticType::Int8:
    return "std::int8_t";
  case SemanticType::Int16:
    return "std::int16_t";
  case SemanticType::Int32:
    return "std::int32_t";
  case SemanticType::Int64:
    return "std::int64_t";
  case SemanticType::UInt8:
    return "std::uint8_t";
  case SemanticType::UInt16:
    return "std::uint16_t";
  case SemanticType::UInt32:
    return "std::uint32_t";
  case SemanticType::UInt64:
    return "std::uint64_t";
  case SemanticType::Float:
    return "float";
  case SemanticType::Double:
    return "double";
  case SemanticType::Bool:
    return "bool";
  case SemanticType::Char:
    // Preserve GTI's distinct code-unit identity in native generic arguments.
    // uint8_t would alias UInt8 and collide in member specializations.
    return "char8_t";
  case SemanticType::StringView:
    return "std::string_view";
  case SemanticType::CString:
    return "const char*";
  case SemanticType::NullPtr:
    return "std::nullptr_t";
  case SemanticType::RawPointer: {
    std::string result;
    if (type.pointerAccess == AccessMode::ReadOnly) {
      result += "const ";
    }
    result += type.arguments.empty() ? "void" : spell(type.arguments.front());
    result += '*';
    return result;
  }
  case SemanticType::NativeFunction: {
    if (!type.hasNativeFunctionShape()) {
      return "void";
    }
    std::string result = "std::add_pointer_t<";
    result += spell(*type.nativeFunctionReturnType());
    result += '(';
    bool separator = false;
    for (const SemanticType &parameter : type.nativeFunctionParameterTypes()) {
      if (separator) {
        result += ", ";
      }
      result += spell(parameter);
      separator = true;
    }
    result += ")>";
    return result;
  }
  case SemanticType::FunctionItem:
    return type.hasFunctionItemShape()
               ? "::gti_internal::backend::function_item<" +
                     std::to_string(type.functionId) + ">"
               : "void";
  case SemanticType::Array: {
    std::string result = "std::array<";
    result += type.arguments.empty() ? "void" : spell(type.arguments.front());
    result += ", ";
    if (type.arrayLengthParameterId != 0) {
      const LoweredGenericParameter *parameter =
          program.findGenericParameter(type.arrayLengthParameterId);
      result += parameter == nullptr ? "0" : parameter->name;
    } else {
      result += std::to_string(type.arrayLength);
    }
    result += '>';
    return result;
  }
  case SemanticType::Class: {
    const LoweredClassDeclaration *classInfo =
        program.findClassDeclaration(type.classId);
    if (classInfo == nullptr) {
      return "void";
    }
    if (classInfo->compilerCapability ==
        CompilerCapabilityTypeKind::ThreadHandle) {
      return "::gti_internal::backend::thread_handle";
    }
    if (classInfo->compilerCapability ==
        CompilerCapabilityTypeKind::AtomicStorage) {
      if (type.arguments.size() != 1) {
        return "void";
      }
      return "::gti_internal::backend::atomic_value<" +
             spell(type.arguments.front()) + ">";
    }
    if (classInfo->compilerCapability ==
        CompilerCapabilityTypeKind::OptionalStorage) {
      if (type.arguments.size() != 1) {
        return "void";
      }
      return "::gti_internal::backend::optional_storage<" +
             spell(type.arguments.front()) + ">";
    }
    if (classInfo->compilerCapability ==
            CompilerCapabilityTypeKind::MutexStorage ||
        classInfo->compilerCapability ==
            CompilerCapabilityTypeKind::MutexLockToken) {
      if (type.arguments.size() != 1) {
        return "void";
      }
      return std::string{"::gti_internal::backend::"} +
             (classInfo->compilerCapability ==
                      CompilerCapabilityTypeKind::MutexStorage
                  ? "mutex_value<"
                  : "mutex_lock_token<") +
             spell(type.arguments.front()) + ">";
    }
    if (const std::optional<std::string> concrete =
            cppConcreteClassTypeSpelling(program, type)) {
      return *concrete;
    }
    std::string result = cppLoweredQualifiedName(classInfo->qualifiedName,
                                                 classInfo->cOpaqueHandle);
    const std::vector<SemanticType> &typeArguments =
        classInfo->exactSpecializationPrimary == 0
            ? type.arguments
            : classInfo->exactTypeArguments;
    const std::vector<CompileTimeValue> &valueArguments =
        classInfo->exactSpecializationPrimary == 0
            ? type.valueArguments
            : classInfo->exactValueArguments;
    if (!typeArguments.empty() || !valueArguments.empty()) {
      result += '<';
      bool separator = false;
      for (const SemanticType &argument : typeArguments) {
        if (separator) {
          result += ", ";
        }
        result += spell(argument);
        separator = true;
      }
      for (const CompileTimeValue &argument : valueArguments) {
        if (separator) {
          result += ", ";
        }
        if (argument.kind == CompileTimeValue::UInt64) {
          result += std::to_string(argument.value);
        } else if (argument.kind == CompileTimeValue::Parameter) {
          const LoweredGenericParameter *parameter =
              program.findGenericParameter(argument.parameterId);
          result += parameter == nullptr ? "0" : parameter->name;
        } else {
          result += '0';
        }
        separator = true;
      }
      result += '>';
    }
    return result;
  }
  case SemanticType::Enum: {
    const LoweredEnumDeclaration *enumInfo =
        program.findEnumDeclaration(type.enumId);
    return enumInfo == nullptr
               ? "void"
               : cppLoweredQualifiedName(enumInfo->qualifiedName, false);
  }
  case SemanticType::Reference: {
    std::string result;
    if (type.referenceAccess == AccessMode::ReadOnly) {
      result += "const ";
    }
    result += type.arguments.empty() ? "void" : spell(type.arguments.front());
    result += " &";
    return result;
  }
  case SemanticType::UniqueOwner: {
    std::string result = "std::unique_ptr<";
    if (!type.arguments.empty()) {
      result += spell(type.arguments.front());
    }
    result += '>';
    return result;
  }
  case SemanticType::PrefixStorage: {
    std::string result = "::gti_internal::backend::prefix_storage<";
    if (!type.arguments.empty()) {
      result += spell(type.arguments.front());
    }
    result += '>';
    return result;
  }
  case SemanticType::Storage: {
    std::string result = "::gti_internal::backend::storage<";
    if (!type.arguments.empty()) {
      result += spell(type.arguments.front());
    }
    result += '>';
    return result;
  }
  case SemanticType::TypeParameter:
  case SemanticType::TypePack: {
    const LoweredGenericParameter *parameter =
        program.findGenericParameter(type.genericParameterId);
    std::string result = parameter == nullptr ? "void" : parameter->name;
    if (type.kind == SemanticType::TypePack && type.concretePack) {
      result += "...";
    }
    return result;
  }
  case SemanticType::Expected: {
    std::string result = standard == CppStandard::Cpp23 ? "std::expected<"
                                                        : "::nonstd::expected<";
    if (type.arguments.size() == 2) {
      result += spell(type.arguments[0]);
      result += ", ";
      result += spell(type.arguments[1]);
    }
    result += '>';
    return result;
  }
  default:
    return "void";
  }
}

std::optional<std::string> cppConcreteClassName(const LoweredProgram &program,
                                                const SemanticType &type) {
  if (type.kind != SemanticType::Class || type.classId == 0) {
    return std::nullopt;
  }
  const LoweredClassDeclaration *declaration =
      program.findClassDeclaration(type.classId);
  if (declaration == nullptr) {
    return std::nullopt;
  }
  const LoweredClassDeclaration *primary =
      concreteClassPrimary(program, *declaration);
  const LoweredDeclaration *primaryRow =
      concreteClassPrimaryRow(program, *declaration);
  if (primary == nullptr || primaryRow == nullptr ||
      primary->genericParameters.empty() ||
      !concreteClassSupportsDirectRepresentation(program, *declaration,
                                                 *primaryRow) ||
      concreteClassIsCallableTemplateCarrier(program, type)) {
    return std::nullopt;
  }

  const std::vector<SemanticType> &arguments =
      declaration->exactSpecializationPrimary == 0
          ? type.arguments
          : declaration->exactTypeArguments;
  const std::vector<CompileTimeValue> &values =
      declaration->exactSpecializationPrimary == 0
          ? type.valueArguments
          : declaration->exactValueArguments;

  std::vector<std::uint8_t> identity;
  appendIdentityText(identity, "GTI-CONCRETE-CLASS-V1");
  appendIdentityText(identity, primary->qualifiedName);
  appendIdentityText(identity, primaryRow->source.source);
  appendIdentityInteger(identity, primary->genericParameters.size());
  for (const LoweredGenericParameter &parameter : primary->genericParameters) {
    appendIdentityInteger(identity, parameter.value ? 1 : 0);
    appendIdentityInteger(identity, parameter.pack ? 1 : 0);
  }
  appendIdentityInteger(identity, arguments.size());
  for (const SemanticType &argument : arguments) {
    if (!appendConcreteTypeIdentity(identity, program, argument)) {
      return std::nullopt;
    }
  }
  appendIdentityInteger(identity, values.size());
  for (const CompileTimeValue &value : values) {
    if (!appendConcreteValueIdentity(identity, value)) {
      return std::nullopt;
    }
  }
  return "__gti_type_" + identityDigestHex(identity) + "_" + primaryRow->name;
}

std::optional<std::string>
cppConcreteClassTypeSpelling(const LoweredProgram &program,
                             const SemanticType &type) {
  const std::optional<std::string> name = cppConcreteClassName(program, type);
  const LoweredClassDeclaration *declaration =
      type.kind == SemanticType::Class
          ? program.findClassDeclaration(type.classId)
          : nullptr;
  const LoweredClassDeclaration *primary =
      declaration == nullptr ? nullptr
                             : concreteClassPrimary(program, *declaration);
  if (!name || primary == nullptr) {
    return std::nullopt;
  }
  const std::size_t separator = primary->qualifiedName.rfind("::");
  const std::string qualified =
      separator == std::string::npos
          ? *name
          : primary->qualifiedName.substr(0, separator + 2) + *name;
  return cppLoweredQualifiedName(qualified, false);
}

std::string cppFunctionSpelling(const LoweredFunctionDeclaration &function,
                                std::string_view sourceName) {
  if (function.overloadedOperator) {
    switch (*function.overloadedOperator) {
    case OverloadedOperator::Equal:
    case OverloadedOperator::NotEqual:
    case OverloadedOperator::Less:
    case OverloadedOperator::LessEqual:
    case OverloadedOperator::Greater:
    case OverloadedOperator::GreaterEqual:
    case OverloadedOperator::Assignment:
      return std::string(operatorSourceSpelling(*function.overloadedOperator));
    case OverloadedOperator::Dereference:
    case OverloadedOperator::PreIncrement:
    case OverloadedOperator::PreDecrement:
    case OverloadedOperator::Plus:
    case OverloadedOperator::Minus:
    case OverloadedOperator::Multiply:
    case OverloadedOperator::Divide:
    case OverloadedOperator::Remainder:
    case OverloadedOperator::AddAssign:
    case OverloadedOperator::SubtractAssign:
    case OverloadedOperator::MultiplyAssign:
    case OverloadedOperator::DivideAssign:
    case OverloadedOperator::RemainderAssign:
    case OverloadedOperator::BitwiseNot:
    case OverloadedOperator::BitwiseAnd:
    case OverloadedOperator::BitwiseOr:
    case OverloadedOperator::BitwiseXor:
    case OverloadedOperator::ShiftLeft:
    case OverloadedOperator::ShiftRight:
    case OverloadedOperator::BitwiseAndAssign:
    case OverloadedOperator::BitwiseOrAssign:
    case OverloadedOperator::BitwiseXorAssign:
    case OverloadedOperator::ShiftLeftAssign:
    case OverloadedOperator::ShiftRightAssign:
    case OverloadedOperator::Arrow:
    case OverloadedOperator::Subscript:
    case OverloadedOperator::Call:
    case OverloadedOperator::ContextualBool:
      break;
    }
  }
  if (function.definitionKind == MirDefinitionKind::RuntimeBinding) {
    return std::string(sourceName);
  }
  if (function.linkage == LanguageLinkage::C) {
    return function.externalSymbol.empty() ? std::string(sourceName)
                                           : function.externalSymbol;
  }
  if (function.entryKind != ProgramEntryKind::None &&
      function.returnType == SemanticType::Int32) {
    return "__gti_entry";
  }
  if (function.id == 0 || function.virtualMethod) {
    return std::string(sourceName);
  }
  return "__gti_fn_" + std::to_string(function.id) + "_" +
         std::string(sourceName);
}

bool cppConcreteFunctionInstanceIsNameable(
    const LoweredProgram &program, const LoweredFunctionInstance &instance,
    const MirFunctionInstance &mirInstance) {
  if (instance.id == 0 || instance.id != mirInstance.id ||
      instance.declaration == 0 ||
      instance.declaration != mirInstance.declaration ||
      instance.owner != mirInstance.owner ||
      instance.typeArguments != mirInstance.typeArguments ||
      instance.valueArguments != mirInstance.valueArguments) {
    return false;
  }
  std::vector<std::uint8_t> identity;
  for (const SemanticType &argument : instance.typeArguments) {
    if (!appendConcreteTypeIdentity(identity, program, argument)) {
      return false;
    }
  }
  for (const CompileTimeValue &argument : instance.valueArguments) {
    if (!appendConcreteValueIdentity(identity, argument)) {
      return false;
    }
  }
  if (!appendConcreteTypeIdentity(identity, program, mirInstance.returnType)) {
    return false;
  }
  for (const SemanticType &parameter : mirInstance.parameterTypes) {
    if (!appendConcreteTypeIdentity(identity, program, parameter)) {
      return false;
    }
  }
  return true;
}

std::optional<std::string>
cppConcreteFreeFunctionSpelling(const LoweredProgram &program,
                                const LoweredDeclaration &row,
                                const LoweredFunctionDeclaration &function,
                                const LoweredFunctionInstance &instance,
                                const MirFunctionInstance &mirInstance) {
  const auto *rowFunction =
      std::get_if<LoweredFunctionDeclaration>(&row.payload);
  if (rowFunction == nullptr || rowFunction->id != function.id ||
      function.id == 0 || function.ownerClass != 0 ||
      function.genericParameters.empty() ||
      function.definitionKind != MirDefinitionKind::Source ||
      function.linkage != LanguageLinkage::Gti ||
      function.intrinsic != IntrinsicKind::None || instance.owner ||
      mirInstance.owner || instance.declaration != function.id ||
      mirInstance.declaration != function.id ||
      !cppConcreteFunctionInstanceIsNameable(program, instance, mirInstance)) {
    return std::nullopt;
  }

  std::vector<std::uint8_t> identity;
  appendIdentityText(identity, "GTI-CONCRETE-FREE-FUNCTION-V1");
  appendIdentityText(identity, function.qualifiedName);
  appendIdentityText(identity, row.name);
  appendIdentityInteger(identity, function.internalLinkage ? 1 : 0);
  if (function.internalLinkage) {
    appendIdentityText(identity, row.source.source);
  }
  appendIdentityInteger(identity, function.genericParameters.size());
  for (const LoweredGenericParameter &parameter : function.genericParameters) {
    appendIdentityInteger(identity, parameter.value ? 1 : 0);
    appendIdentityInteger(identity, parameter.pack ? 1 : 0);
  }
  appendIdentityInteger(identity, instance.typeArguments.size());
  for (const SemanticType &argument : instance.typeArguments) {
    if (!appendConcreteTypeIdentity(identity, program, argument)) {
      return std::nullopt;
    }
  }
  appendIdentityInteger(identity, instance.valueArguments.size());
  for (const CompileTimeValue &argument : instance.valueArguments) {
    if (!appendConcreteValueIdentity(identity, argument)) {
      return std::nullopt;
    }
  }
  if (!appendConcreteTypeIdentity(identity, program, mirInstance.returnType)) {
    return std::nullopt;
  }
  appendIdentityInteger(identity, mirInstance.parameterTypes.size());
  for (const SemanticType &parameter : mirInstance.parameterTypes) {
    if (!appendConcreteTypeIdentity(identity, program, parameter)) {
      return std::nullopt;
    }
  }
  return "__gti_inst_" + identityDigestHex(identity) + "_" + row.name;
}

std::optional<std::string>
cppConcreteMemberFunctionSpelling(const LoweredProgram &program,
                                  const LoweredDeclaration &row,
                                  const LoweredFunctionDeclaration &function,
                                  const LoweredFunctionInstance &instance,
                                  const MirFunctionInstance &mirInstance) {
  const auto *rowFunction =
      std::get_if<LoweredFunctionDeclaration>(&row.payload);
  if (rowFunction == nullptr || rowFunction->id != function.id ||
      function.id == 0 || function.ownerClass == 0 ||
      function.definitionKind != MirDefinitionKind::Source ||
      function.linkage != LanguageLinkage::Gti ||
      function.intrinsic != IntrinsicKind::None || !instance.owner ||
      !mirInstance.owner || instance.declaration != function.id ||
      mirInstance.declaration != function.id ||
      !cppConcreteFunctionInstanceIsNameable(program, instance, mirInstance)) {
    return std::nullopt;
  }
  const MirClassInstance *owner =
      program.mir().findClassInstance(*mirInstance.owner);
  if (owner == nullptr || owner->id != *instance.owner ||
      (!cppConcreteClassName(program, owner->type) &&
       function.genericParameters.empty())) {
    return std::nullopt;
  }
  if (function.overloadedOperator || function.virtualMethod) {
    return cppFunctionSpelling(function, row.name);
  }

  std::vector<std::uint8_t> identity;
  appendIdentityText(identity, "GTI-CONCRETE-MEMBER-FUNCTION-V1");
  if (!appendConcreteTypeIdentity(identity, program, owner->type)) {
    return std::nullopt;
  }
  appendIdentityText(identity, function.qualifiedName);
  appendIdentityText(identity, row.name);
  appendIdentityInteger(
      identity, static_cast<std::uint64_t>(function.receiverMutability));
  appendIdentityInteger(identity, function.staticMember ? 1 : 0);
  appendIdentityInteger(identity, function.genericParameters.size());
  for (const LoweredGenericParameter &parameter : function.genericParameters) {
    appendIdentityInteger(identity, parameter.value ? 1 : 0);
    appendIdentityInteger(identity, parameter.pack ? 1 : 0);
  }
  appendIdentityInteger(identity, instance.typeArguments.size());
  for (const SemanticType &argument : instance.typeArguments) {
    if (!appendConcreteTypeIdentity(identity, program, argument)) {
      return std::nullopt;
    }
  }
  appendIdentityInteger(identity, instance.valueArguments.size());
  for (const CompileTimeValue &argument : instance.valueArguments) {
    if (!appendConcreteValueIdentity(identity, argument)) {
      return std::nullopt;
    }
  }
  if (!appendConcreteTypeIdentity(identity, program, mirInstance.returnType)) {
    return std::nullopt;
  }
  appendIdentityInteger(identity, mirInstance.parameterTypes.size());
  for (const SemanticType &parameter : mirInstance.parameterTypes) {
    if (!appendConcreteTypeIdentity(identity, program, parameter)) {
      return std::nullopt;
    }
  }
  return "__gti_member_" + identityDigestHex(identity) + "_" + row.name;
}

// Ported verbatim from the compatibility emitter's
// emittedStaticVariableBaseName / emittedStaticVariableName, which now
// delegate here.
std::string cppStaticStorageBaseSpelling(SymbolId symbol,
                                         std::string_view name) {
  return "__gti_static_" + std::to_string(symbol) + "_" + std::string(name);
}

std::string cppStaticStorageValueSpelling(SymbolId symbol,
                                          std::string_view name) {
  return cppStaticStorageBaseSpelling(symbol, name) + "::value";
}

} // namespace lang
