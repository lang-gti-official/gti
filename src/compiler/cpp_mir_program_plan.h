#pragma once

#include "gti/mir.h"

#include <cstddef>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace lang {

class CppMirRepresentationSnapshotBuilderAccess;
struct CppMirRepresentationSnapshotTestAccess;
struct CppMirProgramPlan;

// Private C++ representation planning types. These records intentionally own
// only copied, pointer-free facts. They must never retain Program/AST bodies,
// HirBody, SemanticModel, or OptimizationResult authority.
enum class CppMirProgramPlanStatus {
  Complete,
  UnsupportedSurface,
  Incoherent,
};

enum class CppMirBodyRole {
  SourceExecutable,
  AbiDeclaration,
  DataOnly,
  Count,
};

// `GeneralV1` is the sole production executable family and is derived only by
// the sealed snapshot builder after complete generic-emitter preflight.
// `Unsupported` is a fail-closed test/incoherence sentinel; `None` is reserved
// for bodies that do not execute.
enum class CppMirExecutionFamily {
  None,
  Unsupported,
  GeneralV1,
  Count,
};

enum class CppMirBodyDefinitionKind {
  ImplicitSource,
  Source,
  CompilerGenerated,
  RuntimeBinding,
  Declaration,
  Count,
};

struct CppMirBodyIdentity {
  MirBodyAddress address;
  PlaceDomain placeDomain;
  CppMirBodyDefinitionKind definition =
      CppMirBodyDefinitionKind::ImplicitSource;
  // The declaration identity is the function/lambda declaration or the class
  // declaration for initializer bodies. Constructors and destructors do not
  // yet retain a declaration ID in MIR and therefore use zero plus their exact
  // concrete owner.
  std::size_t declaration = 0;
  std::size_t concreteOwner = 0;

  friend bool operator==(const CppMirBodyIdentity &,
                         const CppMirBodyIdentity &) = default;
};

enum class CppMirThunkKind {
  HostedEntry,
  ProgramInitialization,
  StructuralOperatorAdapter,
  CallableAdapter,
  LifecycleCleanup,
  NativeInteropAdapter,
  ConcreteInstanceAdapter,
  Count,
};

struct CppMirThunkIdentity {
  CppMirThunkKind kind = CppMirThunkKind::HostedEntry;
  std::size_t owner = 0;
  std::size_t ordinal = 0;

  friend bool operator==(const CppMirThunkIdentity &,
                         const CppMirThunkIdentity &) = default;
};

enum class CppMirSurfaceSupport {
  Supported,
  Unsupported,
  Count,
};

struct CppMirBodyRepresentation {
  CppMirBodyIdentity identity;
  // Module/0 is SourceExecutable exactly when its verified merged program
  // plan contains an Initializer step; its tagged implicit-zero/constant
  // blocks remain DataOnly. Other class initializer bodies retain the exact
  // canonical-empty distinction. The copied representation snapshot builder
  // owns exhaustive declaration-data inventory beyond MIR.
  CppMirBodyRole role = CppMirBodyRole::SourceExecutable;
  CppMirExecutionFamily family = CppMirExecutionFamily::Unsupported;
  std::vector<CppMirThunkIdentity> requiredThunks;

  friend bool operator==(const CppMirBodyRepresentation &,
                         const CppMirBodyRepresentation &) = default;
};

enum class CppMirDataKind {
  ConstexprBinding,
  EnumDefinition,
  AbiTypeDeclaration,
  ClassTemplateDeclaration,
  CallableTemplateDeclaration,
  NamespaceDeclaration,
  NamespaceAliasDeclaration,
  TypeAliasDeclaration,
  ClassDeclaration,
  CallableDeclaration,
  StorageDeclaration,
  AccessDeclaration,
  LanguageLinkageDeclaration,
  EmptyDeclaration,
  OtherDeclaration,
  Count,
};

struct CppMirDataIdentity {
  CppMirDataKind kind = CppMirDataKind::ConstexprBinding;
  // Source-declaration rows use the exact semantic declaration ID when that
  // declaration kind has one, otherwise the nonzero active-Program traversal
  // ordinal. Their owner is the semantic class owner when applicable and
  // ordinal always preserves the stable traversal position.
  std::size_t declaration = 0;
  std::size_t owner = 0;
  std::size_t ordinal = 0;

  friend bool operator==(const CppMirDataIdentity &,
                         const CppMirDataIdentity &) = default;
};

struct CppMirDataRepresentation {
  CppMirDataIdentity identity;
  CppMirSurfaceSupport support = CppMirSurfaceSupport::Supported;

  friend bool operator==(const CppMirDataRepresentation &,
                         const CppMirDataRepresentation &) = default;
};

// Target-independent native-callback facts copied from verified MIR. The
// backend chooses the adapter spelling and ABI representation; this row only
// identifies the exact source function and its no-unwind containment policy.
struct CppMirNativeCallbackThunk {
  MirNativeCallbackAdapter adapter;

  friend bool operator==(const CppMirNativeCallbackThunk &,
                         const CppMirNativeCallbackThunk &) = default;
};

using CppMirGeneratedThunkPayload =
    std::variant<std::monostate, CppMirNativeCallbackThunk>;

struct CppMirGeneratedThunk {
  CppMirThunkIdentity identity;
  MirBodyAddress sourceBody;
  CppMirSurfaceSupport support = CppMirSurfaceSupport::Supported;
  // Dependencies are emitted before this thunk. The planner requires the
  // graph to be unique, closed, acyclic, and rooted by a body row. The exact
  // ProgramInitialization source and sole root are executable Module/0 when
  // its verified merged plan contains an Initializer step. Legacy executable
  // generic static-initializer bodies do not infer this thunk.
  std::vector<CppMirThunkIdentity> dependencies;
  CppMirGeneratedThunkPayload payload;

  friend bool operator==(const CppMirGeneratedThunk &,
                         const CppMirGeneratedThunk &) = default;
};

struct CppMirRepresentationSnapshot {
  // Canonical verified optimized MIR that the copied representation facts
  // describe. Planning exact-compares this complete structure rather than a
  // printer/hash projection, then discards the duplicate from its result.
  std::optional<MirProgram> mir;
  std::vector<CppMirBodyRepresentation> bodies;
  // MIR has no passive declaration-data inventory. The sealed copied
  // representation snapshot builder owns exhaustive active-Program traversal
  // for these rows. The planner validates and orders every supplied row and
  // checks the builder's private full-copy inventory seal before doing so; it
  // does not independently reconstruct source declarations from MIR.
  std::vector<CppMirDataRepresentation> data;
  std::vector<CppMirGeneratedThunk> thunks;

private:
  struct InventorySeal {
    std::optional<MirProgram> mir;
    std::vector<CppMirBodyRepresentation> bodies;
    std::vector<CppMirDataRepresentation> data;
    std::vector<CppMirGeneratedThunk> thunks;

    friend bool operator==(const InventorySeal &,
                           const InventorySeal &) = default;
  };

  // Only the production builder and the explicitly named private test peer
  // can establish this exact full-copy seal. The planner checks it before it
  // sorts or moves any row, so coordinated omission/staleness cannot be
  // hidden by otherwise coherent row contents.
  std::optional<InventorySeal> inventorySeal_;

  friend class CppMirRepresentationSnapshotBuilderAccess;
  friend struct CppMirRepresentationSnapshotTestAccess;
  friend CppMirProgramPlan planCppMirProgram(const MirProgram &,
                                             CppMirRepresentationSnapshot);
};

enum class CppMirPlanIssueKind {
  InvalidMirProgram,
  StaleMirIdentity,
  InvalidInventorySeal,
  MissingBodyRow,
  DuplicateBodyRow,
  UnexpectedBodyRow,
  StaleBodyIdentity,
  InvalidBodyRole,
  InvalidExecutionFamily,
  InvalidDataIdentity,
  DuplicateDataIdentity,
  InvalidDataSupport,
  InvalidThunkIdentity,
  DuplicateThunkIdentity,
  InvalidThunkSupport,
  InvalidThunkSource,
  InvalidThunkPayload,
  DuplicateBodyThunkDependency,
  DuplicateThunkDependency,
  MissingThunkDependency,
  MissingContractedThunk,
  UnexpectedContractedThunk,
  InvalidContractedThunkGraph,
  InvalidContractedThunkOrder,
  OrphanThunk,
  CyclicThunkDependency,
};

struct CppMirPlanIssue {
  CppMirPlanIssueKind kind = CppMirPlanIssueKind::InvalidMirProgram;
  std::optional<MirBodyAddress> body;
  std::optional<CppMirDataIdentity> data;
  std::optional<CppMirThunkIdentity> thunk;
  std::string detail;
};

enum class CppMirUnsupportedSurfaceKind {
  Body,
  Data,
  Thunk,
};

struct CppMirUnsupportedSurface {
  CppMirUnsupportedSurfaceKind kind = CppMirUnsupportedSurfaceKind::Body;
  std::optional<MirBodyAddress> body;
  std::optional<CppMirDataIdentity> data;
  std::optional<CppMirThunkIdentity> thunk;
};

struct CppMirProgramPlan {
  CppMirProgramPlanStatus status = CppMirProgramPlanStatus::Incoherent;
  // Valid plans store bodies in enumerateMirBodyAddresses order, data in
  // identity order, and thunks in deterministic dependency-before-user order.
  std::vector<CppMirBodyRepresentation> bodies;
  std::vector<CppMirDataRepresentation> data;
  std::vector<CppMirGeneratedThunk> thunks;
  std::vector<CppMirPlanIssue> issues;
  std::vector<CppMirUnsupportedSurface> unsupported;

  [[nodiscard]] bool coherent() const {
    return status != CppMirProgramPlanStatus::Incoherent;
  }
  [[nodiscard]] bool complete() const {
    return status == CppMirProgramPlanStatus::Complete;
  }
};

// Capture helpers deliberately return copied identities. A representation
// snapshot builder uses these after its semantic/HIR selectors have made the
// body-role and execution-family decision.
[[nodiscard]] std::optional<CppMirBodyIdentity>
captureCppMirBodyIdentity(const MirProgram &program, MirBodyAddress address);

// Plan the complete program before any output bytes are produced. Malformed
// or stale representation facts are Incoherent; a structurally valid snapshot
// containing any unsupported body/data/thunk is UnsupportedSurface.
[[nodiscard]] CppMirProgramPlan
planCppMirProgram(const MirProgram &program,
                  CppMirRepresentationSnapshot snapshot);

} // namespace lang
