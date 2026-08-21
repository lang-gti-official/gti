#include "cpp_representation.h"

namespace lang {

// Ported verbatim from the compatibility emitter's emitSemanticType, which
// now delegates here. Every branch, fallback, and ordering choice is the
// emitter's historical behavior; changing a spelling is a representation
// change and must go through the family and oracle gates.
std::string cppSemanticTypeSpelling(const SemanticModel &semantics,
                                    CppStandard standard,
                                    const SemanticType &type) {
  const auto spell = [&](const SemanticType &inner) {
    return cppSemanticTypeSpelling(semantics, standard, inner);
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
    return "std::uint8_t";
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
  case SemanticType::Array: {
    std::string result = "std::array<";
    result += type.arguments.empty() ? "void" : spell(type.arguments.front());
    result += ", ";
    if (type.arrayLengthParameterId != 0) {
      const GenericParameterInfo *parameter =
          semantics.findGenericParameter(type.arrayLengthParameterId);
      result += parameter == nullptr ? "0" : parameter->name.lexeme;
    } else {
      result += std::to_string(type.arrayLength);
    }
    result += '>';
    return result;
  }
  case SemanticType::Class: {
    const ClassTypeInfo *classInfo = semantics.findClassType(type.classId);
    if (classInfo == nullptr || classInfo->declaration == nullptr) {
      return "void";
    }
    std::string result = classInfo->cOpaqueHandle ? "::" : "::__gti_program::";
    for (const std::string &scope : classInfo->namespaceScope) {
      result +=
          scope == "std" ? std::string(cppEmittedStandardNamespace) : scope;
      result += "::";
    }
    result += classInfo->declaration->name().lexeme;
    if (!type.arguments.empty() || !type.valueArguments.empty()) {
      result += '<';
      bool separator = false;
      for (const SemanticType &argument : type.arguments) {
        if (separator) {
          result += ", ";
        }
        result += spell(argument);
        separator = true;
      }
      for (const CompileTimeValue &argument : type.valueArguments) {
        if (separator) {
          result += ", ";
        }
        if (argument.kind == CompileTimeValue::UInt64) {
          result += std::to_string(argument.value);
        } else if (argument.kind == CompileTimeValue::Parameter) {
          const GenericParameterInfo *parameter =
              semantics.findGenericParameter(argument.parameterId);
          result += parameter == nullptr ? "0" : parameter->name.lexeme;
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
    const EnumTypeInfo *enumInfo = semantics.findEnumType(type.enumId);
    if (enumInfo == nullptr || enumInfo->declaration == nullptr) {
      return "void";
    }
    std::string result = "::__gti_program::";
    for (const std::string &scope : enumInfo->namespaceScope) {
      result +=
          scope == "std" ? std::string(cppEmittedStandardNamespace) : scope;
      result += "::";
    }
    result += enumInfo->declaration->name().lexeme;
    return result;
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
    const GenericParameterInfo *parameter =
        semantics.findGenericParameter(type.genericParameterId);
    std::string result = parameter == nullptr ? "void" : parameter->name.lexeme;
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

// Ported verbatim from the compatibility emitter's emittedFunctionName,
// which now delegates here.
std::string cppFunctionSpelling(const SemanticModel &semantics,
                                const FunctionDecl &function) {
  if (function.operatorName()) {
    switch (function.operatorName()->kind) {
    case OverloadedOperator::Equal:
    case OverloadedOperator::NotEqual:
    case OverloadedOperator::Less:
    case OverloadedOperator::LessEqual:
    case OverloadedOperator::Greater:
    case OverloadedOperator::GreaterEqual:
    case OverloadedOperator::Assignment:
      return std::string(operatorSourceSpelling(function.operatorName()->kind));
    case OverloadedOperator::Dereference:
    case OverloadedOperator::PreIncrement:
    case OverloadedOperator::Arrow:
    case OverloadedOperator::Subscript:
    case OverloadedOperator::Call:
    case OverloadedOperator::ContextualBool:
      break;
    }
  }
  if (function.runtimeBinding()) {
    return function.name().lexeme;
  }
  const FunctionInfo *info = semantics.findFunction(function);
  if (function.hasCLinkage()) {
    return info != nullptr && !info->externalSymbol.empty()
               ? info->externalSymbol
               : function.name().lexeme;
  }
  if (info != nullptr && info->entryPoint &&
      info->returnType == SemanticType::Int32) {
    return "__gti_entry";
  }
  if (info == nullptr || info->id == 0) {
    return function.name().lexeme;
  }
  if (info->virtualMethod) {
    return function.name().lexeme;
  }
  return "__gti_fn_" + std::to_string(info->id) + "_" + function.name().lexeme;
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
