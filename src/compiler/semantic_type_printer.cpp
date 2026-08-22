#include "gti/semantic_analyzer.h"

namespace lang {

class SemanticTypePrinterImpl {
public:
  explicit SemanticTypePrinterImpl(const SemanticModel &semantics)
      : semantics(semantics) {}

  [[nodiscard]] std::string print(const SemanticType &type) const {
    switch (type.kind) {
    case SemanticType::Unknown:
      return "unknown";
    case SemanticType::Void:
      return "void";
    case SemanticType::Int8:
      return "int8_t";
    case SemanticType::Int16:
      return "int16_t";
    case SemanticType::Int32:
      return "int32_t";
    case SemanticType::Int64:
      return "int64_t";
    case SemanticType::UInt8:
      return "uint8_t";
    case SemanticType::UInt16:
      return "uint16_t";
    case SemanticType::UInt32:
      return "uint32_t";
    case SemanticType::UInt64:
      return "uint64_t";
    case SemanticType::Float:
      return "float";
    case SemanticType::Double:
      return "double";
    case SemanticType::Bool:
      return "bool";
    case SemanticType::Char:
      return "char";
    case SemanticType::StringView:
      return "std::string_view";
    case SemanticType::CString:
      return "c_string";
    case SemanticType::NullPtr:
      return "nullptr_t";
    case SemanticType::RawPointer:
      if (type.arguments.size() == 1) {
        return (type.pointerAccess == AccessMode::ReadOnly ? "const " : "") +
               print(type.arguments.front()) + "*";
      }
      return "raw pointer";
    case SemanticType::Array:
      if (type.arguments.size() == 1) {
        return print(type.arguments.front()) + "[" + arrayExtent(type) + "]";
      }
      return "array";
    case SemanticType::Class:
      return classType(type);
    case SemanticType::Enum:
      if (const EnumTypeInfo *info = semantics.findEnumType(type.enumId)) {
        return info->qualifiedName;
      }
      return "unknown enum";
    case SemanticType::Reference:
      if (type.arguments.size() == 1) {
        return (type.referenceAccess == AccessMode::Mutable ? "mut " : "") +
               print(type.arguments.front()) + "&";
      }
      return "reference";
    case SemanticType::UniqueOwner:
      return unaryType("gti_internal::unique_owner", type);
    case SemanticType::SharedPointer:
      return unaryType("std::shared_ptr", type);
    case SemanticType::Storage:
      return unaryType("gti_internal::storage", type);
    case SemanticType::PrefixStorage:
      return unaryType("gti_internal::prefix_storage", type);
    case SemanticType::TypeParameter:
      return genericParameter(type.genericParameterId, false);
    case SemanticType::TypePack:
      return genericParameter(type.genericParameterId, true);
    case SemanticType::TypeName:
      if (const ClassTypeInfo *info = semantics.findClassType(type.classId)) {
        return info->qualifiedName;
      }
      return "type";
    case SemanticType::Function:
      return "function";
    case SemanticType::NativeFunction: {
      if (!type.hasNativeFunctionShape()) {
        return "native function";
      }
      std::string result = "(";
      bool separator = false;
      for (const SemanticType &parameter :
           type.nativeFunctionParameterTypes()) {
        if (separator) {
          result += ", ";
        }
        result += print(parameter);
        separator = true;
      }
      result += ") -> ";
      result += print(*type.nativeFunctionReturnType());
      return result;
    }
    case SemanticType::Lambda:
      return "lambda";
    case SemanticType::Expected:
      if (type.arguments.size() == 2) {
        return "expected<" + print(type.arguments[0]) + ", " +
               print(type.arguments[1]) + ">";
      }
      return "expected";
    case SemanticType::Unexpected:
      return unaryType("unexpected", type);
    }
    return "unknown";
  }

private:
  [[nodiscard]] std::string classType(const SemanticType &type) const {
    const ClassTypeInfo *info = semantics.findClassType(type.classId);
    if (info == nullptr) {
      return "unknown class";
    }
    std::string result = info->qualifiedName;
    const std::vector<SemanticType> &typeArguments =
        info->exactSpecializationPrimary == 0 ? type.arguments
                                              : info->exactTypeArguments;
    const std::vector<CompileTimeValue> &valueArguments =
        info->exactSpecializationPrimary == 0 ? type.valueArguments
                                              : info->exactValueArguments;
    if (typeArguments.empty() && valueArguments.empty()) {
      return result;
    }
    result += '<';
    bool first = true;
    for (const SemanticType &argument : typeArguments) {
      if (!first) {
        result += ", ";
      }
      first = false;
      result += print(argument);
    }
    for (const CompileTimeValue &argument : valueArguments) {
      if (!first) {
        result += ", ";
      }
      first = false;
      result += value(argument);
    }
    result += '>';
    return result;
  }

  [[nodiscard]] std::string unaryType(std::string_view name,
                                      const SemanticType &type) const {
    return type.arguments.size() == 1
               ? std::string(name) + '<' + print(type.arguments.front()) + '>'
               : std::string(name);
  }

  [[nodiscard]] std::string arrayExtent(const SemanticType &type) const {
    return type.arrayLengthParameterId == 0
               ? std::to_string(type.arrayLength)
               : genericParameter(type.arrayLengthParameterId, false);
  }

  [[nodiscard]] std::string value(const CompileTimeValue &value) const {
    if (value.kind == CompileTimeValue::UInt64) {
      return std::to_string(value.value);
    }
    if (value.kind == CompileTimeValue::Parameter) {
      return genericParameter(value.parameterId, false);
    }
    return "unknown value";
  }

  [[nodiscard]] std::string genericParameter(GenericParameterId id,
                                             bool pack) const {
    const GenericParameterInfo *parameter = semantics.findGenericParameter(id);
    if (parameter == nullptr) {
      return pack ? "type pack" : "type parameter";
    }
    return parameter->name.lexeme + (pack ? "..." : "");
  }

  const SemanticModel &semantics;
};

SemanticTypePrinter::SemanticTypePrinter(const SemanticModel &semantics)
    : semantics(semantics) {}

std::string SemanticTypePrinter::print(const SemanticType &type) const {
  return SemanticTypePrinterImpl(semantics).print(type);
}

} // namespace lang
