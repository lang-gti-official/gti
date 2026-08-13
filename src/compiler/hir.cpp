#include "gti/hir_instance_index.h"

#include <cstdint>
#include <unordered_map>
#include <utility>

#include "llvm/ADT/Hashing.h"

namespace lang {

namespace {

// Structural hashing over the key types. Equality is the defaulted operator==
// the previous linear scans relied on; the hash only has to be consistent with
// it.

using HashCode = llvm::hash_code;

[[nodiscard]] HashCode combine(std::uint64_t seed, std::uint64_t value) {
  return llvm::hash_combine(seed, value);
}

[[nodiscard]] std::size_t finalize(HashCode code) {
  return static_cast<std::size_t>(code);
}

[[nodiscard]] std::uint64_t hashSemanticType(const SemanticType &type,
                                             std::uint64_t seed);

[[nodiscard]] std::uint64_t hashCompileTimeValue(const CompileTimeValue &value,
                                                 std::uint64_t seed) {
  std::uint64_t result = combine(seed, static_cast<std::uint64_t>(value.kind));
  result = combine(result, value.value);
  result = combine(result, value.parameterId);
  return result;
}

[[nodiscard]] std::uint64_t
hashSemanticTypes(const std::vector<SemanticType> &types, std::uint64_t seed) {
  std::uint64_t result = combine(seed, types.size());
  for (const SemanticType &type : types) {
    result = hashSemanticType(type, result);
  }
  return result;
}

[[nodiscard]] std::uint64_t
hashCompileTimeValues(const std::vector<CompileTimeValue> &values,
                      std::uint64_t seed) {
  std::uint64_t result = combine(seed, values.size());
  for (const CompileTimeValue &value : values) {
    result = hashCompileTimeValue(value, result);
  }
  return result;
}

// Covers every field the defaulted SemanticType equality compares.
std::uint64_t hashSemanticType(const SemanticType &type, std::uint64_t seed) {
  std::uint64_t result = combine(seed, static_cast<std::uint64_t>(type.kind));
  result = hashSemanticTypes(type.arguments, result);
  result = hashCompileTimeValues(type.valueArguments, result);
  result = combine(result, type.classId);
  result = combine(result, type.enumId);
  result = combine(result, type.genericParameterId);
  result = combine(result, type.lambdaId);
  result = combine(result, type.lambdaParameterCount);
  result = combine(result, type.lambdaCaptureCount);
  result = hashSemanticTypes(type.lambdaEnclosingClassTypes, result);
  result = hashSemanticTypes(type.lambdaEnclosingFunctionTypes, result);
  result = hashCompileTimeValues(type.lambdaEnclosingClassValues, result);
  result = hashCompileTimeValues(type.lambdaEnclosingFunctionValues, result);
  result = combine(result, type.arrayLength);
  result = combine(result, type.arrayLengthParameterId);
  result = combine(result, static_cast<std::uint64_t>(type.referenceAccess));
  result = combine(result, static_cast<std::uint64_t>(type.pointerAccess));
  result = combine(result, static_cast<std::uint64_t>(type.concretePack));
  return result;
}

struct ClassKey {
  ClassId declaration = 0;
  std::vector<SemanticType> typeArguments;
  std::vector<CompileTimeValue> valueArguments;

  friend bool operator==(const ClassKey &, const ClassKey &) = default;
};

struct ClassKeyHash {
  [[nodiscard]] std::size_t operator()(const ClassKey &key) const {
    std::uint64_t result = combine(0x67746921ULL, key.declaration);
    result = hashSemanticTypes(key.typeArguments, result);
    result = hashCompileTimeValues(key.valueArguments, result);
    return finalize(result);
  }
};

struct FunctionKey {
  FunctionId declaration = 0;
  std::vector<SemanticType> classTypeArguments;
  std::vector<CompileTimeValue> classValueArguments;
  std::vector<SemanticType> functionTypeArguments;

  friend bool operator==(const FunctionKey &, const FunctionKey &) = default;
};

struct FunctionKeyHash {
  [[nodiscard]] std::size_t operator()(const FunctionKey &key) const {
    std::uint64_t result = combine(0x67746946ULL, key.declaration);
    result = hashSemanticTypes(key.classTypeArguments, result);
    result = hashCompileTimeValues(key.classValueArguments, result);
    result = hashSemanticTypes(key.functionTypeArguments, result);
    return finalize(result);
  }
};

struct ConstructorKey {
  ConstructorId declaration = 0;
  std::size_t owner = 0;

  friend bool operator==(const ConstructorKey &,
                         const ConstructorKey &) = default;
};

struct ConstructorKeyHash {
  [[nodiscard]] std::size_t operator()(const ConstructorKey &key) const {
    return finalize(
        combine(combine(0x67746943ULL, key.declaration), key.owner));
  }
};

} // namespace

struct HirInstanceIndex::Implementation {
  std::unordered_map<ClassKey, std::size_t, ClassKeyHash> classes;
  std::unordered_map<FunctionKey, std::size_t, FunctionKeyHash> functions;
  std::unordered_map<ConstructorKey, std::size_t, ConstructorKeyHash>
      constructors;
  std::unordered_map<std::size_t, std::size_t> destructors;
};

HirInstanceIndex::HirInstanceIndex()
    : implementation(std::make_unique<Implementation>()) {}

HirInstanceIndex::~HirInstanceIndex() = default;

HirInstanceIndex::HirInstanceIndex(HirInstanceIndex &&other) noexcept = default;

HirInstanceIndex &
HirInstanceIndex::operator=(HirInstanceIndex &&other) noexcept = default;

std::optional<std::size_t> HirInstanceIndex::findClass(
    ClassId declaration, const std::vector<SemanticType> &typeArguments,
    const std::vector<CompileTimeValue> &valueArguments) const {
  const auto found = implementation->classes.find(
      ClassKey{declaration, typeArguments, valueArguments});
  if (found == implementation->classes.end()) {
    return std::nullopt;
  }
  return found->second;
}

void HirInstanceIndex::recordClass(ClassId declaration,
                                   std::vector<SemanticType> typeArguments,
                                   std::vector<CompileTimeValue> valueArguments,
                                   std::size_t instance) {
  implementation->classes.emplace(ClassKey{declaration,
                                           std::move(typeArguments),
                                           std::move(valueArguments)},
                                  instance);
}

std::optional<std::size_t> HirInstanceIndex::findFunction(
    FunctionId declaration, const std::vector<SemanticType> &classTypeArguments,
    const std::vector<CompileTimeValue> &classValueArguments,
    const std::vector<SemanticType> &functionTypeArguments) const {
  const auto found = implementation->functions.find(
      FunctionKey{declaration, classTypeArguments, classValueArguments,
                  functionTypeArguments});
  if (found == implementation->functions.end()) {
    return std::nullopt;
  }
  return found->second;
}

void HirInstanceIndex::recordFunction(
    FunctionId declaration, std::vector<SemanticType> classTypeArguments,
    std::vector<CompileTimeValue> classValueArguments,
    std::vector<SemanticType> functionTypeArguments, std::size_t instance) {
  implementation->functions.emplace(
      FunctionKey{declaration, std::move(classTypeArguments),
                  std::move(classValueArguments),
                  std::move(functionTypeArguments)},
      instance);
}

std::optional<std::size_t>
HirInstanceIndex::findConstructor(ConstructorId declaration,
                                  std::size_t owner) const {
  const auto found =
      implementation->constructors.find(ConstructorKey{declaration, owner});
  if (found == implementation->constructors.end()) {
    return std::nullopt;
  }
  return found->second;
}

void HirInstanceIndex::recordConstructor(ConstructorId declaration,
                                         std::size_t owner,
                                         std::size_t instance) {
  implementation->constructors.emplace(ConstructorKey{declaration, owner},
                                       instance);
}

std::optional<std::size_t>
HirInstanceIndex::findDestructor(std::size_t owner) const {
  const auto found = implementation->destructors.find(owner);
  if (found == implementation->destructors.end()) {
    return std::nullopt;
  }
  return found->second;
}

void HirInstanceIndex::recordDestructor(std::size_t owner,
                                        std::size_t instance) {
  implementation->destructors.emplace(owner, instance);
}

} // namespace lang
