#pragma once

#include "gti/diagnostic.h"
#include "gti/mir.h"
#include "gti/target.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace lang {

class HirProgram;
class Program;
class SemanticModel;
struct LoweredProgramTestAccess;

using LoweredDeclarationId = std::size_t;

enum class LoweredBodyDefinitionKind {
  ImplicitSource,
  Source,
  CompilerGenerated,
  RuntimeBinding,
  Declaration,
  Count,
};

enum class LoweredBodyRole {
  SourceExecutable,
  AbiDeclaration,
  DataOnly,
  Count,
};

struct LoweredBodyIdentity {
  MirBodyAddress address;
  PlaceDomain placeDomain;
  LoweredBodyDefinitionKind definition =
      LoweredBodyDefinitionKind::ImplicitSource;
  std::size_t declaration = 0;
  std::size_t concreteOwner = 0;
  SourceSpan source;

  friend bool operator==(const LoweredBodyIdentity &,
                         const LoweredBodyIdentity &) = default;
};

enum class LoweredGeneratedItemKind {
  ProgramInitialization,
  HostedEntry,
  StructuralOperatorAdapter,
  CallableAdapter,
  LifecycleCleanup,
  NativeInteropAdapter,
  ConcreteInstanceAdapter,
  Count,
};

struct LoweredGeneratedItemIdentity {
  LoweredGeneratedItemKind kind =
      LoweredGeneratedItemKind::ProgramInitialization;
  std::size_t owner = 0;
  std::size_t ordinal = 0;

  friend bool operator==(const LoweredGeneratedItemIdentity &,
                         const LoweredGeneratedItemIdentity &) = default;
};

struct LoweredBody {
  LoweredBodyIdentity identity;
  LoweredBodyRole role = LoweredBodyRole::SourceExecutable;
  std::vector<LoweredGeneratedItemIdentity> requiredGeneratedItems;

  friend bool operator==(const LoweredBody &, const LoweredBody &) = default;
};

enum class LoweredDeclarationKind {
  Namespace,
  NamespaceAlias,
  TypeAlias,
  Class,
  Enum,
  Function,
  Constructor,
  Destructor,
  Storage,
  Access,
  LanguageLinkage,
  Concept,
  Empty,
  Other,
  Count,
};

// This is the declaration census and stable identity layer, not a copied AST.
// Rich declaration payloads are added here as backend reads migrate upstream.
struct LoweredDeclaration {
  LoweredDeclarationId id = 0;
  LoweredDeclarationKind kind = LoweredDeclarationKind::Other;
  LoweredDeclarationId parent = 0;
  std::size_t semanticIdentity = 0;
  ClassId ownerClass = 0;
  std::size_t ordinal = 0;
  std::string name;
  std::vector<std::string> namespaceScope;
  SourceSpan source;
  bool generic = false;

  friend bool operator==(const LoweredDeclaration &,
                         const LoweredDeclaration &) = default;
};

struct LoweredNativeCallbackItem {
  MirNativeCallbackAdapter adapter;

  friend bool operator==(const LoweredNativeCallbackItem &,
                         const LoweredNativeCallbackItem &) = default;
};

using LoweredGeneratedItemPayload =
    std::variant<std::monostate, LoweredNativeCallbackItem>;

struct LoweredGeneratedItem {
  LoweredGeneratedItemIdentity identity;
  MirBodyAddress sourceBody;
  std::vector<LoweredGeneratedItemIdentity> dependencies;
  LoweredGeneratedItemPayload payload;

  friend bool operator==(const LoweredGeneratedItem &,
                         const LoweredGeneratedItem &) = default;
};

enum class LoweredProgramIssueKind {
  UnsupportedTarget,
  FrontendMismatch,
  InvalidSourceMir,
  InvalidOptimizedMir,
  InvalidOptimization,
  InvalidFailureMetadata,
  InvalidBodyInventory,
  InvalidDeclarationInventory,
  InvalidGeneratedItemInventory,
  MissingGeneratedItem,
  DuplicateGeneratedItem,
  OrphanGeneratedItem,
  CyclicGeneratedItemDependency,
  InvalidConstructionSeal,
};

struct LoweredProgramIssue {
  LoweredProgramIssueKind kind = LoweredProgramIssueKind::FrontendMismatch;
  std::string detail;
  std::optional<MirBodyAddress> body;
  std::optional<LoweredDeclarationId> declaration;
  std::optional<LoweredGeneratedItemIdentity> generatedItem;
};

class LoweredProgram final {
public:
  LoweredProgram(const LoweredProgram &) = default;
  LoweredProgram(LoweredProgram &&) noexcept = default;
  LoweredProgram &operator=(const LoweredProgram &) = default;
  LoweredProgram &operator=(LoweredProgram &&) noexcept = default;

  [[nodiscard]] const TargetInfo &target() const { return target_; }
  [[nodiscard]] const MirProgram &mir() const { return mir_; }
  [[nodiscard]] const std::vector<LoweredBody> &bodies() const {
    return bodies_;
  }
  [[nodiscard]] const std::vector<LoweredDeclaration> &declarations() const {
    return declarations_;
  }
  [[nodiscard]] const std::vector<LoweredGeneratedItem> &
  generatedItems() const {
    return generatedItems_;
  }

  [[nodiscard]] const LoweredBody *findBody(MirBodyAddress address) const;
  [[nodiscard]] const LoweredDeclaration *
  findDeclaration(LoweredDeclarationId id) const;
  [[nodiscard]] const LoweredGeneratedItem *
  findGeneratedItem(const LoweredGeneratedItemIdentity &identity) const;

private:
  LoweredProgram() = default;

  TargetInfo target_;
  MirProgram mir_;
  std::vector<LoweredBody> bodies_;
  std::vector<LoweredDeclaration> declarations_;
  std::vector<LoweredGeneratedItem> generatedItems_;
  std::optional<std::uint64_t> constructionSeal_;

  friend class LoweredProgramBuilder;
  friend struct LoweredProgramTestAccess;
  friend std::vector<LoweredProgramIssue>
  verifyLoweredProgram(const LoweredProgram &program);
};

struct LoweredProgramBuild {
  std::optional<LoweredProgram> program;
  std::vector<LoweredProgramIssue> issues;

  [[nodiscard]] bool valid() const {
    return program.has_value() && issues.empty();
  }
};

class LoweredProgramBuilder final {
public:
  [[nodiscard]] LoweredProgramBuild
  build(const Program &program, const SemanticModel &semantics,
        const HirProgram &hir, const MirProgram &sourceMir,
        const MirProgram &optimizedMir, const TargetInfo &target) const;
};

[[nodiscard]] std::vector<LoweredProgramIssue>
verifyLoweredProgram(const LoweredProgram &program);

} // namespace lang
