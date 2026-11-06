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
  friend bool operator==(const PersistentDeclarationOwner &,
                         const PersistentDeclarationOwner &) = default;
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
          std::vector<CompileTimeValue> values = {},
          std::vector<Lifetime> lifetimes = {}) const;

private:
  void bindKey(std::string key, SemanticType type);
  std::map<std::string, SemanticType, std::less<>> byKey;
  std::map<std::pair<SemanticType::Kind, std::size_t>, std::string> byId;
};
[[nodiscard]] std::string
persistentClassKey(const ClassTypeInfo &, const PersistentDeclarationOwner &);
[[nodiscard]] std::string persistentEnumKey(const EnumTypeInfo &,
                                            const PersistentDeclarationOwner &);
// Snapshot-local IDs supplied by the owner of the imported declaration.
// Wire recipes encode binder positions, never these IDs. Quantified IDs are
// supplied in structural preorder when reading; writing can discover them
// from the type itself. Every supplied ID must be nonzero and distinct.
struct PersistentLifetimeBindings {
  std::vector<GenericParameterId> parameters;
  std::vector<GenericParameterId> quantifiedParameters;
};

// Canonical scalar, concrete primary nominal, reference, raw-pointer and
// fixed-array recipes. Nominal references require an explicit registered
// declaration at both ends. All other profiles and noncanonical semantic
// payloads reject.
[[nodiscard]] std::string writePersistentType(const SemanticType &,
                                              const PersistentTypeRegistry &);
[[nodiscard]] SemanticType readPersistentType(std::string_view,
                                              const PersistentTypeRegistry &);
// Parameter-bearing recipes require explicit declaration bindings. Reading
// a quantified callable also requires all of its local binder IDs; decoding
// never invents semantic identities or imports a declaration.
[[nodiscard]] std::string
writePersistentType(const SemanticType &, const PersistentTypeRegistry &,
                    const PersistentLifetimeBindings &);
[[nodiscard]] SemanticType
readPersistentType(std::string_view, const PersistentTypeRegistry &,
                   const PersistentLifetimeBindings &);
// Public contracts must be complete, valid, and declaration-local. Bounds are
// normalized by binder position; alpha-renaming preserves the bytes.
[[nodiscard]] std::string
writePersistentLifetimeContract(const LifetimeSignatureContract &,
                                const PersistentLifetimeBindings &);
[[nodiscard]] LifetimeSignatureContract
readPersistentLifetimeContract(std::string_view,
                               const PersistentLifetimeBindings &);
} // namespace lang
