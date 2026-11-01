#pragma once
#include "gti/semantic_model.h"
#include <map>
#include <string>
#include <string_view>

namespace lang {
// Logical owner selected by the driver, never a checkout path or source digest.
// Each component is explicit so relocating sources cannot change identity.
class PersistentTypeRegistry;
struct PersistentDeclarationOwner {
  std::string package;
  std::string target;
  std::string module;
  // Required only for source-local declarations; relative logical source key.
  std::string unit;
};

// First structural profile: nongeneric namespace functions with fixed scalar
// parameter types. This is an identity, NOT a callable interface/borrow proof.
// Unsupported shapes throw invalid_argument rather than losing semantic facts.
// Return types and bodies do not participate in overload identity.
[[nodiscard]] std::string
persistentFunctionKey(const FunctionInfo &function,
                      const PersistentDeclarationOwner &owner);

struct PersistentFunctionKey {
  PersistentDeclarationOwner owner;
  std::string qualifiedName;
  bool sourceLocal = false;
  std::vector<SemanticType> parameters;
};
// Bounded decoder maps wire tags directly to existing SemanticType values.
// It does not import a declaration or authorize calls or compiler capabilities.
[[nodiscard]] PersistentFunctionKey
readPersistentFunctionKey(std::string_view bytes);
[[nodiscard]] std::string
writePersistentFunctionKey(const PersistentFunctionKey &key);
// Recipe-based function keys admit canonical registered nominal/reference
// types.
[[nodiscard]] std::string
writePersistentFunctionKey(const PersistentFunctionKey &,
                           const PersistentTypeRegistry &);
[[nodiscard]] PersistentFunctionKey
readPersistentFunctionKey(std::string_view, const PersistentTypeRegistry &);
// A per-semantic-snapshot mapping, never a persisted table of numeric IDs.
// Binds verified source/import declarations; it does not create declarations,
// establish visibility, import layout or grant compiler trust.
class PersistentTypeRegistry final {
public:
  void bind(const ClassTypeInfo &, const PersistentDeclarationOwner &);
  void bind(const EnumTypeInfo &, const PersistentDeclarationOwner &);
  [[nodiscard]] std::string key(const SemanticType &) const;
  [[nodiscard]] std::string qualifiedName(const SemanticType &) const;
  [[nodiscard]] SemanticType
  resolve(std::string_view key, std::vector<SemanticType> arguments = {},
          std::vector<CompileTimeValue> values = {}) const;

private:
  void bindKey(std::string key, SemanticType type);
  std::map<std::string, SemanticType, std::less<>> byKey;
  std::map<std::pair<SemanticType::Kind, std::size_t>, std::string> byId;
};
[[nodiscard]] std::string
persistentClassKey(const ClassTypeInfo &, const PersistentDeclarationOwner &);
[[nodiscard]] std::string persistentEnumKey(const EnumTypeInfo &,
                                            const PersistentDeclarationOwner &);
// Canonical scalar, concrete primary nominal, reference, raw-pointer and
// fixed-array recipes. Nominal references require an explicit registered
// declaration at both ends. All other profiles and noncanonical semantic
// payloads reject.
[[nodiscard]] std::string writePersistentType(const SemanticType &,
                                              const PersistentTypeRegistry &);
[[nodiscard]] SemanticType readPersistentType(std::string_view,
                                              const PersistentTypeRegistry &);
} // namespace lang
