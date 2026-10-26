#include "gti/native_header.h"

#include "gti/lowered_program.h"

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lang {
namespace {

struct NativeRecord {
  const LoweredDeclaration *declaration = nullptr;
  const LoweredClassDeclaration *info = nullptr;
  std::string cName;
};

using NativeOpaqueHandle = NativeRecord;

struct NativeFunction {
  const LoweredDeclaration *declaration = nullptr;
  const LoweredFunctionDeclaration *info = nullptr;
};

struct NativeVariable {
  const LoweredDeclaration *declaration = nullptr;
  const LoweredStorageDeclaration *info = nullptr;
};

struct NativeCallbackAlias {
  const LoweredDeclaration *declaration = nullptr;
  const LoweredTypeAliasDeclaration *info = nullptr;
  std::string cName;
};

std::string emittedScope(std::string_view scope, std::size_t index) {
  return index == 0 && scope == "std" ? "__gti_std" : std::string(scope);
}

std::string qualifiedCppName(const NativeRecord &record) {
  // Opaque handles are completed by the C++ consumer, so their public source
  // name must remain the actual type rather than an alias to a private type
  // that user code could not define. Passive records are compiler-defined and
  // can safely expose a familiar alias to their isolated representation.
  std::string result = record.info->cOpaqueHandle ? "::" : "::__gti_program::";
  for (std::size_t index = 0; index < record.declaration->namespaceScope.size();
       ++index) {
    result += emittedScope(record.declaration->namespaceScope[index], index);
    result += "::";
  }
  result += record.declaration->name;
  return result;
}

std::string encodedCRecordName(const LoweredClassDeclaration &info) {
  std::ostringstream output;
  output << "gti_cabi_";
  for (const unsigned char byte : info.qualifiedName) {
    output << std::hex << std::setw(2) << std::setfill('0')
           << static_cast<unsigned int>(byte);
  }
  return output.str();
}

std::string encodedCCallbackName(const LoweredTypeAliasDeclaration &info) {
  std::ostringstream output;
  output << "gti_cabi_callback_";
  for (const unsigned char byte : info.qualifiedName) {
    output << std::hex << std::setw(2) << std::setfill('0')
           << static_cast<unsigned int>(byte);
  }
  return output.str();
}

void openCppNamespaces(std::ostream &output,
                       const std::vector<std::string> &scopes) {
  output << "namespace __gti_program {\n";
  for (std::size_t index = 0; index < scopes.size(); ++index) {
    output << "namespace " << emittedScope(scopes[index], index) << " {\n";
  }
}

void closeCppNamespaces(std::ostream &output,
                        const std::vector<std::string> &scopes) {
  for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
    output << "} // namespace "
           << emittedScope(*scope, static_cast<std::size_t>(
                                       std::distance(scope, scopes.rend()) - 1))
           << "\n";
  }
  output << "} // namespace __gti_program\n";
}

void openPublicCppNamespaces(std::ostream &output,
                             const std::vector<std::string> &scopes) {
  for (std::size_t index = 0; index < scopes.size(); ++index) {
    output << "namespace " << emittedScope(scopes[index], index) << " {\n";
  }
}

void closePublicCppNamespaces(std::ostream &output,
                              const std::vector<std::string> &scopes) {
  for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
    output << "} // namespace "
           << emittedScope(*scope, static_cast<std::size_t>(
                                       std::distance(scope, scopes.rend()) - 1))
           << "\n";
  }
}

class NativeHeaderEmitter {
public:
  explicit NativeHeaderEmitter(const LoweredProgram &program)
      : program(program) {}

  std::string signature(const LoweredEmbeddingExport &selected) {
    collect();
    assignCNames();
    std::ostringstream output;
    output << "gti-embedding-signature-v1;" << program.target().os << ';'
           << program.target().arch << ';';
    std::unordered_set<ClassId> visited;
    const auto type = [&](const auto &self, const SemanticType &value) -> void {
      if (value.kind == SemanticType::Array)
        output << "array[" << value.arrayLength << "];";
      else
        output << cType(value) << ';';
      for (const auto &argument : value.arguments)
        self(self, argument);
      if (value.kind != SemanticType::Class ||
          !visited.insert(value.classId).second)
        return;
      const auto &record = nativeTypeFor(value.classId);
      output << record.info->qualifiedName << ';'
             << (record.info->kind == ClassKind::Union ? "union;" : "record;");
      if (record.info->cAbiLayout) {
        const auto &layout = *record.info->cAbiLayout;
        output << layout.sizeBytes << ':' << layout.abiAlignmentBytes << ';';
        for (const auto &field : layout.fields) {
          output << field.offsetBytes << ':';
          self(self, field.type);
        }
      }
    };
    type(type, selected.returnType);
    output << "parameters=" << selected.parameterTypes.size() << ';';
    for (const auto &parameter : selected.parameterTypes)
      type(type, parameter);
    return output.str();
  }

  std::string emit() {
    collect();
    assignCNames();
    orderCallbackAliases();
    orderRecords();

    std::ostringstream body;
    body << "/* Generated by GTI. Do not edit.\n"
            " * This header is a C ABI boundary and is valid in both C17 and "
            "C++20/C++23.\n"
            " * Opaque handles are incomplete nominal types and may cross "
            "only by pointer.\n"
            " * C++ implementations may wrap arbitrary C++ internals, but "
            "must not let exceptions cross these functions.\n"
            " * Define GTI_NATIVE_HEADER_NO_SOURCE_NAMES before inclusion to "
            "suppress optional C++ record aliases and declare C functions and "
            "objects "
            "only in GTI's isolated namespace. Opaque handles retain their "
            "exact incomplete source identity.\n"
            " */\n\n"
            "#include <stddef.h>\n"
            "#include <stdint.h>\n"
            "#include <gti/c_abi.h>\n"
            "#ifdef __cplusplus\n"
            "#include <cstddef>\n"
            "#include <cstdint>\n"
            "#include <type_traits>\n"
            "#endif\n\n";

    if (program.embeddingPlan()) {
      body << "#include <gti/embedding.h>\n\n";
      body << "#ifdef __cplusplus\nextern \"C\" {\n";
      emitCBranch(body, true);
      body << "}\n#else\n";
      emitCBranch(body);
    } else {
      emitCppBranch(body);
      body << "#else\n\n";
      emitCBranch(body);
    }
    body << "#endif\n";

    const std::string contents = body.str();
    const std::string guard = headerGuard(contents);
    return "#ifndef " + guard + "\n#define " + guard + "\n\n" + contents +
           "\n#endif /* " + guard + " */\n";
  }

private:
  void collect() {
    for (const LoweredDeclaration &declaration : program.declarations()) {
      if (declaration.ownerClass != 0) {
        continue;
      }
      if (const auto *alias =
              std::get_if<LoweredTypeAliasDeclaration>(&declaration.payload);
          alias != nullptr &&
          alias->type.kind == SemanticType::NativeFunction &&
          !alias->compilerPrivate) {
        callbackAliases.push_back({.declaration = &declaration, .info = alias});
        continue;
      }
      if (const auto *classDeclaration =
              std::get_if<LoweredClassDeclaration>(&declaration.payload)) {
        if (classDeclaration->cAbiRecord && classDeclaration->cAbiLayout &&
            !classDeclaration->compilerPrivate) {
          const std::size_t index = records.size();
          records.push_back(
              {.declaration = &declaration, .info = classDeclaration});
          recordByClass.emplace(classDeclaration->id, index);
        } else if (classDeclaration->cOpaqueHandle &&
                   !classDeclaration->compilerPrivate) {
          const std::size_t index = opaqueHandles.size();
          opaqueHandles.push_back(
              {.declaration = &declaration, .info = classDeclaration});
          opaqueByClass.emplace(classDeclaration->id, index);
        }
        continue;
      }
      if (const auto *storage =
              std::get_if<LoweredStorageDeclaration>(&declaration.payload);
          storage != nullptr && !storage->externalSymbol.empty() && !program.embeddingPlan()) {
        variables.push_back({.declaration = &declaration, .info = storage});
        continue;
      }
      if (const auto *function =
              std::get_if<LoweredFunctionDeclaration>(&declaration.payload);
          function != nullptr && function->linkage == LanguageLinkage::C &&
          !function->compilerPrivate && !program.embeddingPlan()) {
        functions.push_back({.declaration = &declaration, .info = function});
      }
    }
  }

  void assignCNames() {
    std::unordered_set<std::string> used;
    if (program.embeddingPlan()) {
      used = {"gti_context_v1", "gti_embed_status_v1", "gti_export_descriptor_v1",
              "gti_module_descriptor_v1", "gti_failure_record_v1",
              "gti_failure_artifact_descriptor_v1", "gti_failure_observer_v1",
              "gti_c_string_view"};
      for (const auto &selected : program.embeddingPlan()->exports) {
        if (!used.insert(selected.symbol).second)
          throw BackendUnsupportedError("embedding export collides with a public ABI type: " + selected.symbol);
      }
    }
    const auto assignRoots = [&](auto &types) {
      for (NativeRecord &type : types) {
        if (type.declaration->namespaceScope.empty() &&
            used.insert(type.declaration->name).second) {
          type.cName = type.declaration->name;
        }
      }
    };
    const auto assignNamespaced = [&](auto &types) {
      for (NativeRecord &type : types) {
        if (type.cName.empty()) {
          type.cName = encodedCRecordName(*type.info);
          while (!used.insert(type.cName).second) {
            type.cName += '_';
          }
        }
      }
    };
    assignRoots(records);
    assignRoots(opaqueHandles);
    for (NativeCallbackAlias &alias : callbackAliases) {
      if (alias.declaration->namespaceScope.empty() &&
          used.insert(alias.declaration->name).second) {
        alias.cName = alias.declaration->name;
      }
    }
    assignNamespaced(records);
    assignNamespaced(opaqueHandles);
    for (NativeCallbackAlias &alias : callbackAliases) {
      if (!alias.cName.empty()) {
        continue;
      }
      alias.cName = encodedCCallbackName(*alias.info);
      while (!used.insert(alias.cName).second) {
        alias.cName += '_';
      }
    }
  }

  void orderRecord(ClassId id, std::unordered_set<ClassId> &visiting) {
    if (orderedRecordIds.contains(id)) {
      return;
    }
    const auto found = recordByClass.find(id);
    if (found == recordByClass.end() || !visiting.insert(id).second) {
      throw std::logic_error(
          "checked C ABI record graph is unavailable to native header");
    }
    const NativeRecord &record = records[found->second];
    for (const MirCAbiRecordFieldLayout &field :
         record.info->cAbiLayout->fields) {
      const SemanticType *dependency = &field.type;
      while (dependency->kind == SemanticType::Array &&
             dependency->arguments.size() == 1) {
        dependency = &dependency->arguments.front();
      }
      if (dependency->kind == SemanticType::Class) {
        orderRecord(dependency->classId, visiting);
      }
    }
    visiting.erase(id);
    orderedRecordIds.insert(id);
    orderedRecords.push_back(found->second);
  }

  void orderRecords() {
    std::unordered_set<ClassId> visiting;
    for (const NativeRecord &record : records) {
      orderRecord(record.info->id, visiting);
    }
  }

  [[nodiscard]] std::size_t callbackAliasIndex(const SemanticType &type) const {
    const auto found =
        std::find_if(callbackAliases.begin(), callbackAliases.end(),
                     [&](const NativeCallbackAlias &alias) {
                       return alias.info->type == type;
                     });
    if (found == callbackAliases.end()) {
      throw std::logic_error(
          "checked native function type has no public named alias");
    }
    return static_cast<std::size_t>(
        std::distance(callbackAliases.begin(), found));
  }

  void orderCallbackAlias(std::size_t index, std::vector<std::uint8_t> &state) {
    if (index >= callbackAliases.size()) {
      throw std::logic_error(
          "checked native callback alias dependency is unavailable");
    }
    if (state[index] == 2) {
      return;
    }
    if (state[index] == 1) {
      throw std::logic_error(
          "checked native callback aliases contain a dependency cycle");
    }
    state[index] = 1;
    const SemanticType &type = callbackAliases[index].info->type;
    const auto orderDependency = [&](const SemanticType &dependency) {
      if (dependency.kind == SemanticType::NativeFunction) {
        orderCallbackAlias(callbackAliasIndex(dependency), state);
      }
    };
    if (const SemanticType *returnType = type.nativeFunctionReturnType()) {
      orderDependency(*returnType);
    }
    for (const SemanticType &parameter : type.nativeFunctionParameterTypes()) {
      orderDependency(parameter);
    }
    state[index] = 2;
    orderedCallbackAliases.push_back(index);
  }

  void orderCallbackAliases() {
    std::vector<std::uint8_t> state(callbackAliases.size(), 0);
    for (std::size_t index = 0; index < callbackAliases.size(); ++index) {
      orderCallbackAlias(index, state);
    }
  }

  const NativeRecord &nativeTypeFor(ClassId id) const {
    if (const auto found = recordByClass.find(id);
        found != recordByClass.end()) {
      return records[found->second];
    }
    if (const auto found = opaqueByClass.find(id);
        found != opaqueByClass.end()) {
      return opaqueHandles[found->second];
    }
    throw std::logic_error(
        "checked native C type is unavailable to native header");
  }

  const NativeCallbackAlias &nativeCallbackFor(const SemanticType &type) const {
    const auto found =
        std::find_if(callbackAliases.begin(), callbackAliases.end(),
                     [&](const NativeCallbackAlias &alias) {
                       return alias.info->type == type;
                     });
    if (found == callbackAliases.end()) {
      throw std::logic_error(
          "checked native function type has no public named alias");
    }
    return *found;
  }

  std::string qualifiedCppCallbackName(const NativeCallbackAlias &alias) const {
    std::string result = "::__gti_program::";
    for (std::size_t index = 0;
         index < alias.declaration->namespaceScope.size(); ++index) {
      result += emittedScope(alias.declaration->namespaceScope[index], index);
      result += "::";
    }
    result += alias.declaration->name;
    return result;
  }

  std::string cppType(const SemanticType &type) const {
    if (type.nativeCScalar != NativeCScalarKind::None) {
      if (SemanticType::nativeCScalarType(type.nativeCScalar,
                                          program.target()) != type) {
        throw std::logic_error(
            "invalid native C scalar identity reached native header");
      }
      return std::string(nativeCScalarDeclarationName(type.nativeCScalar));
    }
    switch (type.kind) {
    case SemanticType::Void:
      return "void";
    case SemanticType::Bool:
      return "bool";
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
    case SemanticType::StringView:
      return "::gti_c_string_view";
    case SemanticType::CString:
      return "const char*";
    case SemanticType::NativeFunction:
      return qualifiedCppCallbackName(nativeCallbackFor(type));
    case SemanticType::Class:
      return qualifiedCppName(nativeTypeFor(type.classId));
    case SemanticType::RawPointer: {
      if (type.arguments.size() != 1) {
        break;
      }
      const SemanticType &pointee = type.arguments.front();
      std::string spelling = cppType(pointee);
      if (type.pointerAccess == AccessMode::ReadOnly)
        spelling = pointee.kind == SemanticType::RawPointer ||
                           pointee.kind == SemanticType::CString ||
                           pointee.kind == SemanticType::NativeFunction
                       ? spelling + " const"
                       : "const " + spelling;
      return spelling + "*";
    }
    default:
      break;
    }
    throw std::logic_error(
        "unsupported checked C ABI type reached native header backend");
  }

  std::string cType(const SemanticType &type, bool cpp = false) const {
    if (type.nativeCScalar != NativeCScalarKind::None) {
      if (SemanticType::nativeCScalarType(type.nativeCScalar,
                                          program.target()) != type) {
        throw std::logic_error(
            "invalid native C scalar identity reached native header");
      }
      return std::string(nativeCScalarDeclarationName(type.nativeCScalar));
    }
    switch (type.kind) {
    case SemanticType::Void:
      return "void";
    case SemanticType::Bool:
      return cpp ? "bool" : "_Bool";
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
    case SemanticType::StringView:
      return "gti_c_string_view";
    case SemanticType::CString:
      return "const char*";
    case SemanticType::NativeFunction:
      return nativeCallbackFor(type).cName;
    case SemanticType::Class:
      return nativeTypeFor(type.classId).cName;
    case SemanticType::RawPointer: {
      if (type.arguments.size() != 1) {
        break;
      }
      const SemanticType &pointee = type.arguments.front();
      std::string spelling = cType(pointee, cpp);
      if (type.pointerAccess == AccessMode::ReadOnly)
        spelling = pointee.kind == SemanticType::RawPointer ||
                           pointee.kind == SemanticType::CString ||
                           pointee.kind == SemanticType::NativeFunction
                       ? spelling + " const"
                       : "const " + spelling;
      return spelling + "*";
    }
    default:
      break;
    }
    throw std::logic_error(
        "unsupported checked C ABI type reached native header backend");
  }

  template <typename TypeEmitter>
  std::string fieldDeclaration(const MirCAbiRecordFieldLayout &field,
                               TypeEmitter emitType) const {
    const SemanticType *element = &field.type;
    std::vector<std::uint64_t> extents;
    while (element->kind == SemanticType::Array &&
           element->arguments.size() == 1) {
      extents.push_back(element->arrayLength);
      element = &element->arguments.front();
    }
    const LoweredSymbol *symbol = program.findSymbol(field.field);
    if (symbol == nullptr || symbol->name.empty()) {
      throw std::logic_error(
          "checked C ABI field symbol is unavailable to native header");
    }
    std::string result = emitType(*element) + " " + symbol->name;
    for (const std::uint64_t extent : extents) {
      result += "[" + std::to_string(extent) + "]";
    }
    return result;
  }

  void emitCppForward(std::ostream &output, const NativeRecord &record) const {
    if (record.info->cOpaqueHandle) {
      openPublicCppNamespaces(output, record.declaration->namespaceScope);
    } else {
      openCppNamespaces(output, record.declaration->namespaceScope);
    }
    output << (record.info->kind == ClassKind::Union ? "union " : "struct ")
           << record.declaration->name << ";\n";
    if (record.info->cOpaqueHandle) {
      closePublicCppNamespaces(output, record.declaration->namespaceScope);
    } else {
      closeCppNamespaces(output, record.declaration->namespaceScope);
    }
  }

  void emitCppRecord(std::ostream &output, const NativeRecord &record) const {
    openCppNamespaces(output, record.declaration->namespaceScope);
    output << (record.info->kind == ClassKind::Union ? "union " : "struct ")
           << record.declaration->name << " {\n";
    for (const MirCAbiRecordFieldLayout &field :
         record.info->cAbiLayout->fields) {
      output << "  "
             << fieldDeclaration(
                    field,
                    [this](const SemanticType &type) { return cppType(type); })
             << ";\n";
    }
    output << "};\n";
    closeCppNamespaces(output, record.declaration->namespaceScope);
  }

  void emitCppCallbackAlias(std::ostream &output,
                            const NativeCallbackAlias &alias) const {
    const SemanticType *returnType =
        alias.info->type.nativeFunctionReturnType();
    if (returnType == nullptr) {
      throw std::logic_error("checked native callback alias is malformed");
    }
    openCppNamespaces(output, alias.declaration->namespaceScope);
    output << "using " << alias.declaration->name << " = "
           << cppType(*returnType) << " (*)(";
    const std::span<const SemanticType> parameters =
        alias.info->type.nativeFunctionParameterTypes();
    for (std::size_t index = 0; index < parameters.size(); ++index) {
      if (index != 0) {
        output << ", ";
      }
      output << cppType(parameters[index]);
    }
    output << ");\n";
    closeCppNamespaces(output, alias.declaration->namespaceScope);
  }

  void emitCCallbackAlias(std::ostream &output,
                          const NativeCallbackAlias &alias, bool cpp = false) const {
    const SemanticType *returnType =
        alias.info->type.nativeFunctionReturnType();
    if (returnType == nullptr) {
      throw std::logic_error("checked native callback alias is malformed");
    }
    output << "typedef " << cType(*returnType, cpp) << " (*" << alias.cName << ")(";
    const std::span<const SemanticType> parameters =
        alias.info->type.nativeFunctionParameterTypes();
    if (parameters.empty()) {
      output << "void";
    }
    for (std::size_t index = 0; index < parameters.size(); ++index) {
      if (index != 0) {
        output << ", ";
      }
      output << cType(parameters[index], cpp);
    }
    output << ");\n";
  }

  void emitCppAssertions(std::ostream &output,
                         const NativeRecord &record) const {
    const std::string type = qualifiedCppName(record);
    const MirCAbiRecordLayout &layout = *record.info->cAbiLayout;
    output << "static_assert(std::is_standard_layout_v<" << type
           << "> && std::is_trivially_copyable_v<" << type
           << ">, \"GTI native record must remain a passive C ABI value\");\n"
           << "static_assert(sizeof(" << type << ") == " << layout.sizeBytes
           << ", \"GTI native record size mismatch\");\n"
           << "static_assert(alignof(" << type
           << ") == " << layout.abiAlignmentBytes
           << ", \"GTI native record alignment mismatch\");\n";
    for (const MirCAbiRecordFieldLayout &field : layout.fields) {
      const LoweredSymbol *symbol = program.findSymbol(field.field);
      if (symbol == nullptr || symbol->name.empty()) {
        throw std::logic_error(
            "checked C ABI field symbol is unavailable to native header");
      }
      output << "static_assert(offsetof(" << type << ", " << symbol->name
             << ") == " << field.offsetBytes
             << ", \"GTI native record field offset "
             << "mismatch\");\n";
    }
  }

  template <typename TypeEmitter>
  void emitFunctionSignature(std::ostream &output,
                             const NativeFunction &function,
                             TypeEmitter emitType, bool cBranch) const {
    output << emitType(function.info->returnType) << ' '
           << function.info->externalSymbol << '(';
    if (function.info->parameters.empty() && cBranch) {
      output << "void";
    }
    for (std::size_t index = 0; index < function.info->parameters.size();
         ++index) {
      if (index != 0) {
        output << ", ";
      }
      output << emitType(function.info->parameters[index].type);
      if (!function.info->parameters[index].name.empty()) {
        output << ' ' << function.info->parameters[index].name;
      }
    }
    output << ");\n";
  }

  void emitCppFunction(std::ostream &output, const NativeFunction &function,
                       bool publicSourceNamespace) const {
    if (publicSourceNamespace) {
      openPublicCppNamespaces(output, function.declaration->namespaceScope);
    } else {
      openCppNamespaces(output, function.declaration->namespaceScope);
    }
    output << "extern \"C\" {\n";
    emitFunctionSignature(
        output, function,
        [this](const SemanticType &type) { return cppType(type); }, false);
    output << "}\n";
    if (publicSourceNamespace) {
      closePublicCppNamespaces(output, function.declaration->namespaceScope);
    } else {
      closeCppNamespaces(output, function.declaration->namespaceScope);
    }
  }

  template <typename TypeEmitter>
  void emitVariableDeclaration(std::ostream &output,
                               const NativeVariable &variable,
                               TypeEmitter emitType) const {
    output << "extern " << emitType(variable.info->type);
    // The qualifier applies to the object, including pointer and callback
    // slots; a prefix qualifier would incorrectly qualify their pointees.
    if (variable.info->access == AccessMode::ReadOnly) {
      output << " const";
    }
    output << ' ' << variable.info->externalSymbol << ";\n";
  }

  void emitCppVariable(std::ostream &output, const NativeVariable &variable,
                       bool publicSourceNamespace) const {
    if (publicSourceNamespace) {
      openPublicCppNamespaces(output, variable.declaration->namespaceScope);
    } else {
      openCppNamespaces(output, variable.declaration->namespaceScope);
    }
    output << "extern \"C\" {\n";
    emitVariableDeclaration(output, variable, [this](const SemanticType &type) {
      return cppType(type);
    });
    output << "}\n";
    if (publicSourceNamespace) {
      closePublicCppNamespaces(output, variable.declaration->namespaceScope);
    } else {
      closeCppNamespaces(output, variable.declaration->namespaceScope);
    }
  }

  void emitCppTypeAlias(std::ostream &output,
                        const NativeRecord &record) const {
    openPublicCppNamespaces(output, record.declaration->namespaceScope);
    output << "using " << record.declaration->name << " = "
           << qualifiedCppName(record) << ";\n";
    closePublicCppNamespaces(output, record.declaration->namespaceScope);
  }

  void emitCppCallbackSourceAlias(std::ostream &output,
                                  const NativeCallbackAlias &alias) const {
    openPublicCppNamespaces(output, alias.declaration->namespaceScope);
    output << "using " << alias.declaration->name << " = "
           << qualifiedCppCallbackName(alias) << ";\n";
    closePublicCppNamespaces(output, alias.declaration->namespaceScope);
  }

  void emitCppSourceNameAliases(std::ostream &output) const {
    if (records.empty() && callbackAliases.empty()) {
      return;
    }
    output << "#ifndef GTI_NATIVE_HEADER_NO_SOURCE_NAMES\n";
    for (const NativeRecord &record : records) {
      emitCppTypeAlias(output, record);
    }
    for (const NativeCallbackAlias &alias : callbackAliases) {
      emitCppCallbackSourceAlias(output, alias);
    }
    output << "#endif /* GTI_NATIVE_HEADER_NO_SOURCE_NAMES */\n\n";
  }

  void emitCppFunctions(std::ostream &output) const {
    if (functions.empty() && variables.empty()) {
      return;
    }
    // A using-declaration that imports a C-linkage function from the isolated
    // namespace is not a portable definition point: GCC binds a later public
    // definition back to the imported declaration and performs body lookup in
    // the private namespace. Declare the selected external symbol directly
    // unless the consumer explicitly requests the isolated surface. A GTI
    // `[[link_name]]` alias is not an implementation definition point.
    output << "#ifndef GTI_NATIVE_HEADER_NO_SOURCE_NAMES\n";
    for (const NativeFunction &function : functions) {
      emitCppFunction(output, function, true);
    }
    for (const NativeVariable &variable : variables) {
      emitCppVariable(output, variable, true);
    }
    output << "#else\n";
    for (const NativeFunction &function : functions) {
      emitCppFunction(output, function, false);
    }
    for (const NativeVariable &variable : variables) {
      emitCppVariable(output, variable, false);
    }
    output << "#endif /* GTI_NATIVE_HEADER_NO_SOURCE_NAMES */\n\n";
  }

  void emitCppBranch(std::ostream &output) const {
    output << "#ifdef __cplusplus\n\n";
    for (const NativeRecord &record : records) {
      emitCppForward(output, record);
    }
    for (const NativeOpaqueHandle &handle : opaqueHandles) {
      emitCppForward(output, handle);
    }
    if (!records.empty() || !opaqueHandles.empty()) {
      output << '\n';
    }
    for (const std::size_t index : orderedCallbackAliases) {
      emitCppCallbackAlias(output, callbackAliases[index]);
    }
    if (!callbackAliases.empty()) {
      output << '\n';
    }
    for (const std::size_t index : orderedRecords) {
      emitCppRecord(output, records[index]);
      output << '\n';
    }
    for (const NativeRecord &record : records) {
      emitCppAssertions(output, record);
    }
    if (!records.empty() && !functions.empty()) {
      output << '\n';
    }
    emitCppSourceNameAliases(output);
    emitCppFunctions(output);
    if (program.embeddingPlan()) {
      output << "extern \"C\" {\n";
      emitEmbeddingExports(output, true);
      output << "}\n";
    }
    output << '\n';
  }

  void emitCBranch(std::ostream &output, bool cpp = false) const {
    for (const NativeRecord &record : records) {
      output << "/* " << record.info->qualifiedName << " */\n"
             << "typedef "
             << (record.info->kind == ClassKind::Union ? "union " : "struct ")
             << record.cName << ' ' << record.cName << ";\n";
    }
    for (const NativeOpaqueHandle &handle : opaqueHandles) {
      output << "/* " << handle.info->qualifiedName << " (opaque handle) */\n"
             << "typedef struct " << handle.cName << ' ' << handle.cName
             << ";\n";
    }
    if (!records.empty() || !opaqueHandles.empty()) {
      output << '\n';
    }
    for (const std::size_t index : orderedCallbackAliases) {
      emitCCallbackAlias(output, callbackAliases[index], cpp);
    }
    if (!callbackAliases.empty()) {
      output << '\n';
    }
    for (const std::size_t index : orderedRecords) {
      const NativeRecord &record = records[index];
      output << (record.info->kind == ClassKind::Union ? "union " : "struct ")
             << record.cName << " {\n";
      for (const MirCAbiRecordFieldLayout &field :
           record.info->cAbiLayout->fields) {
        output << "  "
               << fieldDeclaration(
                      field,
                      [this, cpp](const SemanticType &type) { return cType(type, cpp); })
               << ";\n";
      }
      output << "};\n\n";
    }
    for (const NativeRecord &record : records) {
      const MirCAbiRecordLayout &layout = *record.info->cAbiLayout;
      output << (cpp ? "static_assert(sizeof(" : "_Static_assert(sizeof(") << record.cName
             << ") == " << layout.sizeBytes
             << ", \"GTI native record size mismatch\");\n"
             << (cpp ? "static_assert(alignof(" : "_Static_assert(_Alignof(") << record.cName
             << ") == " << layout.abiAlignmentBytes
             << ", \"GTI native record alignment mismatch\");\n";
      for (const MirCAbiRecordFieldLayout &field : layout.fields) {
        const LoweredSymbol *symbol = program.findSymbol(field.field);
        if (symbol == nullptr || symbol->name.empty()) {
          throw std::logic_error(
              "checked C ABI field symbol is unavailable to native header");
        }
        output << (cpp ? "static_assert(offsetof(" : "_Static_assert(offsetof(") << record.cName << ", "
               << symbol->name << ") == " << field.offsetBytes
               << ", \"GTI native record field offset mismatch\");\n";
      }
    }
    if (!records.empty() && !functions.empty()) {
      output << '\n';
    }
    for (const NativeFunction &function : functions) {
      emitFunctionSignature(
          output, function,
          [this](const SemanticType &type) { return cType(type); }, true);
    }
    for (const NativeVariable &variable : variables) {
      emitVariableDeclaration(
          output, variable,
          [this](const SemanticType &type) { return cType(type); });
    }
    emitEmbeddingExports(output, false);
  }

  void emitEmbeddingExports(std::ostream &output, bool cpp) const {
    if (!program.embeddingPlan())
      return;
    const auto type = [&](const SemanticType &value) {
      return cpp ? cppType(value) : cType(value);
    };
    for (const auto &selected : program.embeddingPlan()->exports) {
      output << "gti_embed_status_v1 " << selected.symbol
             << "(gti_context_v1 *context";
      for (std::size_t i = 0; i < selected.parameterTypes.size(); ++i)
        output << ", " << type(selected.parameterTypes[i]) << " argument" << i;
      if (selected.returnType != SemanticType::Void)
        output << ", " << type(selected.returnType) << " *out_result";
      output << ", gti_failure_record_v1 *out_failure);\n";
    }
  }

  static std::string headerGuard(std::string_view contents) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char byte : contents) {
      hash ^= byte;
      hash *= 1099511628211ULL;
    }
    std::ostringstream output;
    output << "GTI_NATIVE_BRIDGE_" << std::uppercase << std::hex
           << std::setw(16) << std::setfill('0') << hash << "_H";
    return output.str();
  }

  const LoweredProgram &program;
  std::vector<NativeRecord> records;
  std::unordered_map<ClassId, std::size_t> recordByClass;
  std::vector<NativeOpaqueHandle> opaqueHandles;
  std::unordered_map<ClassId, std::size_t> opaqueByClass;
  std::vector<std::size_t> orderedRecords;
  std::unordered_set<ClassId> orderedRecordIds;
  std::vector<NativeCallbackAlias> callbackAliases;
  std::vector<std::size_t> orderedCallbackAliases;
  std::vector<NativeFunction> functions;
  std::vector<NativeVariable> variables;
};

} // namespace

std::string nativeEmbeddingSignature(
    const LoweredProgram &program, const LoweredEmbeddingExport &selected) {
  return NativeHeaderEmitter(program).signature(selected);
}

BackendArtifact NativeHeaderBackend::generate(LoweredProgram program) {
  std::vector<LoweredProgramIssue> issues;
  const std::optional<VerifiedLoweredProgram> verified =
      verifiedLoweredProgram(std::move(program), issues);
  if (!verified) {
    throw std::logic_error("native-header backend requires a verified lowered "
                           "program: " +
                           issues.front().detail);
  }
  return {.kind = BackendArtifactKind::Source,
          .contents = NativeHeaderEmitter(verified->program()).emit(),
          .extension = ".h"};
}

} // namespace lang
