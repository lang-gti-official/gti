#include "cpp_representation.h"

#include <stdexcept>

#include "gti/lowered_program.h"

#include <llvm/ADT/ArrayRef.h>
#include <llvm/Support/SHA256.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lang {

struct CppRepresentationScope::Facts {
  explicit Facts(const LoweredProgram &program) : program(&program) {
    for (const LoweredDeclaration &row : program.declarations()) {
      children[row.parent].push_back(&row);
    }
    for (const MirBodyAddress address :
         enumerateMirBodyAddresses(program.mir())) {
      if (const MirBody *body = findMirBody(program.mir(), address)) {
        bodies.insert(body);
      }
    }
  }

  const LoweredProgram *program;
  std::unordered_map<LoweredDeclarationId,
                     std::vector<const LoweredDeclaration *>>
      children;
  std::map<std::pair<ClassId, LoweredDeclarationId>, bool> directRepresentation;
  std::optional<std::unordered_set<ClassId>> templateClosure;
  std::optional<bool> callableTemplateCarrier;
  std::unordered_set<const MirBody *> bodies;
  std::map<std::pair<const MirBody *, std::string>, std::shared_ptr<void>>
      bodyAnalyses;
};

namespace {

thread_local CppRepresentationScope::Facts *activeRepresentationFacts = nullptr;

[[nodiscard]] CppRepresentationScope::Facts *
representationFacts(const LoweredProgram &program) {
  return activeRepresentationFacts != nullptr &&
                 activeRepresentationFacts->program == &program
             ? activeRepresentationFacts
             : nullptr;
}

} // namespace

CppRepresentationScope::CppRepresentationScope(
    const VerifiedLoweredProgram &program)
    : previous(activeRepresentationFacts) {
  // A nested stage of the same consumer keeps the facts already computed.
  if (previous == nullptr || previous->program != &program.program()) {
    facts = std::make_unique<Facts>(program.program());
    activeRepresentationFacts = facts.get();
  }
}

CppRepresentationScope::~CppRepresentationScope() {
  activeRepresentationFacts = previous;
}

std::shared_ptr<void> *cppScopedBodyAnalysis(const MirBody &body,
                                             std::string_view kind) {
  if (activeRepresentationFacts == nullptr ||
      !activeRepresentationFacts->bodies.contains(&body)) {
    return nullptr;
  }
  return &activeRepresentationFacts->bodyAnalyses[{&body, std::string(kind)}];
}

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
  return program.findClassDeclarationRow(id);
}

[[nodiscard]] const LoweredDeclaration *
enumDeclarationRow(const LoweredProgram &program, const SemanticType &type) {
  return program.findEnumDeclarationRow(type);
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

[[nodiscard]] bool computeConcreteClassSupportsDirectRepresentation(
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
  std::vector<const LoweredDeclaration *> scanned;
  const std::vector<const LoweredDeclaration *> *children = nullptr;
  if (const auto *facts = representationFacts(program)) {
    static const std::vector<const LoweredDeclaration *> none;
    const auto found = facts->children.find(primaryRow.id);
    children = found == facts->children.end() ? &none : &found->second;
  } else {
    for (const LoweredDeclaration &candidate : program.declarations()) {
      if (candidate.parent == primaryRow.id) {
        scanned.push_back(&candidate);
      }
    }
    children = &scanned;
  }
  for (const LoweredDeclaration *child : *children) {
    const LoweredDeclaration &candidate = *child;
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
        (std::holds_alternative<LoweredTypeAliasDeclaration>(
             candidate.payload) &&
         std::none_of(declaration.genericParameters.begin(),
                      declaration.genericParameters.end(),
                      [](const LoweredGenericParameter &parameter) {
                        return parameter.value;
                      }))) {
      // Keep the established shared source-template identity of type-only
      // alias carriers and their transitive signatures. Value-generic aliases
      // instead need the verified concrete alias catalog: dependent arithmetic
      // and finite layout must not be re-evaluated by native instantiation.
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool concreteClassSupportsDirectRepresentation(
    const LoweredProgram &program, const LoweredClassDeclaration &declaration,
    const LoweredDeclaration &primaryRow) {
  auto *facts = representationFacts(program);
  if (facts == nullptr) {
    return computeConcreteClassSupportsDirectRepresentation(
        program, declaration, primaryRow);
  }
  const std::pair<ClassId, LoweredDeclarationId> key{declaration.id,
                                                     primaryRow.id};
  if (const auto found = facts->directRepresentation.find(key);
      found != facts->directRepresentation.end()) {
    return found->second;
  }
  const bool result = computeConcreteClassSupportsDirectRepresentation(
      program, declaration, primaryRow);
  facts->directRepresentation.emplace(key, result);
  return result;
}

[[nodiscard]] bool
semanticTypeContainsLexicalClosure(const SemanticType &type) {
  return type.kind == SemanticType::Lambda ||
         std::any_of(type.arguments.begin(), type.arguments.end(),
                     semanticTypeContainsLexicalClosure);
}

// A dependent native template signature/field must instantiate the same carrier
// that concrete MIR calls address. Propagate template representation through
// those declaration dependencies; otherwise a dependent `Carrier<T>` silently
// creates a second native class beside the hashed concrete `Carrier<int>`.
// The resulting closure of template-represented classes depends on the whole
// program, not on the class being spelled.
[[nodiscard]] std::unordered_set<ClassId>
templateDependencyClosure(const LoweredProgram &program) {
  std::vector<ClassId> templates;
  const auto add = [&](ClassId id) {
    const LoweredClassDeclaration *declaration =
        program.findClassDeclaration(id);
    if (declaration == nullptr) {
      return;
    }
    const auto *primary = concreteClassPrimary(program, *declaration);
    if (primary != nullptr && !primary->genericParameters.empty() &&
        std::find(templates.begin(), templates.end(), primary->id) ==
            templates.end()) {
      templates.push_back(primary->id);
    }
  };
  for (const LoweredDeclaration &row : program.declarations()) {
    const auto *declaration =
        std::get_if<LoweredClassDeclaration>(&row.payload);
    if (declaration != nullptr &&
        declaration->exactSpecializationPrimary == 0 &&
        !declaration->genericParameters.empty() &&
        !concreteClassSupportsDirectRepresentation(program, *declaration,
                                                   row)) {
      add(declaration->id);
    }
  }
  const auto dependent = [&](const auto &self,
                             const SemanticType &type) -> bool {
    return type.kind == SemanticType::TypeParameter ||
           (type.kind == SemanticType::TypePack && !type.concretePack) ||
           type.arrayLengthParameterId != 0 ||
           std::any_of(type.valueArguments.begin(), type.valueArguments.end(),
                       [](const CompileTimeValue &value) {
                         return value.kind != CompileTimeValue::UInt64;
                       }) ||
           std::any_of(type.arguments.begin(), type.arguments.end(),
                       [&](const SemanticType &argument) {
                         return self(self, argument);
                       });
  };
  const auto visit = [&](const auto &self, const SemanticType &type) -> void {
    if (type.kind == SemanticType::Class && dependent(dependent, type)) {
      add(type.classId);
    }
    for (const SemanticType &argument : type.arguments) {
      self(self, argument);
    }
  };
  for (std::size_t index = 0; index < templates.size(); ++index) {
    const ClassId owner = templates[index];
    const auto *declaration = program.findClassDeclaration(owner);
    const auto *ownerRow = classDeclarationRow(program, owner);
    for (const auto &base : declaration->bases) {
      visit(visit, base.type);
    }
    for (const LoweredDeclaration &row : program.declarations()) {
      if (const auto *storage =
              std::get_if<LoweredStorageDeclaration>(&row.payload);
          storage != nullptr && storage->ownerClass == owner) {
        visit(visit, storage->type);
      } else if (const auto *function =
                     std::get_if<LoweredFunctionDeclaration>(&row.payload);
                 function != nullptr && function->ownerClass == owner) {
        visit(visit, function->returnType);
        for (const auto &parameter : function->parameters) {
          visit(visit, parameter.type);
        }
      } else if (const auto *constructor =
                     std::get_if<LoweredConstructorDeclaration>(&row.payload);
                 constructor != nullptr && constructor->owner == owner) {
        for (const auto &parameter : constructor->parameters) {
          visit(visit, parameter.type);
        }
      } else if (ownerRow != nullptr && row.parent == ownerRow->id) {
        if (const auto *alias =
                std::get_if<LoweredTypeAliasDeclaration>(&row.payload)) {
          visit(visit, alias->type);
        } else if (const auto *enumeration =
                       std::get_if<LoweredEnumDeclaration>(&row.payload)) {
          for (const auto &enumerator : enumeration->enumerators) {
            for (const auto &payload : enumerator.payloadTypes) {
              visit(visit, payload);
            }
          }
        }
      }
    }
  }
  return {templates.begin(), templates.end()};
}

[[nodiscard]] bool
concreteClassHasTemplateDependency(const LoweredProgram &program,
                                   ClassId requested) {
  auto *facts = representationFacts(program);
  if (facts == nullptr) {
    return templateDependencyClosure(program).contains(requested);
  }
  if (!facts->templateClosure) {
    facts->templateClosure = templateDependencyClosure(program);
  }
  return facts->templateClosure->contains(requested);
}

[[nodiscard]] bool
computeConcreteClassIsCallableTemplateCarrier(const LoweredProgram &program) {
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

[[nodiscard]] bool
concreteClassIsCallableTemplateCarrier(const LoweredProgram &program,
                                       const SemanticType &type) {
  static_cast<void>(type);
  auto *facts = representationFacts(program);
  if (facts == nullptr) {
    return computeConcreteClassIsCallableTemplateCarrier(program);
  }
  if (!facts->callableTemplateCarrier) {
    facts->callableTemplateCarrier =
        computeConcreteClassIsCallableTemplateCarrier(program);
  }
  return *facts->callableTemplateCarrier;
}

[[nodiscard]] bool appendConcreteTypeIdentity(std::vector<std::uint8_t> &output,
                                              const LoweredProgram &program,
                                              const SemanticType &type) {
  if (type.nativeCScalar != NativeCScalarKind::None) {
    if (SemanticType::nativeCScalarType(type.nativeCScalar, program.target()) !=
        type) {
      return false;
    }
    appendIdentityText(output, nativeCScalarKindName(type.nativeCScalar));
    return true;
  }
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
        program.findEnumDeclaration(type);
    const LoweredDeclaration *row = enumDeclarationRow(program, type);
    if (declaration == nullptr || row == nullptr) {
      return false;
    }
    appendIdentityText(output, "enum");
    appendIdentityText(output, declaration->qualifiedName);
    appendIdentityText(output, row->source.source);
    return appendArguments();
  }
  case SemanticType::Reference:
    appendIdentityText(output, "reference");
    appendIdentityInteger(output,
                          static_cast<std::uint64_t>(type.referenceAccess));
    return appendArguments();
  case SemanticType::UniqueOwner:
    appendIdentityText(output, "unique-owner");
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
  case SemanticType::FunctionSignature:
    if (type.arguments.empty()) {
      return false;
    }
    appendIdentityText(output, "function-signature");
    appendIdentityText(
        output, std::to_string(static_cast<int>(type.signatureReceiver)));
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

bool cppFunctionHasFailureResultChannel(
    const LoweredProgram &program, const LoweredFunctionDeclaration &function) {
  if (function.returnType != SemanticType::Void)
    return true;
  if (!function.virtualMethod)
    return false;
  // Multiple inherited contracts can share one override. Use the connected
  // declaration family, not just one selected root, so all native status
  // signatures keep the same result arity after substitution.
  std::vector<FunctionId> connected{function.id};
  const auto append = [&](FunctionId id) {
    if (id != 0 &&
        std::find(connected.begin(), connected.end(), id) == connected.end())
      connected.push_back(id);
  };
  for (std::size_t index = 0; index < connected.size(); ++index) {
    const FunctionId id = connected[index];
    if (const auto *declaration = program.findFunctionDeclaration(id)) {
      if (declaration->returnType != SemanticType::Void)
        return true;
      for (const FunctionId root : declaration->virtualRoots)
        append(root);
    }
    for (const auto &row : program.declarations()) {
      const auto *candidate =
          std::get_if<LoweredFunctionDeclaration>(&row.payload);
      if (candidate != nullptr &&
          std::find(candidate->virtualRoots.begin(),
                    candidate->virtualRoots.end(),
                    id) != candidate->virtualRoots.end())
        append(candidate->id);
    }
  }
  return false;
}

bool cppVirtualFamilyHasConditionalResultChannel(
    const LoweredProgram &program, const LoweredFunctionDeclaration &function) {
  if (!function.virtualMethod || function.linkage != LanguageLinkage::Gti)
    return false;
  std::vector<FunctionId> connected{function.id};
  const auto append = [&](FunctionId id) {
    if (id != 0 &&
        std::find(connected.begin(), connected.end(), id) == connected.end())
      connected.push_back(id);
  };
  for (std::size_t index = 0; index < connected.size(); ++index) {
    const FunctionId id = connected[index];
    if (const auto *declaration = program.findFunctionDeclaration(id)) {
      const auto *owner = program.findClassDeclaration(declaration->ownerClass);
      const SemanticType *result = &declaration->returnType;
      while (result->kind == SemanticType::Array &&
             result->arguments.size() == 1)
        result = &result->arguments.front();
      if (owner && result->kind == SemanticType::TypeParameter &&
          std::any_of(owner->genericParameters.begin(),
                      owner->genericParameters.end(),
                      [&](const auto &parameter) {
                        return parameter.id == result->genericParameterId;
                      }))
        return true;
      for (FunctionId root : declaration->virtualRoots)
        append(root);
    }
    for (const auto &row : program.declarations()) {
      const auto *candidate =
          std::get_if<LoweredFunctionDeclaration>(&row.payload);
      if (candidate && std::find(candidate->virtualRoots.begin(),
                                 candidate->virtualRoots.end(),
                                 id) != candidate->virtualRoots.end())
        append(candidate->id);
    }
  }
  return false;
}

std::string cppClassTemplateNameSpelling(const LoweredProgram &program,
                                         ClassId id) {
  const LoweredClassDeclaration *info = program.findClassDeclaration(id);
  return info == nullptr ? std::string{}
                         : cppLoweredQualifiedName(info->qualifiedName,
                                                   info->cOpaqueHandle);
}

std::string cppNativeTypeSpelling(const LoweredProgram &program,
                                  CppStandard standard,
                                  const SemanticType &type) {
  if (type.kind == SemanticType::StringView)
    return "::gti_c_string_view";
  if (type.kind == SemanticType::RawPointer) {
    std::string result =
        type.arguments.empty()
            ? "void"
            : cppNativeTypeSpelling(program, standard, type.arguments.front());
    if (type.pointerAccess == AccessMode::ReadOnly) {
      const bool pointerObject =
          !type.arguments.empty() &&
          (type.arguments.front().kind == SemanticType::RawPointer ||
           type.arguments.front().kind == SemanticType::CString ||
           type.arguments.front().kind == SemanticType::NativeFunction);
      result = pointerObject ? result + " const" : "const " + result;
    }
    return result + '*';
  }
  if (type.kind == SemanticType::Array && type.arguments.size() == 1 &&
      type.arrayLengthParameterId == 0)
    return "std::array<" +
           cppNativeTypeSpelling(program, standard, type.arguments.front()) +
           ", " + std::to_string(type.arrayLength) + ">";
  return cppSemanticTypeSpelling(program, standard, type);
}

bool cppUsesDependentByteField(const SemanticType &type) {
  return type.kind == SemanticType::TypeParameter ||
         (type.kind == SemanticType::Array && type.arguments.size() == 1 &&
          cppUsesDependentByteField(type.arguments.front()));
}

bool cppRequiresIndirectValueTransport(const MirProgram &program,
                                       const SemanticType &type) {
  if (type.kind == SemanticType::Array && type.arguments.size() == 1)
    return cppRequiresIndirectValueTransport(program, type.arguments.front());
  if (type.kind != SemanticType::Class)
    return false;
  const auto instance = std::find_if(
      program.classInstances().begin(), program.classInstances().end(),
      [&](const auto &item) { return item.type == type; });
  return instance != program.classInstances().end() && instance->cAbiRecord &&
         cppRequiresBytePreservingTransport(program, type);
}

bool cppRequiresBytePreservingTransport(const MirProgram &program,
                                        const SemanticType &type) {
  std::vector<SemanticType> visiting;
  const auto inspect = [&](const auto &self, const SemanticType &current,
                           bool nativeField) -> bool {
    if (current.kind == SemanticType::RawPointer ||
        current.kind == SemanticType::CString)
      return nativeField;
    if (current.kind == SemanticType::Reference ||
        current.kind == SemanticType::UniqueOwner ||
        current.kind == SemanticType::NativeFunction)
      return false;
    if (std::find(visiting.begin(), visiting.end(), current) != visiting.end())
      return false; // By-value recursion is rejected before the backend.
    visiting.push_back(current);
    const bool result = [&] {
      if (current.kind == SemanticType::Array ||
          current.kind == SemanticType::Expected ||
          current.kind == SemanticType::Unexpected)
        return std::any_of(
            current.arguments.begin(), current.arguments.end(),
            [&](const auto &item) { return self(self, item, nativeField); });
      if (current.kind == SemanticType::Lambda) {
        const auto *lambda = program.findLambda(current.lambdaId);
        if (!lambda)
          return false;
        for (std::size_t index = 0; index < lambda->captureTypes.size();
             ++index)
          if (self(self, lambda->captureTypes[index], false))
            return true;
        return false;
      }
      if (current.kind == SemanticType::Enum) {
        const auto *payload = program.findPayloadEnum(current);
        if (payload)
          for (const auto &variant : payload->variants)
            for (const auto &field : variant.payloadTypes)
              if (self(self, field, false))
                return true;
        return false;
      }
      if (current.kind != SemanticType::Class)
        return false;
      const auto instance = std::find_if(
          program.classInstances().begin(), program.classInstances().end(),
          [&](const auto &item) { return item.type == current; });
      if (instance == program.classInstances().end())
        return false;
      if (instance->compilerCapability ==
          CompilerCapabilityTypeKind::OptionalStorage)
        return std::any_of(
            current.arguments.begin(), current.arguments.end(),
            [&](const auto &item) { return self(self, item, false); });
      for (const auto &base : instance->bases)
        if (self(self, base.type, false))
          return true;
      for (const auto &field : instance->declaredFields)
        if (self(self, field.type, instance->cAbiRecord))
          return true;
      return false;
    }();
    visiting.pop_back();
    return result;
  };
  return inspect(inspect, type, false);
}

std::string cppSemanticTypeSpelling(const LoweredProgram &program,
                                    CppStandard standard,
                                    const SemanticType &type) {
  if (type.nativeCScalar != NativeCScalarKind::None) {
    if (SemanticType::nativeCScalarType(type.nativeCScalar, program.target()) !=
        type) {
      throw std::logic_error(
          "invalid native C scalar identity reached C++ representation");
    }
    return std::string(nativeCScalarDeclarationName(type.nativeCScalar));
  }
  const auto spell = [&](const SemanticType &inner) {
    return cppSemanticTypeSpelling(program, standard, inner);
  };
  // Spell declaration-bound dependent metadata; never evaluate it here.
  // Executable concrete instances already carry checked literal arguments.
  const auto spellValueExpression =
      [&](const auto &self,
          const GenericValueExpression &expression) -> std::string {
    if (expression.kind == GenericValueExpression::UInt64)
      return "std::uint64_t(" + std::to_string(expression.value) + "ULL)";
    if (expression.kind == GenericValueExpression::Parameter) {
      const auto *parameter =
          program.findGenericParameter(expression.parameterId);
      if (parameter == nullptr)
        throw std::logic_error(
            "dependent value representation lost its declaration identity");
      return parameter->name;
    }
    if (!expression.left || !expression.right)
      throw std::logic_error(
          "dependent value representation has an incomplete expression");
    std::string_view operation;
    switch (expression.token.kind) {
    case TokenKind::PLUS:
      operation = "+";
      break;
    case TokenKind::MINUS:
      operation = "-";
      break;
    case TokenKind::STAR:
      operation = "*";
      break;
    case TokenKind::SLASH:
      operation = "/";
      break;
    case TokenKind::PERCENT:
      operation = "%";
      break;
    case TokenKind::SHIFT_LEFT:
      operation = "<<";
      break;
    case TokenKind::SHIFT_RIGHT:
      operation = ">>";
      break;
    case TokenKind::AMPERSAND:
      operation = "&";
      break;
    case TokenKind::PIPE:
      operation = "|";
      break;
    case TokenKind::CARET:
      operation = "^";
      break;
    default:
      throw std::logic_error(
          "dependent value representation has an unsupported typed operation");
    }
    return '(' + self(self, *expression.left) + ' ' + std::string(operation) +
           ' ' + self(self, *expression.right) + ')';
  };
  const auto dependent = [&](const auto &self,
                             const SemanticType &inner) -> bool {
    return inner.kind == SemanticType::TypeParameter ||
           inner.kind == SemanticType::TypePack ||
           std::any_of(
               inner.arguments.begin(), inner.arguments.end(),
               [&](const auto &argument) { return self(self, argument); });
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
    std::string result = "::gti_internal::backend::raw_address<";
    const bool qualifiedPointerObject =
        type.pointerAccess == AccessMode::ReadOnly && !type.arguments.empty() &&
        (type.arguments.front().kind == SemanticType::RawPointer ||
         type.arguments.front().kind == SemanticType::CString);
    if (type.pointerAccess == AccessMode::ReadOnly) {
      result += qualifiedPointerObject ? "std::add_const_t<" : "const ";
    }
    // A nested pointer names a native pointer slot, not another address
    // carrier object. Qualify that pointer object at this exact layer.
    result +=
        type.arguments.empty() ? "void"
        : type.arguments.front().kind == SemanticType::RawPointer
            ? cppNativeTypeSpelling(program, standard, type.arguments.front())
            : spell(type.arguments.front());
    if (qualifiedPointerObject)
      result += '>';
    result += '>';
    return result;
  }
  case SemanticType::FunctionSignature:
  case SemanticType::NativeFunction: {
    if (type.arguments.empty()) {
      return "void";
    }
    const bool native = type.kind == SemanticType::NativeFunction;
    std::string result = native ? "std::add_pointer_t<" : "";
    result += native ? cppNativeTypeSpelling(program, standard,
                                             type.arguments.front())
                     : spell(type.arguments.front());
    result += '(';
    bool separator = false;
    for (const SemanticType &parameter :
         std::span<const SemanticType>(type.arguments).subspan(1)) {
      if (separator) {
        result += ", ";
      }
      result += native ? cppNativeTypeSpelling(program, standard, parameter)
                       : spell(parameter);
      if (parameter.kind == SemanticType::TypePack && !parameter.concretePack) {
        result += "...";
      }
      separator = true;
    }
    result += native ? ")>" : ")";
    if (!native) {
      // Qualifiers distinguish metadata types only. Invocation access is
      // selected by GTI semantics and carried by MIR, never native overloads.
      if (type.signatureReceiver == ReceiverMutability::Mutable)
        result += " &";
      else if (type.signatureReceiver == ReceiverMutability::Consuming)
        result += " &&";
    }
    return result;
  }
  case SemanticType::FunctionItem:
    return type.hasFunctionItemShape()
               ? "::gti_internal::backend::function_item<" +
                     std::to_string(type.functionId) + ">"
               : "void";
  case SemanticType::Array: {
    const bool pointerElements =
        type.arguments.size() == 1 &&
        type.arguments.front().kind == SemanticType::RawPointer &&
        type.arguments.front().arguments.size() == 1;
    std::string result = pointerElements
                             ? "::gti_internal::backend::raw_pointer_array<"
                             : "std::array<";
    if (pointerElements) {
      const SemanticType &pointer = type.arguments.front();
      const std::string pointee =
          cppNativeTypeSpelling(program, standard, pointer.arguments.front());
      result += pointer.pointerAccess == AccessMode::ReadOnly
                    ? "std::add_const_t<" + pointee + ">"
                    : pointee;
    } else {
      result += type.arguments.empty() ? "void" : spell(type.arguments.front());
    }
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
        CompilerCapabilityTypeKind::SharedOwner) {
      if (type.arguments.size() != 1) {
        return "void";
      }
      return "::gti_internal::backend::shared_owner<" +
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
        CompilerCapabilityTypeKind::UninitializedStorage) {
      if (type.arguments.size() != 1)
        return "void";
      // A retained source template has no concrete lifecycle instance yet.
      // Match semantic generic staging rather than emitting an invalid void
      // field. These native operations grant no GTI permission: concrete
      // reanalysis and MIR verification establish carrier copy/move bounds
      // before any executable instance exists. Concrete spellings below use
      // those exact verified traits, never native C++ type traits.
      if (type.arguments.front().kind == SemanticType::TypeParameter)
        return "::gti_internal::backend::uninitialized_storage<" +
               spell(type.arguments.front()) + ", true, true>";
      const auto instance =
          std::find_if(program.mir().classInstances().begin(),
                       program.mir().classInstances().end(),
                       [&](const auto &item) { return item.type == type; });
      if (instance == program.mir().classInstances().end())
        return "void";
      return "::gti_internal::backend::uninitialized_storage<" +
             spell(type.arguments.front()) + ", " +
             (instance->traits.copyable ? "true" : "false") + ", " +
             (instance->traits.movable ? "true" : "false") + ">";
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
    if (!typeArguments.empty() || !valueArguments.empty() ||
        !classInfo->genericParameters.empty()) {
      result += '<';
      bool separator = false;
      for (const SemanticType &argument : typeArguments) {
        if (separator) {
          result += ", ";
        }
        result += spell(argument);
        if (argument.kind == SemanticType::TypePack && !argument.concretePack) {
          result += "...";
        }
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
        } else if (argument.expression) {
          result +=
              spellValueExpression(spellValueExpression, *argument.expression);
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
    const LoweredEnumDeclaration *enumInfo = program.findEnumDeclaration(type);
    if (enumInfo == nullptr)
      return "void";
    if (const auto name = cppConcreteEnumName(program, type)) {
      const auto separator = enumInfo->qualifiedName.rfind("::");
      return cppLoweredQualifiedName(
          separator == std::string::npos
              ? *name
              : enumInfo->qualifiedName.substr(0, separator + 2) + *name,
          false);
    }
    return cppLoweredQualifiedName(enumInfo->qualifiedName, false);
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
    if (type.arguments.size() == 2 &&
        type.arguments.front().kind == SemanticType::Reference &&
        type.arguments.front().referenceAccess == AccessMode::ReadOnly &&
        type.arguments.front().arguments.size() == 1) {
      return "::gti_internal::backend::borrowed_expected<" +
             spell(type.arguments.front().arguments.front()) + ", " +
             spell(type.arguments.back()) + ">";
    }
    const bool byteTransport = std::any_of(
        type.arguments.begin(), type.arguments.end(),
        [&](const auto &argument) {
          return cppRequiresIndirectValueTransport(program.mir(), argument);
        });
    std::string result =
        dependent(dependent, type) ? "::gti_internal::backend::expected_result<"
        : byteTransport            ? "::gti_internal::backend::byte_expected<"
        : standard == CppStandard::Cpp23 ? "std::expected<"
                                         : "::nonstd::expected<";
    if (type.arguments.size() == 2) {
      result += spell(type.arguments[0]);
      result += ", ";
      result += spell(type.arguments[1]);
    }
    result += '>';
    return result;
  }
  case SemanticType::Unexpected: {
    const bool byteTransport =
        !type.arguments.empty() && cppRequiresIndirectValueTransport(
                                       program.mir(), type.arguments.front());
    std::string result =
        dependent(dependent, type)
            ? "::gti_internal::backend::unexpected_result<"
        : byteTransport ? "::gti_internal::backend::byte_unexpected<"
        : standard == CppStandard::Cpp23 ? "std::unexpected<"
                                         : "::nonstd::unexpected_type<";
    if (type.arguments.size() == 1)
      result += spell(type.arguments.front());
    result += '>';
    return result;
  }
  default:
    return "void";
  }
}

std::optional<std::string> cppConcreteEnumName(const LoweredProgram &program,
                                               const SemanticType &type) {
  if (type.kind != SemanticType::Enum ||
      (type.arguments.empty() && type.valueArguments.empty()))
    return std::nullopt;
  const LoweredEnumDeclaration *declaration = program.findEnumDeclaration(type);
  if (declaration == nullptr)
    return std::nullopt;
  std::vector<std::uint8_t> identity;
  appendIdentityText(identity, "GTI-CONCRETE-ENUM-V1");
  if (!appendConcreteTypeIdentity(identity, program, type))
    return std::nullopt;
  const auto separator = declaration->qualifiedName.rfind("::");
  const std::string name =
      separator == std::string::npos
          ? declaration->qualifiedName
          : declaration->qualifiedName.substr(separator + 2);
  return name + "__gti_enum_" + identityDigestHex(identity);
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
      concreteClassHasTemplateDependency(program, primary->id) ||
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
    // C++ declaration identity is distinct from the semantic link identity.
    // This prevents support-header prototypes from capturing an accepted GTI
    // declaration; the declaration writer binds this private name to the exact
    // unchanged external symbol.
    return "__gti_c_fn_" + std::to_string(function.id);
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
  if (instance.owner) {
    const MirClassInstance *owner =
        program.mir().findClassInstance(*instance.owner);
    if (owner == nullptr ||
        !appendConcreteTypeIdentity(identity, program, owner->type)) {
      return false;
    }
  }
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
cppCallableFreeFunctionSpelling(const LoweredProgram &program,
                                const LoweredDeclaration &row,
                                const LoweredFunctionDeclaration &function,
                                const LoweredFunctionInstance &instance,
                                const MirFunctionInstance &mirInstance) {
  const auto *rowFunction =
      std::get_if<LoweredFunctionDeclaration>(&row.payload);
  const auto *selected = program.findFunctionInstance(instance.id);
  const auto *selectedMir = program.mir().findFunctionInstance(mirInstance.id);
  if (rowFunction == nullptr || rowFunction->id != function.id ||
      function.id == 0 || function.ownerClass != 0 ||
      function.genericParameters.empty() ||
      function.definitionKind != MirDefinitionKind::Source ||
      function.linkage != LanguageLinkage::Gti ||
      function.intrinsic != IntrinsicKind::None || instance.owner ||
      mirInstance.owner || instance.id == 0 || instance.id != mirInstance.id ||
      instance.declaration != function.id ||
      mirInstance.declaration != function.id ||
      instance.typeArguments != mirInstance.typeArguments ||
      instance.valueArguments != mirInstance.valueArguments ||
      selected == nullptr || selectedMir == nullptr ||
      (selected != &instance && *selected != instance) ||
      (selectedMir != &mirInstance && *selectedMir != mirInstance) ||
      cppConcreteFunctionInstanceIsNameable(program, instance, mirInstance))
    return std::nullopt;
  const auto containsClosure = [](const auto &self,
                                  const SemanticType &type) -> bool {
    return type.kind == SemanticType::Lambda ||
           std::any_of(
               type.arguments.begin(), type.arguments.end(),
               [&](const auto &argument) { return self(self, argument); });
  };
  if (!containsClosure(containsClosure, mirInstance.returnType) &&
      std::none_of(mirInstance.parameterTypes.begin(),
                   mirInstance.parameterTypes.end(),
                   [&](const auto &type) {
                     return containsClosure(containsClosure, type);
                   }) &&
      std::none_of(instance.typeArguments.begin(), instance.typeArguments.end(),
                   [&](const auto &type) {
                     return containsClosure(containsClosure, type);
                   }))
    return std::nullopt;
  return "__gti_callable_inst_" + std::to_string(instance.id) + "_" + row.name;
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
