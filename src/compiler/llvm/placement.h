#pragma once

#include "gti/mir.h"

#include <map>
#include <set>

namespace lang::native_llvm {

// Class-containing aggregates carry object identity, not LLVM SSA snapshots.
[[nodiscard]] inline bool addressValue(const SemanticType &type) {
  return type.kind == SemanticType::Class ||
         (type.kind == SemanticType::Array && type.arguments.size() == 1 &&
          addressValue(type.arguments[0]));
}

[[nodiscard]] inline bool addressValue(const MirProgram &program,
                                       const SemanticType &type) {
  return type.kind == SemanticType::Class ||
         (type.kind == SemanticType::Enum && program.findPayloadEnum(type)) ||
         type.kind == SemanticType::Lambda ||
         (type.kind == SemanticType::Array && type.arguments.size() == 1 &&
          addressValue(program, type.arguments[0]));
}

// A unique owner's pointee: a complete object type, or a polymorphic class
// view (possibly an interface or abstract class) whose complete object the
// owner's dispatch descriptor identifies. Either way the owner is one handle.
// A place that always denotes a complete object of its class: a whole
// binding, value or hidden temporary, a field, array or pack element, or any
// place of a class nothing in the sealed program derives from. Replacing such
// an object cannot change the dynamic type of an enclosing object.
[[nodiscard]] bool completeObjectPlace(const MirProgram &program,
                                       const MirPlace &place);

[[nodiscard]] bool uniqueOwnerPointee(const MirProgram &program,
                                      const SemanticType &pointee);

// Exclusion only: inline hierarchy state cannot use ordinary object-byte
// transport. Indirections transport addresses, not their referents. Unknown,
// missing or cyclic storage fails closed; false is never inferred from it.
// This does not establish layout, borrowing, fresh placement or admission.
[[nodiscard]] bool containsHierarchyStorage(const MirProgram &program,
                                            const SemanticType &type);

// A candidate is not permission to transfer. The complete pure-MIR plan below
// binds its exact source, generated policy, ownership and fresh publication.
[[nodiscard]] bool
isFreshHierarchyTransferProducer(const MirInstruction &instruction);

// Where a planned hierarchy transfer publishes its fresh object: a whole fresh
// local or constructor field (Initialize), a prepared by-value argument
// (CallInput), the body's ABI return destination, an inline polymorphic slot
// whose construction relocates the moved object itself, or the value's own
// storage as the receiver of one consuming call, as the replacement an
// assignment relocates into its destination, or as the task a managed-thread
// spawn relocates into its runtime task box.
enum class FreshHierarchyPublication {
  Local,
  Argument,
  Returned,
  Slot,
  Receiver,
  Replacement,
  Task
};

struct FreshHierarchyTransfer {
  MirOperand source;
  MirPlaceId destination = 0;
  const MirInstruction *publication = nullptr;
  FreshHierarchyPublication publishes = FreshHierarchyPublication::Local;
  ConstructorKind kind = ConstructorKind::Copy;
  HirClassInstanceId owner = 0;
  MirDropObligationId sourceDrop = 0;
  MirDropObligationId valueDrop = 0;
};

[[nodiscard]] FreshHierarchyTransfer
planFreshHierarchyTransfer(const MirProgram &program, const MirBody &body,
                           const MirInstruction &producer);

// The generated copy of a concrete hierarchy class whose typed field copy is
// implemented: borrow-free ordinary storage with generated copy members and
// no active cleanup, recursively through scalar and record fields.
void preflightGeneratedHierarchyCopy(const MirProgram &program,
                                     const SemanticType &type);

// Fresh values have one physical destination. This plan follows only verified
// value transport; it never elides a MIR Copy/Move or aliases a named local to
// the caller's return storage.
class Placement final {
public:
  struct Target {
    MirPlaceId place = 0;
    MirValueId aggregate = 0;
    std::size_t element = 0;
    std::optional<std::size_t> variant;
    bool returned = false;
    friend bool operator==(const Target &, const Target &) = default;
  };
  explicit Placement(const MirBody &body);
  // Explicit opt-in for typed hierarchy transfer emission. Ordinary Load/Move
  // snapshots retain baseline placement; only exact successful MIR plans add
  // hierarchy final-local destinations. Neither constructor grants admission.
  Placement(const MirProgram &program, const MirBody &body);
  [[nodiscard]] const Target *target(MirValueId value) const;
  [[nodiscard]] bool fresh(MirValueId value) const;

private:
  std::map<MirValueId, const MirInstruction *> producers;
  std::map<MirValueId, Target> targets;
  std::set<MirValueId> hierarchyTransfers;
  void collect(const MirProgram *program, const MirBody &body);
  void bind(MirValueId value, Target target);
};

// A class object produced directly in its own storage: an ordinary
// construction, or a by-value GTI call whose callee publishes the object in
// the ABI result destination. Copies, moves and loaded values never qualify.
[[nodiscard]] bool isFreshObjectProducer(const MirProgram &program,
                                         const MirInstruction &producer);

// A fixed array of hierarchy storage aggregated from its elements. Each
// element is a fresh object constructed directly in its slot of the array's
// own final storage; the aggregate itself moves no object bytes.
[[nodiscard]] bool isFreshHierarchyAggregate(const MirProgram &program,
                                             const MirInstruction &producer);

// Ordinary construction publishes directly into fresh final storage. This is
// not copy/move permission. Both admission and emission use the same verified
// producer/publication identity for locals and constructor field destinations.
// A fresh by-value call result, or a fresh hierarchy aggregate, publishes the
// same way.
[[nodiscard]] std::optional<MirPlaceId>
freshConstructionDestination(const MirProgram &program, MirBodyAddress address,
                             const MirBody &body, const Placement &placement,
                             const MirInstruction &producer);

// Every final storage a fresh producer may publish into without a transfer:
// a whole fresh local or constructor field, the body's own ABI return
// destination, the producer's own full-expression temporary, which is only
// borrowed through its value-rooted places and dropped there, or the exact
// prepared storage of a by-value argument whose ownership the callee takes,
// or its slot in a fresh hierarchy aggregate that publishes in final storage.
enum class FreshObjectStorage { Local, Returned, Temporary, Argument, Element };
[[nodiscard]] std::optional<FreshObjectStorage>
freshObjectStorage(const MirProgram &program, MirBodyAddress address,
                   const MirBody &body, const Placement &placement,
                   const MirInstruction &producer);

} // namespace lang::native_llvm
