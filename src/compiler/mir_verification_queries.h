#pragma once

#include "gti/concrete_formals.h"

#include "gti/mir.h"
#include "gti/mir_verifier.h"
#include "gti/reference_contents.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <span>

namespace lang::mir_detail {

// Read-only verification acceleration, never proof authority. The body must
// remain immutable for this scope. Nested scopes reuse the same body's index
// or restore the previous body's index. No index survives an editor mutation.
class MirBodyReadScope {
public:
  explicit MirBodyReadScope(const MirBody &body);
  ~MirBodyReadScope();
  MirBodyReadScope(const MirBodyReadScope &) = delete;
  MirBodyReadScope &operator=(const MirBodyReadScope &) = delete;

private:
  struct Index;
  static thread_local const Index *current;
  const Index *previous;
  std::unique_ptr<Index> owned;
  friend std::optional<std::span<const MirValue *const>>
  indexedSourceValues(const MirBody &, HirValueId);
  friend std::optional<const MirInstruction *>
  indexedInstruction(const MirBody &, const MirBlock &, MirInstructionId);
};
// nullopt means no applicable index: the caller retains its scan semantics.
[[nodiscard]] std::optional<std::span<const MirValue *const>>
indexedSourceValues(const MirBody &body, HirValueId source);
[[nodiscard]] std::optional<const MirInstruction *>
indexedInstruction(const MirBody &body, const MirBlock &block,
                   MirInstructionId id);

// Validate the semantic representation witness against the executable null
// path, without interpreting the non-null destructor or inspecting source.
[[nodiscard]] bool nullGuardedDestructor(const MirProgram &program,
                                         const MirClassInstance &owner);

using ::lang::MirDebugPoint;

using ::lang::MirDebugAvailabilityReason;
using ::lang::MirDebugReadability;

struct MirExistingPlaceQuery {
  MirPlaceId place = 0;
  MirDebugPoint point;
  friend bool operator==(const MirExistingPlaceQuery &,
                         const MirExistingPlaceQuery &) = default;
};

struct MirExistingPlaceFact {
  MirExistingPlaceQuery request;
  MirDebugReadability readability = MirDebugReadability::Unknown;
  MirDebugAvailabilityReason reason = MirDebugAvailabilityReason::Unproven;
};

struct MirExistingPlaceQueryBatch {
  MirVerificationResult verification;
  std::vector<MirExistingPlaceFact> requested;
  // A positive interval cannot cross these canonical inactive/consuming cuts.
  std::vector<MirExistingPlaceQuery> requiredStops;
};

// Fresh observations of the verifier's canonical transfers. This transient
// result is not a proof token and cannot authorize a lowered snapshot.
[[nodiscard]] MirExistingPlaceQueryBatch
verifyMirBodyWithPlaceQueries(const MirBody &body, std::size_t owner,
                              std::span<const MirExistingPlaceQuery> requests);

struct MirDebugEntryPlaces {
  MirVerificationResult verification;
  std::vector<MirPlaceId> initialized;
};
// Exact concrete formal/receiver entry facts, not a lifetime or borrow proof.
// The program-aware query combines these fresh facts with canonical body flow.
[[nodiscard]] MirDebugEntryPlaces debugEntryPlaces(const MirProgram &program,
                                                   MirBodyAddress address);
[[nodiscard]] MirExistingPlaceQueryBatch
verifyMirBodyWithPlaceQueries(const MirProgram &program, MirBodyAddress address,
                              std::span<const MirExistingPlaceQuery> requests);

// Shape of one closed consuming-arm setup: cache every payload address, retire
// one exact dependency group, then publish its successor bindings. This is not
// authority by itself: body flow must observe the old group alive, and program
// verification must prove the sealed field inventory and complete origin sets.
struct MirSelectedReferenceHandoff {
  MirBlockId block = 0;
  MirPlaceId carrier = 0;
  MirLoanId group = 0;
  MirInstructionId retirement = 0;
  std::size_t endPosition = 0;
  std::vector<MirLoanId> successors;
};
[[nodiscard]] std::optional<MirSelectedReferenceHandoff>
selectedReferenceHandoff(const MirBody &body, const MirLoan &selected);

// Recover a physical payload address from the executable extraction and exact
// typed enum instance. This grants no external-owner or consuming authority;
// switch-edge, loan liveness and cleanup verification remain mandatory.
[[nodiscard]] const MirPlace *
physicalPayloadSelectionSource(const MirProgram &, const MirBody &,
                               const MirReferenceSelection &);

// Derive a stored reference's declared type from its exact executable payload
// extraction and actual enum view. This establishes neither external origins
// nor parent activity, switch dominance or consuming handoff authority.
[[nodiscard]] std::optional<SemanticType>
storedPayloadSelectionReference(const MirProgram &, const MirBody &,
                                const MirReferenceSelection &);

// Body-local executable shape only. This does not prove Base owner/selection,
// field types or reference crossings; mandatory program verification must
// replay the typed path with walkBorrowPath before accepting those facts.
[[nodiscard]] std::optional<std::size_t> borrowPathProjectionPrefix(
    std::span<const MirPlaceProjection>, std::span<const PlaceProjection>,
    bool trailingReference);

// Structural producer identity only; the program verifier replays the selected
// constructor, lifetime application and complete staged argument dependencies.
[[nodiscard]] const MirInstruction *baseConstructorPublicationStage(
    const MirBody &, const MirLoan &);

enum class MirBorrowPathRoot { Value, PhysicalReference };

struct MirBorrowPathReferenceCrossing {
  std::size_t pathOffset = 0;
  std::size_t projectionOffset = 0;
  SemanticType reference = SemanticType::Unknown;
  bool physicalRoot = false;
  friend bool operator==(const MirBorrowPathReferenceCrossing &,
                         const MirBorrowPathReferenceCrossing &) = default;
};

struct MirBorrowPathPayloadSelection {
  std::size_t pathOffset = 0;
  std::size_t projectionOffset = 0;
  SemanticType enumeration = SemanticType::Unknown;
  std::size_t variant = 0;
  std::size_t payloadIndex = 0;
  friend bool operator==(const MirBorrowPathPayloadSelection &,
                         const MirBorrowPathPayloadSelection &) = default;
};

struct MirBorrowPathView {
  SemanticType type = SemanticType::Unknown;
  AccessMode referenceAccessLimit = AccessMode::ReadOnly;
  std::vector<MirPlaceProjection> projections;
  std::vector<MirBorrowPathReferenceCrossing> referenceCrossings;
  std::vector<MirBorrowPathPayloadSelection> payloadSelections;
  friend bool operator==(const MirBorrowPathView &,
                         const MirBorrowPathView &) = default;
};

// Typed structure only: these queries grant no origin, active-variant,
// capability, parent, liveness, escape, write or cleanup authority. Physical
// root-reference treatment is explicit; value roots cannot hide a reference.
// Base uses the full direct declaration ordinal (including interfaces) in
// constantIndex and the independently selected concrete instance in selection.
[[nodiscard]] std::optional<MirBorrowPathView> walkBorrowPath(
    const MirProgram &, const SemanticType &root, AccessMode rootAccess,
    std::span<const PlaceProjection>, std::span<const BorrowDependencyCondition>,
    MirBorrowPathRoot);

// Derive which exact typed enum domains a stable payload-symbol path crosses.
// This proves structural selection only; active-variant/dominance permission
// remains with the caller. Duplicate symbols or any failed prefix replay reject.
[[nodiscard]] std::optional<std::vector<BorrowDependencyCondition>>
deriveBorrowPathConditions(const MirProgram &, const SemanticType &, AccessMode,
                           std::span<const PlaceProjection>, MirBorrowPathRoot);

// Recover only a path whose forward replay exactly reproduces the canonical
// projections and payload selections. An SSA index needs an independently
// canonicalized constant first; absent base ordinals and ambiguous views fail.
[[nodiscard]] std::optional<BorrowPath> borrowPathForProjections(
    const MirProgram &, const SemanticType &root, AccessMode rootAccess,
    std::span<const MirPlaceProjection>, std::span<const BorrowDependencyCondition>,
    std::span<const MirBorrowPathPayloadSelection>, MirBorrowPathRoot);

// Additional inline Base/field disjointness only. Both canonical projection
// chains must reverse-replay against this exact root and nominal catalog. A
// physical reference root may retain or omit its one initial dereference in
// canonical form; no later reference/pointer crossing or enum selection proves
// disjoint external storage. False means unknown/overlap, not a valid path.
[[nodiscard]] bool inlineBorrowProjectionsDisjoint(
    const MirProgram &, SemanticType root,
    std::span<const MirPlaceProjection> left,
    std::span<const MirPlaceProjection> right);

// Concrete complete object representation, independent of any lifetime action.
[[nodiscard]] bool completeObjectType(const MirProgram &program,
                                      const SemanticType &type);

// Trivial generated default and same-type copy/move construction have no
// source constructor target. The program verifier checks the class's exact
// special-member policy, including the absence of a required default schedule.
// Allocation and staged input ownership still have distinct MIR identities.
[[nodiscard]] bool
generatedUniqueConstruction(const MirInstruction &instruction);

// A zero-input ordinary Construct explicitly represents generated default
// construction without a source constructor instance. An unexplained Call is
// never construction authority. This is only its producer shape:
// program verification must prove the exact owner's generated policy and
// absence of a required initializer schedule, while ordinary materialization,
// full-expression drop, and call-loan verification remain mandatory.
[[nodiscard]] bool
generatedDefaultConstruction(const MirInstruction &instruction);

// Structural same-type copy has no selected constructor body. This proves
// only the exact instruction and identity field-map shape; program checks
// independently prove the generated policy, typed field inventory and source
// contents. A lifetime supplier is not an executable source address.
[[nodiscard]] bool
generatedCopyConstruction(const MirInstruction &instruction);


// A passive union's one-input first-field initialization is independent of
// generated default construction. Prove its exact concrete owner, overlapping
// field layout, first field type and neutral construction metadata. This does
// not grant source-call, cleanup, borrow or precise effect authority.
[[nodiscard]] bool
passiveUnionFieldConstruction(const MirProgram &program,
                              const MirInstruction &instruction);

// Only passive scalar destinations participate. NotApplicable grants no
// reference, aggregate, text, callable or ownership-return authority. Existing
// initialization conversion policy owns eligibility. Copy/Move bind the source
// type to their exact selected place; ordinary operand and availability
// verification remains mandatory.
enum class MirScalarReturnConversion {
  NotApplicable,
  Exact,
  Initialization,
  Invalid
};
[[nodiscard]] MirScalarReturnConversion
scalarReturnConversion(const MirBody &body, const MirOperand &operand);

// Runtime representation queries have no runtime input and cannot acquire
// ownership or borrow authority merely by naming a type.
[[nodiscard]] bool objectLayoutQuery(const MirProgram &program,
                                     const MirInstruction &instruction);
// Exact raw-address lifetime end. Cleanup/failure recipes are independently
// derived by the program verifier; this query checks the operation's shape.
[[nodiscard]] bool objectDestroy(const MirProgram &program,
                                 const MirInstruction &instruction);
[[nodiscard]] bool objectConstruct(const MirProgram &program,
                                   const MirInstruction &instruction);
[[nodiscard]] bool objectRelocate(const MirProgram &program,
                                  const MirInstruction &instruction);
[[nodiscard]] bool objectTake(const MirProgram &program,
                              const MirInstruction &instruction);

// Inert, const-preserving typed/erased address conversion; never a borrow or
// an assertion that the pointee is initialized, aligned, live or writable.
[[nodiscard]] bool rawPointerConversion(const MirInstruction &instruction);

// The runtime address comes from operand zero; the checked lifetime and
// access authority come from the explicit witness in operand one.
[[nodiscard]] const MirLoan *
rawBorrowResultLoan(const MirBody &body, const MirInstruction &instruction);
[[nodiscard]] const MirLoan *
erasedReferenceResultLoan(const MirProgram &program, const MirBody &body,
                          const MirInstruction &instruction);
// A restored borrow-carrying referent retains the exact opaque checked
// witness. This body-local query traces explicit restore publications and
// transfers only; program verification separately proves the concrete
// intrinsic/helper contract and complete reference-field paths.
[[nodiscard]] const MirInstruction *
restoredBorrowAuthority(const MirBody &body, const MirLoan &loan);
// Program-wide proof that one restored-reference call is backed by an exact,
// finite source-helper chain ending at the checked restore intrinsic. This is
// the verifier's authority for the call's opaque witness, type, fields, access
// and argument origin; a summary flag or public helper name is never enough.
[[nodiscard]] bool
verifiedReferenceRestoreCall(const MirProgram &program, const MirBody &body,
                             const MirInstruction &instruction);
// Derived address identity through trusted erasure and finite source helpers.
// The returned operand is the exact caller argument at the erasing boundary,
// including its CallInput value if staged. Every normal helper return must
// forward the same whole reference formal; lifetime-owner sources, summaries,
// raw witnesses and projected helper fields never establish pointer identity.
// This is not liveness, escape, contents or lifetime-conversion authority.
struct MirReferenceErasureSource {
  MirOperand operand;
  SemanticType referenceType = SemanticType::Unknown;
};
[[nodiscard]] std::optional<MirReferenceErasureSource>
verifiedReferenceErasureSource(const MirProgram &program, const MirBody &body,
                               const MirOperand &opaqueOperand);
// Nonempty, exact local reference aliases of a CallResult. Every alias has
// one dominating Initialize from this loan, after its unique typed producer
// and before consumerPosition; its pointer storage is never reseated, moved,
// copied, addressed or passed to another operation. Returns that exact
// producer. Ordinary call, type, provenance and live-loan verification remain
// mandatory; this query proves alias storage identity, not independent escape
// authority.
[[nodiscard]] const MirInstruction *
callResultReferenceAliases(const MirBody &body, const MirLoan &loan,
                           const MirBlock &consumer,
                           std::size_t consumerPosition);
// Direct caller-owned restoration shape, usable while lowering an incomplete
// CFG. It grants no escape authority without the complete alias/order proof.
[[nodiscard]] bool restoredReferenceReturnWitness(const MirBody &body,
                                                  const MirLoan &loan);
// A named restored reference may publish only the same direct caller-owned
// opaque witness. This adds no origin or lifetime to the alias identity proof.
[[nodiscard]] bool restoredReferenceReturnAliases(const MirBody &body,
                                                  const MirLoan &loan,
                                                  const MirBlock &consumer,
                                                  std::size_t consumerPosition);
// Exact safe GTI invocation signature and stateless selected body. These are
// language-call boundaries, not native C function-pointer adaptation.
[[nodiscard]] const SemanticType *
functionPointerSignature(const MirProgram &program, const SemanticType &type);
// A reference field may bind the exact reference returned by an ordinary
// checked helper. This correlates the initializer call, argument-origin
// contract and escaping Stored publication without assuming equal referent
// types or replacing execution of the helper with its input address.
[[nodiscard]] const MirInstruction *
referenceInitializerCall(const MirProgram &program,
                         const MirConstructorInstance &constructor,
                         const MirConstructorInitializer &initializer);
// Derive concrete lanes from exact grouped binding records. Nested pack
// indices remain part of identity even when their leaf types are equal.
[[nodiscard]] std::optional<std::vector<ConcreteFormalInput>>
concreteFormalInputs(const HirBody &, std::span<const SemanticType>,
                     std::span<const HirBindingId>);
[[nodiscard]] std::optional<std::vector<ConcreteFormalInput>>
concreteFormalInputs(const MirBody &, std::span<const SemanticType>,
                     std::span<const HirBindingId>);
using MirReturnOriginFormal = ConcreteFormalInput;
[[nodiscard]] std::optional<MirReturnOriginFormal>
returnOriginFormal(const HirBody &, std::span<const SemanticType>,
                   std::span<const HirBindingId>, std::size_t expandedIndex);
[[nodiscard]] std::optional<MirReturnOriginFormal>
returnOriginFormal(const MirBody &, std::span<const SemanticType>,
                   std::span<const HirBindingId>, std::size_t expandedIndex);
// Exact typed formal shell. It does not assert that an entry loan is valid,
// live or permitted to escape; those facts remain with the owning verifier.
[[nodiscard]] bool formalInputPlace(const MirPlace &, const ConcreteFormalInput &);
// A text result that returns one direct text field of its origin formal or
// receiver (L-OWN-02). It carries the dependency the caller's value holds in
// that field; inside the callee the field's text is ownerless, so no loan
// names the origin there. TextContracts proves the returned field.
[[nodiscard]] bool textFieldResult(const SemanticType &returnType,
                                   BorrowOriginKind origin,
                                   std::size_t parameter,
                                   std::span<const BorrowedFieldOrigin> fields);

[[nodiscard]] bool functionPointerOperation(const MirProgram &program,
                                            const MirInstruction &instruction);
// Unsafe raw text assertions retain a checked witness without assigning
// ownership or validity to the address itself. The exact ordered input
// stages and resulting loan source are shared by verification and emission.
[[nodiscard]] const MirLoan *
textAddressResultLoan(const MirBody &body, const MirInstruction &instruction,
                      const MirProgram *program = nullptr);
// A distinct loan-rooted address retains either descendant provenance or a
// parallel read-only loan over the identical source/dependencies. Check typed
// call publication too; this proves identity and access, never liveness or
// permission to use a suspended loan.
[[nodiscard]] bool referenceAddressRetainsLoan(const MirBody &body,
                                               const MirPlace &address,
                                               const MirLoan &retained);

// A stable, checked field projection through an entry reference parameter is
// still caller-owned. This establishes owner containment, not the referent
// address or permission to escape; call publication and loan flow prove those.
[[nodiscard]] bool referenceParameterContainsPlace(const MirBody &body,
                                                   const MirLoan &parameter,
                                                   MirPlaceId place);

// Publish an exact runtime alias of an entry-reference dependency. A unique,
// dominating initializer chain must establish its typed address, and the return
// expression must identify the selected alias. Shared by verification and
// backend admission; carrier membership alone never proves the referent.
// A scalar result selects one exact unconditional stored-reference path
// from an input/receiver entry group, preserving its evaluated field pointer.
// Program verification separately replays the declaration's typed path.
// Declaration reachability is separate from the finite inventory stored in
// one value. Crossings prove typed selectors only, never runtime read access.
[[nodiscard]] ReferenceSupplierQuery referenceSupplierPaths(
    const MirProgram &program, const SemanticType &type);

// Staged structural profile only: exact evaluated reference type/address
// shape, complete direct unconditional shared fields, explicit source tags and
// finite read ancestry. This does not prove that the reads authorize this
// address, dominate formation or remain active; it grants no alias, lifetime,
// contents-flow or execution permission. The admission guards remain closed.
[[nodiscard]] std::optional<ReferenceContentsInventory>
referenceContentsOpeningShape(const MirProgram &, const MirBody &, const MirLoan &);

// Detect the staged checked-reference contents vocabulary before legacy
// physical-place consumers can mistake an absent place for absent provenance.
[[nodiscard]] bool hasReferenceContents(const MirBorrowDependencySet &dependencies);
[[nodiscard]] bool hasReferenceContents(const MirBody &body);

// Exact whole-group publication into an inline constructor field. This is
// independent of variant guards; program verification still derives the typed
// initializer fact and checks all normal exits, while loan flow checks liveness.
[[nodiscard]] bool storedCarrierConstructorPublication(const MirBody &body,
                                                       const MirLoan &loan);

// A lowering candidate only: preserve every original supplier while exporting
// the exact incoming reference shell. Program admission must independently
// replay address-preserving erasure/restoration and complete producer facts.
// This initial structural profile has only shared, unconditional suppliers.
struct MirReferenceReturnProjection {
  const MirInstruction *restoration = nullptr;
  const MirInstruction *erasure = nullptr;
  MirOperand erasureInput;
  SemanticType referenceType = SemanticType::Unknown;
  MirLoanId parameter = 0;
  MirLoanId stored = 0;
  MirBorrowDependencySet originalSuppliers;
  MirBorrowDependencySet publication;
};
[[nodiscard]] std::optional<MirReferenceReturnProjection>
candidateSharedErasedReferenceReturn(const MirBody &, const MirLoan &);
[[nodiscard]] std::optional<MirReferenceReturnProjection>
verifiedSharedErasedReferenceReturn(const MirProgram &, const MirBody &,
                                   const MirLoan &, std::size_t owner);
[[nodiscard]] bool matchesSharedErasedReferenceReturn(
    const MirBody &, const MirBlock &, const MirInstruction &,
    const MirReferenceReturnProjection &);

[[nodiscard]] bool storedReferenceReturnPublication(const MirBody &body,
                                                    const MirBlock &block,
                                                    const MirInstruction &instruction);
[[nodiscard]] bool
referenceParameterReturnPublication(const MirBody &body, const MirBlock &block,
                                    const MirInstruction &instruction);

// Recognize the exact, single-use signed-minimum literal schedule shared by
// native emission and constant execution. Ordinary runtime negation is not
// interchangeable with this failure-free literal formation.
[[nodiscard]] const MirInstruction *
fusedSignedMinimumLiteral(const MirBody &body, const MirInstruction &negation);
[[nodiscard]] const MirInstruction *
fusedSignedMinimumNegation(const MirBody &body, const MirInstruction &carrier);

[[nodiscard]] const MirInstruction *definitionFor(const MirBody &body,
                                                  MirValueId value);
// Recover a cleanup-bound carrier's original publication after subsequent
// moves changed its current carrier list. This proves identity, not liveness;
// ordinary ownership/loan dataflow still checks availability at each use.
[[nodiscard]] bool bindingReceivedStoredLoan(const MirBody &body,
                                             HirBindingId binding,
                                             MirLoanId dependency,
                                             std::size_t depth = 0);
// Structural entry-domain projection only. The caller still proves root,
// formal identity, access and liveness. A grouped entry retains its complete
// concrete PackElement prefix; references to packs remain atomic.
[[nodiscard]] std::optional<std::size_t> entryProjectionPrefix(
    const MirBody &, const MirPlace &entry,
    std::span<const MirPlaceProjection> selected);
// Match a whole entry lane or its exact physical reference referent. The
// complete pack prefix and type remain exact; no field, payload or second
// reference crossing is implied. Access, guards and liveness remain separate.
[[nodiscard]] bool wholeEntryPlaceMatches(const MirBody &, const MirPlace &entry,
                                          const MirPlace &selected);
[[nodiscard]] std::optional<MirPlaceId>
borrowSourceForPlace(const MirBody &body, MirPlaceId place, std::size_t depth);
[[nodiscard]] std::optional<MirPlaceId>
borrowSourceForOperand(const MirBody &body, const MirOperand &operand,
                       std::size_t depth = 0);
[[nodiscard]] std::optional<MirPlaceId>
borrowSourceForValue(const MirBody &body, MirValueId value, std::size_t depth);

} // namespace lang::mir_detail
