#include "cpp_mir_body_emitter.h"
#include <algorithm>
#include <iomanip>
#include <iterator>
#include <limits>
#include <numeric>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>

namespace lang {
namespace {

// A fixed-array element place carries one or more Index projections over its
// unprojected sibling array place (same binding). Nonterminal projections are
// admitted only when their constant indices are proven in bounds: the MIR
// instruction owns one terminal failure edge, so the text step must not hide
// another potentially failing access in the parent expression.
struct ArrayElementAccess {
  MirPlaceId array = 0;
  std::vector<MirPlaceProjection> indices;

  [[nodiscard]] const MirPlaceProjection &terminalIndex() const {
    return indices.back();
  }
};

[[nodiscard]] inline std::optional<ArrayElementAccess>
arrayElementAccess(const MirBody &body, const MirPlace &place) {
  if (place.root != MirPlaceRootKind::Binding || place.binding == 0 ||
      place.projections.empty() ||
      !std::all_of(place.projections.begin(), place.projections.end(),
                   [](const MirPlaceProjection &projection) {
                     return projection.kind == MirProjectionKind::Index &&
                            (projection.index != 0 ||
                             projection.constantIndex.has_value());
                   })) {
    return std::nullopt;
  }
  for (const MirPlace &candidate : body.places) {
    if (candidate.id != place.id &&
        candidate.root == MirPlaceRootKind::Binding &&
        candidate.binding == place.binding && candidate.projections.empty() &&
        candidate.type.kind == SemanticType::Array) {
      SemanticType current = candidate.type;
      bool valid = true;
      for (std::size_t index = 0; index < place.projections.size(); ++index) {
        if (current.kind != SemanticType::Array ||
            current.arguments.size() != 1) {
          valid = false;
          break;
        }
        if (index + 1 < place.projections.size()) {
          const std::optional<std::uint64_t> constant =
              place.projections[index].constantIndex;
          if (!constant || current.arrayLengthParameterId != 0 ||
              *constant >= current.arrayLength) {
            valid = false;
            break;
          }
        }
        // Copy before assignment: the element lives inside the vector the
        // assignment replaces.
        SemanticType element = current.arguments.front();
        current = std::move(element);
      }
      if (valid && current == place.type) {
        return ArrayElementAccess{candidate.id, place.projections};
      }
    }
  }
  return std::nullopt;
}

// A fixed-array field element on a binding-rooted record. The sibling field
// place owns the complete field projection and concrete array type; the
// element adds one or more Index projections. Nonterminal indices must be
// compile-time in bounds because one MIR instruction carries only the
// terminal access's failure edge.
struct BindingArrayFieldElementAccess {
  MirPlaceId array = 0;
  std::vector<MirPlaceProjection> indices;

  [[nodiscard]] const MirPlaceProjection &terminalIndex() const {
    return indices.back();
  }
};

[[nodiscard]] std::optional<BindingArrayFieldElementAccess>
bindingArrayFieldElementAccess(const MirBody &body, const MirPlace &place) {
  if (place.root != MirPlaceRootKind::Binding || place.binding == 0 ||
      place.projections.size() < 2) {
    return std::nullopt;
  }
  const auto firstIndex =
      std::find_if(place.projections.begin(), place.projections.end(),
                   [](const MirPlaceProjection &projection) {
                     return projection.kind == MirProjectionKind::Index;
                   });
  if (firstIndex == place.projections.begin() ||
      firstIndex == place.projections.end() ||
      !std::all_of(place.projections.begin(), firstIndex,
                   [](const MirPlaceProjection &projection) {
                     return projection.kind == MirProjectionKind::Field &&
                            projection.field != 0;
                   }) ||
      !std::all_of(firstIndex, place.projections.end(),
                   [](const MirPlaceProjection &projection) {
                     return projection.kind == MirProjectionKind::Index &&
                            (projection.index != 0 ||
                             projection.constantIndex.has_value());
                   })) {
    return std::nullopt;
  }

  const std::vector<MirPlaceProjection> fields(place.projections.begin(),
                                               firstIndex);
  const std::vector<MirPlaceProjection> indices(firstIndex,
                                                place.projections.end());
  for (const MirPlace &candidate : body.places) {
    if (candidate.id == place.id ||
        candidate.root != MirPlaceRootKind::Binding ||
        candidate.binding != place.binding || candidate.projections != fields ||
        candidate.type.kind != SemanticType::Array) {
      continue;
    }
    SemanticType current = candidate.type;
    bool valid = true;
    for (std::size_t index = 0; index < indices.size(); ++index) {
      if (current.kind != SemanticType::Array ||
          current.arguments.size() != 1) {
        valid = false;
        break;
      }
      if (index + 1 < indices.size()) {
        if (!indices[index].constantIndex ||
            current.arrayLengthParameterId != 0 ||
            *indices[index].constantIndex >= current.arrayLength) {
          valid = false;
          break;
        }
      }
      SemanticType element = current.arguments.front();
      current = std::move(element);
    }
    if (valid && current == place.type) {
      return BindingArrayFieldElementAccess{candidate.id, indices};
    }
  }
  return std::nullopt;
}

struct ConstantArrayElementFieldAccess {
  ArrayElementAccess element;
  SemanticType elementType = SemanticType::Unknown;
  std::vector<MirPlaceProjection> fields;
};

// A field projected from a compile-time in-bounds fixed-array element needs no
// second storage row and no runtime bounds edge. Dynamic or unchecked indices
// stay outside this shape so a field projection cannot hide a failing access.
[[nodiscard]] std::optional<ConstantArrayElementFieldAccess>
constantArrayElementFieldAccess(const MirBody &body, const MirPlace &place) {
  if (place.root != MirPlaceRootKind::Binding || place.binding == 0 ||
      place.projections.size() < 2) {
    return std::nullopt;
  }
  const auto firstField =
      std::find_if(place.projections.begin(), place.projections.end(),
                   [](const MirPlaceProjection &projection) {
                     return projection.kind != MirProjectionKind::Index;
                   });
  if (firstField == place.projections.begin() ||
      firstField == place.projections.end() ||
      std::any_of(firstField, place.projections.end(),
                  [](const MirPlaceProjection &projection) {
                    return projection.kind != MirProjectionKind::Field ||
                           projection.field == 0;
                  })) {
    return std::nullopt;
  }

  for (const MirPlace &candidate : body.places) {
    if (candidate.id == place.id ||
        candidate.root != MirPlaceRootKind::Binding ||
        candidate.binding != place.binding || !candidate.projections.empty() ||
        candidate.type.kind != SemanticType::Array) {
      continue;
    }
    SemanticType current = candidate.type;
    std::vector<MirPlaceProjection> indices;
    bool valid = true;
    for (auto projection = place.projections.begin(); projection != firstField;
         ++projection) {
      if (current.kind != SemanticType::Array ||
          current.arguments.size() != 1 ||
          current.arrayLengthParameterId != 0 || !projection->constantIndex ||
          *projection->constantIndex >= current.arrayLength) {
        valid = false;
        break;
      }
      indices.push_back(*projection);
      SemanticType element = current.arguments.front();
      current = std::move(element);
    }
    if (!valid || current.kind != SemanticType::Class) {
      continue;
    }
    return ConstantArrayElementFieldAccess{
        .element = ArrayElementAccess{.array = candidate.id,
                                      .indices = std::move(indices)},
        .elementType = current,
        .fields = std::vector<MirPlaceProjection>(firstField,
                                                  place.projections.end())};
  }
  return std::nullopt;
}

// A string-view element place reads through the terminal bounds-checked
// helper, exactly like the compatibility subscript: string_view_at
// contains a violated bound itself, so both text forms spell the plain
// call. The access reuses the array-element shape with the sibling
// string-view local as the base.
[[nodiscard]] std::optional<ArrayElementAccess>
viewElementAccess(const MirBody &body, const MirPlace &place) {
  if (place.root != MirPlaceRootKind::Binding || place.binding == 0 ||
      place.projections.size() != 1 ||
      place.projections[0].kind != MirProjectionKind::Index) {
    return std::nullopt;
  }
  for (const MirPlace &candidate : body.places) {
    if (candidate.id != place.id &&
        candidate.root == MirPlaceRootKind::Binding &&
        candidate.binding == place.binding && candidate.projections.empty() &&
        candidate.type.kind == SemanticType::StringView) {
      return ArrayElementAccess{candidate.id, {place.projections[0]}};
    }
  }
  return std::nullopt;
}

[[nodiscard]] bool rawPointerOffsetType(const SemanticType &type) {
  switch (type.kind) {
  case SemanticType::Int8:
  case SemanticType::Int16:
  case SemanticType::Int32:
  case SemanticType::Int64:
  case SemanticType::UInt8:
  case SemanticType::UInt16:
  case SemanticType::UInt32:
  case SemanticType::UInt64:
    return true;
  default:
    return false;
  }
}

struct RawMemoryPlaceAccess {
  MirValueId pointer = 0;
  MirValueId index = 0;
  MirProjectionKind projection = MirProjectionKind::RawDereference;
  SemanticType pointerType = SemanticType::Unknown;
  SemanticType pointeeType = SemanticType::Unknown;
};

// Raw memory places are deliberately narrow MIR lvalue views: one verified
// raw-pointer SSA root followed by one unchecked index or dereference and an
// optional field-only projection chain. The frontend owns unsafe permission,
// MIR verification owns the root/type relation, and the body probe validates
// every trailing field against the sealed representation rows.
[[nodiscard]] std::optional<RawMemoryPlaceAccess>
rawMemoryPlaceAccess(const MirBody &body, const MirPlace &place) {
  if (place.root != MirPlaceRootKind::Value || place.value == 0 ||
      place.projections.empty() ||
      !std::all_of(std::next(place.projections.begin()),
                   place.projections.end(),
                   [](const MirPlaceProjection &projection) {
                     return projection.kind == MirProjectionKind::Field;
                   })) {
    return std::nullopt;
  }
  const MirPlaceProjection &projection = place.projections.front();
  if (projection.kind != MirProjectionKind::RawIndex &&
      projection.kind != MirProjectionKind::RawDereference) {
    return std::nullopt;
  }
  const MirValue *pointer = body.findValue(place.value);
  if (pointer == nullptr ||
      pointer->info.type.kind != SemanticType::RawPointer ||
      pointer->info.type.arguments.size() != 1) {
    return std::nullopt;
  }
  if (projection.kind == MirProjectionKind::RawDereference) {
    if (projection.index != 0 || projection.constantIndex ||
        projection.field != 0 || projection.selection != 0) {
      return std::nullopt;
    }
    return RawMemoryPlaceAccess{.pointer = place.value,
                                .projection = projection.kind,
                                .pointerType = pointer->info.type,
                                .pointeeType =
                                    pointer->info.type.arguments.front()};
  }
  const MirValue *index = body.findValue(projection.index);
  if (projection.index == 0 || projection.constantIndex ||
      projection.field != 0 || projection.selection != 0 || index == nullptr ||
      !rawPointerOffsetType(index->info.type)) {
    return std::nullopt;
  }
  return RawMemoryPlaceAccess{.pointer = place.value,
                              .index = projection.index,
                              .projection = projection.kind,
                              .pointerType = pointer->info.type,
                              .pointeeType =
                                  pointer->info.type.arguments.front()};
}

[[nodiscard]] std::string
rawMemoryPlaceSpelling(const RawMemoryPlaceAccess &access) {
  const std::string pointer = "__gti_mir_v_" + std::to_string(access.pointer);
  if (access.projection == MirProjectionKind::RawDereference) {
    return "(*" + pointer + ")";
  }
  return pointer + "[__gti_mir_v_" + std::to_string(access.index) + "]";
}

// A storage symbol is self-identifying when the sealed snapshot contains one
// row. Reused symbols from concrete generic class instances require the
// current body owner; an unrelated body with several candidates remains
// ambiguous until MIR carries the concrete storage owner itself.
[[nodiscard]] const CppMirSymbolRepresentation *
storageRepresentation(const CppMirBodyEmissionMap &representations,
                      std::optional<HirClassInstanceId> owner,
                      SymbolId symbol) {
  const CppMirSymbolRepresentation *only = nullptr;
  const CppMirSymbolRepresentation *owned = nullptr;
  std::size_t matches = 0;
  std::size_t ownedMatches = 0;
  for (const CppMirSymbolRepresentation &row : representations.symbols()) {
    if (row.kind != CppMirSymbolRepresentationKind::Storage ||
        row.symbol != symbol || row.ordinal != 0) {
      continue;
    }
    only = &row;
    ++matches;
    if (owner && row.owner == *owner) {
      owned = &row;
      ++ownedMatches;
    }
  }
  if (matches == 1) {
    return only;
  }
  return ownedMatches == 1 ? owned : nullptr;
}

[[nodiscard]] const CppMirSymbolRepresentation *storageRepresentationForBody(
    const MirProgram &program, const CppMirBodyEmissionMap &representations,
    std::optional<HirClassInstanceId> owner, SymbolId symbol) {
  if (const MirProgramInitializationStep *step =
          program.programInitializationPlan().findStepForSymbol(symbol)) {
    owner = step->ownerClass;
  }
  return storageRepresentation(representations, owner, symbol);
}

struct ReceiverArrayElementAccess {
  SymbolId field = 0;
  MirPlaceProjection index;
};

// A fixed-array field element on the current receiver. The sibling field
// place proves the field's concrete array type; this exact one-index shape can
// use the same checked read/write operation as a binding-rooted array.
[[nodiscard]] std::optional<ReceiverArrayElementAccess>
receiverArrayElementAccess(const MirBody &body, const MirPlace &place) {
  if (place.root != MirPlaceRootKind::This || place.projections.size() != 2 ||
      place.projections[0].kind != MirProjectionKind::Field ||
      place.projections[0].field == 0 ||
      place.projections[1].kind != MirProjectionKind::Index ||
      (place.projections[1].index == 0 &&
       !place.projections[1].constantIndex)) {
    return std::nullopt;
  }
  for (const MirPlace &candidate : body.places) {
    if (candidate.id == place.id || candidate.root != MirPlaceRootKind::This ||
        candidate.projections.size() != 1 ||
        candidate.projections[0].kind != MirProjectionKind::Field ||
        candidate.projections[0].field != place.projections[0].field ||
        candidate.type.kind != SemanticType::Array ||
        candidate.type.arguments.size() != 1) {
      continue;
    }
    if (candidate.type.arguments.front() == place.type) {
      return ReceiverArrayElementAccess{place.projections[0].field,
                                        place.projections[1]};
    }
  }
  return std::nullopt;
}

struct ClassSubscriptAccess {
  MirPlaceId base = 0;
  HirClassInstanceId owner = 0;
  MirValueId index = 0;
  std::optional<std::uint64_t> constantIndex;
  SemanticType indexType = SemanticType::Unknown;
};

// A class-subscription place: an Index projection over a sibling
// class-typed local. The compatibility path spells it as the class's
// subscript member call, so the access carries the member-resolution
// facts: the base place, its class instance, and the index identity.
[[nodiscard]] std::optional<ClassSubscriptAccess>
classSubscriptAccess(const MirProgram &program, const MirBody &body,
                     const MirPlace &place) {
  if (place.root != MirPlaceRootKind::Binding || place.binding == 0 ||
      place.projections.size() != 1 ||
      place.projections[0].kind != MirProjectionKind::Index) {
    return std::nullopt;
  }
  for (const MirPlace &candidate : body.places) {
    if (candidate.id == place.id ||
        candidate.root != MirPlaceRootKind::Binding ||
        candidate.binding != place.binding || !candidate.projections.empty() ||
        candidate.type.kind != SemanticType::Class) {
      continue;
    }
    for (const MirClassInstance &instance : program.classInstances()) {
      if (instance.type != candidate.type) {
        continue;
      }
      const MirValue *index = body.findValue(place.projections[0].index);
      if (index == nullptr) {
        return std::nullopt;
      }
      return ClassSubscriptAccess{
          candidate.id, instance.id, place.projections[0].index,
          place.projections[0].constantIndex, index->info.type};
    }
    return std::nullopt;
  }
  return std::nullopt;
}

// Resolves the unique subscript member for one access direction and
// proves it: a source-defined GTI member of the base class instance with
// the exact receiver mutability and index parameter type, carrying a
// body-name row for its spelling, whose own emitted body contains
// failure terminally — cycles fail closed exactly like the plain-callee
// convention.
[[nodiscard]] const MirFunctionInstance *containedSubscriptMember(
    const MirProgram &program, const CppMirBodyEmissionMap &representations,
    HirClassInstanceId owner, ReceiverMutability mutability,
    const SemanticType &indexType) {
  const MirFunctionInstance *found = nullptr;
  for (const MirFunctionInstance &candidate : program.functionInstances()) {
    if (!candidate.overloadedOperator ||
        *candidate.overloadedOperator != OverloadedOperator::Subscript ||
        !candidate.owner || *candidate.owner != owner ||
        candidate.receiverMutability != mutability ||
        candidate.parameterTypes.size() != 1 ||
        candidate.parameterTypes.front() != indexType ||
        candidate.linkage != LanguageLinkage::Gti ||
        candidate.definitionKind !=
            MirFunctionInstance::DefinitionKind::Source) {
      continue;
    }
    if (found != nullptr) {
      return nullptr;
    }
    found = &candidate;
  }
  if (found == nullptr) {
    return nullptr;
  }
  const MirBodyAddress address{.kind = MirBodyKind::Function,
                               .owner = found->id};
  const auto row = std::find_if(
      representations.bodies().begin(), representations.bodies().end(),
      [&](const CppMirBodyNameRepresentation &candidate) {
        return candidate.address == address;
      });
  if (row == representations.bodies().end() || row->spelling.empty()) {
    return nullptr;
  }
  thread_local std::vector<HirFunctionInstanceId> probing;
  if (std::find(probing.begin(), probing.end(), found->id) != probing.end()) {
    return nullptr;
  }
  probing.push_back(found->id);
  // The success form contains terminally inside its own text; the failure
  // form's boundary wrapper keeps the original member name and signature
  // and terminates on a propagated record, so the plain call is exact
  // against either emitted form.
  const CppMirBodyEmitter emitter(program, representations);
  const bool contained = emitter.supportsBodyText(address) ||
                         emitter.supportsFailureBodyText(address);
  probing.pop_back();
  return contained ? found : nullptr;
}

template <typename Enum>
[[nodiscard]] constexpr std::size_t ordinal(Enum value) {
  return static_cast<std::size_t>(value);
}

[[nodiscard]] CppMirBodyEmissionReadiness
mergeReadiness(CppMirBodyEmissionReadiness left,
               CppMirBodyEmissionReadiness right) {
  return ordinal(left) >= ordinal(right) ? left : right;
}

[[nodiscard]] CppMirBodyEmissionReadiness
readinessForIssue(CppMirBodyEmissionIssueKind kind) {
  switch (kind) {
  case CppMirBodyEmissionIssueKind::MissingTypeRepresentation:
  case CppMirBodyEmissionIssueKind::MissingBodyRepresentation:
  case CppMirBodyEmissionIssueKind::MissingSymbolRepresentation:
  case CppMirBodyEmissionIssueKind::MissingEnumRepresentation:
  case CppMirBodyEmissionIssueKind::MissingCapabilityRepresentation:
  case CppMirBodyEmissionIssueKind::UnsupportedTextVocabulary:
    return CppMirBodyEmissionReadiness::MissingRepresentation;
  case CppMirBodyEmissionIssueKind::MissingOrderedCompoundMir:
  case CppMirBodyEmissionIssueKind::MissingCheckedFailureControlFlow:
  case CppMirBodyEmissionIssueKind::MissingAggregateRollbackMir:
  case CppMirBodyEmissionIssueKind::MissingCallInputScheduleMir:
  case CppMirBodyEmissionIssueKind::MissingConstructionScheduleMir:
  case CppMirBodyEmissionIssueKind::MissingPartialConstructionRollbackMir:
  case CppMirBodyEmissionIssueKind::MissingFailureCleanupMir:
  case CppMirBodyEmissionIssueKind::MissingProgramInitializationMir:
  case CppMirBodyEmissionIssueKind::MissingHostedStartupMir:
    return CppMirBodyEmissionReadiness::MissingMirAuthority;
  case CppMirBodyEmissionIssueKind::InvalidMirProgram:
  case CppMirBodyEmissionIssueKind::InvalidBodyAddress:
  case CppMirBodyEmissionIssueKind::InvalidRepresentationEnum:
  case CppMirBodyEmissionIssueKind::InvalidRepresentationRow:
  case CppMirBodyEmissionIssueKind::DuplicateTypeRepresentation:
  case CppMirBodyEmissionIssueKind::DuplicateBodyRepresentation:
  case CppMirBodyEmissionIssueKind::DuplicateSymbolRepresentation:
  case CppMirBodyEmissionIssueKind::DuplicateEnumRepresentation:
  case CppMirBodyEmissionIssueKind::DuplicateCapabilityRepresentation:
  case CppMirBodyEmissionIssueKind::InvalidBodyKind:
  case CppMirBodyEmissionIssueKind::InvalidInstructionKind:
  case CppMirBodyEmissionIssueKind::InvalidOperation:
  case CppMirBodyEmissionIssueKind::InvalidOperandKind:
  case CppMirBodyEmissionIssueKind::InvalidPlaceRootKind:
  case CppMirBodyEmissionIssueKind::InvalidProjectionKind:
  case CppMirBodyEmissionIssueKind::InvalidTerminatorKind:
  case CppMirBodyEmissionIssueKind::Count:
    return CppMirBodyEmissionReadiness::Incoherent;
  }
  return CppMirBodyEmissionReadiness::Incoherent;
}

[[nodiscard]] std::optional<CppMirTypeRepresentationKind>
expectedTypeRepresentation(const SemanticType &type) {
  switch (type.kind) {
  case SemanticType::Void:
    return CppMirTypeRepresentationKind::Void;
  case SemanticType::Int8:
  case SemanticType::Int16:
  case SemanticType::Int32:
  case SemanticType::Int64:
  case SemanticType::UInt8:
  case SemanticType::UInt16:
  case SemanticType::UInt32:
  case SemanticType::UInt64:
  case SemanticType::Float:
  case SemanticType::Double:
  case SemanticType::Bool:
  case SemanticType::Char:
    return CppMirTypeRepresentationKind::Scalar;
  case SemanticType::StringView:
    return CppMirTypeRepresentationKind::StringView;
  case SemanticType::CString:
    return CppMirTypeRepresentationKind::RawPointer;
  case SemanticType::NullPtr:
    return CppMirTypeRepresentationKind::NullPointer;
  case SemanticType::RawPointer:
    return CppMirTypeRepresentationKind::RawPointer;
  case SemanticType::Array:
    return CppMirTypeRepresentationKind::FixedArray;
  case SemanticType::Class:
    return CppMirTypeRepresentationKind::Class;
  case SemanticType::Enum:
    return CppMirTypeRepresentationKind::Enum;
  case SemanticType::Reference:
    return CppMirTypeRepresentationKind::Reference;
  case SemanticType::UniqueOwner:
    return CppMirTypeRepresentationKind::UniqueOwner;
  case SemanticType::SharedPointer:
    return CppMirTypeRepresentationKind::SharedPointer;
  case SemanticType::Storage:
  case SemanticType::PrefixStorage:
    return CppMirTypeRepresentationKind::Storage;
  case SemanticType::TypeParameter:
  case SemanticType::TypePack:
  case SemanticType::TypeName:
    return CppMirTypeRepresentationKind::Meta;
  case SemanticType::Function:
    return CppMirTypeRepresentationKind::Function;
  case SemanticType::Lambda:
    return CppMirTypeRepresentationKind::Lambda;
  case SemanticType::Expected:
    return CppMirTypeRepresentationKind::Expected;
  case SemanticType::Unexpected:
    return CppMirTypeRepresentationKind::Unexpected;
  case SemanticType::Unknown:
    return std::nullopt;
  }
  return std::nullopt;
}

// An enum result crosses the transformed failure boundary by value: the
// receiving local declares value-initialized and the publication assigns.
// A plain enum is scalar-like. A payload enum's variant record keeps that
// boundary default-constructible and move-assignable only when every
// copied variant field is itself scalar or a view, so this proof demands
// exactly that from the copied enum row, plus the enum's own type row.
[[nodiscard]] bool
cppMirEnumBoundaryRow(const CppMirBodyEmissionMap &representations,
                      const SemanticType &type) {
  if (type.kind != SemanticType::Enum) {
    return false;
  }
  const auto typeRow = std::find_if(
      representations.types().begin(), representations.types().end(),
      [&](const CppMirTypeRepresentation &row) { return row.type == type; });
  if (typeRow == representations.types().end() || typeRow->spelling.empty()) {
    return false;
  }
  const auto enumRow = std::find_if(representations.enums().begin(),
                                    representations.enums().end(),
                                    [&](const CppMirEnumRepresentation &row) {
                                      return row.owner == type.enumId;
                                    });
  if (enumRow == representations.enums().end()) {
    return false;
  }
  for (const CppMirPayloadVariantRepresentation &variant :
       enumRow->payloadVariants) {
    for (const SemanticType &field : variant.fieldTypes) {
      const std::optional<CppMirTypeRepresentationKind> kind =
          expectedTypeRepresentation(field);
      if (!kind || (*kind != CppMirTypeRepresentationKind::Scalar &&
                    *kind != CppMirTypeRepresentationKind::StringView)) {
        return false;
      }
    }
  }
  return true;
}

[[nodiscard]] bool isInitializerBody(MirBodyKind kind) {
  return kind == MirBodyKind::Module ||
         kind == MirBodyKind::FieldInitializers ||
         kind == MirBodyKind::StaticFieldInitializers;
}

[[nodiscard]] bool isCanonicalNoExecutionInitializer(const MirBody &body) {
  if (!isInitializerBody(body.kind) || body.returnType != SemanticType::Void ||
      body.entry != 1 || body.blocks.size() != 1 || !body.places.empty() ||
      !body.loans.empty() || !body.fullExpressions.empty() ||
      !body.cleanupBoundaries.empty() || !body.dropObligations.empty() ||
      !body.failureRecords.empty() || !body.values.empty() ||
      !body.valueUses.empty()) {
    return false;
  }
  MirBlock expected;
  expected.id = 1;
  expected.terminator.kind = MirTerminatorKind::Exit;
  expected.reachable = true;
  return body.blocks.front() == expected;
}

[[nodiscard]] bool
hasExecutableProgramInitialization(const MirProgram &program) {
  return std::any_of(
      program.programInitializationPlan().steps.begin(),
      program.programInitializationPlan().steps.end(), [](const auto &step) {
        return step.role == ProgramInitializationStepRole::Initializer;
      });
}

[[nodiscard]] bool moduleMayRaiseDefinedFailure(const MirProgram &program) {
  return std::any_of(program.module().blocks.begin(),
                     program.module().blocks.end(), [](const MirBlock &block) {
                       return std::any_of(
                           block.instructions.begin(), block.instructions.end(),
                           [](const MirInstruction &instruction) {
                             return !instruction.definedFailure.empty();
                           });
                     });
}

[[nodiscard]] const MirProgramInitializationStep *
moduleDataOnlyInitialization(const MirProgram &program,
                             const MirInstruction &instruction) {
  const auto found = std::find_if(
      program.programInitializationPlan().steps.begin(),
      program.programInitializationPlan().steps.end(),
      [&](const MirProgramInitializationStep &step) {
        return step.role == ProgramInitializationStepRole::DataOnly &&
               step.storageInitialization == instruction.id;
      });
  if (found == program.programInitializationPlan().steps.end() ||
      instruction.kind != MirInstructionKind::Initialize ||
      !instruction.destination ||
      *instruction.destination != found->storagePlace ||
      !instruction.operands.empty() || instruction.result ||
      instruction.receiver || !instruction.localFailureSites.empty() ||
      !instruction.definedFailure.empty() || !instruction.lifecycle.empty()) {
    return nullptr;
  }
  const MirPlace *storage = program.module().findPlace(found->storagePlace);
  return storage != nullptr && storage->root == MirPlaceRootKind::Binding &&
                 storage->binding == found->binding &&
                 storage->symbol == found->symbol &&
                 storage->projections.empty() &&
                 storage->type == instruction.info.type
             ? &*found
             : nullptr;
}

[[nodiscard]] const MirInstruction *findInstruction(const MirBody &body,
                                                    MirInstructionId id) {
  if (id == 0) {
    return nullptr;
  }
  for (const MirBlock &block : body.blocks) {
    const auto found =
        std::find_if(block.instructions.begin(), block.instructions.end(),
                     [id](const MirInstruction &instruction) {
                       return instruction.id == id;
                     });
    if (found != block.instructions.end()) {
      return &*found;
    }
  }
  return nullptr;
}

[[nodiscard]] std::optional<std::uint64_t>
signedMinimumMagnitude(const SemanticType &type) {
  switch (type.kind) {
  case SemanticType::Int8:
    return std::uint64_t{1} << 7U;
  case SemanticType::Int16:
    return std::uint64_t{1} << 15U;
  case SemanticType::Int32:
    return std::uint64_t{1} << 31U;
  case SemanticType::Int64:
    return std::uint64_t{1} << 63U;
  default:
    return std::nullopt;
  }
}

[[nodiscard]] bool integerLiteralCarrierCanRepresent(const SemanticType &type,
                                                     std::uint64_t magnitude) {
  switch (type.kind) {
  case SemanticType::Int8:
    return magnitude <= static_cast<std::uint64_t>(INT8_MAX);
  case SemanticType::Int16:
    return magnitude <= static_cast<std::uint64_t>(INT16_MAX);
  case SemanticType::Int32:
    return magnitude <= static_cast<std::uint64_t>(INT32_MAX);
  case SemanticType::Int64:
    return magnitude <= static_cast<std::uint64_t>(INT64_MAX);
  case SemanticType::UInt8:
    return magnitude <= UINT8_MAX;
  case SemanticType::UInt16:
    return magnitude <= UINT16_MAX;
  case SemanticType::UInt32:
    return magnitude <= UINT32_MAX;
  case SemanticType::UInt64:
    return true;
  default:
    return false;
  }
}

// GTI parses the signed minimum as a positive magnitude followed by unary
// negation. The magnitude is intentionally one past the positive range, so it
// has no independently spellable value of the result type. Fuse the exact
// one-use source chain. Parentheses can retain an explicit MIR Identity
// between the magnitude and negation; those identities must be exact,
// failure-free, same-type, and single-use. Program-constant substitution may
// retain the result type on the magnitude, while a contextually typed direct
// literal may use a wider integer carrier that can represent the positive
// magnitude.
[[nodiscard]] const MirInstruction *
fusedSignedMinimumLiteral(const MirBody &body, const MirInstruction &negation) {
  if (negation.kind != MirInstructionKind::Compute ||
      negation.operation != MirOperation::Negate || !negation.result ||
      negation.operands.size() != 1 ||
      negation.operands.front().kind != MirOperandKind::Value ||
      !negation.localFailureSites.empty() || !negation.definedFailure.empty()) {
    return nullptr;
  }
  const MirInstruction *consumer = &negation;
  const MirValue *operand = body.findValue(negation.operands.front().value);
  const MirInstruction *literal = nullptr;
  while (operand != nullptr) {
    const MirInstruction *producer = findInstruction(body, operand->definition);
    if (producer == nullptr || !producer->result ||
        *producer->result != operand->id) {
      return nullptr;
    }
    const std::vector<MirValueUse> &uses = body.usesOf(operand->id);
    if (uses.size() != 1 ||
        uses.front().kind != MirValueUseKind::InstructionOperand ||
        uses.front().instruction != consumer->id ||
        uses.front().operandIndex != 0) {
      return nullptr;
    }
    if (producer->operation != MirOperation::Identity) {
      literal = producer;
      break;
    }
    if (producer->kind != MirInstructionKind::Compute ||
        producer->operands.size() != 1 ||
        producer->operands.front().kind != MirOperandKind::Value ||
        producer->info.type != producer->operands.front().type ||
        !producer->localFailureSites.empty() ||
        !producer->definedFailure.empty()) {
      return nullptr;
    }
    consumer = producer;
    operand = body.findValue(producer->operands.front().value);
  }
  const std::optional<std::uint64_t> minimum =
      signedMinimumMagnitude(negation.info.type);
  const auto *magnitude = literal != nullptr && literal->literal
                              ? std::get_if<std::uint64_t>(&*literal->literal)
                              : nullptr;
  if (literal == nullptr || literal->kind != MirInstructionKind::Compute ||
      literal->operation != MirOperation::Literal || !literal->result ||
      operand == nullptr || *literal->result != operand->id ||
      literal->info.type != negation.operands.front().type ||
      !literal->localFailureSites.empty() || !literal->definedFailure.empty() ||
      !minimum || magnitude == nullptr || *magnitude != *minimum ||
      (literal->info.type != negation.info.type &&
       !integerLiteralCarrierCanRepresent(literal->info.type, *magnitude))) {
    return nullptr;
  }
  return literal;
}

[[nodiscard]] const MirInstruction *
fusedSignedMinimumNegation(const MirBody &body, const MirInstruction &carrier) {
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &candidate : block.instructions) {
      const MirInstruction *literal =
          fusedSignedMinimumLiteral(body, candidate);
      if (literal == nullptr) {
        continue;
      }
      const MirValue *operand =
          body.findValue(candidate.operands.front().value);
      while (operand != nullptr) {
        const MirInstruction *producer =
            findInstruction(body, operand->definition);
        if (producer == nullptr) {
          break;
        }
        if (producer == &carrier) {
          return &candidate;
        }
        if (producer == literal ||
            producer->operation != MirOperation::Identity ||
            producer->operands.size() != 1 ||
            producer->operands.front().kind != MirOperandKind::Value) {
          break;
        }
        operand = body.findValue(producer->operands.front().value);
      }
    }
  }
  return nullptr;
}

// Inline closure chains: a C++ closure type is unnameable, so no
// lambda-typed place or value ever declares a local. A Closure compute
// either feeds an invocation receiver directly or initializes a dedicated
// lambda-typed local whose loads feed further initializations or
// invocation receivers, and every consumer spells the full literal inline
// at its own use. Resolution walks the chain backwards; validation walks
// it forwards and freezes the captured places so a literal spelled at a
// later invocation still captures exactly the values the Closure saw.
[[nodiscard]] bool callableValueInvocation(const MirInstruction &instruction) {
  return instruction.kind == MirInstructionKind::Call &&
         instruction.intrinsic == IntrinsicKind::None &&
         !instruction.functionTarget && instruction.receiver &&
         instruction.receiver->kind == MirOperandKind::Value &&
         instruction.receiver->type.kind == SemanticType::Lambda;
}

// A callable-parameter invocation stages its receiver place through one
// Load (or Move) whose result feeds exactly the invocation: the call
// spells the place expression (or its std::move) directly, matching the
// compatibility `operation(args)` form with no intermediate copy. Only a
// deduced-callable template emission carries a type row for the place's
// concrete callable type, so this shape stays dormant under production
// rows.
[[nodiscard]] const MirInstruction *callableReceiverStage(const MirBody &body,
                                                          MirValueId id) {
  const MirValue *value = body.findValue(id);
  const MirInstruction *definition =
      value == nullptr ? nullptr : findInstruction(body, value->definition);
  if (definition == nullptr ||
      (definition->kind != MirInstructionKind::Load &&
       definition->kind != MirInstructionKind::Move) ||
      definition->operands.size() != 1 ||
      definition->operands.front().place == 0 || body.usesOf(id).size() != 1) {
    return nullptr;
  }
  const MirPlace *place = body.findPlace(definition->operands.front().place);
  // Only an initially-available place (a parameter) stages: a local
  // carrier written by an Initialize belongs to the fused closure chain
  // and never declares, so spelling it here would name a nonexistent
  // local.
  if (place == nullptr || place->type.kind != SemanticType::Lambda ||
      place->root != MirPlaceRootKind::Binding || !place->projections.empty() ||
      !place->initiallyAvailable) {
    return nullptr;
  }
  return definition;
}

// The bounds-checked element borrow publishing a Return loan: a Borrow
// of a This-rooted Field+Index place with exactly one failure site whose
// loan is the return loan. The compatibility spelling is the terminal
// array_at accessor, which contains the bounds failure inside itself.
[[nodiscard]] const MirInstruction *
elementBorrowLoanProducer(const MirBody &body, const MirLoan &loan) {
  if (loan.kind != MirLoanKind::Return) {
    return nullptr;
  }
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      if (instruction.kind != MirInstructionKind::Borrow || !instruction.loan ||
          *instruction.loan != loan.id ||
          instruction.localFailureSites.size() != 1 ||
          instruction.operands.size() != 1 ||
          (instruction.operands.front().kind != MirOperandKind::BorrowRead &&
           instruction.operands.front().kind != MirOperandKind::BorrowWrite)) {
        continue;
      }
      const MirPlace *place =
          body.findPlace(instruction.operands.front().place);
      if (place == nullptr || place->root != MirPlaceRootKind::This ||
          place->projections.size() != 2 ||
          place->projections[0].kind != MirProjectionKind::Field ||
          place->projections[1].kind != MirProjectionKind::Index ||
          place->projections[1].index == 0) {
        continue;
      }
      return &instruction;
    }
  }
  return nullptr;
}

[[nodiscard]] bool deducedCallableCallee(const MirProgram &program,
                                         const MirInstruction &instruction) {
  if (instruction.kind != MirInstructionKind::Call ||
      !instruction.functionTarget ||
      instruction.intrinsic != IntrinsicKind::None) {
    return false;
  }
  const MirFunctionInstance *target =
      program.findFunctionInstance(*instruction.functionTarget);
  return target != nullptr && !target->callableParameters.empty() &&
         target->linkage == LanguageLinkage::Gti &&
         target->definitionKind == MirFunctionInstance::DefinitionKind::Source;
}

// A runtime binding and, until GTI has a separate-compilation failure ABI, an
// ordinary bodyless free declaration name plain external symbols. Their
// conservative MIR failure edge cannot be observed through that ABI: a native
// return is success, and native termination never produces a GTI failure
// record. Keep these distinct from source-body terminal containment because a
// plain boundary cannot skip a recoverable caller cleanup edge.
[[nodiscard]] bool
plainExternalBoundaryCallee(const MirProgram &program,
                            const MirInstruction &instruction) {
  if (instruction.kind != MirInstructionKind::Call ||
      !instruction.functionTarget ||
      instruction.intrinsic != IntrinsicKind::None) {
    return false;
  }
  const MirFunctionInstance *target =
      program.findFunctionInstance(*instruction.functionTarget);
  if (target == nullptr || target->owner || !target->mayRaiseDefinedFailure ||
      target->linkage != LanguageLinkage::Gti ||
      (target->definitionKind !=
           MirFunctionInstance::DefinitionKind::Declaration &&
       target->definitionKind !=
           MirFunctionInstance::DefinitionKind::RuntimeBinding) ||
      (!target->externalSymbol.empty() &&
       target->definitionKind !=
           MirFunctionInstance::DefinitionKind::RuntimeBinding)) {
    return false;
  }
  return target->callableParameters.empty() &&
         target->entryKind == ProgramEntryKind::None &&
         !target->virtualMethod && !target->pureVirtual &&
         !target->overrideMethod;
}

// A may-raise free callee whose own body proves the plain success shape:
// its failure is terminally contained inside its own emitted text, so a
// caller in either form calls the plain name and the paired invoke edge
// is a plain goto — the deduced-callable convention generalized to
// concrete free functions. Cycles fail closed: a body currently being
// probed higher in this chain cannot vouch for itself.
[[nodiscard]] bool
terminallyContainedPlainCallee(const MirProgram &program,
                               const CppMirBodyEmissionMap &representations,
                               const MirInstruction &instruction) {
  if (instruction.kind != MirInstructionKind::Call ||
      !instruction.functionTarget ||
      instruction.intrinsic != IntrinsicKind::None) {
    return false;
  }
  const MirFunctionInstance *target =
      program.findFunctionInstance(*instruction.functionTarget);
  if (plainExternalBoundaryCallee(program, instruction)) {
    return true;
  }
  if (target == nullptr || target->owner || !target->mayRaiseDefinedFailure ||
      target->linkage != LanguageLinkage::Gti ||
      !target->callableParameters.empty() ||
      target->entryKind != ProgramEntryKind::None) {
    return false;
  }
  if (target->definitionKind != MirFunctionInstance::DefinitionKind::Source) {
    return false;
  }
  thread_local std::vector<HirFunctionInstanceId> probing;
  if (std::find(probing.begin(), probing.end(), target->id) != probing.end()) {
    return false;
  }
  probing.push_back(target->id);
  // Only the success shape contains terminally inside its own text. A
  // failure-admitted callee keeps the transformed propagation convention
  // and never claims the plain call AS A CALLEE PROPERTY: a caller that
  // handles the callee's failure needs the transformed sibling's error
  // value, and claiming containment here would terminate where the
  // caller propagates (mir_backend_first_family pins that convention).
  // The sound widening, if ever needed, is per call site — a site whose
  // failure continuation is provably unused — not per callee. Two
  // oracle rejections traced to a separate null-loan-capture defect;
  // the third attempt was caught by the suite pin above.
  const bool contained = CppMirBodyEmitter(program, representations)
                             .supportsBodyText({.kind = MirBodyKind::Function,
                                                .owner = target->id});
  probing.pop_back();
  return contained;
}

// The member analogue of the terminally contained plain callee: a
// may-raise member whose own emitted body proves either form contains
// failure away from the caller — the success shape terminally inside its
// text, or the transformed sibling behind the boundary wrapper that
// keeps the original member name and terminates on a propagated record.
// Either way the caller's plain member call never observes failure, so
// its paired invoke edge is a plain goto. Cycles fail closed.
[[nodiscard]] bool
terminallyContainedMemberCallee(const MirProgram &program,
                                const CppMirBodyEmissionMap &representations,
                                const MirInstruction &instruction) {
  if (instruction.kind != MirInstructionKind::Call ||
      !instruction.functionTarget ||
      instruction.intrinsic != IntrinsicKind::None) {
    return false;
  }
  const MirFunctionInstance *target =
      program.findFunctionInstance(*instruction.functionTarget);
  if (target == nullptr || !target->owner || !target->mayRaiseDefinedFailure ||
      target->linkage != LanguageLinkage::Gti ||
      target->definitionKind != MirFunctionInstance::DefinitionKind::Source ||
      !target->callableParameters.empty() ||
      target->entryKind != ProgramEntryKind::None) {
    return false;
  }
  thread_local std::vector<HirFunctionInstanceId> probing;
  if (std::find(probing.begin(), probing.end(), target->id) != probing.end()) {
    return false;
  }
  probing.push_back(target->id);
  // Success-shape containment only: a failure-admitted member keeps the
  // transformed propagation convention.
  const bool contained = CppMirBodyEmitter(program, representations)
                             .supportsBodyText({.kind = MirBodyKind::Function,
                                                .owner = target->id});
  probing.pop_back();
  return contained;
}

[[nodiscard]] const MirInstruction *callableArgumentStage(const MirBody &body,
                                                          MirValueId id) {
  const MirValue *value = body.findValue(id);
  const MirInstruction *definition =
      value == nullptr ? nullptr : findInstruction(body, value->definition);
  if (definition == nullptr || definition->kind != MirInstructionKind::Load ||
      definition->operands.size() != 1 ||
      definition->operands.front().place == 0 || body.usesOf(id).size() != 1 ||
      body.usesOf(id).front().kind != MirValueUseKind::InstructionOperand) {
    return nullptr;
  }
  const MirInstruction *user =
      findInstruction(body, body.usesOf(id).front().instruction);
  if (user == nullptr || user->kind != MirInstructionKind::Call ||
      !user->functionTarget || user->intrinsic != IntrinsicKind::None) {
    return nullptr;
  }
  const MirPlace *place = body.findPlace(definition->operands.front().place);
  if (place == nullptr || place->type.kind != SemanticType::Lambda ||
      place->root != MirPlaceRootKind::Binding || !place->projections.empty() ||
      !place->initiallyAvailable) {
    return nullptr;
  }
  return definition;
}

// A loan-staged call input carries a borrowed entry parameter into a staged
// invocation: the call spells the dereferenced pointer carrier (ADR 018 §4)
// and the staged reference value never materializes as a local.
[[nodiscard]] const MirInstruction *loanStagedCallInput(const MirBody &body,
                                                        MirValueId id) {
  const MirValue *value = body.findValue(id);
  const MirInstruction *definition =
      value == nullptr ? nullptr : findInstruction(body, value->definition);
  if (definition == nullptr ||
      definition->kind != MirInstructionKind::CallInput ||
      definition->receiver || definition->operands.size() != 1 ||
      definition->operands.front().kind != MirOperandKind::Loan ||
      definition->operands.front().loan == 0 ||
      body.findLoan(definition->operands.front().loan) == nullptr ||
      body.usesOf(id).size() != 1 ||
      body.usesOf(id).front().kind != MirValueUseKind::InstructionOperand) {
    return nullptr;
  }
  return definition;
}

// True when the value's only records are bare value-rooted places that
// no instruction, loan, or terminator touches: the value has no consumer
// and may publish elsewhere without disturbing any spelled state. Unlike
// the Class-scoped root-record filter, this applies to any type.
[[nodiscard]] bool onlyRootRecordUses(const MirBody &body, MirValueId id) {
  for (const MirValueUse &use : body.usesOf(id)) {
    if (use.kind != MirValueUseKind::PlaceRoot) {
      return false;
    }
    const MirPlace *place = body.findPlace(use.place);
    if (place == nullptr || !place->projections.empty()) {
      return false;
    }
    for (const MirLoan &loan : body.loans) {
      if (loan.source == place->id) {
        return false;
      }
    }
    for (const MirBlock &block : body.blocks) {
      if (block.terminator.value &&
          block.terminator.value->place == place->id) {
        return false;
      }
      for (const MirInstruction &instruction : block.instructions) {
        if (instruction.destination == place->id ||
            (instruction.receiver &&
             instruction.receiver->place == place->id) ||
            std::any_of(instruction.operands.begin(),
                        instruction.operands.end(),
                        [&](const MirOperand &operand) {
                          return operand.place == place->id;
                        })) {
          return false;
        }
      }
    }
  }
  return true;
}

// Constructor initializer arguments are retained in the constructor instance
// rather than as ordinary MIR uses. Recognize the exact single-field
// publication so the probe and writer agree that its staged value does not
// need an independently representable local type.
[[nodiscard]] const MirConstructorInitializer *
stagedConstructorFieldPublication(const MirProgram &program,
                                  const MirBody &body, std::uint64_t owner,
                                  const MirInstruction &instruction) {
  if (body.kind != MirBodyKind::Constructor || instruction.hirValue == 0 ||
      !instruction.result || !onlyRootRecordUses(body, *instruction.result)) {
    return nullptr;
  }
  const MirConstructorInstance *constructor =
      program.findConstructorInstance(owner);
  if (constructor == nullptr) {
    return nullptr;
  }
  for (const MirConstructorInitializer &initializer :
       constructor->initializers) {
    if (initializer.kind == ConstructorInitializerTargetKind::Field &&
        initializer.field != 0 && initializer.arguments.size() == 1 &&
        initializer.arguments.front() == instruction.hirValue &&
        !initializer.storesReference) {
      return &initializer;
    }
  }
  return nullptr;
}

// A stores-reference initializer is represented by the constructor's paired
// Stored loan and the native member-initializer list. Its source Load is only
// the HIR initializer argument record; materializing it would copy the
// referent and disconnect the field from the parameter. Recognize only the
// exact load/source/initializer relation already proven by the bijective
// stored-reference schedule.
[[nodiscard]] const MirConstructorInitializer *
storedReferenceFieldPublication(const MirProgram &program, const MirBody &body,
                                std::uint64_t owner,
                                const MirInstruction &instruction) {
  if (body.kind != MirBodyKind::Constructor ||
      instruction.kind != MirInstructionKind::Load ||
      instruction.hirValue == 0 || !instruction.result ||
      instruction.operands.size() != 1 ||
      instruction.operands.front().kind != MirOperandKind::Copy ||
      instruction.operands.front().place == 0 ||
      !instruction.localFailureSites.empty() ||
      !instruction.definedFailure.empty() || !instruction.lifecycle.empty() ||
      !onlyRootRecordUses(body, *instruction.result)) {
    return nullptr;
  }
  const MirConstructorInstance *constructor =
      program.findConstructorInstance(owner);
  const std::optional<std::vector<CppMirStoredReferenceBinding>> bindings =
      constructor == nullptr ? std::nullopt
                             : cppMirStoredReferenceBindings(*constructor);
  if (constructor == nullptr || !bindings) {
    return nullptr;
  }
  for (const CppMirStoredReferenceBinding &binding : *bindings) {
    if (binding.initializer >= constructor->initializers.size()) {
      return nullptr;
    }
    const MirConstructorInitializer &initializer =
        constructor->initializers[binding.initializer];
    if (!initializer.storesReference || initializer.arguments.size() != 1 ||
        initializer.arguments.front() != instruction.hirValue) {
      continue;
    }
    const MirLoan *stored = nullptr;
    for (const MirLoan &loan : body.loans) {
      if (loan.kind != MirLoanKind::Stored ||
          loan.storedField != binding.field) {
        continue;
      }
      if (stored != nullptr) {
        return nullptr;
      }
      stored = &loan;
    }
    const MirPlace *source =
        stored == nullptr ? nullptr : body.findPlace(stored->source);
    const MirValue *result = body.findValue(*instruction.result);
    if (stored != nullptr && source != nullptr && result != nullptr &&
        stored->source == instruction.operands.front().place &&
        source->type == instruction.info.type &&
        result->info.type == instruction.info.type) {
      return &initializer;
    }
  }
  return nullptr;
}

// A bare value-rooted place that no instruction, loan, or terminator
// references is a pure root record: the rooted value flows through its
// own uses and the place spells nothing, so it needs no declaration and
// no representation row.
[[nodiscard]] bool unreferencedValueRootedPlace(const MirBody &body,
                                                const MirPlace &place) {
  if (place.root != MirPlaceRootKind::Value || !place.projections.empty()) {
    return false;
  }
  for (const MirLoan &loan : body.loans) {
    if (loan.source == place.id) {
      return false;
    }
  }
  for (const MirBlock &block : body.blocks) {
    if (block.terminator.value && block.terminator.value->place == place.id) {
      return false;
    }
    for (const MirInstruction &instruction : block.instructions) {
      if (instruction.destination == place.id ||
          (instruction.receiver && instruction.receiver->place == place.id) ||
          std::any_of(instruction.operands.begin(), instruction.operands.end(),
                      [&](const MirOperand &operand) {
                        return operand.place == place.id;
                      })) {
        return false;
      }
    }
  }
  return true;
}

struct ConcretePackParameterLayout {
  const MirFunctionInstance *owner = nullptr;
  const MirPlace *place = nullptr;
  std::size_t firstArgument = 0;
  std::size_t argumentCount = 0;
};

[[nodiscard]] const MirFunctionInstance *
packOwningFunction(const MirProgram &program, const MirBody &body,
                   MirBodyAddress address) {
  if (address.kind != MirBodyKind::Function) {
    return nullptr;
  }
  const MirFunctionInstance *owner =
      program.findFunctionInstance(address.owner);
  return owner != nullptr && &owner->body == &body ? owner : nullptr;
}

// A concrete pack instance retains one source binding while its native ABI
// carries one parameter per element. Prove the exact layout from that binding
// before a projected PackElement place names a flattened native argument.
[[nodiscard]] std::optional<ConcretePackParameterLayout>
concretePackParameterLayout(const MirProgram &program, const MirBody &body,
                            MirBodyAddress address,
                            const SemanticType &packType) {
  if (packType.kind != SemanticType::TypePack || !packType.concretePack) {
    return std::nullopt;
  }
  const MirFunctionInstance *owner = packOwningFunction(program, body, address);
  if (owner == nullptr) {
    return std::nullopt;
  }
  const auto place = std::find_if(
      body.places.begin(), body.places.end(), [&](const MirPlace &candidate) {
        return candidate.root == MirPlaceRootKind::Binding &&
               candidate.projections.empty() && candidate.binding != 0 &&
               candidate.type == packType;
      });
  if (place == body.places.end()) {
    return std::nullopt;
  }
  const auto binding =
      std::find(owner->parameterBindings.begin(),
                owner->parameterBindings.end(), place->binding);
  if (binding == owner->parameterBindings.end()) {
    return std::nullopt;
  }
  const std::size_t first = static_cast<std::size_t>(
      std::distance(owner->parameterBindings.begin(), binding));
  if (first > owner->parameterTypes.size()) {
    return std::nullopt;
  }

  const bool retained =
      first < owner->parameterTypes.size() &&
      owner->parameterTypes[first].kind == SemanticType::TypePack;
  if (retained) {
    const SemanticType &parameterPack = owner->parameterTypes[first];
    // A forwarded concrete pack can retain the generic-parameter identity of
    // the callee declaration while the lowered body place retains the caller
    // declaration's identity. The native layout depends only on the proved
    // concrete element sequence, matching MIR call lowering's contract.
    if (first + 1 != owner->parameterTypes.size() ||
        !parameterPack.concretePack ||
        parameterPack.arguments != packType.arguments) {
      return std::nullopt;
    }
  } else {
    if (owner->parameterTypes.size() != first + packType.arguments.size() ||
        !std::equal(packType.arguments.begin(), packType.arguments.end(),
                    owner->parameterTypes.begin() + first)) {
      return std::nullopt;
    }
  }
  return ConcretePackParameterLayout{.owner = owner,
                                     .place = &*place,
                                     .firstArgument = first,
                                     .argumentCount =
                                         packType.arguments.size()};
}

[[nodiscard]] std::optional<std::size_t>
packElementParameterIndex(const MirProgram &program, const MirBody &body,
                          MirBodyAddress address, const MirPlace &place) {
  if (place.root != MirPlaceRootKind::Binding ||
      place.projections.size() != 1 ||
      place.projections.front().kind != MirProjectionKind::PackElement ||
      !place.projections.front().constantIndex) {
    return std::nullopt;
  }
  const auto root = std::find_if(
      body.places.begin(), body.places.end(), [&](const MirPlace &candidate) {
        return candidate.id != place.id &&
               candidate.root == MirPlaceRootKind::Binding &&
               candidate.binding == place.binding &&
               candidate.projections.empty() &&
               candidate.type.kind == SemanticType::TypePack;
      });
  const std::optional<ConcretePackParameterLayout> layout =
      root == body.places.end()
          ? std::nullopt
          : concretePackParameterLayout(program, body, address, root->type);
  const std::size_t element =
      static_cast<std::size_t>(*place.projections.front().constantIndex);
  if (!layout || element >= layout->argumentCount ||
      element >= root->type.arguments.size() ||
      root->type.arguments[element] != place.type) {
    return std::nullopt;
  }
  return layout->firstArgument + element;
}

// Uses of a value excluding PlaceRoot records of pure root-record
// places: the root record spells nothing, so it must not defeat a
// single-consumer proof.
[[nodiscard]] std::vector<MirValueUse> nonRootRecordUses(const MirBody &body,
                                                         MirValueId id) {
  std::vector<MirValueUse> uses;
  for (const MirValueUse &use : body.usesOf(id)) {
    if (use.kind == MirValueUseKind::PlaceRoot) {
      const MirPlace *place = body.findPlace(use.place);
      if (place != nullptr && unreferencedValueRootedPlace(body, *place)) {
        continue;
      }
    }
    uses.push_back(use);
  }
  return uses;
}

struct ClassCopyAssignmentFusion {
  const MirInstruction *load = nullptr;
  const MirInstruction *assignment = nullptr;
  const MirPlace *source = nullptr;
  const MirPlace *destination = nullptr;

  [[nodiscard]] explicit operator bool() const {
    return load != nullptr && assignment != nullptr && source != nullptr &&
           destination != nullptr;
  }
};

struct ClassMoveArrayAssignmentFusion {
  const MirInstruction *move = nullptr;
  const MirInstruction *assignment = nullptr;
  const MirPlace *source = nullptr;
  const MirPlace *destination = nullptr;

  [[nodiscard]] explicit operator bool() const {
    return move != nullptr && assignment != nullptr && source != nullptr &&
           destination != nullptr;
  }
};

// A failure-free class Move consumed immediately by one fixed-array Assign has
// no independent MIR storage or drop identity. Preserve the exact schedule by
// staging the source into the checked-write helper's by-value parameter. That
// parameter is the moved SSA value: it is created before the bounds check and
// consumed by the assignment on success.
[[nodiscard]] ClassMoveArrayAssignmentFusion
classMoveArrayAssignmentFusion(const MirBody &body,
                               const MirInstruction &candidate) {
  const MirInstruction *move = &candidate;
  if (candidate.kind == MirInstructionKind::Assign &&
      candidate.operands.size() == 1 &&
      candidate.operands.front().kind == MirOperandKind::Value) {
    const MirValue *operand = body.findValue(candidate.operands.front().value);
    move = operand == nullptr ? nullptr
                              : findInstruction(body, operand->definition);
  }
  if (move == nullptr || move->kind != MirInstructionKind::Move ||
      move->intrinsic != IntrinsicKind::Move || !move->result ||
      move->destination || move->receiver ||
      move->operation != MirOperation::None || move->operands.size() != 1 ||
      move->operands.front().kind != MirOperandKind::Move ||
      move->operands.front().place == 0 ||
      move->info.type.kind != SemanticType::Class ||
      !move->info.traits.movable || !move->localFailureSites.empty() ||
      !move->definedFailure.empty() || !move->lifecycle.empty() ||
      move->fullExpressionEnd != 0 || move->cleanupBoundaryEnd != 0 ||
      !move->ownership || move->ownership->kind != OwnershipEventKind::Move) {
    return {};
  }

  const MirValueId movedValue = *move->result;
  const MirValue *value = body.findValue(movedValue);
  const MirPlace *source = body.findPlace(move->operands.front().place);
  const std::vector<MirValueUse> uses = body.usesOf(movedValue);
  if (value == nullptr || value->definition != move->id ||
      value->info.type != move->info.type || source == nullptr ||
      source->root != MirPlaceRootKind::Binding ||
      !source->projections.empty() || source->type != move->info.type ||
      move->operands.front().type != source->type || uses.size() != 1 ||
      uses.front().kind != MirValueUseKind::InstructionOperand ||
      uses.front().operandIndex != 0) {
    return {};
  }

  const MirInstruction *assignment =
      findInstruction(body, uses.front().instruction);
  const MirPlace *destination = assignment != nullptr && assignment->destination
                                    ? body.findPlace(*assignment->destination)
                                    : nullptr;
  const MirValue *assignmentResult = assignment != nullptr && assignment->result
                                         ? body.findValue(*assignment->result)
                                         : nullptr;
  if (assignment == nullptr || destination == nullptr ||
      assignmentResult == nullptr ||
      assignment->kind != MirInstructionKind::Assign ||
      assignment->operation != MirOperation::Assign || assignment->receiver ||
      assignment->operands.size() != 1 ||
      assignment->operands.front().kind != MirOperandKind::Value ||
      assignment->operands.front().value != movedValue ||
      assignment->operands.front().type != source->type ||
      assignment->info.type != source->type ||
      assignmentResult->definition != assignment->id ||
      assignmentResult->info.type != source->type ||
      destination->type != source->type ||
      !arrayElementAccess(body, *destination) ||
      !assignment->lifecycle.empty() || assignment->fullExpressionEnd != 0 ||
      assignment->cleanupBoundaryEnd != 0 ||
      !nonRootRecordUses(body, assignmentResult->id).empty()) {
    return {};
  }

  if (std::any_of(body.dropObligations.begin(), body.dropObligations.end(),
                  [&](const MirDropObligation &drop) {
                    return drop.generatedValue == movedValue ||
                           drop.generatedValue == assignmentResult->id;
                  })) {
    return {};
  }

  for (const MirBlock &block : body.blocks) {
    for (std::size_t index = 0; index + 1 < block.instructions.size();
         ++index) {
      if (block.instructions[index].id == move->id &&
          block.instructions[index + 1].id == assignment->id) {
        return {.move = move,
                .assignment = assignment,
                .source = source,
                .destination = destination};
      }
    }
  }
  return {};
}

// A class copy used only by the immediately following assignment has no MIR
// lifetime of its own: no place or drop obligation owns the loaded SSA value,
// and the discarded assignment result is metadata only. Fuse this exact bare
// binding shape into `destination = source`; materializing an intermediate C++
// object would invent construction and destruction that MIR does not contain.
[[nodiscard]] ClassCopyAssignmentFusion
classCopyAssignmentFusion(const MirBody &body,
                          const MirInstruction &candidate) {
  const MirInstruction *load = &candidate;
  if (candidate.kind == MirInstructionKind::Assign &&
      candidate.operands.size() == 1 &&
      candidate.operands.front().kind == MirOperandKind::Value) {
    const MirValue *operand = body.findValue(candidate.operands.front().value);
    load = operand == nullptr ? nullptr
                              : findInstruction(body, operand->definition);
  }
  if (load == nullptr || load->kind != MirInstructionKind::Load ||
      !load->result || load->destination || load->receiver ||
      load->operation != MirOperation::None || load->operands.size() != 1 ||
      load->operands.front().kind != MirOperandKind::Copy ||
      load->operands.front().place == 0 ||
      load->info.type.kind != SemanticType::Class ||
      !load->info.traits.copyable || !load->localFailureSites.empty() ||
      !load->definedFailure.empty() || !load->lifecycle.empty() ||
      load->fullExpressionEnd != 0 || load->cleanupBoundaryEnd != 0) {
    return {};
  }

  const MirValueId copiedValue = *load->result;
  const MirValue *copied = body.findValue(copiedValue);
  const MirPlace *source = body.findPlace(load->operands.front().place);
  const std::vector<MirValueUse> uses = nonRootRecordUses(body, copiedValue);
  if (copied == nullptr || copied->definition != load->id ||
      copied->info.type != load->info.type || source == nullptr ||
      source->root != MirPlaceRootKind::Binding ||
      !source->projections.empty() || source->type != load->info.type ||
      !source->traits.copyable || load->operands.front().type != source->type ||
      uses.size() != 1 ||
      uses.front().kind != MirValueUseKind::InstructionOperand ||
      uses.front().operandIndex != 0) {
    return {};
  }

  const MirInstruction *assignment =
      findInstruction(body, uses.front().instruction);
  const MirPlace *destination = assignment != nullptr && assignment->destination
                                    ? body.findPlace(*assignment->destination)
                                    : nullptr;
  const MirValue *assignmentResult = assignment != nullptr && assignment->result
                                         ? body.findValue(*assignment->result)
                                         : nullptr;
  if (assignment == nullptr || destination == nullptr ||
      assignmentResult == nullptr ||
      assignment->kind != MirInstructionKind::Assign ||
      assignment->operation != MirOperation::Assign || assignment->receiver ||
      assignment->operands.size() != 1 ||
      assignment->operands.front().kind != MirOperandKind::Value ||
      assignment->operands.front().value != copiedValue ||
      assignment->operands.front().type != source->type ||
      assignment->info.type != source->type ||
      assignmentResult->definition != assignment->id ||
      assignmentResult->info.type != source->type ||
      destination->root != MirPlaceRootKind::Binding ||
      !destination->projections.empty() || destination->type != source->type ||
      destination->access != AccessMode::Mutable ||
      !destination->traits.copyable || !assignment->localFailureSites.empty() ||
      !assignment->definedFailure.empty() || !assignment->lifecycle.empty() ||
      !nonRootRecordUses(body, assignmentResult->id).empty()) {
    return {};
  }

  if (std::any_of(body.dropObligations.begin(), body.dropObligations.end(),
                  [&](const MirDropObligation &drop) {
                    return drop.generatedValue == copiedValue ||
                           drop.generatedValue == assignmentResult->id;
                  })) {
    return {};
  }

  for (const MirBlock &block : body.blocks) {
    for (std::size_t index = 0; index + 1 < block.instructions.size();
         ++index) {
      if (block.instructions[index].id == load->id &&
          block.instructions[index + 1].id == assignment->id) {
        return {.load = load,
                .assignment = assignment,
                .source = source,
                .destination = destination};
      }
    }
  }
  return {};
}

// Assign yields an SSA record for expression bookkeeping. When that record has
// no executable use, reading the destination back would invent a copy and an
// independent lifetime that MIR does not contain.
[[nodiscard]] bool discardedAssignmentResult(const MirBody &body,
                                             MirValueId valueId) {
  const MirValue *value = body.findValue(valueId);
  const MirInstruction *definition =
      value == nullptr ? nullptr : findInstruction(body, value->definition);
  return definition != nullptr &&
         definition->kind == MirInstructionKind::Assign && definition->result &&
         *definition->result == valueId &&
         nonRootRecordUses(body, valueId).empty();
}

// A class Move in a constructor prologue may flow through one transient value
// into the explicit MIR Initialize for this.field. The transient preserves the
// Move's sequencing but needs no copied representation row of its own.
[[nodiscard]] const MirInstruction *
constructorFieldMoveInitialize(const MirBody &body, MirValueId id) {
  const MirValue *value = body.findValue(id);
  const MirInstruction *definition =
      value == nullptr ? nullptr : findInstruction(body, value->definition);
  const std::vector<MirValueUse> uses = nonRootRecordUses(body, id);
  if (body.kind != MirBodyKind::Constructor || definition == nullptr ||
      definition->kind != MirInstructionKind::Move || !definition->result ||
      *definition->result != id || uses.size() != 1 ||
      uses.front().kind != MirValueUseKind::InstructionOperand) {
    return nullptr;
  }
  const MirInstruction *initialize =
      findInstruction(body, uses.front().instruction);
  if (initialize == nullptr ||
      initialize->kind != MirInstructionKind::Initialize ||
      initialize->constructorInitializer == 0 || !initialize->destination ||
      initialize->operands.size() != 1 ||
      initialize->operands.front().kind != MirOperandKind::Value ||
      initialize->operands.front().value != id) {
    return nullptr;
  }
  const MirPlace *destination = body.findPlace(*initialize->destination);
  if (destination == nullptr || destination->root != MirPlaceRootKind::This ||
      destination->projections.size() != 1 ||
      destination->projections.front().kind != MirProjectionKind::Field ||
      destination->type != value->info.type) {
    return nullptr;
  }
  return initialize;
}

// The constructor metadata's ownedParameter field binding is the semantic
// authority for a direct member initializer. This helper ties it back to the
// exact entry-block Move/Initialize pair so body emission can erase those two
// records only when no user instruction can observe a different order.
[[nodiscard]] const MirConstructorInitializer *
ownedParameterFieldInitializer(const MirProgram &program, const MirBody &body,
                               HirConstructorInstanceId constructorId,
                               MirValueId movedValue) {
  const MirConstructorInstance *constructor =
      program.findConstructorInstance(constructorId);
  const MirValue *value = body.findValue(movedValue);
  const MirInstruction *move =
      value == nullptr ? nullptr : findInstruction(body, value->definition);
  const MirInstruction *initialize =
      constructorFieldMoveInitialize(body, movedValue);
  if (constructor == nullptr || &constructor->body != &body ||
      move == nullptr || initialize == nullptr || !initialize->destination ||
      move->operands.size() != 1 ||
      move->operands.front().kind != MirOperandKind::Move ||
      move->operands.front().place == 0) {
    return nullptr;
  }
  const MirPlace *source = body.findPlace(move->operands.front().place);
  const MirPlace *destination = body.findPlace(*initialize->destination);
  if (source == nullptr || destination == nullptr ||
      source->root != MirPlaceRootKind::Binding ||
      !source->projections.empty() ||
      destination->root != MirPlaceRootKind::This ||
      destination->projections.size() != 1 ||
      destination->projections.front().kind != MirProjectionKind::Field) {
    return nullptr;
  }
  const MirConstructorInitializer *matched = nullptr;
  for (const MirConstructorInitializer &initializer :
       constructor->initializers) {
    if (initializer.kind != ConstructorInitializerTargetKind::Field ||
        initializer.field != destination->projections.front().field ||
        initializer.storesReference || !initializer.ownedParameter ||
        *initializer.ownedParameter >= constructor->parameterBindings.size() ||
        constructor->parameterBindings[*initializer.ownedParameter] !=
            source->binding ||
        *initializer.ownedParameter >= constructor->parameterTypes.size() ||
        constructor->parameterTypes[*initializer.ownedParameter] !=
            source->type ||
        initializer.targetType != destination->type) {
      continue;
    }
    if (matched != nullptr) {
      return nullptr;
    }
    matched = &initializer;
  }
  if (matched == nullptr) {
    return nullptr;
  }
  const MirBlock *entry = nullptr;
  for (const MirBlock &block : body.blocks) {
    if (block.id == body.entry) {
      entry = &block;
      break;
    }
  }
  return entry != nullptr && entry->instructions.size() >= 2 &&
                 entry->instructions[0].id == move->id &&
                 entry->instructions[1].id == initialize->id
             ? matched
             : nullptr;
}

[[nodiscard]] std::optional<CppMirOwnedParameterFieldBinding>
ownedParameterFieldBinding(const MirProgram &program, const MirBody &body,
                           HirConstructorInstanceId constructorId,
                           MirValueId movedValue) {
  const MirConstructorInstance *constructor =
      program.findConstructorInstance(constructorId);
  if (constructor == nullptr || &constructor->body != &body) {
    return std::nullopt;
  }
  const std::optional<std::vector<CppMirOwnedParameterFieldBinding>> bindings =
      cppMirOwnedParameterFieldBindings(program, *constructor);
  if (!bindings) {
    return std::nullopt;
  }
  const auto matched =
      std::find_if(bindings->begin(), bindings->end(),
                   [&](const CppMirOwnedParameterFieldBinding &binding) {
                     return binding.movedValue == movedValue;
                   });
  return matched == bindings->end()
             ? std::nullopt
             : std::optional<CppMirOwnedParameterFieldBinding>{*matched};
}

[[nodiscard]] std::optional<CppMirCopyParameterFieldBinding>
copyParameterFieldBinding(const MirProgram &program, const MirBody &body,
                          HirConstructorInstanceId constructorId,
                          MirValueId loadedValue) {
  const MirConstructorInstance *constructor =
      program.findConstructorInstance(constructorId);
  if (constructor == nullptr || &constructor->body != &body) {
    return std::nullopt;
  }
  const std::optional<std::vector<CppMirCopyParameterFieldBinding>> bindings =
      cppMirCopyParameterFieldBindings(program, *constructor);
  if (!bindings) {
    return std::nullopt;
  }
  const auto matched =
      std::find_if(bindings->begin(), bindings->end(),
                   [&](const CppMirCopyParameterFieldBinding &binding) {
                     return binding.loadedValue == loadedValue;
                   });
  return matched == bindings->end()
             ? std::nullopt
             : std::optional<CppMirCopyParameterFieldBinding>{*matched};
}

[[nodiscard]] std::optional<CppMirCopyParameterFieldBinding>
copyParameterFieldInstructionBinding(const MirProgram &program,
                                     const MirBody &body,
                                     HirConstructorInstanceId constructorId,
                                     MirInstructionId instruction) {
  const MirConstructorInstance *constructor =
      program.findConstructorInstance(constructorId);
  if (constructor == nullptr || &constructor->body != &body) {
    return std::nullopt;
  }
  const std::optional<std::vector<CppMirCopyParameterFieldBinding>> bindings =
      cppMirCopyParameterFieldBindings(program, *constructor);
  if (!bindings) {
    return std::nullopt;
  }
  const auto matched =
      std::find_if(bindings->begin(), bindings->end(),
                   [&](const CppMirCopyParameterFieldBinding &binding) {
                     return binding.loadInstruction == instruction ||
                            binding.initializeInstruction == instruction;
                   });
  return matched == bindings->end()
             ? std::nullopt
             : std::optional<CppMirCopyParameterFieldBinding>{*matched};
}

// A direct owned-parameter field initializer consumes its source argument in
// the native member-initializer list. The parameter must therefore not also
// acquire a body-local lifetime slot (which would move from the argument a
// second time). Tie this exemption to the shared exact schedule proof.
[[nodiscard]] bool ownedParameterFieldSourcePlace(const MirProgram &program,
                                                  const MirBody &body,
                                                  const MirPlace &place) {
  if (body.kind != MirBodyKind::Constructor ||
      place.root != MirPlaceRootKind::Binding || !place.projections.empty()) {
    return false;
  }
  const MirConstructorInstance *constructor = nullptr;
  for (const MirConstructorInstance &candidate :
       program.constructorInstances()) {
    if (&candidate.body == &body) {
      if (constructor != nullptr) {
        return false;
      }
      constructor = &candidate;
    }
  }
  if (constructor == nullptr) {
    return false;
  }
  const std::optional<std::vector<CppMirOwnedParameterFieldBinding>> bindings =
      cppMirOwnedParameterFieldBindings(program, *constructor);
  return bindings &&
         std::count_if(bindings->begin(), bindings->end(),
                       [&](const CppMirOwnedParameterFieldBinding &binding) {
                         return binding.sourcePlace == place.id;
                       }) == 1;
}

[[nodiscard]] bool
ownedParameterFieldSourceDrop(const MirProgram &program, const MirBody &body,
                              const MirInstruction &instruction) {
  if (instruction.kind != MirInstructionKind::Drop ||
      !instruction.destination || instruction.lifecycle.size() != 1) {
    return false;
  }
  const MirConstructorInstance *constructor = nullptr;
  for (const MirConstructorInstance &candidate :
       program.constructorInstances()) {
    if (&candidate.body == &body) {
      if (constructor != nullptr) {
        return false;
      }
      constructor = &candidate;
    }
  }
  if (constructor == nullptr) {
    return false;
  }
  const std::optional<std::vector<CppMirOwnedParameterFieldBinding>> bindings =
      cppMirOwnedParameterFieldBindings(program, *constructor);
  return bindings &&
         std::count_if(bindings->begin(), bindings->end(),
                       [&](const CppMirOwnedParameterFieldBinding &binding) {
                         return binding.dropInstruction == instruction.id;
                       }) == 1;
}

// Expected extraction produces an lvalue into the payload; it does not create
// a new class object. This exact shape carries that lvalue through a value-root
// place so the failure ABI can publish the payload address without copying or
// default-constructing T.
[[nodiscard]] const MirPlace *expectedClassExtractionPlace(const MirBody &body,
                                                           MirValueId valueId) {
  const MirValue *value = body.findValue(valueId);
  const MirInstruction *definition =
      value == nullptr ? nullptr : findInstruction(body, value->definition);
  if (value == nullptr || value->info.type.kind != SemanticType::Class ||
      value->info.category != ValueCategory::Place || definition == nullptr ||
      definition->kind != MirInstructionKind::Call || !definition->result ||
      *definition->result != valueId || !definition->receiver ||
      !definition->operands.empty() || definition->functionTarget ||
      definition->constructorTarget || definition->lambdaTarget ||
      definition->bodyTarget || definition->callableInvocation ||
      (definition->intrinsic != IntrinsicKind::ExpectedValue &&
       definition->intrinsic != IntrinsicKind::ExpectedError) ||
      definition->receiver->type.kind != SemanticType::Expected ||
      definition->receiver->type.arguments.size() != 2 ||
      definition->localFailureSites.size() != 1 ||
      definition->info.type != value->info.type) {
    return nullptr;
  }
  const std::size_t payloadIndex =
      definition->intrinsic == IntrinsicKind::ExpectedValue ? 0u : 1u;
  if (definition->receiver->type.arguments[payloadIndex] != value->info.type ||
      body.usesOf(valueId).size() != 1 ||
      body.usesOf(valueId).front().kind != MirValueUseKind::PlaceRoot) {
    return nullptr;
  }
  const MirPlace *place = body.findPlace(body.usesOf(valueId).front().place);
  if (place == nullptr || place->root != MirPlaceRootKind::Value ||
      place->value != valueId || !place->projections.empty() ||
      place->type != value->info.type || place->access != value->info.access) {
    return nullptr;
  }
  const bool ownsStorage = std::any_of(
      body.dropObligations.begin(), body.dropObligations.end(),
      [&](const MirDropObligation &drop) {
        return drop.place == place->id || drop.generatedValue == valueId;
      });
  return ownsStorage ? nullptr : place;
}

struct StagedClassResult {
  const MirInstruction *producer = nullptr;
  const MirInstruction *stage = nullptr;
  const MirInstruction *consumer = nullptr;
  const MirPlace *slot = nullptr;
  const MirDropObligation *drop = nullptr;
};

[[nodiscard]] bool invokeSuccessActivates(const MirBody &body,
                                          const MirInstruction &producer,
                                          MirDropObligationId obligation) {
  const MirBlock *producerBlock = nullptr;
  for (const MirBlock &block : body.blocks) {
    if (block.terminator.kind != MirTerminatorKind::Invoke ||
        block.terminator.invokeInstruction != producer.id) {
      continue;
    }
    if (producerBlock != nullptr || block.instructions.empty() ||
        block.instructions.back().id != producer.id) {
      return false;
    }
    producerBlock = &block;
  }
  if (producerBlock == nullptr ||
      producerBlock->terminator.successLifecycle.size() != 1) {
    return false;
  }
  const MirLifecycleEvent &activation =
      producerBlock->terminator.successLifecycle.front();
  return activation.kind == MirLifecycleEventKind::Initialize &&
         activation.source == 0 && activation.target == obligation &&
         !activation.conditional && !activation.failureCleanup;
}

// A class result passed immediately by value to another call has an explicit
// PreparedParameter place in MIR. A transformed function call, an ordinary
// non-raising construction, or an exact targetless default construction may
// publish directly into that slot; the CallInput becomes metadata, and the
// consumer moves from and retires the slot. Every ownership and lifecycle
// record is checked here so a nearby but incomplete staging shape fails
// closed.
[[nodiscard]] StagedClassResult
stagedClassResultForSource(const MirBody &body, MirValueId sourceId) {
  const MirValue *source = body.findValue(sourceId);
  const MirInstruction *producer =
      source == nullptr ? nullptr : findInstruction(body, source->definition);
  const bool functionCallProducer =
      producer != nullptr && producer->kind == MirInstructionKind::Call &&
      producer->functionTarget && !producer->constructorTarget &&
      !producer->lambdaTarget && !producer->bodyTarget &&
      !producer->callableInvocation &&
      producer->intrinsic == IntrinsicKind::None;
  const bool constructorFailureContained =
      producer != nullptr && producer->constructorTarget &&
      producer->definedFailure.propagation ==
          FailurePropagationKind::Constructor &&
      producer->definedFailure.localOrigins.empty() &&
      producer->localFailureSites.empty() &&
      std::none_of(
          body.blocks.begin(), body.blocks.end(), [&](const MirBlock &block) {
            return block.terminator.kind == MirTerminatorKind::Invoke &&
                   block.terminator.invokeInstruction == producer->id;
          });
  const bool ordinaryConstructionProducer =
      producer != nullptr && producer->kind == MirInstructionKind::Construct &&
      producer->constructorTarget && !producer->destination &&
      !producer->receiver && !producer->functionTarget &&
      !producer->lambdaTarget && !producer->bodyTarget &&
      !producer->callableInvocation &&
      producer->intrinsic == IntrinsicKind::None &&
      producer->constructorKind == ConstructorKind::Ordinary &&
      producer->localFailureSites.empty() &&
      (producer->definedFailure.propagation == FailurePropagationKind::None ||
       constructorFailureContained) &&
      !producer->successResultDrop && !producer->successResultDestination;
  const bool defaultConstructionProducer =
      producer != nullptr && producer->kind == MirInstructionKind::Call &&
      !producer->functionTarget && !producer->constructorTarget &&
      !producer->lambdaTarget && !producer->bodyTarget &&
      !producer->callableInvocation && !producer->receiver &&
      (producer->intrinsic == IntrinsicKind::None ||
       producer->intrinsic ==
           IntrinsicKind::DefaultTypeParameterConstruction) &&
      producer->operands.empty() && producer->callableArguments.empty() &&
      producer->localFailureSites.empty() &&
      producer->definedFailure.propagation == FailurePropagationKind::None &&
      !producer->successResultDrop && !producer->successResultDestination;
  if (source == nullptr || source->info.type.kind != SemanticType::Class ||
      producer == nullptr ||
      (!functionCallProducer && !ordinaryConstructionProducer &&
       !defaultConstructionProducer) ||
      !producer->result || *producer->result != sourceId ||
      producer->info.type != source->info.type) {
    return {};
  }
  const std::vector<MirValueUse> sourceUses = nonRootRecordUses(body, sourceId);
  if (sourceUses.size() != 1 ||
      sourceUses.front().kind != MirValueUseKind::InstructionOperand) {
    return {};
  }
  const MirInstruction *stage =
      findInstruction(body, sourceUses.front().instruction);
  if (stage == nullptr || stage->kind != MirInstructionKind::CallInput ||
      stage->callInputKind != HirCallInputKind::MoveValue || !stage->result ||
      !stage->destination || stage->receiver || stage->operands.size() != 1 ||
      stage->operands.front().kind != MirOperandKind::Value ||
      stage->operands.front().value != sourceId ||
      !stage->preparedParameterDrop || !stage->localFailureSites.empty() ||
      stage->lifecycle.size() != 1) {
    return {};
  }
  const MirPlace *slot = body.findPlace(*stage->destination);
  const MirDropObligation *drop =
      body.findDropObligation(*stage->preparedParameterDrop);
  const MirLifecycleEvent &stageLifecycle = stage->lifecycle.front();
  const auto producerResultDrop = [&]() -> const MirDropObligation * {
    if (stageLifecycle.kind != MirLifecycleEventKind::Reparent ||
        stageLifecycle.source == 0) {
      return nullptr;
    }
    const MirDropObligation *sourceDrop =
        body.findDropObligation(stageLifecycle.source);
    const MirPlace *sourcePlace =
        sourceDrop == nullptr ? nullptr : body.findPlace(sourceDrop->place);
    if (sourceDrop == nullptr ||
        sourceDrop->kind != MirDropObligationKind::Value ||
        sourceDrop->dropType.type != source->info.type ||
        sourceDrop->initiallyActive || sourcePlace == nullptr ||
        sourcePlace->root != MirPlaceRootKind::Value ||
        sourcePlace->value != sourceId || !sourcePlace->projections.empty() ||
        sourcePlace->type != source->info.type ||
        (producer->successResultDrop &&
         *producer->successResultDrop != sourceDrop->id)) {
      return nullptr;
    }

    std::vector<MirDropObligationId> inputDrops;
    const auto collectInputDrop = [&](const MirOperand &operand) {
      const MirValue *inputValue = operand.kind == MirOperandKind::Value
                                       ? body.findValue(operand.value)
                                       : nullptr;
      const MirInstruction *input =
          inputValue == nullptr ? nullptr
                                : findInstruction(body, inputValue->definition);
      if (input != nullptr && input->kind == MirInstructionKind::CallInput &&
          input->preparedParameterDrop) {
        inputDrops.push_back(*input->preparedParameterDrop);
      }
    };
    if (producer->receiver) {
      collectInputDrop(*producer->receiver);
    }
    for (const MirOperand &operand : producer->operands) {
      collectInputDrop(operand);
    }
    std::vector<bool> transferred(inputDrops.size(), false);
    std::size_t activations = 0;
    if (producer->successResultDrop) {
      if (*producer->successResultDrop != sourceDrop->id ||
          !invokeSuccessActivates(body, *producer, sourceDrop->id)) {
        return nullptr;
      }
      activations = 1;
    }
    for (const MirLifecycleEvent &event : producer->lifecycle) {
      if (event.conditional || event.failureCleanup) {
        return nullptr;
      }
      if (event.kind == MirLifecycleEventKind::Initialize &&
          event.source == 0 && event.target == sourceDrop->id) {
        ++activations;
        continue;
      }
      if (event.kind != MirLifecycleEventKind::TransferOut ||
          event.source == 0 || event.target != 0) {
        return nullptr;
      }
      const auto matched =
          std::find(inputDrops.begin(), inputDrops.end(), event.source);
      if (matched == inputDrops.end()) {
        return nullptr;
      }
      const std::size_t index =
          static_cast<std::size_t>(matched - inputDrops.begin());
      if (transferred[index]) {
        return nullptr;
      }
      transferred[index] = true;
    }
    return activations == 1 &&
                   std::all_of(transferred.begin(), transferred.end(),
                               [](bool value) { return value; })
               ? sourceDrop
               : nullptr;
  }();
  const bool directActivation =
      stageLifecycle.kind == MirLifecycleEventKind::Initialize &&
      stageLifecycle.source == 0 && drop != nullptr &&
      stageLifecycle.target == drop->id;
  const bool reparentedActivation =
      producerResultDrop != nullptr && drop != nullptr &&
      stageLifecycle.kind == MirLifecycleEventKind::Reparent &&
      stageLifecycle.source == producerResultDrop->id &&
      stageLifecycle.target == drop->id;
  if (slot == nullptr || slot->root != MirPlaceRootKind::Temporary ||
      !slot->projections.empty() || slot->type != source->info.type ||
      drop == nullptr ||
      drop->kind != MirDropObligationKind::PreparedParameter ||
      drop->place != slot->id || drop->dropType.type != slot->type ||
      drop->initiallyActive || (!directActivation && !reparentedActivation) ||
      stageLifecycle.conditional || stageLifecycle.failureCleanup ||
      (producer->successResultDestination &&
       *producer->successResultDestination != slot->id)) {
    return {};
  }
  const MirValue *stagedValue = body.findValue(*stage->result);
  const std::vector<MirValueUse> stagedUses =
      nonRootRecordUses(body, *stage->result);
  if (stagedValue == nullptr || stagedValue->info.type != source->info.type ||
      stagedUses.size() != 1 ||
      stagedUses.front().kind != MirValueUseKind::InstructionOperand) {
    return {};
  }
  const MirInstruction *consumer =
      findInstruction(body, stagedUses.front().instruction);
  if (consumer == nullptr || consumer->kind != MirInstructionKind::Call ||
      std::count_if(consumer->operands.begin(), consumer->operands.end(),
                    [&](const MirOperand &operand) {
                      return operand.kind == MirOperandKind::Value &&
                             operand.value == *stage->result;
                    }) != 1 ||
      std::count_if(consumer->lifecycle.begin(), consumer->lifecycle.end(),
                    [&](const MirLifecycleEvent &event) {
                      return event.kind == MirLifecycleEventKind::TransferOut &&
                             event.source == drop->id && event.target == 0 &&
                             !event.conditional && !event.failureCleanup;
                    }) != 1) {
    return {};
  }
  return {.producer = producer,
          .stage = stage,
          .consumer = consumer,
          .slot = slot,
          .drop = drop};
}

[[nodiscard]] StagedClassResult
stagedClassResultForResult(const MirBody &body, MirValueId stagedId) {
  const MirValue *value = body.findValue(stagedId);
  const MirInstruction *stage =
      value == nullptr ? nullptr : findInstruction(body, value->definition);
  if (stage == nullptr || stage->kind != MirInstructionKind::CallInput ||
      !stage->result || *stage->result != stagedId ||
      stage->operands.size() != 1 ||
      stage->operands.front().kind != MirOperandKind::Value) {
    return {};
  }
  StagedClassResult result =
      stagedClassResultForSource(body, stage->operands.front().value);
  return result.stage == stage ? result : StagedClassResult{};
}

// A class SSA value whose only executable consumer is one Initialize into an
// empty class binding can publish directly into that binding's lifetime slot.
// The PlaceRoot record for the temporary drop identity is metadata only; the
// Initialize's lifecycle event reparents that identity to the binding.
[[nodiscard]] const MirPlace *classValueDestinationSlot(const MirBody &body,
                                                        MirValueId valueId) {
  const MirValue *value = body.findValue(valueId);
  if (value == nullptr || value->info.type.kind != SemanticType::Class) {
    return nullptr;
  }
  const MirInstruction *definition = findInstruction(body, value->definition);
  const bool directConstruct =
      definition != nullptr &&
      definition->kind == MirInstructionKind::Construct && definition->result &&
      *definition->result == valueId && !definition->destination &&
      !definition->receiver;
  const bool directMove =
      definition != nullptr && definition->kind == MirInstructionKind::Move &&
      definition->result && *definition->result == valueId &&
      definition->operands.size() == 1 &&
      definition->operands.front().kind == MirOperandKind::Move &&
      definition->operands.front().place != 0;
  const bool directFunctionCall =
      definition != nullptr && definition->kind == MirInstructionKind::Call &&
      definition->result && *definition->result == valueId &&
      definition->functionTarget && !definition->constructorTarget &&
      !definition->lambdaTarget && !definition->bodyTarget &&
      !definition->callableInvocation &&
      definition->intrinsic == IntrinsicKind::None;
  const bool directDefaultConstruction =
      definition != nullptr && definition->kind == MirInstructionKind::Call &&
      definition->result && *definition->result == valueId &&
      !definition->functionTarget && !definition->constructorTarget &&
      !definition->lambdaTarget && !definition->bodyTarget &&
      !definition->callableInvocation && !definition->receiver &&
      (definition->intrinsic == IntrinsicKind::None ||
       definition->intrinsic ==
           IntrinsicKind::DefaultTypeParameterConstruction) &&
      definition->operands.empty() && definition->callableArguments.empty() &&
      definition->localFailureSites.empty();
  if (!directConstruct && !directMove && !directFunctionCall &&
      !directDefaultConstruction) {
    return nullptr;
  }
  if (directFunctionCall) {
    const StagedClassResult staged = stagedClassResultForSource(body, valueId);
    if (staged.slot != nullptr) {
      return staged.slot;
    }
  }
  const std::vector<MirValueUse> uses = nonRootRecordUses(body, valueId);
  if (uses.size() != 1 ||
      uses.front().kind != MirValueUseKind::InstructionOperand) {
    return nullptr;
  }
  const MirInstruction *initialize =
      findInstruction(body, uses.front().instruction);
  if (initialize == nullptr ||
      initialize->kind != MirInstructionKind::Initialize ||
      !initialize->destination || initialize->operands.size() != 1 ||
      initialize->operands.front().kind != MirOperandKind::Value ||
      initialize->operands.front().value != valueId) {
    return nullptr;
  }
  if (definition->successResultDestination &&
      definition->successResultDestination != initialize->destination) {
    return nullptr;
  }
  const MirPlace *destination = body.findPlace(*initialize->destination);
  if (destination == nullptr ||
      destination->root != MirPlaceRootKind::Binding ||
      !destination->projections.empty() ||
      destination->type != value->info.type) {
    return nullptr;
  }
  return destination;
}

struct DirectTemporaryReceiver {
  const MirInstruction *producer = nullptr;
  const MirInstruction *call = nullptr;
  const MirPlace *slot = nullptr;
  const MirDropObligation *drop = nullptr;

  [[nodiscard]] explicit operator bool() const {
    return producer != nullptr && call != nullptr && slot != nullptr &&
           drop != nullptr;
  }
};

// A source temporary used only as a read-only member receiver already owns
// explicit MIR storage and cleanup. Keep it in that slot rather than
// materializing an SSA object or relying on a C++ temporary destructor. Both
// ordinary construction and a class-returning call may be the producer; the
// exact activation and later MIR Drop remain the sole lifetime authority.
[[nodiscard]] DirectTemporaryReceiver
directTemporaryReceiver(const MirBody &body, const MirInstruction &call) {
  if (call.kind != MirInstructionKind::Call || call.callSite != 0 ||
      !call.functionTarget || call.intrinsic != IntrinsicKind::None ||
      !call.receiver || call.receiver->kind != MirOperandKind::Value ||
      call.receiver->value == 0 ||
      call.receiver->type.kind != SemanticType::Class ||
      !call.localFailureSites.empty() || !call.lifecycle.empty()) {
    return {};
  }

  const MirValue *value = body.findValue(call.receiver->value);
  const MirInstruction *producer =
      value == nullptr ? nullptr : findInstruction(body, value->definition);
  const MirDropObligation *resultDrop =
      producer != nullptr && producer->successResultDrop
          ? body.findDropObligation(*producer->successResultDrop)
          : nullptr;
  const MirPlace *slot =
      producer != nullptr && producer->destination
          ? body.findPlace(*producer->destination)
          : (resultDrop != nullptr ? body.findPlace(resultDrop->place)
                                   : nullptr);
  const bool ordinaryConstruction =
      producer != nullptr && producer->kind == MirInstructionKind::Construct &&
      producer->constructorKind == ConstructorKind::Ordinary &&
      !producer->receiver && producer->intrinsic == IntrinsicKind::None;
  const bool functionCall =
      producer != nullptr && producer->kind == MirInstructionKind::Call &&
      producer->functionTarget && !producer->constructorTarget &&
      !producer->lambdaTarget && !producer->bodyTarget &&
      !producer->callableInvocation &&
      producer->intrinsic == IntrinsicKind::None;
  if (value == nullptr || producer == nullptr || slot == nullptr ||
      (!ordinaryConstruction && !functionCall) ||
      value->info.type != call.receiver->type || !producer->result ||
      *producer->result != value->id ||
      producer->info.type != value->info.type ||
      !producer->localFailureSites.empty() ||
      (producer->successResultDestination &&
       *producer->successResultDestination != slot->id) ||
      (slot->root != MirPlaceRootKind::Temporary &&
       (slot->root != MirPlaceRootKind::Value || slot->value != value->id)) ||
      !slot->projections.empty() || slot->type != value->info.type ||
      slot->access != AccessMode::Mutable) {
    return {};
  }

  std::size_t receiverUses = 0;
  for (const MirValueUse &use : body.usesOf(value->id)) {
    if (use.kind == MirValueUseKind::PlaceRoot && use.place == slot->id) {
      continue;
    }
    if (use.kind != MirValueUseKind::InstructionReceiver ||
        use.instruction != call.id || ++receiverUses != 1) {
      return {};
    }
  }
  if (receiverUses != 1) {
    return {};
  }

  MirDropObligationId activatedDrop = 0;
  for (const MirLifecycleEvent &event : producer->lifecycle) {
    if (event.kind != MirLifecycleEventKind::Initialize || event.source != 0 ||
        event.target == 0 || event.conditional || event.failureCleanup ||
        activatedDrop != 0) {
      return {};
    }
    activatedDrop = event.target;
  }
  if (producer->successResultDrop) {
    if (activatedDrop != 0 ||
        !invokeSuccessActivates(body, *producer,
                                *producer->successResultDrop)) {
      return {};
    }
    activatedDrop = *producer->successResultDrop;
  }
  const MirDropObligation *drop =
      activatedDrop == 0 ? nullptr : body.findDropObligation(activatedDrop);
  if (drop == nullptr || (resultDrop != nullptr && resultDrop != drop) ||
      drop->kind != MirDropObligationKind::Value || drop->place != slot->id ||
      drop->dropType.type != slot->type || drop->initiallyActive) {
    return {};
  }

  std::size_t scheduledDrops = 0;
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      for (const MirLifecycleEvent &event : instruction.lifecycle) {
        if (event.source != drop->id && event.target != drop->id) {
          continue;
        }
        if (&instruction == producer &&
            event.kind == MirLifecycleEventKind::Initialize &&
            event.source == 0 && event.target == drop->id) {
          continue;
        }
        if (instruction.kind != MirInstructionKind::Drop ||
            instruction.destination != slot->id ||
            event.kind != MirLifecycleEventKind::Drop ||
            event.source != drop->id || event.target != 0) {
          return {};
        }
        ++scheduledDrops;
      }
    }
  }
  return scheduledDrops != 0 ? DirectTemporaryReceiver{.producer = producer,
                                                       .call = &call,
                                                       .slot = slot,
                                                       .drop = drop}
                             : DirectTemporaryReceiver{};
}

[[nodiscard]] DirectTemporaryReceiver
directTemporaryReceiverForValue(const MirBody &body, MirValueId valueId) {
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      if (instruction.receiver &&
          instruction.receiver->kind == MirOperandKind::Value &&
          instruction.receiver->value == valueId) {
        return directTemporaryReceiver(body, instruction);
      }
    }
  }
  return {};
}

[[nodiscard]] DirectTemporaryReceiver
directTemporaryReceiverForSlot(const MirBody &body, const MirPlace &slot) {
  for (const MirValue &value : body.values) {
    const DirectTemporaryReceiver receiver =
        directTemporaryReceiverForValue(body, value.id);
    if (receiver.slot == &slot) {
      return receiver;
    }
  }
  return {};
}

[[nodiscard]] const MirPlace *classValuePublicationSlot(const MirBody &body,
                                                        MirValueId valueId) {
  const DirectTemporaryReceiver temporary =
      directTemporaryReceiverForValue(body, valueId);
  return temporary.slot != nullptr ? temporary.slot
                                   : classValueDestinationSlot(body, valueId);
}

// A destination-less class Construct can publish only into the one exact
// storage consumer represented in MIR. Keep this classifier shared by the
// support probe and writer so newly admitted slot families cannot diverge.
[[nodiscard]] MirPlaceId
constructDestinationSlot(const MirBody &body, const MirInstruction &construct) {
  if (!construct.result) {
    return 0;
  }
  const DirectTemporaryReceiver temporary =
      directTemporaryReceiverForValue(body, *construct.result);
  if (temporary.producer == &construct && temporary.slot != nullptr &&
      construct.destination == temporary.slot->id) {
    return temporary.slot->id;
  }
  MirPlaceId selected = 0;
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      if (instruction.kind != MirInstructionKind::Initialize ||
          instruction.operands.size() != 1 ||
          instruction.operands.front().kind != MirOperandKind::Value ||
          instruction.operands.front().value != *construct.result ||
          !instruction.destination) {
        continue;
      }
      if (selected != 0) {
        return 0;
      }
      selected = *instruction.destination;
    }
  }
  return selected;
}

struct ConditionalClassReturnJoinArm {
  const MirValue *value = nullptr;
  const MirInstruction *move = nullptr;
  const MirInstruction *initialize = nullptr;
  const MirBlock *block = nullptr;
  const MirPlace *slot = nullptr;
  const MirDropObligation *slotDrop = nullptr;
};

struct ConditionalClassReturnJoin {
  const MirPlace *slot = nullptr;
  const MirDropObligation *slotDrop = nullptr;
  const MirInstruction *returnMove = nullptr;
  const MirInstruction *transferOut = nullptr;
  std::vector<ConditionalClassReturnJoinArm> arms;

  [[nodiscard]] explicit operator bool() const { return slot != nullptr; }
};

// One arm of a class-valued conditional moves a source place into a value
// drop and immediately reparents that drop into the shared temporary. This is
// only the local arm shape; conditionalClassReturnJoin() below proves the
// complete two-arm CFG and return transfer before the backend uses it.
[[nodiscard]] ConditionalClassReturnJoinArm
conditionalClassReturnJoinArm(const MirBody &body, MirValueId valueId) {
  const MirValue *value = body.findValue(valueId);
  const MirInstruction *move =
      value == nullptr ? nullptr : findInstruction(body, value->definition);
  if (value == nullptr || value->info.type.kind != SemanticType::Class ||
      move == nullptr || move->kind != MirInstructionKind::Move ||
      !move->result || *move->result != valueId || move->destination ||
      move->receiver || move->operands.size() != 1 ||
      move->operands.front().kind != MirOperandKind::Move ||
      move->operands.front().place == 0 ||
      move->operands.front().type != value->info.type ||
      !move->localFailureSites.empty() ||
      !move->definedFailure.localOrigins.empty() ||
      move->definedFailure.propagation != FailurePropagationKind::None ||
      move->lifecycle.size() != 1) {
    return {};
  }
  const MirPlace *source = body.findPlace(move->operands.front().place);
  const MirLifecycleEvent &moved = move->lifecycle.front();
  const MirDropObligation *sourceDrop = body.findDropObligation(moved.source);
  const MirDropObligation *valueDrop = body.findDropObligation(moved.target);
  const MirPlace *valuePlace =
      valueDrop == nullptr ? nullptr : body.findPlace(valueDrop->place);
  if (source == nullptr || source->type != value->info.type ||
      sourceDrop == nullptr || sourceDrop->place != source->id ||
      sourceDrop->dropType.type != value->info.type ||
      moved.kind != MirLifecycleEventKind::Move || moved.source == 0 ||
      moved.target == 0 || moved.conditional || moved.failureCleanup ||
      valueDrop == nullptr || valueDrop->kind != MirDropObligationKind::Value ||
      valueDrop->initiallyActive ||
      valueDrop->dropType.type != value->info.type || valuePlace == nullptr ||
      valuePlace->root != MirPlaceRootKind::Value ||
      valuePlace->value != valueId || !valuePlace->projections.empty() ||
      valuePlace->type != value->info.type) {
    return {};
  }

  const std::vector<MirValueUse> uses = nonRootRecordUses(body, valueId);
  const MirInstruction *initialize =
      uses.size() == 1 &&
              uses.front().kind == MirValueUseKind::InstructionOperand &&
              uses.front().operandIndex == 0
          ? findInstruction(body, uses.front().instruction)
          : nullptr;
  if (initialize == nullptr ||
      initialize->kind != MirInstructionKind::Initialize ||
      !initialize->destination || initialize->receiver || initialize->result ||
      initialize->operands.size() != 1 ||
      initialize->operands.front().kind != MirOperandKind::Value ||
      initialize->operands.front().value != valueId ||
      initialize->operands.front().type != value->info.type ||
      !initialize->localFailureSites.empty() ||
      !initialize->definedFailure.localOrigins.empty() ||
      initialize->definedFailure.propagation != FailurePropagationKind::None ||
      initialize->lifecycle.size() != 1) {
    return {};
  }
  const MirPlace *slot = body.findPlace(*initialize->destination);
  const MirLifecycleEvent &reparent = initialize->lifecycle.front();
  const MirDropObligation *slotDrop = body.findDropObligation(reparent.target);
  if (slot == nullptr || slot->root != MirPlaceRootKind::Temporary ||
      slot->temporary == 0 || !slot->projections.empty() ||
      slot->type != value->info.type || slot->initiallyAvailable ||
      reparent.kind != MirLifecycleEventKind::Reparent ||
      reparent.source != valueDrop->id || reparent.target == 0 ||
      reparent.conditional || reparent.failureCleanup || slotDrop == nullptr ||
      slotDrop->kind != MirDropObligationKind::Value ||
      slotDrop->place != slot->id || slotDrop->initiallyActive ||
      slotDrop->dropType.type != slot->type ||
      !slotDrop->dropType.requiresActiveCleanup) {
    return {};
  }

  const MirBlock *block = body.findBlock(value->definitionBlock);
  if (block == nullptr || uses.front().block != block->id) {
    return {};
  }
  bool adjacent = false;
  for (std::size_t index = 0; index + 1 < block->instructions.size(); ++index) {
    if (block->instructions[index].id == move->id &&
        block->instructions[index + 1].id == initialize->id) {
      adjacent = true;
      break;
    }
  }
  return adjacent ? ConditionalClassReturnJoinArm{value, move, initialize,
                                                  block, slot, slotDrop}
                  : ConditionalClassReturnJoinArm{};
}

// A class-valued conditional return owns one initially-empty temporary slot.
// Each branch constructs that slot exactly once, the join moves it into the
// function result, and the explicit TransferOut ends the moved-from C++
// object's lifetime. Requiring the whole diamond prevents a generic
// class-phi heuristic from inventing initialization or cleanup policy.
[[nodiscard]] ConditionalClassReturnJoin
conditionalClassReturnJoin(const MirBody &body, const MirPlace &slot) {
  if (slot.root != MirPlaceRootKind::Temporary || slot.temporary == 0 ||
      !slot.projections.empty() || slot.type.kind != SemanticType::Class ||
      slot.initiallyAvailable) {
    return {};
  }

  ConditionalClassReturnJoin result;
  for (const MirValue &value : body.values) {
    ConditionalClassReturnJoinArm arm =
        conditionalClassReturnJoinArm(body, value.id);
    if (arm.slot == nullptr || arm.slot->id != slot.id) {
      continue;
    }
    if (result.slotDrop != nullptr && result.slotDrop != arm.slotDrop) {
      return {};
    }
    result.slotDrop = arm.slotDrop;
    result.arms.push_back(arm);
  }
  if (result.arms.size() != 2 || result.slotDrop == nullptr ||
      result.arms[0].block == result.arms[1].block ||
      result.arms[0].block->terminator.kind != MirTerminatorKind::Goto ||
      result.arms[1].block->terminator.kind != MirTerminatorKind::Goto ||
      result.arms[0].block->terminator.target == 0 ||
      result.arms[0].block->terminator.target !=
          result.arms[1].block->terminator.target) {
    return {};
  }

  const MirBlockId joinId = result.arms[0].block->terminator.target;
  const MirBlock *join = body.findBlock(joinId);
  const MirBlock *branch = nullptr;
  for (const MirBlock &candidate : body.blocks) {
    const bool exactDiamond =
        candidate.terminator.kind == MirTerminatorKind::Branch &&
        ((candidate.terminator.target == result.arms[0].block->id &&
          candidate.terminator.elseTarget == result.arms[1].block->id) ||
         (candidate.terminator.target == result.arms[1].block->id &&
          candidate.terminator.elseTarget == result.arms[0].block->id));
    if (exactDiamond) {
      if (branch != nullptr) {
        return {};
      }
      branch = &candidate;
    }
  }
  if (join == nullptr || branch == nullptr ||
      join->terminator.kind != MirTerminatorKind::Return ||
      !join->terminator.value ||
      join->terminator.value->kind != MirOperandKind::Value ||
      join->terminator.value->type != slot.type) {
    return {};
  }

  std::size_t joinPredecessors = 0;
  for (const MirBlock &candidate : body.blocks) {
    bool reachesJoin = candidate.terminator.target == joinId ||
                       candidate.terminator.elseTarget == joinId;
    reachesJoin =
        reachesJoin || std::any_of(candidate.terminator.switchTargets.begin(),
                                   candidate.terminator.switchTargets.end(),
                                   [&](const MirSwitchTarget &target) {
                                     return target.target == joinId;
                                   });
    if (!reachesJoin) {
      continue;
    }
    if (candidate.id != result.arms[0].block->id &&
        candidate.id != result.arms[1].block->id) {
      return {};
    }
    ++joinPredecessors;
  }
  if (joinPredecessors != 2) {
    return {};
  }

  std::size_t returnMoveIndex = 0;
  std::size_t transferIndex = 0;
  for (std::size_t index = 0; index < join->instructions.size(); ++index) {
    const MirInstruction &instruction = join->instructions[index];
    const bool readsSlot = std::any_of(
        instruction.operands.begin(), instruction.operands.end(),
        [&](const MirOperand &operand) { return operand.place == slot.id; });
    if (readsSlot) {
      if (result.returnMove != nullptr ||
          instruction.kind != MirInstructionKind::Move || !instruction.result ||
          instruction.destination || instruction.receiver ||
          instruction.operands.size() != 1 ||
          instruction.operands.front().kind != MirOperandKind::Move ||
          instruction.operands.front().type != slot.type ||
          instruction.info.type != slot.type ||
          !instruction.localFailureSites.empty() ||
          !instruction.definedFailure.localOrigins.empty() ||
          instruction.definedFailure.propagation !=
              FailurePropagationKind::None ||
          !instruction.lifecycle.empty()) {
        return {};
      }
      result.returnMove = &instruction;
      returnMoveIndex = index;
    }
    if (instruction.destination && *instruction.destination == slot.id) {
      return {};
    }
    if (instruction.kind == MirInstructionKind::Drop &&
        instruction.destination && *instruction.destination == slot.id) {
      return {};
    }
    if (instruction.kind != MirInstructionKind::Lifecycle ||
        instruction.lifecycle.size() != 1) {
      continue;
    }
    const MirLifecycleEvent &event = instruction.lifecycle.front();
    if (event.kind != MirLifecycleEventKind::TransferOut ||
        event.source != result.slotDrop->id) {
      continue;
    }
    if (result.transferOut != nullptr || event.target != 0 ||
        event.conditional || event.failureCleanup ||
        instruction.fullExpressionEnd != 0 ||
        instruction.cleanupBoundaryEnd != 0) {
      return {};
    }
    result.transferOut = &instruction;
    transferIndex = index;
  }
  if (result.returnMove == nullptr || result.transferOut == nullptr ||
      returnMoveIndex >= transferIndex ||
      join->terminator.value->value != *result.returnMove->result) {
    return {};
  }
  const std::vector<MirValueUse> returnUses =
      nonRootRecordUses(body, *result.returnMove->result);
  if (returnUses.size() != 1 ||
      returnUses.front().kind != MirValueUseKind::Terminator ||
      returnUses.front().block != join->id) {
    return {};
  }

  result.slot = &slot;
  return result;
}

[[nodiscard]] const MirPlace *
conditionalClassReturnJoinSlotForValue(const MirBody &body,
                                       MirValueId valueId) {
  const ConditionalClassReturnJoinArm arm =
      conditionalClassReturnJoinArm(body, valueId);
  if (arm.slot == nullptr) {
    return nullptr;
  }
  const ConditionalClassReturnJoin join =
      conditionalClassReturnJoin(body, *arm.slot);
  if (!join) {
    return nullptr;
  }
  return std::any_of(join.arms.begin(), join.arms.end(),
                     [&](const ConditionalClassReturnJoinArm &candidate) {
                       return candidate.value == arm.value;
                     })
             ? join.slot
             : nullptr;
}

[[nodiscard]] const MirPlace *
conditionalClassReturnJoinSlotForTransfer(const MirBody &body,
                                          const MirInstruction &instruction) {
  for (const MirPlace &place : body.places) {
    const ConditionalClassReturnJoin join =
        conditionalClassReturnJoin(body, place);
    if (join && join.transferOut == &instruction) {
      return join.slot;
    }
  }
  return nullptr;
}

[[nodiscard]] const MirDropObligation *
uniqueValueDrop(const MirBody &body, MirValueId valueId,
                const SemanticType &type);
[[nodiscard]] bool hasCompleteCallInputSchedule(const MirBody &body,
                                                const MirInstruction &call);

struct ConditionalClassBindingJoinArm {
  const MirValue *value = nullptr;
  const MirInstruction *construct = nullptr;
  const MirInstruction *initialize = nullptr;
  const MirBlock *block = nullptr;
  const MirPlace *valuePlace = nullptr;
  const MirDropObligation *valueDrop = nullptr;
  const MirPlace *slot = nullptr;
  const MirDropObligation *slotDrop = nullptr;
};

struct ConditionalClassBindingJoin {
  const MirPlace *slot = nullptr;
  const MirDropObligation *slotDrop = nullptr;
  const MirBlock *join = nullptr;
  const MirInstruction *move = nullptr;
  const MirInstruction *initialize = nullptr;
  const MirPlace *destination = nullptr;
  const MirDropObligation *destinationDrop = nullptr;
  std::vector<ConditionalClassBindingJoinArm> arms;

  [[nodiscard]] explicit operator bool() const {
    return slot != nullptr && move != nullptr && initialize != nullptr &&
           destination != nullptr;
  }
};

// A local class-valued conditional constructs one shared temporary from one
// of two failure-free constructor arms, then moves that object into one
// binding. The arm value places and drops are ownership identities only; the
// shared slot is the physical storage. The terminal reparent requires the
// backend to end the moved-from representation after constructing the binding.
[[nodiscard]] ConditionalClassBindingJoinArm
conditionalClassBindingJoinArm(const MirBody &body, MirValueId valueId) {
  const MirValue *value = body.findValue(valueId);
  const MirInstruction *construct =
      value == nullptr ? nullptr : findInstruction(body, value->definition);
  const MirDropObligation *valueDrop =
      value == nullptr ? nullptr
                       : uniqueValueDrop(body, valueId, value->info.type);
  const MirPlace *valuePlace =
      valueDrop == nullptr ? nullptr : body.findPlace(valueDrop->place);
  if (value == nullptr || value->info.type.kind != SemanticType::Class ||
      value->info.category != ValueCategory::Value ||
      value->info.traits.drop != DropKind::Lexical || construct == nullptr ||
      construct->kind != MirInstructionKind::Construct || !construct->result ||
      *construct->result != valueId || construct->destination ||
      construct->receiver || !construct->constructorTarget ||
      construct->functionTarget || construct->lambdaTarget ||
      construct->bodyTarget || construct->callableInvocation ||
      construct->intrinsic != IntrinsicKind::None ||
      construct->constructorKind != ConstructorKind::Ordinary ||
      !construct->localFailureSites.empty() ||
      !construct->definedFailure.empty() || construct->successResultDrop ||
      construct->successResultDestination || construct->lifecycle.size() != 1 ||
      !hasCompleteCallInputSchedule(body, *construct) || valueDrop == nullptr ||
      valueDrop->kind != MirDropObligationKind::Value ||
      valueDrop->initiallyActive ||
      !valueDrop->dropType.requiresActiveCleanup || valuePlace == nullptr ||
      valuePlace->root != MirPlaceRootKind::Value ||
      valuePlace->value != valueId || !valuePlace->projections.empty() ||
      valuePlace->type != value->info.type || valuePlace->initiallyAvailable) {
    return {};
  }
  const MirLifecycleEvent &activation = construct->lifecycle.front();
  if (activation.kind != MirLifecycleEventKind::Initialize ||
      activation.source != 0 || activation.target != valueDrop->id ||
      activation.conditional || activation.failureCleanup) {
    return {};
  }

  const MirValueUse *rootUse = nullptr;
  const MirValueUse *initializeUse = nullptr;
  for (const MirValueUse &use : body.usesOf(valueId)) {
    if (use.kind == MirValueUseKind::PlaceRoot && use.place == valuePlace->id &&
        rootUse == nullptr) {
      rootUse = &use;
      continue;
    }
    if (use.kind == MirValueUseKind::InstructionOperand &&
        use.operandIndex == 0 && initializeUse == nullptr) {
      initializeUse = &use;
      continue;
    }
    return {};
  }
  const MirInstruction *initialize =
      initializeUse == nullptr
          ? nullptr
          : findInstruction(body, initializeUse->instruction);
  if (rootUse == nullptr || initialize == nullptr ||
      initialize->kind != MirInstructionKind::Initialize ||
      !initialize->destination || initialize->receiver || initialize->result ||
      initialize->operands.size() != 1 ||
      initialize->operands.front().kind != MirOperandKind::Value ||
      initialize->operands.front().value != valueId ||
      initialize->operands.front().type != value->info.type ||
      !initialize->localFailureSites.empty() ||
      !initialize->definedFailure.empty() ||
      initialize->lifecycle.size() != 1) {
    return {};
  }
  const MirPlace *slot = body.findPlace(*initialize->destination);
  const MirLifecycleEvent &reparent = initialize->lifecycle.front();
  const MirDropObligation *slotDrop = body.findDropObligation(reparent.target);
  if (slot == nullptr || slot->root != MirPlaceRootKind::Temporary ||
      slot->temporary == 0 || !slot->projections.empty() ||
      slot->type != value->info.type || slot->initiallyAvailable ||
      reparent.kind != MirLifecycleEventKind::Reparent ||
      reparent.source != valueDrop->id || reparent.target == 0 ||
      reparent.conditional || reparent.failureCleanup || slotDrop == nullptr ||
      slotDrop->kind != MirDropObligationKind::Value ||
      slotDrop->place != slot->id || slotDrop->initiallyActive ||
      slotDrop->dropType.type != slot->type ||
      !slotDrop->dropType.requiresActiveCleanup) {
    return {};
  }
  const MirBlock *block = body.findBlock(value->definitionBlock);
  if (block == nullptr || initializeUse->block != block->id) {
    return {};
  }
  for (std::size_t index = 0; index + 1 < block->instructions.size(); ++index) {
    if (block->instructions[index].id == construct->id &&
        block->instructions[index + 1].id == initialize->id) {
      return ConditionalClassBindingJoinArm{.value = value,
                                            .construct = construct,
                                            .initialize = initialize,
                                            .block = block,
                                            .valuePlace = valuePlace,
                                            .valueDrop = valueDrop,
                                            .slot = slot,
                                            .slotDrop = slotDrop};
    }
  }
  return {};
}

[[nodiscard]] ConditionalClassBindingJoin
conditionalClassBindingJoin(const MirBody &body, const MirPlace &slot) {
  if (slot.root != MirPlaceRootKind::Temporary || slot.temporary == 0 ||
      !slot.projections.empty() || slot.type.kind != SemanticType::Class ||
      slot.initiallyAvailable) {
    return {};
  }

  ConditionalClassBindingJoin result;
  for (const MirValue &value : body.values) {
    ConditionalClassBindingJoinArm arm =
        conditionalClassBindingJoinArm(body, value.id);
    if (arm.slot != &slot) {
      continue;
    }
    if (result.slotDrop != nullptr && result.slotDrop != arm.slotDrop) {
      return {};
    }
    result.slotDrop = arm.slotDrop;
    result.arms.push_back(arm);
  }
  if (result.arms.size() != 2 || result.slotDrop == nullptr ||
      result.arms[0].block == result.arms[1].block ||
      result.arms[0].block->terminator.kind != MirTerminatorKind::Goto ||
      result.arms[1].block->terminator.kind != MirTerminatorKind::Goto ||
      result.arms[0].block->terminator.target == 0 ||
      result.arms[0].block->terminator.target !=
          result.arms[1].block->terminator.target) {
    return {};
  }

  const MirBlockId joinId = result.arms[0].block->terminator.target;
  const MirBlock *branch = nullptr;
  for (const MirBlock &candidate : body.blocks) {
    const bool exactDiamond =
        candidate.terminator.kind == MirTerminatorKind::Branch &&
        ((candidate.terminator.target == result.arms[0].block->id &&
          candidate.terminator.elseTarget == result.arms[1].block->id) ||
         (candidate.terminator.target == result.arms[1].block->id &&
          candidate.terminator.elseTarget == result.arms[0].block->id));
    if (exactDiamond) {
      if (branch != nullptr) {
        return {};
      }
      branch = &candidate;
    }
  }
  const MirBlock *join = body.findBlock(joinId);
  if (branch == nullptr || join == nullptr) {
    return {};
  }
  std::size_t joinPredecessors = 0;
  for (const MirBlock &candidate : body.blocks) {
    bool reachesJoin = candidate.terminator.target == joinId ||
                       candidate.terminator.elseTarget == joinId;
    reachesJoin =
        reachesJoin || std::any_of(candidate.terminator.switchTargets.begin(),
                                   candidate.terminator.switchTargets.end(),
                                   [&](const MirSwitchTarget &target) {
                                     return target.target == joinId;
                                   });
    if (!reachesJoin) {
      continue;
    }
    if (candidate.id != result.arms[0].block->id &&
        candidate.id != result.arms[1].block->id) {
      return {};
    }
    ++joinPredecessors;
  }
  if (joinPredecessors != 2) {
    return {};
  }

  for (std::size_t index = 0; index + 1 < join->instructions.size(); ++index) {
    const MirInstruction &move = join->instructions[index];
    const MirInstruction &initialize = join->instructions[index + 1];
    if (move.kind != MirInstructionKind::Move || !move.result ||
        move.destination || move.receiver || move.operands.size() != 1 ||
        move.operands.front().kind != MirOperandKind::Move ||
        move.operands.front().place != slot.id ||
        move.operands.front().type != slot.type ||
        move.info.type != slot.type || !move.localFailureSites.empty() ||
        !move.definedFailure.empty() || !move.lifecycle.empty() ||
        initialize.kind != MirInstructionKind::Initialize ||
        !initialize.destination || initialize.receiver || initialize.result ||
        initialize.operands.size() != 1 ||
        initialize.operands.front().kind != MirOperandKind::Value ||
        initialize.operands.front().value != *move.result ||
        initialize.operands.front().type != slot.type ||
        !initialize.localFailureSites.empty() ||
        !initialize.definedFailure.empty() ||
        initialize.lifecycle.size() != 1) {
      continue;
    }
    const MirPlace *destination = body.findPlace(*initialize.destination);
    const MirLifecycleEvent &reparent = initialize.lifecycle.front();
    const MirDropObligation *destinationDrop =
        body.findDropObligation(reparent.target);
    if (destination == nullptr ||
        destination->root != MirPlaceRootKind::Binding ||
        destination->binding == 0 || !destination->projections.empty() ||
        destination->type != slot.type || destination->initiallyAvailable ||
        destinationDrop == nullptr ||
        destinationDrop->kind != MirDropObligationKind::Binding ||
        destinationDrop->place != destination->id ||
        destinationDrop->initiallyActive ||
        destinationDrop->dropType.type != destination->type ||
        reparent.kind != MirLifecycleEventKind::Reparent ||
        reparent.source != result.slotDrop->id ||
        reparent.target != destinationDrop->id || reparent.conditional ||
        reparent.failureCleanup) {
      return {};
    }
    const std::vector<MirValueUse> uses = body.usesOf(*move.result);
    if (uses.size() != 1 ||
        uses.front().kind != MirValueUseKind::InstructionOperand ||
        uses.front().instruction != initialize.id ||
        uses.front().operandIndex != 0) {
      return {};
    }
    if (result.move != nullptr) {
      return {};
    }
    result.join = join;
    result.move = &move;
    result.initialize = &initialize;
    result.destination = destination;
    result.destinationDrop = destinationDrop;
  }
  result.slot = &slot;
  if (!result) {
    return {};
  }

  std::size_t armReparents = 0;
  std::size_t terminalReparents = 0;
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      const bool expectedArm =
          std::any_of(result.arms.begin(), result.arms.end(),
                      [&](const ConditionalClassBindingJoinArm &arm) {
                        return arm.initialize == &instruction;
                      });
      if (instruction.destination == slot.id && !expectedArm) {
        return {};
      }
      const bool readsSlot = std::any_of(
          instruction.operands.begin(), instruction.operands.end(),
          [&](const MirOperand &operand) { return operand.place == slot.id; });
      if (readsSlot && &instruction != result.move) {
        return {};
      }
      for (const MirLifecycleEvent &event : instruction.lifecycle) {
        if (event.source != result.slotDrop->id &&
            event.target != result.slotDrop->id) {
          continue;
        }
        if (expectedArm && event.kind == MirLifecycleEventKind::Reparent &&
            event.target == result.slotDrop->id && !event.conditional &&
            !event.failureCleanup) {
          ++armReparents;
          continue;
        }
        if (&instruction == result.initialize &&
            event.kind == MirLifecycleEventKind::Reparent &&
            event.source == result.slotDrop->id &&
            event.target == result.destinationDrop->id && !event.conditional &&
            !event.failureCleanup) {
          ++terminalReparents;
          continue;
        }
        return {};
      }
    }
  }
  if (armReparents != 2 || terminalReparents != 1) {
    return {};
  }
  return result;
}

[[nodiscard]] const MirPlace *
conditionalClassBindingJoinSlotForValue(const MirBody &body,
                                        MirValueId valueId) {
  const ConditionalClassBindingJoinArm arm =
      conditionalClassBindingJoinArm(body, valueId);
  if (arm.slot == nullptr) {
    return nullptr;
  }
  const ConditionalClassBindingJoin join =
      conditionalClassBindingJoin(body, *arm.slot);
  return join && std::any_of(
                     join.arms.begin(), join.arms.end(),
                     [&](const ConditionalClassBindingJoinArm &candidate) {
                       return candidate.value == arm.value;
                     })
             ? join.slot
             : nullptr;
}

[[nodiscard]] ConditionalClassBindingJoin
conditionalClassBindingJoinForInitialize(const MirBody &body,
                                         const MirInstruction &instruction) {
  for (const MirPlace &place : body.places) {
    const ConditionalClassBindingJoin join =
        conditionalClassBindingJoin(body, place);
    if (join && join.initialize == &instruction) {
      return join;
    }
  }
  return {};
}

// A class result of a receiver-only, operand-free, terminally contained
// member callee whose single consumer is a later call in the same block
// spells inline at that consuming argument — the emitted consumer
// expression is then textually identical to the compatibility call
// site's nested expression, so evaluation order matches by construction.
// The window between producer and consumer must be reorder-safe: only
// read-only call-input staging, computes, loads, and sibling producers
// of the same consumer may intervene, and nothing may write a place.
[[nodiscard]] const MirInstruction *
inlineNestedCallResult(const MirProgram &program,
                       const CppMirBodyEmissionMap &representations,
                       const MirBody &body, MirValueId valueId) {
  const MirValue *value = body.findValue(valueId);
  if (value == nullptr || value->info.type.kind != SemanticType::Class) {
    return nullptr;
  }
  const MirInstruction *definition = findInstruction(body, value->definition);
  if (definition == nullptr || definition->kind != MirInstructionKind::Call ||
      !definition->functionTarget || !definition->receiver ||
      !definition->operands.empty() || !definition->result ||
      *definition->result != valueId ||
      !terminallyContainedMemberCallee(program, representations, *definition)) {
    return nullptr;
  }
  const std::vector<MirValueUse> uses = nonRootRecordUses(body, valueId);
  if (uses.size() != 1 ||
      uses.front().kind != MirValueUseKind::InstructionOperand) {
    return nullptr;
  }
  const MirInstruction *consumer =
      findInstruction(body, uses.front().instruction);
  if (consumer == nullptr || consumer->kind != MirInstructionKind::Call) {
    return nullptr;
  }
  for (const MirBlock &block : body.blocks) {
    bool sawDefinition = false;
    for (const MirInstruction &instruction : block.instructions) {
      if (instruction.id == definition->id) {
        sawDefinition = true;
        continue;
      }
      if (!sawDefinition) {
        continue;
      }
      if (instruction.id == consumer->id) {
        return definition;
      }
      if (instruction.destination) {
        return nullptr;
      }
      for (const MirOperand &operand : instruction.operands) {
        if (operand.kind == MirOperandKind::BorrowWrite ||
            operand.kind == MirOperandKind::Move) {
          return nullptr;
        }
      }
      if (instruction.kind == MirInstructionKind::CallInput ||
          instruction.kind == MirInstructionKind::Compute ||
          instruction.kind == MirInstructionKind::Load) {
        continue;
      }
      if (instruction.kind == MirInstructionKind::Call &&
          instruction.functionTarget && instruction.receiver &&
          instruction.operands.empty() &&
          terminallyContainedMemberCallee(program, representations,
                                          instruction)) {
        continue;
      }
      return nullptr;
    }
    if (sawDefinition) {
      // The block ended without reaching the consumer.
      return nullptr;
    }
  }
  return nullptr;
}

// A Move-defined value whose single record is one Call operand in the
// same block declares at its move — `auto v = std::move(<place>);` — and
// the consuming argument spells std::move(v). The sequenced local keeps
// MIR's move-before-call order (an inline std::move(<place>) in the
// argument list would race sibling arguments under C++'s unspecified
// evaluation order), and auto needs no representation row. Nothing
// between the move and the call may touch the source place.
[[nodiscard]] const MirInstruction *sequencedMovedArgument(const MirBody &body,
                                                           MirValueId valueId) {
  const MirValue *value = body.findValue(valueId);
  const MirInstruction *definition =
      value == nullptr ? nullptr : findInstruction(body, value->definition);
  if (definition == nullptr || definition->kind != MirInstructionKind::Move ||
      definition->operands.size() != 1 ||
      definition->operands.front().place == 0) {
    return nullptr;
  }
  const std::vector<MirValueUse> uses = nonRootRecordUses(body, valueId);
  if (uses.size() != 1 ||
      uses.front().kind != MirValueUseKind::InstructionOperand) {
    return nullptr;
  }
  const MirInstruction *consumer =
      findInstruction(body, uses.front().instruction);
  if (consumer == nullptr || consumer->kind != MirInstructionKind::Call) {
    return nullptr;
  }
  const MirPlaceId movedPlace = definition->operands.front().place;
  for (const MirBlock &block : body.blocks) {
    bool sawDefinition = false;
    for (const MirInstruction &instruction : block.instructions) {
      if (instruction.id == definition->id) {
        sawDefinition = true;
        continue;
      }
      if (!sawDefinition) {
        continue;
      }
      if (instruction.id == consumer->id) {
        return definition;
      }
      switch (instruction.kind) {
      case MirInstructionKind::Move:
      case MirInstructionKind::CallInput:
      case MirInstructionKind::Compute:
      case MirInstructionKind::Load:
      case MirInstructionKind::Lifecycle:
        break;
      default:
        return nullptr;
      }
      if (instruction.destination == movedPlace ||
          (instruction.receiver && instruction.receiver->place == movedPlace) ||
          std::any_of(instruction.operands.begin(), instruction.operands.end(),
                      [&](const MirOperand &operand) {
                        return operand.place == movedPlace;
                      })) {
        return nullptr;
      }
    }
    if (sawDefinition) {
      return nullptr;
    }
  }
  return nullptr;
}

// The terminal consumer of a moved-value staging chain: each link is a
// destination-less single-consumer CallInput, and the walk ends at the
// first non-link instruction.
struct MovedChainTerminal {
  MirValueId top = 0;
  const MirInstruction *consumer = nullptr;
};
[[nodiscard]] MovedChainTerminal movedChainTerminal(const MirBody &body,
                                                    MirValueId id) {
  MirValueId current = id;
  for (std::size_t depth = 0; depth < 5; ++depth) {
    const std::vector<MirValueUse> uses = nonRootRecordUses(body, current);
    if (uses.size() != 1) {
      return {};
    }
    const MirInstruction *user =
        findInstruction(body, uses.front().instruction);
    if (user == nullptr) {
      return {};
    }
    if (user->kind == MirInstructionKind::CallInput && !user->destination &&
        !user->receiver && user->result) {
      current = *user->result;
      continue;
    }
    return {current, user};
  }
  return {};
}

// A moved place flowing through plain value staging into one consumer in
// the move's own block, with nothing between the move and the consumer
// touching the source place (drops of the moved-from source are
// representation no-ops): the chain spells std::move over the place at
// the consumer's argument position and no link materializes a local.
[[nodiscard]] const MirPlace *
movedPlaceChainSource(const MirBody &body, MirValueId id,
                      const MirInstruction &consumer) {
  MirValueId current = id;
  const MirInstruction *move = nullptr;
  for (std::size_t depth = 0; depth < 5; ++depth) {
    if (nonRootRecordUses(body, current).size() != 1) {
      return nullptr;
    }
    const MirValue *value = body.findValue(current);
    const MirInstruction *definition =
        value == nullptr ? nullptr : findInstruction(body, value->definition);
    if (definition == nullptr) {
      return nullptr;
    }
    if (definition->kind == MirInstructionKind::Move &&
        definition->operands.size() == 1 &&
        definition->operands.front().kind == MirOperandKind::Move &&
        definition->operands.front().place != 0 &&
        definition->localFailureSites.empty() &&
        definition->definedFailure.empty()) {
      move = definition;
      break;
    }
    if (definition->kind == MirInstructionKind::CallInput &&
        !definition->destination && !definition->receiver &&
        definition->operands.size() == 1 &&
        definition->operands.front().kind == MirOperandKind::Value) {
      current = definition->operands.front().value;
      continue;
    }
    return nullptr;
  }
  if (move == nullptr) {
    return nullptr;
  }
  const MirPlaceId source = move->operands.front().place;
  for (const MirBlock &block : body.blocks) {
    bool sawMove = false;
    for (const MirInstruction &member : block.instructions) {
      if (member.id == move->id) {
        sawMove = true;
        continue;
      }
      if (!sawMove) {
        continue;
      }
      if (member.id == consumer.id) {
        return body.findPlace(source);
      }
      if (member.kind == MirInstructionKind::Drop) {
        continue;
      }
      if ((member.destination && *member.destination == source) ||
          (member.receiver && member.receiver->place == source) ||
          std::any_of(member.operands.begin(), member.operands.end(),
                      [&](const MirOperand &operand) {
                        return operand.place == source;
                      })) {
        return nullptr;
      }
    }
    if (sawMove) {
      return nullptr;
    }
  }
  return nullptr;
}

// The source feeding a value-staged temporary: a Copy operand names its
// place directly, and a Value operand fed by a Move of a place names the
// moved source (spelled std::move(source) at the consuming call).
struct StagedTemporarySource {
  const MirPlace *place = nullptr;
  bool moved = false;
};
[[nodiscard]] StagedTemporarySource
stagedTemporarySourceFor(const MirBody &body, const MirInstruction &stage) {
  const MirOperand &operand = stage.operands.front();
  if (operand.kind == MirOperandKind::Copy && operand.place != 0) {
    return {body.findPlace(operand.place), false};
  }
  if (operand.kind == MirOperandKind::Value && operand.value != 0 &&
      nonRootRecordUses(body, operand.value).size() == 1) {
    const MirValue *value = body.findValue(operand.value);
    const MirInstruction *definition =
        value == nullptr ? nullptr : findInstruction(body, value->definition);
    if (definition != nullptr && definition->kind == MirInstructionKind::Move &&
        definition->operands.size() == 1 &&
        definition->operands.front().kind == MirOperandKind::Move &&
        definition->operands.front().place != 0 &&
        definition->localFailureSites.empty() &&
        definition->definedFailure.empty()) {
      return {body.findPlace(definition->operands.front().place), true};
    }
  }
  return {};
}

// A by-value argument staging temporary: one CallInput carries a source
// place — copied, or moved through its staged value — into a bare
// class-typed temporary that nothing else references, and the staged
// value feeds exactly one call. The consuming call spells the source
// place (moved sources under std::move) and C++ materializes the
// temporary at the call boundary, exactly like the compatibility call.
[[nodiscard]] const MirInstruction *
copyStageForTemporary(const MirBody &body, const MirPlace &place) {
  if (place.root != MirPlaceRootKind::Temporary || !place.projections.empty() ||
      place.type.kind != SemanticType::Class) {
    return nullptr;
  }
  const MirInstruction *stage = nullptr;
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      const bool references =
          instruction.destination == place.id ||
          (instruction.receiver && instruction.receiver->place == place.id) ||
          std::any_of(instruction.operands.begin(), instruction.operands.end(),
                      [&](const MirOperand &operand) {
                        return operand.place == place.id;
                      });
      if (!references) {
        continue;
      }
      if (stage != nullptr ||
          instruction.kind != MirInstructionKind::CallInput ||
          instruction.destination != place.id || !instruction.result ||
          instruction.receiver || instruction.operands.size() != 1 ||
          stagedTemporarySourceFor(body, instruction).place == nullptr) {
        return nullptr;
      }
      stage = &instruction;
    }
  }
  if (stage == nullptr || body.usesOf(*stage->result).size() != 1 ||
      (body.usesOf(*stage->result).front().kind !=
           MirValueUseKind::InstructionOperand &&
       body.usesOf(*stage->result).front().kind !=
           MirValueUseKind::InstructionReceiver)) {
    return nullptr;
  }
  const MirInstruction *user =
      findInstruction(body, body.usesOf(*stage->result).front().instruction);
  if (user == nullptr || user->kind != MirInstructionKind::Call) {
    return nullptr;
  }
  return stage;
}

// The value-side view of the same shape, keyed by the staged value.
[[nodiscard]] const MirInstruction *copyStagedCallInput(const MirBody &body,
                                                        MirValueId id) {
  const MirValue *value = body.findValue(id);
  const MirInstruction *definition =
      value == nullptr ? nullptr : findInstruction(body, value->definition);
  if (definition == nullptr ||
      definition->kind != MirInstructionKind::CallInput ||
      !definition->destination) {
    return nullptr;
  }
  const MirPlace *destination = body.findPlace(*definition->destination);
  if (destination == nullptr ||
      copyStageForTemporary(body, *destination) != definition) {
    return nullptr;
  }
  return definition;
}

[[nodiscard]] const MirInstruction *closureChainDefinition(const MirBody &body,
                                                           MirValueId id) {
  const MirValue *value = body.findValue(id);
  const MirInstruction *definition =
      value == nullptr ? nullptr : findInstruction(body, value->definition);
  if (definition == nullptr) {
    return nullptr;
  }
  if (definition->kind == MirInstructionKind::Compute &&
      definition->operation == MirOperation::Closure) {
    return definition;
  }
  if (definition->kind != MirInstructionKind::Load ||
      definition->operands.size() != 1) {
    return nullptr;
  }
  const MirPlaceId carrier = definition->operands.front().place;
  const MirPlace *place = body.findPlace(carrier);
  if (place == nullptr || place->type.kind != SemanticType::Lambda) {
    return nullptr;
  }
  const MirInstruction *initialize = nullptr;
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &candidate : block.instructions) {
      if (candidate.kind == MirInstructionKind::Initialize &&
          candidate.destination && *candidate.destination == carrier) {
        if (initialize != nullptr) {
          return nullptr;
        }
        initialize = &candidate;
      }
    }
  }
  if (initialize == nullptr || initialize->operands.size() != 1 ||
      initialize->operands.front().kind != MirOperandKind::Value) {
    return nullptr;
  }
  return closureChainDefinition(body, initialize->operands.front().value);
}

// A Drop of a unique-owner place whose value an earlier Move in the
// same block unconditionally consumed: the C++ local is moved-from, its
// representation's scope-end destruction is a no-op by construction
// (a null owner deletes nothing), so the Drop spells as a comment.
[[nodiscard]] bool movedOutOwnerDrop(const MirBody &body,
                                     const MirInstruction &instruction) {
  if (instruction.kind != MirInstructionKind::Drop ||
      !instruction.destination) {
    return false;
  }
  const MirPlace *destination = body.findPlace(*instruction.destination);
  if (destination == nullptr ||
      destination->type.kind != SemanticType::UniqueOwner ||
      destination->root != MirPlaceRootKind::Binding ||
      !destination->projections.empty()) {
    return false;
  }
  for (const MirLifecycleEvent &event : instruction.lifecycle) {
    if (event.conditional) {
      return false;
    }
  }
  const MirBlock *block = nullptr;
  for (const MirBlock &candidate : body.blocks) {
    for (const MirInstruction &member : candidate.instructions) {
      if (member.id == instruction.id) {
        block = &candidate;
      }
    }
  }
  if (block == nullptr) {
    return false;
  }
  bool moved = false;
  for (const MirInstruction &candidate : block->instructions) {
    if (candidate.id == instruction.id) {
      break;
    }
    if (candidate.kind == MirInstructionKind::Move &&
        candidate.operands.size() == 1 &&
        candidate.operands.front().kind == MirOperandKind::Move &&
        candidate.operands.front().place == destination->id) {
      moved = true;
    }
  }
  return moved;
}

// A storage growth step moves the replacement value out of its staging
// local and stores it into the receiver field; the store is the value's
// single consuming use and spells as a C++ move-assignment, so the Drop
// of the moved-from value afterwards is a no-op by representation and
// spells as a comment. The proof demands the exact shape: the dropped
// place is a projection-free Value-rooted storage place, every lifecycle
// event on the Drop is unconditional non-failure cleanup, the value is
// read exactly once in the whole body — by an Initialize or Assign that
// precedes the Drop in its own block — and no other place roots at it.
[[nodiscard]] bool
storeConsumedStorageValueDrop(const MirBody &body,
                              const MirInstruction &instruction) {
  if (instruction.kind != MirInstructionKind::Drop ||
      !instruction.destination) {
    return false;
  }
  const MirPlace *destination = body.findPlace(*instruction.destination);
  if (destination == nullptr ||
      (destination->type.kind != SemanticType::Storage &&
       destination->type.kind != SemanticType::PrefixStorage) ||
      destination->root != MirPlaceRootKind::Value ||
      !destination->projections.empty()) {
    return false;
  }
  for (const MirLifecycleEvent &event : instruction.lifecycle) {
    if (event.conditional || event.failureCleanup) {
      return false;
    }
  }
  for (const MirPlace &place : body.places) {
    if (place.id != destination->id && place.root == MirPlaceRootKind::Value &&
        place.value == destination->value) {
      return false;
    }
  }
  const MirInstruction *consumer = nullptr;
  std::size_t reads = 0;
  for (const MirBlock &candidate : body.blocks) {
    for (const MirInstruction &member : candidate.instructions) {
      for (const MirOperand &operand : member.operands) {
        if (operand.kind == MirOperandKind::Value &&
            operand.value == destination->value) {
          ++reads;
          consumer = &member;
        }
      }
    }
    if (candidate.terminator.value &&
        candidate.terminator.value->kind == MirOperandKind::Value &&
        candidate.terminator.value->value == destination->value) {
      return false;
    }
  }
  if (reads != 1 || consumer == nullptr ||
      (consumer->kind != MirInstructionKind::Initialize &&
       consumer->kind != MirInstructionKind::Assign) ||
      !consumer->destination) {
    return false;
  }
  // The consuming store precedes the Drop inside the Drop's own block.
  bool sawConsumer = false;
  for (const MirBlock &candidate : body.blocks) {
    for (const MirInstruction &member : candidate.instructions) {
      if (member.id == consumer->id) {
        sawConsumer = true;
      }
      if (member.id == instruction.id) {
        return sawConsumer;
      }
    }
    sawConsumer = false;
  }
  return false;
}

// A Drop is trivial when every drop obligation governing it — the ones
// its lifecycle events name and the ones anchored on its destination
// place — carries neither a destructor nor active cleanup: C++ scope-end
// destruction of the declared local (or nothing, for a fused closure) is
// exactly the verified semantics, so the Drop spells as a comment. An
// unresolvable obligation fails closed.
[[nodiscard]] bool trivialMirDrop(const MirBody &body,
                                  const MirInstruction &instruction,
                                  bool usesLifetimeSlot) {
  const MirPlace *destination = instruction.destination
                                    ? body.findPlace(*instruction.destination)
                                    : nullptr;
  if (destination == nullptr) {
    return false;
  }
  // A slot-shaped place stays on the lifetime-slot protocol regardless of
  // obligation triviality: the slot's engage/destroy pairing is the verified
  // escape check. The caller supplies the shared lifetime-slot proof so this
  // rule cannot drift as new storage families are admitted.
  if (usesLifetimeSlot) {
    return false;
  }
  const auto trivialObligation = [&](MirDropObligationId id) {
    if (id == 0) {
      return true;
    }
    for (const MirDropObligation &obligation : body.dropObligations) {
      if (obligation.id == id) {
        return !obligation.dropType.destructor &&
               !obligation.dropType.requiresActiveCleanup;
      }
    }
    return false;
  };
  bool governed = false;
  for (const MirLifecycleEvent &event : instruction.lifecycle) {
    governed = governed || event.source != 0 || event.target != 0;
    if (!trivialObligation(event.source) || !trivialObligation(event.target)) {
      return false;
    }
  }
  for (const MirDropObligation &obligation : body.dropObligations) {
    if (obligation.place != *instruction.destination) {
      continue;
    }
    governed = true;
    if (obligation.dropType.destructor ||
        obligation.dropType.requiresActiveCleanup) {
      return false;
    }
  }
  return governed;
}

// Plain-shape checked arithmetic: the compatibility path's terminal helper
// family checks and contains the defined failure itself and never returns
// on failure. Inline lambda literals keep exactly that spelling, so their
// MIR failure edges are unreachable in emitted text.
// The closed narrowing compound assignment keeps its fused origin by
// design (docs/architecture/mir.md): semantics folds the arithmetic and
// the checked conversion into one HIR-authored origin, and the terminal
// compatibility helper spells arithmetic, conversion, and write in one
// call that contains the failure at the site.
std::string_view cppMirCompoundAssignHelperSpelling(MirOperation operation) {
  switch (operation) {
  case MirOperation::AddAssign:
    return "::gti_internal::backend::add_assign";
  case MirOperation::SubtractAssign:
    return "::gti_internal::backend::subtract_assign";
  case MirOperation::MultiplyAssign:
    return "::gti_internal::backend::multiply_assign";
  case MirOperation::DivideAssign:
    return "::gti_internal::backend::divide_assign";
  case MirOperation::RemainderAssign:
    return "::gti_internal::backend::remainder_assign";
  case MirOperation::BitwiseAndAssign:
    return "::gti_internal::backend::bitwise_and_assign";
  case MirOperation::BitwiseOrAssign:
    return "::gti_internal::backend::bitwise_or_assign";
  case MirOperation::BitwiseXorAssign:
    return "::gti_internal::backend::bitwise_xor_assign";
  case MirOperation::ShiftLeftAssign:
    return "::gti_internal::backend::shift_left_assign";
  case MirOperation::ShiftRightAssign:
    return "::gti_internal::backend::shift_right_assign";
  default:
    return {};
  }
}

[[nodiscard]] std::string_view
cppMirTerminalCheckedHelperSpelling(MirOperation operation) {
  switch (operation) {
  case MirOperation::Add:
    return "::gti_internal::backend::add";
  case MirOperation::Subtract:
    return "::gti_internal::backend::subtract";
  case MirOperation::Multiply:
    return "::gti_internal::backend::multiply";
  case MirOperation::Divide:
    return "::gti_internal::backend::divide";
  case MirOperation::Remainder:
    return "::gti_internal::backend::modulo";
  case MirOperation::Negate:
    return "::gti_internal::backend::negate";
  default:
    return {};
  }
}

// Forward half of one Closure's fused chain. Captured places must stay
// frozen after the Closure: their only writes are entry-block Initializes
// that precede it, nothing loans or drops them, and the entry block is
// never re-entered, so a literal spelled at any later invocation captures
// the same values the Closure saw. Move captures collapse to exactly one
// direct same-block invocation because a duplicated or delayed literal
// would move a captured place twice or after an interleaved failure edge.
[[nodiscard]] bool closureChainAdmits(const MirProgram &program,
                                      const MirBody &body,
                                      const MirInstruction &closure) {
  const MirLambdaInstance *lambda =
      closure.lambdaTarget ? program.findLambda(*closure.lambdaTarget)
                           : nullptr;
  const MirValue *result =
      closure.result ? body.findValue(*closure.result) : nullptr;
  if (lambda == nullptr || result == nullptr ||
      closure.operands.size() != lambda->captureSymbols.size() ||
      closure.operands.size() != lambda->captureModes.size() ||
      closure.operands.size() != lambda->captureTypes.size()) {
    return false;
  }
  bool movesCapture = false;
  for (std::size_t index = 0; index < closure.operands.size(); ++index) {
    const MirOperand &operand = closure.operands[index];
    const LambdaCaptureMode mode = lambda->captureModes[index];
    const bool modeMatches = (mode == LambdaCaptureMode::Copy &&
                              operand.kind == MirOperandKind::Copy) ||
                             (mode == LambdaCaptureMode::Move &&
                              operand.kind == MirOperandKind::Move);
    movesCapture = movesCapture || mode == LambdaCaptureMode::Move;
    const MirPlace *captured =
        operand.place == 0 ? nullptr : body.findPlace(operand.place);
    // A captured lambda or owned object would itself need the unnameable
    // or slot-managed local this chain exists to avoid; both decline.
    if (!modeMatches || captured == nullptr ||
        captured->root != MirPlaceRootKind::Binding ||
        !captured->projections.empty() ||
        captured->type.kind == SemanticType::Lambda ||
        captured->type.kind == SemanticType::Class ||
        captured->type.kind == SemanticType::Storage ||
        captured->type.kind == SemanticType::PrefixStorage) {
      return false;
    }
    for (const MirLoan &loan : body.loans) {
      if (loan.source == captured->id) {
        return false;
      }
    }
    for (const MirBlock &block : body.blocks) {
      for (const MirInstruction &writer : block.instructions) {
        if (!writer.destination || *writer.destination != captured->id) {
          continue;
        }
        if (writer.kind != MirInstructionKind::Initialize ||
            block.id != body.entry || result->definitionBlock != body.entry) {
          return false;
        }
        bool writerPrecedes = false;
        for (const MirInstruction &ordered : block.instructions) {
          if (ordered.id == writer.id) {
            writerPrecedes = true;
            break;
          }
          if (ordered.id == closure.id) {
            break;
          }
        }
        if (!writerPrecedes) {
          return false;
        }
      }
    }
  }
  if (!closure.operands.empty()) {
    for (const MirBlock &block : body.blocks) {
      const MirTerminator &terminator = block.terminator;
      if (terminator.target == body.entry ||
          terminator.elseTarget == body.entry) {
        return false;
      }
      for (const MirSwitchTarget &target : terminator.switchTargets) {
        if (target.target == body.entry) {
          return false;
        }
      }
    }
  }
  // Forward walk: every transitive consumer is an Initialize into a fresh
  // single-write lambda local or the receiver of an invocation.
  std::size_t invocations = 0;
  bool directOnly = true;
  std::vector<MirPlaceId> visitedCarriers;
  std::vector<MirValueId> pending{*closure.result};
  while (!pending.empty()) {
    const MirValueId current = pending.back();
    pending.pop_back();
    for (const MirValueUse &use : body.usesOf(current)) {
      const MirInstruction *user = findInstruction(body, use.instruction);
      if (use.kind == MirValueUseKind::InstructionReceiver) {
        if (user == nullptr || !callableValueInvocation(*user)) {
          return false;
        }
        ++invocations;
        directOnly = directOnly && current == *closure.result &&
                     use.block == result->definitionBlock;
        continue;
      }
      if (use.kind == MirValueUseKind::InstructionOperand && user != nullptr &&
          deducedCallableCallee(program, *user)) {
        // The literal spells inline as the template call's deduced
        // callable argument, exactly like the compatibility call.
        ++invocations;
        directOnly = directOnly && current == *closure.result &&
                     use.block == result->definitionBlock;
        continue;
      }
      if (use.kind != MirValueUseKind::InstructionOperand || user == nullptr ||
          user->kind != MirInstructionKind::Initialize || !user->destination) {
        return false;
      }
      directOnly = false;
      const MirPlaceId carrier = *user->destination;
      const MirPlace *place = body.findPlace(carrier);
      if (place == nullptr || place->root != MirPlaceRootKind::Binding ||
          !place->projections.empty() ||
          place->type.kind != SemanticType::Lambda ||
          std::find(visitedCarriers.begin(), visitedCarriers.end(), carrier) !=
              visitedCarriers.end()) {
        return false;
      }
      visitedCarriers.push_back(carrier);
      for (const MirLoan &loan : body.loans) {
        if (loan.source == carrier) {
          return false;
        }
      }
      // The carrier's whole life is this one Initialize plus loads whose
      // results rejoin the chain.
      for (const MirBlock &block : body.blocks) {
        for (const MirInstruction &reference : block.instructions) {
          if (reference.destination && *reference.destination == carrier &&
              reference.id != user->id) {
            return false;
          }
          bool readsCarrier =
              reference.receiver && reference.receiver->place == carrier;
          for (const MirOperand &operand : reference.operands) {
            readsCarrier = readsCarrier || operand.place == carrier;
          }
          if (!readsCarrier || reference.id == user->id) {
            continue;
          }
          if (reference.kind != MirInstructionKind::Load || !reference.result) {
            return false;
          }
          pending.push_back(*reference.result);
        }
      }
    }
  }
  if (movesCapture && (invocations != 1 || !directOnly)) {
    return false;
  }
  return true;
}

// A lambda SSA value whose only executable consumer reparents it into one
// bare lambda binding can publish directly into that binding's lifetime slot.
// This is the lambda analogue of classValueDestinationSlot(), but it stays
// separate because a concrete C++ closure type is normally unnameable.
enum class LambdaValuePublication {
  None,
  Passive,
  Initialize,
  Reparent,
};

struct LambdaValueDestination {
  const MirPlace *slot = nullptr;
  LambdaValuePublication publication = LambdaValuePublication::None;
};

[[nodiscard]] LambdaValueDestination
lambdaValueDestination(const MirBody &body, MirValueId valueId) {
  const MirValue *value = body.findValue(valueId);
  const MirInstruction *definition =
      value == nullptr ? nullptr : findInstruction(body, value->definition);
  const bool directFunctionCall =
      definition != nullptr && definition->kind == MirInstructionKind::Call &&
      definition->result && *definition->result == valueId &&
      definition->functionTarget && !definition->constructorTarget &&
      !definition->lambdaTarget && !definition->bodyTarget &&
      !definition->callableInvocation &&
      definition->intrinsic == IntrinsicKind::None;
  const bool directCopyLoad =
      definition != nullptr && definition->kind == MirInstructionKind::Load &&
      definition->result && *definition->result == valueId &&
      definition->operands.size() == 1 &&
      definition->operands.front().kind == MirOperandKind::Copy &&
      definition->operands.front().place != 0;
  if (value == nullptr || value->info.type.kind != SemanticType::Lambda ||
      definition == nullptr || !definition->result ||
      *definition->result != valueId ||
      !((definition->kind == MirInstructionKind::Compute &&
         definition->operation == MirOperation::Closure) ||
        (definition->kind == MirInstructionKind::Move &&
         definition->operands.size() == 1 &&
         definition->operands.front().kind == MirOperandKind::Move &&
         definition->operands.front().place != 0) ||
        directCopyLoad || directFunctionCall)) {
    return {};
  }
  const std::vector<MirValueUse> uses = nonRootRecordUses(body, valueId);
  if (uses.size() != 1 ||
      uses.front().kind != MirValueUseKind::InstructionOperand ||
      uses.front().operandIndex != 0) {
    return {};
  }
  const MirInstruction *initialize =
      findInstruction(body, uses.front().instruction);
  if (initialize == nullptr ||
      initialize->kind != MirInstructionKind::Initialize ||
      !initialize->destination || initialize->result || initialize->receiver ||
      initialize->operands.size() != 1 ||
      initialize->operands.front().kind != MirOperandKind::Value ||
      initialize->operands.front().value != valueId ||
      initialize->operands.front().type != value->info.type) {
    return {};
  }
  if (definition->successResultDestination &&
      definition->successResultDestination != initialize->destination) {
    return {};
  }
  const MirPlace *destination = body.findPlace(*initialize->destination);
  if (destination == nullptr ||
      destination->root != MirPlaceRootKind::Binding ||
      !destination->projections.empty() ||
      destination->type != value->info.type) {
    return {};
  }
  const MirLifecycleEvent *publication = initialize->lifecycle.size() == 1
                                             ? &initialize->lifecycle.front()
                                             : nullptr;
  const bool exactLexicalPublication =
      destination->traits.drop == DropKind::Lexical && publication != nullptr &&
      publication->target != 0 && !publication->conditional &&
      !publication->failureCleanup;
  const bool lexicalInitialize =
      exactLexicalPublication &&
      publication->kind == MirLifecycleEventKind::Initialize &&
      publication->source == 0;
  const bool lexicalReparent =
      exactLexicalPublication &&
      publication->kind == MirLifecycleEventKind::Reparent &&
      publication->source != 0;
  const bool passivePublication =
      destination->traits.drop == DropKind::Trivial &&
      value->info.traits.drop == DropKind::Trivial &&
      initialize->lifecycle.empty();
  if (lexicalInitialize) {
    return {.slot = destination,
            .publication = LambdaValuePublication::Initialize};
  }
  if (lexicalReparent) {
    return {.slot = destination,
            .publication = LambdaValuePublication::Reparent};
  }
  if (passivePublication) {
    return {.slot = destination,
            .publication = LambdaValuePublication::Passive};
  }
  return {};
}

[[nodiscard]] const MirPlace *lambdaValueDestinationSlot(const MirBody &body,
                                                         MirValueId valueId) {
  return lambdaValueDestination(body, valueId).slot;
}

struct MaterializedClosureCapture {
  const MirPlace *source = nullptr;
  LambdaCaptureMode mode = LambdaCaptureMode::Copy;
};

struct MaterializedClosure {
  const MirInstruction *closure = nullptr;
  const MirLambdaInstance *lambda = nullptr;
  const MirPlace *destination = nullptr;
  std::vector<MaterializedClosureCapture> captures;

  [[nodiscard]] explicit operator bool() const { return closure != nullptr; }
};

// A closure requiring a real local representation. The proof is intentionally
// confined to one exact schedule: each capture is a direct passive copy or a
// no-intervening-use move chain, the Closure creates one lexical lambda
// value, and that value reparents into one bare binding. Frozen/passive
// closures keep using closureChainAdmits() and never enter this path.
[[nodiscard]] MaterializedClosure
materializedClosure(const MirProgram &program, const MirBody &body,
                    const MirInstruction &closure) {
  const MirLambdaInstance *lambda =
      closure.lambdaTarget ? program.findLambda(*closure.lambdaTarget)
                           : nullptr;
  const MirValue *result =
      closure.result ? body.findValue(*closure.result) : nullptr;
  const LambdaValueDestination publication =
      closure.result ? lambdaValueDestination(body, *closure.result)
                     : LambdaValueDestination{};
  const MirPlace *destination = publication.slot;
  if (closure.kind != MirInstructionKind::Compute ||
      closure.operation != MirOperation::Closure || lambda == nullptr ||
      result == nullptr || result->info.type != lambda->type ||
      destination == nullptr || closure.destination || closure.receiver ||
      closure.literal || !closure.localFailureSites.empty() ||
      !closure.definedFailure.empty() ||
      closure.operands.size() != lambda->captureSymbols.size() ||
      closure.operands.size() != lambda->captureModes.size() ||
      closure.operands.size() != lambda->captureTypes.size() ||
      closure.closureCaptureTypes != lambda->captureTypes ||
      closure.closureCaptureModes != lambda->captureModes) {
    return {};
  }

  std::size_t initialized = 0;
  std::size_t transferred = 0;
  for (const MirLifecycleEvent &event : closure.lifecycle) {
    if (event.conditional || event.failureCleanup) {
      return {};
    }
    if (event.kind == MirLifecycleEventKind::Initialize && event.source == 0 &&
        event.target != 0) {
      ++initialized;
      continue;
    }
    if (event.kind == MirLifecycleEventKind::TransferOut && event.source != 0 &&
        event.target == 0) {
      ++transferred;
      continue;
    }
    return {};
  }

  MaterializedClosure shape{
      .closure = &closure, .lambda = lambda, .destination = destination};
  std::size_t moveCaptures = 0;
  for (std::size_t index = 0; index < closure.operands.size(); ++index) {
    const MirOperand &operand = closure.operands[index];
    const LambdaCaptureMode mode = lambda->captureModes[index];
    const MirPlace *source = nullptr;
    if (mode == LambdaCaptureMode::Copy &&
        operand.kind == MirOperandKind::Copy && operand.place != 0) {
      source = body.findPlace(operand.place);
    } else if (mode == LambdaCaptureMode::Move) {
      ++moveCaptures;
      if (operand.kind == MirOperandKind::Move && operand.place != 0) {
        source = body.findPlace(operand.place);
      } else if (operand.kind == MirOperandKind::Value && operand.value != 0) {
        source = movedPlaceChainSource(body, operand.value, closure);
      }
    }
    if (source == nullptr || source->type != lambda->captureTypes[index] ||
        operand.type != lambda->captureTypes[index]) {
      return {};
    }
    shape.captures.push_back({.source = source, .mode = mode});
  }
  // A copy-only lexical closure can create its obligation at the destination
  // Initialize. A closure with move captures owns an obligation first and the
  // destination reparents it, so the Closure must carry one Initialize beside
  // the capture TransferOut events.
  const bool lexicalPublication =
      (publication.publication == LambdaValuePublication::Initialize &&
       moveCaptures == 0 && initialized == 0) ||
      (publication.publication == LambdaValuePublication::Reparent &&
       initialized == 1);
  const bool lexicalClosure = result->info.traits.drop == DropKind::Lexical &&
                              destination->traits.drop == DropKind::Lexical &&
                              lexicalPublication && transferred == moveCaptures;
  const bool passiveCopyClosure =
      result->info.traits.drop == DropKind::Trivial &&
      destination->traits.drop == DropKind::Trivial && moveCaptures == 0 &&
      publication.publication == LambdaValuePublication::Passive &&
      initialized == 0 && transferred == 0;
  if (!lexicalClosure && !passiveCopyClosure) {
    return {};
  }
  return shape;
}

[[nodiscard]] MaterializedClosure
materializedClosureForType(const MirProgram &program, const MirBody &body,
                           const SemanticType &type) {
  MaterializedClosure selected;
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      if (instruction.kind != MirInstructionKind::Compute ||
          instruction.operation != MirOperation::Closure ||
          !instruction.result) {
        continue;
      }
      const MirValue *result = body.findValue(*instruction.result);
      if (result == nullptr || result->info.type != type ||
          closureChainAdmits(program, body, instruction)) {
        continue;
      }
      MaterializedClosure candidate =
          materializedClosure(program, body, instruction);
      if (!candidate || selected) {
        return {};
      }
      selected = std::move(candidate);
    }
  }
  return selected;
}

[[nodiscard]] bool semanticTypeContainsLambda(const SemanticType &type) {
  return type.kind == SemanticType::Lambda ||
         std::any_of(type.arguments.begin(), type.arguments.end(),
                     semanticTypeContainsLambda);
}

[[nodiscard]] const CppMirTypeRepresentation *
representationTypeRow(const CppMirBodyEmissionMap &representations,
                      const SemanticType &type) {
  const auto found = std::find_if(
      representations.types().begin(), representations.types().end(),
      [&](const CppMirTypeRepresentation &row) { return row.type == type; });
  return found == representations.types().end() ? nullptr : &*found;
}

[[nodiscard]] const MirClassInstance *
passiveCAbiRecordInstance(const MirProgram &program, const SemanticType &type) {
  if (type.kind != SemanticType::Class) {
    return nullptr;
  }
  const MirClassInstance *selected = nullptr;
  for (const MirClassInstance &candidate : program.classInstances()) {
    if (candidate.type != type) {
      continue;
    }
    if (selected != nullptr) {
      return nullptr;
    }
    selected = &candidate;
  }
  return selected != nullptr && selected->cAbiRecord &&
                 selected->cAbiLayout.has_value() &&
                 !selected->unionLayout.has_value() &&
                 !selected->requiresActiveDropState &&
                 !selected->requiresActiveCleanup
             ? selected
             : nullptr;
}

// A class containing a closure type is nameable only by rebuilding its
// template-id from the copied primary-template name and a body-local closure
// alias (or an enclosing template overlay). The semantic argument tree, not a
// parsed C++ spelling, is the composition authority.
[[nodiscard]] bool lambdaDependentTypeRepresentable(
    const MirProgram &program, const CppMirBodyEmissionMap &representations,
    const MirBody &body, const SemanticType &type) {
  if (type.kind == SemanticType::Lambda) {
    const CppMirTypeRepresentation *row =
        representationTypeRow(representations, type);
    return (row != nullptr && !row->spelling.empty()) ||
           static_cast<bool>(materializedClosureForType(program, body, type));
  }
  if (!semanticTypeContainsLambda(type)) {
    const CppMirTypeRepresentation *row =
        representationTypeRow(representations, type);
    return row != nullptr && !row->spelling.empty();
  }
  const CppMirTypeRepresentation *row =
      representationTypeRow(representations, type);
  return type.kind == SemanticType::Class && row != nullptr &&
         row->kind == CppMirTypeRepresentationKind::Class &&
         !row->templateNameSpelling.empty() && !type.arguments.empty() &&
         type.valueArguments.empty() &&
         std::all_of(type.arguments.begin(), type.arguments.end(),
                     [&](const SemanticType &argument) {
                       return lambdaDependentTypeRepresentable(
                           program, representations, body, argument);
                     });
}

// A materialized closure invocation reads its exact lambda binding. The Load
// SSA record itself remains representation-free; the binding's lifetime slot
// carries the callable object.
[[nodiscard]] const MirPlace *
materializedCallableReceiverPlace(const MirProgram &program,
                                  const MirBody &body, MirValueId valueId) {
  const MirValue *value = body.findValue(valueId);
  const MirInstruction *load =
      value == nullptr ? nullptr : findInstruction(body, value->definition);
  if (value == nullptr || value->info.type.kind != SemanticType::Lambda ||
      load == nullptr || load->kind != MirInstructionKind::Load ||
      load->operands.size() != 1 || load->operands.front().place == 0) {
    return nullptr;
  }
  const MirPlace *source = body.findPlace(load->operands.front().place);
  if (source == nullptr || source->type != value->info.type ||
      !source->projections.empty()) {
    return nullptr;
  }
  const bool localMaterialized =
      source->root == MirPlaceRootKind::Binding &&
      (source->traits.drop == DropKind::Lexical ||
       source->traits.drop == DropKind::Trivial) &&
      materializedClosureForType(program, body, source->type);
  const bool capturedMaterialized = body.kind == MirBodyKind::Lambda &&
                                    source->root == MirPlaceRootKind::Symbol &&
                                    source->capture != 0;
  return localMaterialized || capturedMaterialized ? source : nullptr;
}

[[nodiscard]] const MirInstruction *definitionFor(const MirBody &body,
                                                  const MirOperand &operand) {
  if (operand.kind != MirOperandKind::Value || operand.value == 0) {
    return nullptr;
  }
  const MirValue *value = body.findValue(operand.value);
  return value == nullptr ? nullptr : findInstruction(body, value->definition);
}

[[nodiscard]] bool hasExactCallInput(const MirBody &body,
                                     const MirOperand &operand,
                                     HirValueId callSite, MirCallInputRole role,
                                     std::size_t index) {
  const MirInstruction *input = definitionFor(body, operand);
  if (input != nullptr && input->kind == MirInstructionKind::CallInput &&
      input->callSite == callSite && input->callInputRole == role &&
      input->callInputIndex == index) {
    return true;
  }
  // A direct borrow operand is its own schedule entry: forming the place
  // lvalue has no effects, so the evaluation order it must preserve is
  // vacuous — exactly like the direct-borrow receiver of a self-member
  // call.
  return (operand.kind == MirOperandKind::BorrowRead ||
          operand.kind == MirOperandKind::BorrowWrite) &&
         operand.place != 0 && body.findPlace(operand.place) != nullptr;
}

// A borrow-staged call input carries a read borrow of a place instead of a
// scalar value: the call spells the place expression directly, so the
// staged value never materializes as a local.
[[nodiscard]] const MirInstruction *
borrowStagedCallInput(const MirBody &body, const MirOperand &operand) {
  const MirInstruction *input = definitionFor(body, operand);
  return input != nullptr && input->kind == MirInstructionKind::CallInput &&
                 input->operands.size() == 1 &&
                 (input->operands.front().kind == MirOperandKind::BorrowRead ||
                  input->operands.front().kind ==
                      MirOperandKind::BorrowWrite) &&
                 input->operands.front().place != 0
             ? input
             : nullptr;
}

[[nodiscard]] bool isBorrowStagedResult(const MirBody &body,
                                        const MirValue &value) {
  const MirInstruction *definition = findInstruction(body, value.definition);
  return definition != nullptr &&
         definition->kind == MirInstructionKind::CallInput &&
         definition->operands.size() == 1 &&
         (definition->operands.front().kind == MirOperandKind::BorrowRead ||
          definition->operands.front().kind == MirOperandKind::BorrowWrite) &&
         definition->operands.front().place != 0;
}

[[nodiscard]] bool hasCompleteCallInputSchedule(const MirBody &body,
                                                const MirInstruction &call) {
  if (call.callSite == 0) {
    // HostedStartup is compiler-generated and has no source HIR call site.
    // Its nonzero operation tag is closed over the exact call/input schedule
    // by verifyMirProgram before this private classifier runs.
    if (body.kind == MirBodyKind::HostedStartup &&
        call.hostedStartupOperation != 0) {
      return true;
    }
    // A compiler-generated call carries its receiver as a place-carrying
    // staged borrow and its arguments as direct value operands: no
    // CallInput stages exist by construction, so the operand list itself
    // is the complete schedule.
    const bool stagedReceiver =
        call.receiver &&
        (call.receiver->kind == MirOperandKind::BorrowRead ||
         call.receiver->kind == MirOperandKind::BorrowWrite) &&
        call.receiver->place != 0;
    const bool temporaryReceiver =
        static_cast<bool>(directTemporaryReceiver(body, call));
    const bool directValues =
        std::all_of(call.operands.begin(), call.operands.end(),
                    [&](const MirOperand &operand) {
                      return (operand.kind == MirOperandKind::Value &&
                              operand.value != 0) ||
                             // A direct borrow argument binds the live place
                             // lvalue; its formation has no effects to order.
                             ((operand.kind == MirOperandKind::BorrowRead ||
                               operand.kind == MirOperandKind::BorrowWrite) &&
                              operand.place != 0 &&
                              body.findPlace(operand.place) != nullptr);
                    });
    return (!call.receiver || stagedReceiver || temporaryReceiver) &&
           directValues;
  }
  if (call.receiver && !hasExactCallInput(body, *call.receiver, call.callSite,
                                          MirCallInputRole::Receiver, 0)) {
    return false;
  }
  for (std::size_t index = 0; index < call.operands.size(); ++index) {
    if (!hasExactCallInput(body, call.operands[index], call.callSite,
                           MirCallInputRole::Argument, index)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool
isHostedStartupArgumentIndexAdvance(const MirBody &body,
                                    const MirInstruction &instruction) {
  // The hosted-startup verifier binds every nonzero operation tag to one
  // exact plan row. It is therefore the sole authority for the generated
  // Modify/PreIncrement schedule; source Modify remains unsupported here.
  return body.kind == MirBodyKind::HostedStartup &&
         instruction.hostedStartupOperation != 0 &&
         instruction.kind == MirInstructionKind::Modify &&
         instruction.operation == MirOperation::PreIncrement;
}

[[nodiscard]] bool instructionHasInvoke(const MirBlock &block,
                                        const MirInstruction &instruction) {
  return block.terminator.kind == MirTerminatorKind::Invoke &&
         block.terminator.invokeInstruction == instruction.id;
}

// True when every block reachable from the invoke's else target ends in
// the terminal failure family, with only goto/branch cleanup glue in
// between: the failure path performs cleanup and terminates, never
// rejoining normal flow or returning, so a plain-form spelling that
// never takes the edge observes exactly the compatibility behavior
// (whose helper terminated before any cleanup could run).
[[nodiscard]] bool failurePathTerminates(const MirBody &body,
                                         MirBlockId elseTarget) {
  std::vector<MirBlockId> pending{elseTarget};
  std::vector<MirBlockId> seen;
  while (!pending.empty()) {
    const MirBlockId currentId = pending.back();
    pending.pop_back();
    if (std::find(seen.begin(), seen.end(), currentId) != seen.end()) {
      continue;
    }
    seen.push_back(currentId);
    if (seen.size() > 32) {
      return false;
    }
    const MirBlock *current = nullptr;
    for (const MirBlock &candidate : body.blocks) {
      if (candidate.id == currentId) {
        current = &candidate;
      }
    }
    if (current == nullptr) {
      return false;
    }
    switch (current->terminator.kind) {
    case MirTerminatorKind::PropagateFailure:
    case MirTerminatorKind::ContainFailure:
    case MirTerminatorKind::TerminateCleanupFailure:
      continue;
    case MirTerminatorKind::Goto:
      pending.push_back(current->terminator.target);
      continue;
    case MirTerminatorKind::Branch:
      pending.push_back(current->terminator.target);
      pending.push_back(current->terminator.elseTarget);
      continue;
    default:
      return false;
    }
  }
  return true;
}

// A plain-form call site whose failure continuation is provably unused
// may take an ff-admitted callee's plain (wrapper) name: the site
// propagates transparently or is flow-discharged, and any paired invoke
// else path only cleans up and terminates. A handled site — one whose
// else path reads the error or rejoins flow — never matches, which is
// the convention the mir_backend fixtures pin.
[[nodiscard]] bool
wrapperContainedCallSite(const MirProgram &program,
                         const CppMirBodyEmissionMap &representations,
                         const MirBody &body, const MirBlock &block,
                         const MirInstruction &instruction) {
  if (instruction.kind != MirInstructionKind::Call ||
      !instruction.functionTarget ||
      instruction.intrinsic != IntrinsicKind::None ||
      (instruction.definedFailure.propagation !=
           FailurePropagationKind::DirectCall &&
       instruction.definedFailure.propagation !=
           FailurePropagationKind::None) ||
      !instruction.definedFailure.localOrigins.empty() ||
      !instruction.localFailureSites.empty()) {
    return false;
  }
  if (instructionHasInvoke(block, instruction) &&
      !failurePathTerminates(body, block.terminator.elseTarget)) {
    return false;
  }
  const MirFunctionInstance *target =
      program.findFunctionInstance(*instruction.functionTarget);
  if (target == nullptr || !target->mayRaiseDefinedFailure ||
      target->linkage != LanguageLinkage::Gti ||
      target->definitionKind != MirFunctionInstance::DefinitionKind::Source ||
      !target->callableParameters.empty() ||
      target->entryKind != ProgramEntryKind::None) {
    return false;
  }
  thread_local std::vector<HirFunctionInstanceId> probing;
  if (std::find(probing.begin(), probing.end(), target->id) != probing.end()) {
    return false;
  }
  probing.push_back(target->id);
  const bool contained =
      CppMirBodyEmitter(program, representations)
          .supportsFailureBodyText(
              {.kind = MirBodyKind::Function, .owner = target->id});
  probing.pop_back();
  return contained;
}

// True when any block's Invoke terminator pairs with this instruction:
// the failure edge exists, so the status protocol owns the spelling. A
// site with no edge is MIR's own assertion of terminal containment.
[[nodiscard]] bool invokePairedInstruction(const MirBody &body,
                                           MirInstructionId id) {
  for (const MirBlock &block : body.blocks) {
    if (block.terminator.kind == MirTerminatorKind::Invoke &&
        block.terminator.invokeInstruction == id) {
      return true;
    }
  }
  return false;
}

// Some transformed bodies use the status ABI only for placement return. If
// MIR has no failure record or routing terminator, the status is provably
// always true and a caller needs no synthetic failure edge merely to consume
// the placement result. Local failure metadata may remain: without an Invoke
// it is terminally contained by the emitted helper and cannot produce false.
[[nodiscard]] bool failureStatusCannotFail(const MirBody &body) {
  if (!body.failureRecords.empty()) {
    return false;
  }
  for (const MirBlock &block : body.blocks) {
    if (block.terminator.kind == MirTerminatorKind::Invoke ||
        block.terminator.kind == MirTerminatorKind::PropagateFailure ||
        block.terminator.kind == MirTerminatorKind::TerminateCleanupFailure) {
      return false;
    }
  }
  return true;
}

// A source constructor whose ordinary MIR body is spellable contains every
// failure in that body at its native boundary. This is enough for an ordinary
// in-class initializer: C++ constructs the field directly and any failing
// helper terminates before publishing the field. A failure-form caller needs
// the stricter status proof below before it may erase the failure edge.
[[nodiscard]] bool ordinaryConstructorBodyContained(
    const MirProgram &program, const CppMirBodyEmissionMap &representations,
    HirConstructorInstanceId constructor, const SemanticType &type) {
  const MirConstructorInstance *target =
      program.findConstructorInstance(constructor);
  const MirClassInstance *owner =
      target == nullptr ? nullptr : program.findClassInstance(target->owner);
  if (target == nullptr || owner == nullptr || owner->type != type ||
      target->definitionKind != MirDefinitionKind::Source) {
    return false;
  }
  thread_local std::vector<HirConstructorInstanceId> probing;
  if (std::find(probing.begin(), probing.end(), target->id) != probing.end()) {
    return false;
  }
  probing.push_back(target->id);
  const bool contained =
      CppMirBodyEmitter(program, representations)
          .supportsBodyText(
              {.kind = MirBodyKind::Constructor, .owner = target->id});
  probing.pop_back();
  return contained;
}

[[nodiscard]] bool plainConstructorBodyContainsFailure(
    const MirProgram &program, const CppMirBodyEmissionMap &representations,
    const MirInstruction &instruction) {
  if (instruction.kind != MirInstructionKind::Construct ||
      !instruction.constructorTarget || instruction.destination ||
      instruction.receiver || instruction.functionTarget ||
      instruction.lambdaTarget || instruction.bodyTarget ||
      instruction.callableInvocation ||
      instruction.intrinsic != IntrinsicKind::None ||
      instruction.constructorKind != ConstructorKind::Ordinary ||
      instruction.definedFailure.propagation !=
          FailurePropagationKind::Constructor ||
      !instruction.definedFailure.localOrigins.empty() ||
      !instruction.localFailureSites.empty()) {
    return false;
  }
  return ordinaryConstructorBodyContained(program, representations,
                                          *instruction.constructorTarget,
                                          instruction.info.type);
}

// A construction in a failure-form caller may use the ordinary constructor
// only when the target's complete initializer chain cannot return a failure
// status and that ordinary MIR body is itself spellable. The call then either
// completes successfully or terminates inside a contained helper, so the
// paired Invoke's failure edge is unreachable without inventing rollback for
// a state-bearing base subobject.
[[nodiscard]] bool terminallyContainedPlainConstructor(
    const MirProgram &program, const CppMirBodyEmissionMap &representations,
    const MirInstruction &instruction) {
  return instruction.constructorTarget &&
         cppMirConstructorStatusCannotFail(program,
                                           *instruction.constructorTarget) &&
         plainConstructorBodyContainsFailure(program, representations,
                                             instruction);
}

// An owning in-class initializer may materialize a class temporary in MIR and
// immediately reparent its obligation into the field. Native in-class
// initialization constructs that same value directly in the field, so the
// temporary and ConstructionRollback records are representation-only. Erase
// them only for an exact ordinary Construct or targetless default Call,
// followed by its one Initialize transfer.
[[nodiscard]] bool directOwningFieldInitializerTransfers(const MirBody &body) {
  if (body.dropObligations.empty()) {
    return true;
  }
  if (body.kind != MirBodyKind::FieldInitializers ||
      !body.cleanupBoundaries.empty()) {
    return false;
  }
  for (const MirBlock &block : body.blocks) {
    if (std::any_of(block.instructions.begin(), block.instructions.end(),
                    [](const MirInstruction &instruction) {
                      return instruction.kind == MirInstructionKind::Drop;
                    })) {
      return false;
    }
  }

  std::unordered_set<MirDropObligationId> matched;
  for (const MirDropObligation &temporaryDrop : body.dropObligations) {
    if (temporaryDrop.kind == MirDropObligationKind::ConstructionRollback) {
      continue;
    }
    if (temporaryDrop.kind != MirDropObligationKind::Value ||
        temporaryDrop.initiallyActive ||
        temporaryDrop.dropType.type.kind != SemanticType::Class ||
        !temporaryDrop.dropType.requiresActiveCleanup) {
      return false;
    }
    const MirPlace *temporary = body.findPlace(temporaryDrop.place);
    const MirValue *value =
        temporary != nullptr && temporary->root == MirPlaceRootKind::Value &&
                temporary->value != 0 && temporary->projections.empty()
            ? body.findValue(temporary->value)
            : nullptr;
    const MirInstruction *producer =
        value == nullptr ? nullptr : findInstruction(body, value->definition);
    const bool ordinaryConstruct =
        producer != nullptr &&
        producer->kind == MirInstructionKind::Construct &&
        producer->constructorTarget && !producer->functionTarget &&
        !producer->lambdaTarget && !producer->bodyTarget &&
        !producer->callableInvocation &&
        producer->intrinsic == IntrinsicKind::None &&
        producer->constructorKind == ConstructorKind::Ordinary &&
        producer->successResultDrop == temporaryDrop.id &&
        producer->definedFailure.propagation ==
            FailurePropagationKind::Constructor &&
        producer->definedFailure.localOrigins.empty() &&
        invokeSuccessActivates(body, *producer, temporaryDrop.id);
    const bool targetlessDefaultCall =
        producer != nullptr && producer->kind == MirInstructionKind::Call &&
        !producer->functionTarget && !producer->constructorTarget &&
        !producer->lambdaTarget && !producer->bodyTarget &&
        !producer->callableInvocation &&
        (producer->intrinsic == IntrinsicKind::None ||
         producer->intrinsic ==
             IntrinsicKind::DefaultTypeParameterConstruction) &&
        producer->operands.empty() && producer->callableArguments.empty() &&
        !producer->successResultDrop &&
        producer->definedFailure.propagation == FailurePropagationKind::None &&
        producer->definedFailure.localOrigins.empty() &&
        producer->lifecycle.size() == 1 &&
        producer->lifecycle.front().kind == MirLifecycleEventKind::Initialize &&
        producer->lifecycle.front().source == 0 &&
        producer->lifecycle.front().target == temporaryDrop.id &&
        !producer->lifecycle.front().conditional &&
        !producer->lifecycle.front().failureCleanup;
    if (temporary == nullptr || value == nullptr || producer == nullptr ||
        (!ordinaryConstruct && !targetlessDefaultCall) || !producer->result ||
        *producer->result != value->id || producer->destination ||
        producer->receiver || producer->successResultDestination ||
        producer->info.type != value->info.type ||
        producer->info.type != temporary->type ||
        temporaryDrop.dropType.type != temporary->type ||
        !producer->localFailureSites.empty()) {
      return false;
    }

    const std::vector<MirValueUse> uses = nonRootRecordUses(body, value->id);
    const MirInstruction *initialize =
        uses.size() == 1 &&
                uses.front().kind == MirValueUseKind::InstructionOperand &&
                uses.front().operandIndex == 0
            ? findInstruction(body, uses.front().instruction)
            : nullptr;
    if (initialize == nullptr ||
        initialize->kind != MirInstructionKind::Initialize ||
        !initialize->destination || initialize->receiver ||
        initialize->result || initialize->operands.size() != 1 ||
        initialize->operands.front().kind != MirOperandKind::Value ||
        initialize->operands.front().value != value->id ||
        initialize->operands.front().type != value->info.type ||
        !initialize->localFailureSites.empty() ||
        !initialize->definedFailure.empty() ||
        initialize->lifecycle.size() != 1) {
      return false;
    }
    const MirPlace *field = body.findPlace(*initialize->destination);
    const MirLifecycleEvent &reparent = initialize->lifecycle.front();
    const MirDropObligation *fieldDrop =
        body.findDropObligation(reparent.target);
    if (field == nullptr || field->root != MirPlaceRootKind::Binding ||
        field->symbol == 0 || !field->projections.empty() ||
        field->type != value->info.type ||
        reparent.kind != MirLifecycleEventKind::Reparent ||
        reparent.source != temporaryDrop.id || reparent.target == 0 ||
        reparent.conditional || reparent.failureCleanup ||
        fieldDrop == nullptr ||
        fieldDrop->kind != MirDropObligationKind::ConstructionRollback ||
        fieldDrop->place != field->id || fieldDrop->initiallyActive ||
        fieldDrop->dropType != temporaryDrop.dropType) {
      return false;
    }

    std::size_t activations = 0;
    std::size_t reparents = 0;
    std::size_t transfers = 0;
    bool invalidEvent = false;
    const auto inspect = [&](const MirLifecycleEvent &event) {
      if (event.source != temporaryDrop.id &&
          event.target != temporaryDrop.id && event.source != fieldDrop->id &&
          event.target != fieldDrop->id) {
        return;
      }
      if (event.kind == MirLifecycleEventKind::Initialize &&
          event.source == 0 && event.target == temporaryDrop.id &&
          !event.conditional && !event.failureCleanup) {
        ++activations;
      } else if (event == reparent) {
        ++reparents;
      } else if (event.kind == MirLifecycleEventKind::TransferOut &&
                 event.source == fieldDrop->id && event.target == 0 &&
                 !event.conditional && !event.failureCleanup) {
        ++transfers;
      } else {
        invalidEvent = true;
      }
    };
    for (const MirBlock &block : body.blocks) {
      for (const MirLifecycleEvent &event : block.terminator.successLifecycle) {
        inspect(event);
      }
      for (const MirInstruction &instruction : block.instructions) {
        for (const MirLifecycleEvent &event : instruction.lifecycle) {
          inspect(event);
        }
      }
    }
    if (invalidEvent || activations != 1 || reparents != 1 || transfers != 1 ||
        !matched.insert(temporaryDrop.id).second ||
        !matched.insert(fieldDrop->id).second) {
      return false;
    }
  }
  return matched.size() == body.dropObligations.size();
}

// True when the constructor's verified MIR carries the complete rollback
// authority for its owner: no state-bearing bases, no unarmed subobject
// transfer (the body still routes failure edges), and every declared field
// with a non-trivial drop armed exactly one ConstructionRollback obligation.
[[nodiscard]] bool
constructorRollbackCovered(const MirConstructorInstance &constructor,
                           const MirClassInstance *owner);

[[nodiscard]] bool classHasStateBearingBase(const MirClassInstance &instance) {
  return std::any_of(
      instance.bases.begin(), instance.bases.end(),
      [](const HirBaseInstance &base) { return !base.interface; });
}

// C++ runs the source destructor body before destroying fields and bases. The
// backend may use that native composition only when MIR names the exact same
// concrete subobjects and carries the complete reverse field-drop schedule.
// This is representation lowering, not implicit lifecycle authority: a stale
// or partial owner record remains ineligible even when native C++ would
// happen to compile it.
[[nodiscard]] bool
nativeDestructorCompositionCovered(const MirProgram &program,
                                   const MirClassInstance &owner,
                                   const MirDestructorInstance &destructor) {
  if (owner.kind == ClassKind::Union || owner.cAbiRecord || owner.cAbiLayout ||
      owner.unionLayout || owner.destructor != destructor.id ||
      destructor.owner != owner.id ||
      owner.destructorStatus != SpecialMemberStatus::Declared ||
      !owner.requiresActiveDropState || !owner.requiresActiveCleanup ||
      owner.bases != owner.structuralBases) {
    return false;
  }

  for (const HirBaseInstance &base : owner.structuralBases) {
    const MirClassInstance *instance = program.findClassInstance(base.instance);
    if (instance == nullptr || instance->type != base.type ||
        base.interface != (instance->kind == ClassKind::Interface)) {
      return false;
    }
  }

  std::vector<MirClassFieldLifecycle> lifecycleFields;
  lifecycleFields.reserve(owner.declaredFields.size());
  for (const MirClassFieldInfo &field : owner.declaredFields) {
    if (field.dropKind != DropKind::Lexical) {
      continue;
    }
    lifecycleFields.push_back(
        {.field = field.field,
         .symbol = field.symbol,
         .type = field.type,
         .dropKind = field.dropKind,
         .requiresActiveCleanup = field.requiresActiveCleanup});
  }
  if (lifecycleFields != owner.fields ||
      owner.fieldDropOrder.size() != lifecycleFields.size()) {
    return false;
  }
  for (std::size_t index = 0; index < lifecycleFields.size(); ++index) {
    const MirClassFieldLifecycle &field =
        lifecycleFields[lifecycleFields.size() - index - 1];
    const MirFieldDrop &drop = owner.fieldDropOrder[index];
    if (drop.field != field.field || drop.symbol != field.symbol ||
        drop.type != field.type ||
        drop.requiresActiveCleanup != field.requiresActiveCleanup) {
      return false;
    }
  }
  return true;
}

// A constructor body with no failure records and no Invoke terminators
// has no within-body failure path at all: every failure source inside it
// terminates at its own site, so partial-construction rollback is
// vacuously complete. The shared lowering predicate stays untouched —
// this exemption is a backend emission fact, not a lowering decision.
[[nodiscard]] bool constructorBodyFailureEdgeFree(const MirBody &body) {
  return body.failureRecords.empty() &&
         std::none_of(
             body.blocks.begin(), body.blocks.end(), [](const MirBlock &block) {
               return block.terminator.kind == MirTerminatorKind::Invoke;
             });
}

[[nodiscard]] std::string_view
constructorRollbackGap(const MirConstructorInstance &constructor,
                       const MirClassInstance *owner) {
  if (owner == nullptr) {
    return "constructor lost its owner instance";
  }
  if (classHasStateBearingBase(*owner)) {
    return "owner carries a state-bearing base subobject";
  }
  if (constructorBodyFailureEdgeFree(constructor.body)) {
    return {};
  }
  if (!mirBodyRoutesFailureEdges(constructor.body)) {
    return "constructor body does not route its failure edges";
  }
  for (const MirClassFieldInfo &field : owner->declaredFields) {
    if (field.dropKind == DropKind::Trivial) {
      continue;
    }
    const bool armed = std::any_of(
        constructor.body.dropObligations.begin(),
        constructor.body.dropObligations.end(),
        [&](const MirDropObligation &obligation) {
          if (obligation.kind != MirDropObligationKind::ConstructionRollback) {
            return false;
          }
          const MirPlace *place = constructor.body.findPlace(obligation.place);
          return place != nullptr && place->projections.size() == 1 &&
                 place->projections.front().field == field.symbol;
        });
    if (!armed) {
      return "a non-trivial field carries no construction-rollback "
             "obligation";
    }
  }
  return {};
}

bool constructorRollbackCovered(const MirConstructorInstance &constructor,
                                const MirClassInstance *owner) {
  if (owner == nullptr || classHasStateBearingBase(*owner)) {
    return false;
  }
  if (constructorBodyFailureEdgeFree(constructor.body)) {
    return true;
  }
  if (!mirBodyRoutesFailureEdges(constructor.body)) {
    return false;
  }
  for (const MirClassFieldInfo &field : owner->declaredFields) {
    if (field.dropKind == DropKind::Trivial) {
      continue;
    }
    const bool armed = std::any_of(
        constructor.body.dropObligations.begin(),
        constructor.body.dropObligations.end(),
        [&](const MirDropObligation &obligation) {
          if (obligation.kind != MirDropObligationKind::ConstructionRollback) {
            return false;
          }
          const MirPlace *place = constructor.body.findPlace(obligation.place);
          return place != nullptr && place->projections.size() == 1 &&
                 place->projections.front().field == field.symbol;
        });
    if (!armed) {
      return false;
    }
  }
  return true;
}

// A constructor failure overload creates a complete C++ object, reports the
// GTI outcome through explicit data, and lets the caller destroy that
// unpublished object when construction fails. This is equivalent to MIR's
// field rollback only for the bounded shape below: no base or user destructor
// can add observable cleanup, passive field defaults cannot fail before the
// body, and every failure-cleanup Drop is one exact ConstructionRollback
// field. Broader construction needs a field-wise builder representation
// rather than weakening this boundary.
[[nodiscard]] bool passiveConstructorAggregateType(const SemanticType &type) {
  switch (type.kind) {
  case SemanticType::Int8:
  case SemanticType::Int16:
  case SemanticType::Int32:
  case SemanticType::Int64:
  case SemanticType::UInt8:
  case SemanticType::UInt16:
  case SemanticType::UInt32:
  case SemanticType::UInt64:
  case SemanticType::Float:
  case SemanticType::Double:
  case SemanticType::Bool:
  case SemanticType::Char:
  case SemanticType::StringView:
  case SemanticType::CString:
  case SemanticType::NullPtr:
  case SemanticType::RawPointer:
  case SemanticType::Enum:
    return true;
  case SemanticType::Array:
    return type.arguments.size() == 1 && type.arrayLength != 0 &&
           type.arrayLengthParameterId == 0 &&
           passiveConstructorAggregateType(type.arguments.front());
  default:
    return false;
  }
}

[[nodiscard]] bool passiveConstructorFieldInitializers(const MirBody &body) {
  if (!body.failureRecords.empty() ||
      std::any_of(body.blocks.begin(), body.blocks.end(),
                  [](const MirBlock &block) {
                    return block.terminator.kind == MirTerminatorKind::Invoke;
                  })) {
    return false;
  }
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      if (!instruction.localFailureSites.empty() ||
          !instruction.definedFailure.empty()) {
        return false;
      }
      switch (instruction.kind) {
      case MirInstructionKind::Lifecycle:
        break;
      case MirInstructionKind::Compute: {
        const bool literal = instruction.operation == MirOperation::Literal;
        const bool passiveAggregate =
            instruction.operation == MirOperation::Aggregate &&
            instruction.info.type.kind == SemanticType::Array &&
            instruction.info.type.arguments.size() == 1 &&
            instruction.info.type.arrayLengthParameterId == 0 &&
            instruction.operands.size() <= instruction.info.type.arrayLength &&
            passiveConstructorAggregateType(instruction.info.type) &&
            instruction.info.traits.drop == DropKind::Trivial &&
            !instruction.info.traits.containsBorrowedState &&
            std::all_of(instruction.operands.begin(),
                        instruction.operands.end(),
                        [&](const MirOperand &operand) {
                          return operand.kind == MirOperandKind::Value &&
                                 operand.type ==
                                     instruction.info.type.arguments.front();
                        });
        if (!literal && !passiveAggregate) {
          return false;
        }
        break;
      }
      case MirInstructionKind::Initialize:
        if (instruction.operands.size() > 1 ||
            (instruction.operands.size() == 1 &&
             instruction.operands.front().kind != MirOperandKind::Value &&
             instruction.operands.front().kind != MirOperandKind::Constant)) {
          return false;
        }
        break;
      default:
        return false;
      }
    }
  }
  return true;
}

// An ordinary native constructor may delegate construction of a state-bearing
// base to C++ when no derived operation can fail after that base is published.
// The initial slice is deliberately exact: a passive derived field schedule,
// an edge-free derived body, and one explicit zero-argument source constructor
// for every concrete base. The selected base body and its in-class field
// schedule must both be independently spellable from MIR. Arbitrary base
// arguments and transformed failure constructors remain outside this proof.
[[nodiscard]] bool nativeContainedBaseConstruction(
    const MirProgram &program, const CppMirBodyEmissionMap &representations,
    const MirClassInstance &owner, const MirConstructorInstance &constructor) {
  if (owner.bases.empty() || owner.bases != owner.structuralBases ||
      constructor.owner != owner.id ||
      !constructorBodyFailureEdgeFree(constructor.body) ||
      !passiveConstructorFieldInitializers(owner.fieldInitializers)) {
    return false;
  }

  const CppMirBodyEmitter emitter(program, representations);
  for (const HirBaseInstance &baseInfo : owner.bases) {
    if (baseInfo.interface) {
      return false;
    }
    const MirClassInstance *base = program.findClassInstance(baseInfo.instance);
    const MirConstructorInitializer *initializer = nullptr;
    for (const MirConstructorInitializer &candidate :
         constructor.initializers) {
      if (candidate.kind != ConstructorInitializerTargetKind::Base ||
          candidate.base != baseInfo.instance) {
        continue;
      }
      if (initializer != nullptr) {
        return false;
      }
      initializer = &candidate;
    }
    if (base == nullptr || base->type != baseInfo.type ||
        base->kind != ClassKind::Class || base->cAbiRecord ||
        base->unionLayout ||
        base->destructorStatus == SpecialMemberStatus::Deleted ||
        initializer == nullptr || initializer->targetType != base->type ||
        !initializer->constructorTarget || !initializer->arguments.empty() ||
        initializer->storesReference || initializer->ownedParameter) {
      return false;
    }
    const MirConstructorInstance *target =
        program.findConstructorInstance(*initializer->constructorTarget);
    if (target == nullptr || target->owner != base->id ||
        !target->parameterTypes.empty() ||
        !ordinaryConstructorBodyContained(program, representations, target->id,
                                          base->type)) {
      return false;
    }
    const CppMirInitializerScheduleText fields = emitter.initializerSchedule(
        {.kind = MirBodyKind::FieldInitializers, .owner = base->id});
    if (!fields.supported ||
        fields.fields.size() != base->declaredFields.size()) {
      return false;
    }
    for (const MirClassFieldInfo &field : base->declaredFields) {
      if (std::count_if(fields.fields.begin(), fields.fields.end(),
                        [&](const CppMirFieldInitializerSpelling &candidate) {
                          return candidate.field == field.symbol;
                        }) != 1) {
        return false;
      }
    }
  }
  return true;
}

// A transformed constructor first creates an observation-free native C++
// object and then publishes each MIR initializer into its field. For an
// owning field this is equivalent to GTI's construction schedule only when
// that empty state and the assignment which replaces it are compiler-
// generated and failure-free. Prove that property from concrete class
// metadata recursively; in particular, do not recognize any standard-library
// wrapper by name. A source-declared constructor may delete the ordinary
// default constructor: the backend's private empty-state tag provides this
// representation without changing the GTI constructor surface.
[[nodiscard]] bool failureConstructorEmptyDefaultFieldType(
    const MirProgram &program, const SemanticType &type,
    std::unordered_set<HirClassInstanceId> &visiting) {
  if (type.kind == SemanticType::Storage ||
      type.kind == SemanticType::PrefixStorage ||
      type.kind == SemanticType::UniqueOwner ||
      type.kind == SemanticType::SharedPointer) {
    return true;
  }
  if (type.kind != SemanticType::Class) {
    return false;
  }

  const MirClassInstance *instance = nullptr;
  for (const MirClassInstance &candidate : program.classInstances()) {
    if (candidate.type != type) {
      continue;
    }
    if (instance != nullptr) {
      return false;
    }
    instance = &candidate;
  }
  if (instance == nullptr || instance->declaration == 0 || instance->abstract ||
      instance->polymorphic || instance->cAbiRecord || instance->unionLayout ||
      !instance->bases.empty() || !instance->structuralBases.empty() ||
      instance->moveAssignment != SpecialMemberStatus::Generated ||
      instance->destructorStatus != SpecialMemberStatus::Generated ||
      instance->destructor || instance->requiresActiveDropState ||
      instance->traits.containsBorrowedState ||
      !instance->traits.moveAssignable ||
      !passiveConstructorFieldInitializers(instance->fieldInitializers) ||
      !visiting.insert(instance->id).second) {
    return false;
  }

  const bool fieldsReady = std::all_of(
      instance->declaredFields.begin(), instance->declaredFields.end(),
      [&](const MirClassFieldInfo &field) {
        if (field.field == 0 || field.symbol == 0) {
          return false;
        }
        if (field.dropKind == DropKind::Trivial) {
          return !field.requiresActiveCleanup &&
                 passiveConstructorAggregateType(field.type);
        }
        return field.requiresActiveCleanup &&
               failureConstructorEmptyDefaultFieldType(program, field.type,
                                                       visiting);
      });
  visiting.erase(instance->id);
  return fieldsReady;
}

[[nodiscard]] bool
failureConstructorEmptyDefaultFieldType(const MirProgram &program,
                                        const SemanticType &type) {
  std::unordered_set<HirClassInstanceId> visiting;
  return failureConstructorEmptyDefaultFieldType(program, type, visiting);
}

[[nodiscard]] bool
aggregateConstructorRollbackDrop(const MirBody &body,
                                 const MirInstruction &instruction) {
  if (instruction.kind != MirInstructionKind::Drop ||
      !instruction.destination || instruction.lifecycle.size() != 1 ||
      !instruction.lifecycle.front().failureCleanup ||
      instruction.lifecycle.front().source == 0) {
    return false;
  }
  const MirDropObligation *obligation =
      body.findDropObligation(instruction.lifecycle.front().source);
  const MirPlace *place = body.findPlace(*instruction.destination);
  return obligation != nullptr &&
         obligation->kind == MirDropObligationKind::ConstructionRollback &&
         obligation->place == *instruction.destination && place != nullptr &&
         place->root == MirPlaceRootKind::This &&
         place->projections.size() == 1 &&
         place->projections.front().kind == MirProjectionKind::Field;
}

// A passive abstract base contributes only C++'s ordinary default-base
// initialization to a derived object's transformed constructor. The exact
// generated initializer plus trivial base fields prove that construction and
// rollback need no hidden backend schedule. User-selected constructors,
// nontrivial state, user destructors, and nested base graphs remain outside
// this bounded proof.
[[nodiscard]] bool
passiveDefaultBaseSurface(const MirProgram &program,
                          const MirClassInstance &owner,
                          const MirConstructorInstance &constructor) {
  if (owner.bases.empty() && owner.structuralBases.empty()) {
    return true;
  }
  if (!owner.polymorphic || owner.bases.empty() ||
      owner.bases != owner.structuralBases) {
    return false;
  }
  return std::all_of(
      owner.bases.begin(), owner.bases.end(),
      [&](const HirBaseInstance &baseInfo) {
        const MirClassInstance *base =
            program.findClassInstance(baseInfo.instance);
        const auto initializer = std::find_if(
            constructor.initializers.begin(), constructor.initializers.end(),
            [&](const MirConstructorInitializer &candidate) {
              return candidate.kind == ConstructorInitializerTargetKind::Base &&
                     candidate.base == baseInfo.instance;
            });
        if (base == nullptr || !base->abstract || !base->polymorphic ||
            base->cAbiRecord || base->unionLayout || !base->bases.empty() ||
            !base->structuralBases.empty() ||
            base->defaultConstructor == SpecialMemberStatus::Deleted ||
            base->destructorStatus == SpecialMemberStatus::Deleted ||
            base->destructor || base->requiresActiveDropState ||
            base->requiresActiveCleanup || !base->fieldDropOrder.empty() ||
            !passiveConstructorFieldInitializers(base->fieldInitializers)) {
          return false;
        }
        if (baseInfo.interface) {
          return base->kind == ClassKind::Interface &&
                 base->declaredFields.empty() && base->fields.empty();
        }
        return base->kind == ClassKind::Class &&
               std::all_of(base->declaredFields.begin(),
                           base->declaredFields.end(),
                           [](const MirClassFieldInfo &field) {
                             return field.field != 0 && field.symbol != 0 &&
                                    field.dropKind == DropKind::Trivial &&
                                    !field.requiresActiveCleanup;
                           }) &&
               initializer != constructor.initializers.end() &&
               initializer->targetType == base->type &&
               initializer->generatedDefault &&
               !initializer->constructorTarget &&
               initializer->arguments.empty() &&
               !initializer->storesReference && !initializer->ownedParameter;
      });
}

[[nodiscard]] bool
failureConstructorDisarmsDeclaredDestructor(const MirProgram &program,
                                            const MirClassInstance &owner) {
  // A transformed C++ constructor returns normally even when GTI construction
  // fails, so the caller must end the C++ object lifetime. Keep the generated
  // lifecycle guard disarmed until MIR reaches success: destroy_at() then
  // skips the user destructor body. Native field destruction may stand in for
  // MIR construction rollback only when the concrete owner seals the exact
  // same reverse field schedule and every non-trivial field has the bounded
  // failure-constructor empty state. This keeps the proof structural: no
  // source-library type name is recognized here.
  const MirDestructorInstance *destructor =
      owner.destructor ? program.findDestructorInstance(*owner.destructor)
                       : nullptr;
  if (owner.declaration == 0 || destructor == nullptr ||
      !nativeDestructorCompositionCovered(program, owner, *destructor) ||
      owner.abstract || owner.polymorphic || !owner.bases.empty() ||
      !owner.structuralBases.empty()) {
    return false;
  }
  return std::all_of(
      owner.declaredFields.begin(), owner.declaredFields.end(),
      [&](const MirClassFieldInfo &field) {
        if (field.field == 0 || field.symbol == 0) {
          return false;
        }
        if (field.dropKind == DropKind::Trivial) {
          return !field.requiresActiveCleanup &&
                 (passiveConstructorAggregateType(field.type) ||
                  failureConstructorEmptyDefaultFieldType(program, field.type));
        }
        return field.dropKind == DropKind::Lexical &&
               field.requiresActiveCleanup &&
               failureConstructorEmptyDefaultFieldType(program, field.type);
      });
}

[[nodiscard]] bool
failureConstructorBoundaryEligible(const MirProgram &program,
                                   const MirConstructorInstance &constructor) {
  const auto concreteType = [](const auto &self,
                               const SemanticType &type) -> bool {
    if (type.kind == SemanticType::TypeParameter ||
        type.kind == SemanticType::TypePack) {
      return false;
    }
    return std::all_of(
        type.arguments.begin(), type.arguments.end(),
        [&](const SemanticType &argument) { return self(self, argument); });
  };
  const MirClassInstance *owner = program.findClassInstance(constructor.owner);
  const bool edgeFree = constructorBodyFailureEdgeFree(constructor.body);
  const bool disarmedDeclaredDestructor =
      owner != nullptr &&
      failureConstructorDisarmsDeclaredDestructor(program, *owner);
  const bool passiveBases =
      owner != nullptr &&
      passiveDefaultBaseSurface(program, *owner, constructor);
  const bool concreteOwner =
      owner != nullptr && concreteType(concreteType, owner->type);
  const bool rollbackCovered =
      edgeFree || constructorRollbackCovered(constructor, owner);
  const bool passiveFields =
      owner != nullptr &&
      passiveConstructorFieldInitializers(owner->fieldInitializers);
  const auto storedReferences = cppMirStoredReferenceBindings(constructor);
  const auto ownedParameters =
      cppMirOwnedParameterFieldBindings(program, constructor);
  if (constructor.definitionKind != MirDefinitionKind::Source ||
      !constructor.mayRaiseDefinedFailure || owner == nullptr ||
      owner->abstract || owner->cAbiRecord || owner->unionLayout ||
      !passiveBases || !concreteOwner || !rollbackCovered || !passiveFields ||
      !storedReferences || !ownedParameters) {
    return false;
  }
  if (edgeFree) {
    // The status overload can only report success, so no partially
    // initialized object is ever published or rolled back. Active cleanup
    // and a generated/user destructor are therefore ordinary completed-object
    // lifecycle, not construction-failure machinery.
    return true;
  }
  if ((!disarmedDeclaredDestructor &&
       (owner->destructor || owner->requiresActiveDropState)) ||
      constructor.body.failureRecords.empty() ||
      !mirBodyRoutesFailureEdges(constructor.body)) {
    return false;
  }
  // The aggregate fallback relies on each lexical field having a
  // failure-free empty C++ representation. Raw backend owners provide that
  // directly; a source class is admitted only through the recursive
  // generated-lifecycle proof above.
  for (const MirClassFieldInfo &field : owner->declaredFields) {
    if (field.dropKind == DropKind::Trivial) {
      continue;
    }
    if (!failureConstructorEmptyDefaultFieldType(program, field.type)) {
      return false;
    }
  }
  for (const MirBlock &block : constructor.body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      if (instruction.kind == MirInstructionKind::Drop &&
          std::any_of(instruction.lifecycle.begin(),
                      instruction.lifecycle.end(),
                      [](const MirLifecycleEvent &event) {
                        return event.failureCleanup;
                      }) &&
          !aggregateConstructorRollbackDrop(constructor.body, instruction)) {
        return false;
      }
    }
  }
  return true;
}

struct InlineFailureConstructorArgument {
  const MirInstruction *producer = nullptr;
  const MirInstruction *stage = nullptr;
  const MirInstruction *consumer = nullptr;

  [[nodiscard]] explicit operator bool() const {
    return producer != nullptr && stage != nullptr && consumer != nullptr;
  }
};

// A failure-free class construction immediately moved into one
// failure-capable outer constructor can remain a C++ prvalue. MIR records the
// source construction and its MoveValue CallInput separately, but neither has
// an owning place or drop obligation. Requiring the exact adjacent chain
// keeps evaluation order identical to MIR and prevents this spelling from
// hiding an inner defined-failure edge.
[[nodiscard]] InlineFailureConstructorArgument
inlineFailureConstructorArgument(const MirProgram &program, const MirBody &body,
                                 MirValueId valueId) {
  const MirValue *candidate = body.findValue(valueId);
  const MirInstruction *definition =
      candidate == nullptr ? nullptr
                           : findInstruction(body, candidate->definition);
  const MirInstruction *producer = nullptr;
  const MirInstruction *stage = nullptr;
  MirValueId sourceId = 0;
  if (definition != nullptr &&
      definition->kind == MirInstructionKind::Construct && definition->result &&
      *definition->result == valueId) {
    producer = definition;
    sourceId = valueId;
  } else if (definition != nullptr &&
             definition->kind == MirInstructionKind::CallInput &&
             definition->result && *definition->result == valueId &&
             definition->operands.size() == 1 &&
             definition->operands.front().kind == MirOperandKind::Value) {
    stage = definition;
    sourceId = definition->operands.front().value;
    const MirValue *source = body.findValue(sourceId);
    producer =
        source == nullptr ? nullptr : findInstruction(body, source->definition);
  }
  const MirValue *source = body.findValue(sourceId);
  const MirConstructorInstance *inner =
      producer != nullptr && producer->constructorTarget
          ? program.findConstructorInstance(*producer->constructorTarget)
          : nullptr;
  const MirClassInstance *innerOwner =
      inner == nullptr ? nullptr : program.findClassInstance(inner->owner);
  if (source == nullptr || source->info.type.kind != SemanticType::Class ||
      producer == nullptr || producer->kind != MirInstructionKind::Construct ||
      !producer->result || *producer->result != sourceId ||
      producer->destination || producer->receiver ||
      producer->constructorKind != ConstructorKind::Ordinary ||
      !producer->constructorTarget || !producer->localFailureSites.empty() ||
      producer->definedFailure.propagation != FailurePropagationKind::None ||
      inner == nullptr || innerOwner == nullptr ||
      innerOwner->type != source->info.type ||
      inner->definitionKind != MirDefinitionKind::Source ||
      inner->mayRaiseDefinedFailure ||
      !std::all_of(producer->operands.begin(), producer->operands.end(),
                   [](const MirOperand &operand) {
                     const auto kind = expectedTypeRepresentation(operand.type);
                     return (operand.kind == MirOperandKind::Value ||
                             operand.kind == MirOperandKind::Constant) &&
                            kind &&
                            (*kind == CppMirTypeRepresentationKind::Scalar ||
                             *kind == CppMirTypeRepresentationKind::Enum);
                   }) ||
      body.usesOf(sourceId).size() != 1) {
    return {};
  }
  const std::vector<MirValueUse> sourceUses = nonRootRecordUses(body, sourceId);
  if (sourceUses.size() != 1 ||
      sourceUses.front().kind != MirValueUseKind::InstructionOperand) {
    return {};
  }
  const MirInstruction *resolvedStage =
      findInstruction(body, sourceUses.front().instruction);
  if (stage != nullptr && stage != resolvedStage) {
    return {};
  }
  stage = resolvedStage;
  if (stage == nullptr || stage->kind != MirInstructionKind::CallInput ||
      stage->callInputKind != HirCallInputKind::MoveValue || !stage->result ||
      stage->destination || stage->receiver || stage->operands.size() != 1 ||
      stage->operands.front().kind != MirOperandKind::Value ||
      stage->operands.front().value != sourceId ||
      stage->operands.front().type != source->info.type ||
      stage->info.type != source->info.type ||
      !stage->localFailureSites.empty() || !stage->lifecycle.empty() ||
      body.usesOf(*stage->result).size() != 1) {
    return {};
  }
  const std::vector<MirValueUse> stageUses =
      nonRootRecordUses(body, *stage->result);
  if (stageUses.size() != 1 ||
      stageUses.front().kind != MirValueUseKind::InstructionOperand) {
    return {};
  }
  const MirInstruction *consumer =
      findInstruction(body, stageUses.front().instruction);
  const MirConstructorInstance *outer =
      consumer != nullptr && consumer->constructorTarget
          ? program.findConstructorInstance(*consumer->constructorTarget)
          : nullptr;
  if (consumer == nullptr || consumer->kind != MirInstructionKind::Construct ||
      !consumer->result || consumer->destination || consumer->receiver ||
      consumer->constructorKind != ConstructorKind::Ordinary ||
      consumer->definedFailure.propagation !=
          FailurePropagationKind::Constructor ||
      stageUses.front().operandIndex >= consumer->operands.size() ||
      consumer->operands[stageUses.front().operandIndex].kind !=
          MirOperandKind::Value ||
      consumer->operands[stageUses.front().operandIndex].value !=
          *stage->result ||
      outer == nullptr ||
      !failureConstructorBoundaryEligible(program, *outer)) {
    return {};
  }
  for (const MirBlock &block : body.blocks) {
    for (std::size_t index = 0; index + 2 < block.instructions.size();
         ++index) {
      if (block.instructions[index].id == producer->id &&
          block.instructions[index + 1].id == stage->id &&
          block.instructions[index + 2].id == consumer->id) {
        return {.producer = producer, .stage = stage, .consumer = consumer};
      }
    }
  }
  return {};
}

[[nodiscard]] const MirDropObligation *
failureDestructorDrop(const MirBody &body, const MirInstruction &instruction) {
  if (instruction.kind != MirInstructionKind::Drop ||
      !instruction.destination || instruction.lifecycle.size() != 1 ||
      instruction.lifecycle.front().kind != MirLifecycleEventKind::Drop ||
      instruction.lifecycle.front().source == 0 ||
      instruction.lifecycle.front().target != 0 ||
      instruction.definedFailure.propagation !=
          FailurePropagationKind::Destructor ||
      !instruction.definedFailure.localOrigins.empty() ||
      !instruction.localFailureSites.empty()) {
    return nullptr;
  }
  const MirDropObligation *drop =
      body.findDropObligation(instruction.lifecycle.front().source);
  return drop != nullptr && drop->place == *instruction.destination &&
                 drop->dropType.requiresActiveCleanup &&
                 drop->dropType.destructor
             ? drop
             : nullptr;
}

[[nodiscard]] const MirDestructorInstance *
failureDestructorTarget(const MirProgram &program, const MirBody &body,
                        const MirInstruction &instruction) {
  const MirDropObligation *drop = failureDestructorDrop(body, instruction);
  const MirDestructorInstance *target =
      drop == nullptr || !drop->dropType.destructor
          ? nullptr
          : program.findDestructorInstance(*drop->dropType.destructor);
  const MirClassInstance *owner =
      target == nullptr ? nullptr : program.findClassInstance(target->owner);
  return target != nullptr && owner != nullptr &&
                 target->definitionKind == MirDefinitionKind::Source &&
                 target->mayRaiseDefinedFailure &&
                 drop->dropType.classInstance == owner->id &&
                 drop->dropType.type == owner->type &&
                 owner->requiresActiveDropState && owner->requiresActiveCleanup
             ? target
             : nullptr;
}

[[nodiscard]] const MirClassInstance *
uniqueClassInstanceForType(const MirProgram &program,
                           const SemanticType &type) {
  const MirClassInstance *found = nullptr;
  for (const MirClassInstance &instance : program.classInstances()) {
    if (instance.type != type) {
      continue;
    }
    if (found != nullptr) {
      return nullptr;
    }
    found = &instance;
  }
  return found;
}

struct ResolvedMirField {
  const MirClassInstance *owner = nullptr;
  const MirClassFieldInfo *field = nullptr;

  [[nodiscard]] explicit operator bool() const {
    return owner != nullptr && field != nullptr;
  }
};

// Resolve one globally identified field through the exact concrete base
// graph. MIR places retain the dynamic receiver type, while an inherited
// field row is owned by the base instance that declared it. Ambiguous or
// cyclic graphs fail closed rather than selecting a spelling by traversal
// order.
[[nodiscard]] ResolvedMirField resolveMirField(const MirProgram &program,
                                               const SemanticType &receiverType,
                                               SymbolId symbol) {
  const MirClassInstance *root =
      uniqueClassInstanceForType(program, receiverType);
  if (root == nullptr) {
    return {};
  }
  ResolvedMirField result;
  bool ambiguous = false;
  std::unordered_set<HirClassInstanceId> seen;
  const auto visit = [&](const auto &self,
                         const MirClassInstance &instance) -> void {
    if (ambiguous || !seen.insert(instance.id).second) {
      return;
    }
    for (const MirClassFieldInfo &field : instance.declaredFields) {
      if (field.symbol != symbol) {
        continue;
      }
      if (result) {
        ambiguous = true;
        return;
      }
      result = {.owner = &instance, .field = &field};
    }
    for (const HirBaseInstance &base : instance.structuralBases) {
      const MirClassInstance *baseInstance =
          program.findClassInstance(base.instance);
      if (baseInstance == nullptr) {
        ambiguous = true;
        return;
      }
      self(self, *baseInstance);
    }
  };
  visit(visit, *root);
  return ambiguous ? ResolvedMirField{} : result;
}

[[nodiscard]] bool classTypeIsOrDerivesFrom(const MirProgram &program,
                                            const SemanticType &derivedType,
                                            HirClassInstanceId baseId) {
  const MirClassInstance *derived =
      uniqueClassInstanceForType(program, derivedType);
  if (derived == nullptr || program.findClassInstance(baseId) == nullptr) {
    return false;
  }
  std::unordered_set<HirClassInstanceId> seen;
  const auto reaches = [&](const auto &self,
                           const MirClassInstance &instance) -> bool {
    if (!seen.insert(instance.id).second) {
      return false;
    }
    if (instance.id == baseId) {
      return true;
    }
    return std::any_of(
        instance.structuralBases.begin(), instance.structuralBases.end(),
        [&](const HirBaseInstance &base) {
          const MirClassInstance *baseInstance =
              program.findClassInstance(base.instance);
          return baseInstance != nullptr && self(self, *baseInstance);
        });
  };
  return reaches(reaches, *derived);
}

[[nodiscard]] const MirDropObligation *
uniqueValueDrop(const MirBody &body, MirValueId valueId,
                const SemanticType &type) {
  const MirDropObligation *found = nullptr;
  for (const MirDropObligation &drop : body.dropObligations) {
    const MirPlace *place = body.findPlace(drop.place);
    if (drop.kind != MirDropObligationKind::Value || place == nullptr ||
        place->root != MirPlaceRootKind::Value || place->value != valueId ||
        place->type != type || drop.dropType.type != type ||
        !drop.dropType.requiresActiveCleanup) {
      continue;
    }
    if (found != nullptr) {
      return nullptr;
    }
    found = &drop;
  }
  return found;
}

struct ConstructorFieldResultSlot {
  const MirValue *value = nullptr;
  const MirInstruction *producer = nullptr;
  const MirInstruction *initialize = nullptr;
  const MirPlace *slot = nullptr;
  const MirPlace *field = nullptr;
  const MirDropObligation *valueDrop = nullptr;
  const MirDropObligation *fieldDrop = nullptr;

  [[nodiscard]] explicit operator bool() const { return value != nullptr; }
};

// A failure-capable class call or constructor used as one constructor field
// initializer first owns its result in a Value-rooted MIR place. The
// successful Invoke edge arms that value, and the following Initialize
// reparents it into this.field. The transformed C++ constructor can preserve
// that schedule with one sealed result slot, a failure-free move assignment
// into the field's proved empty state, and immediate destruction of the
// moved-from shell.
//
// Keep this proof deliberately structural. In particular, it does not know a
// standard-library wrapper name: the field type must independently prove the
// generated empty/move lifecycle accepted by failure constructors.
[[nodiscard]] ConstructorFieldResultSlot
constructorFieldResultSlot(const MirProgram &program, const MirBody &body,
                           MirValueId valueId) {
  if (body.kind != MirBodyKind::Constructor) {
    return {};
  }

  const MirConstructorInstance *constructor = nullptr;
  for (const MirConstructorInstance &candidate :
       program.constructorInstances()) {
    if (&candidate.body != &body) {
      continue;
    }
    if (constructor != nullptr) {
      return {};
    }
    constructor = &candidate;
  }
  const MirClassInstance *owner =
      constructor == nullptr ? nullptr
                             : program.findClassInstance(constructor->owner);
  const MirValue *value = body.findValue(valueId);
  const MirInstruction *producer =
      value == nullptr ? nullptr : findInstruction(body, value->definition);
  const MirFunctionInstance *functionTarget =
      producer != nullptr && producer->functionTarget
          ? program.findFunctionInstance(*producer->functionTarget)
          : nullptr;
  const MirConstructorInstance *constructorTarget =
      producer != nullptr && producer->constructorTarget
          ? program.findConstructorInstance(*producer->constructorTarget)
          : nullptr;
  const MirClassInstance *constructedOwner =
      constructorTarget == nullptr
          ? nullptr
          : program.findClassInstance(constructorTarget->owner);
  const MirDropObligation *valueDrop =
      value == nullptr ? nullptr
                       : uniqueValueDrop(body, valueId, value->info.type);
  const bool commonProducer =
      value != nullptr && producer != nullptr && producer->result &&
      *producer->result == valueId && !producer->destination &&
      !producer->receiver && !producer->bodyTarget && !producer->lambdaTarget &&
      !producer->callableInvocation &&
      producer->intrinsic == IntrinsicKind::None &&
      producer->definedFailure.localOrigins.empty() &&
      producer->localFailureSites.empty() &&
      producer->callableArguments.empty() && producer->lifecycle.empty() &&
      !producer->successResultDestination &&
      producer->parameterTypes.size() == producer->operands.size() &&
      std::all_of(producer->operands.begin(), producer->operands.end(),
                  [](const MirOperand &operand) {
                    return operand.kind == MirOperandKind::Value;
                  });
  const bool functionProducer =
      commonProducer && producer->kind == MirInstructionKind::Call &&
      producer->functionTarget && !producer->constructorTarget &&
      producer->dispatch == CallDispatch::Static &&
      producer->definedFailure.propagation ==
          FailurePropagationKind::DirectCall &&
      functionTarget != nullptr &&
      functionTarget->returnType == value->info.type &&
      functionTarget->parameterTypes == producer->parameterTypes &&
      functionTarget->mayRaiseDefinedFailure &&
      functionTarget->definitionKind == MirDefinitionKind::Source &&
      functionTarget->linkage == LanguageLinkage::Gti;
  const bool constructorProducer =
      commonProducer && producer->kind == MirInstructionKind::Construct &&
      !producer->functionTarget && producer->constructorTarget &&
      producer->constructorKind == ConstructorKind::Ordinary &&
      producer->definedFailure.propagation ==
          FailurePropagationKind::Constructor &&
      constructorTarget != nullptr && constructedOwner != nullptr &&
      constructorTarget->parameterTypes == producer->parameterTypes &&
      constructorTarget->mayRaiseDefinedFailure &&
      constructorTarget->definitionKind == MirDefinitionKind::Source &&
      constructedOwner->type == value->info.type &&
      failureConstructorBoundaryEligible(program, *constructorTarget);
  if (constructor == nullptr || owner == nullptr || value == nullptr ||
      value->info.type.kind != SemanticType::Class ||
      value->info.category != ValueCategory::Value ||
      value->info.traits.drop != DropKind::Lexical ||
      (!functionProducer && !constructorProducer) || valueDrop == nullptr ||
      valueDrop->initiallyActive ||
      producer->successResultDrop != valueDrop->id ||
      !invokeSuccessActivates(body, *producer, valueDrop->id)) {
    return {};
  }

  const MirPlace *slot = body.findPlace(valueDrop->place);
  if (slot == nullptr || slot->root != MirPlaceRootKind::Value ||
      slot->value != valueId || !slot->projections.empty() ||
      slot->type != value->info.type ||
      slot->traits.drop != DropKind::Lexical || slot->initiallyAvailable) {
    return {};
  }

  const std::vector<MirValueUse> uses = nonRootRecordUses(body, valueId);
  if (uses.size() != 1 ||
      uses.front().kind != MirValueUseKind::InstructionOperand ||
      uses.front().operandIndex != 0) {
    return {};
  }
  const MirInstruction *initialize =
      findInstruction(body, uses.front().instruction);
  if (initialize == nullptr ||
      initialize->kind != MirInstructionKind::Initialize ||
      initialize->constructorInitializer == 0 || !initialize->destination ||
      initialize->result || initialize->receiver ||
      initialize->operands.size() != 1 ||
      initialize->operands.front().kind != MirOperandKind::Value ||
      initialize->operands.front().value != valueId ||
      initialize->operands.front().type != value->info.type ||
      initialize->info.type != value->info.type ||
      !initialize->localFailureSites.empty() ||
      !initialize->definedFailure.empty() ||
      initialize->lifecycle.size() != 1) {
    return {};
  }

  const MirPlace *field = body.findPlace(*initialize->destination);
  const MirLifecycleEvent &reparent = initialize->lifecycle.front();
  const MirDropObligation *fieldDrop = body.findDropObligation(reparent.target);
  if (field == nullptr || field->root != MirPlaceRootKind::This ||
      field->projections.size() != 1 ||
      field->projections.front().kind != MirProjectionKind::Field ||
      field->type != value->info.type ||
      field->traits.drop != DropKind::Lexical || field->initiallyAvailable ||
      reparent.kind != MirLifecycleEventKind::Reparent ||
      reparent.source != valueDrop->id || reparent.target == 0 ||
      reparent.conditional || reparent.failureCleanup || fieldDrop == nullptr ||
      fieldDrop->kind != MirDropObligationKind::ConstructionRollback ||
      fieldDrop->place != field->id ||
      fieldDrop->dropType.type != field->type || fieldDrop->initiallyActive ||
      !fieldDrop->dropType.requiresActiveCleanup) {
    return {};
  }

  const std::size_t initializerIndex = initialize->constructorInitializer - 1;
  if (initializerIndex >= constructor->initializers.size()) {
    return {};
  }
  const MirConstructorInitializer &initializer =
      constructor->initializers[initializerIndex];
  const auto declaredField = std::find_if(
      owner->declaredFields.begin(), owner->declaredFields.end(),
      [&](const MirClassFieldInfo &candidate) {
        return candidate.symbol == field->projections.front().field;
      });
  if (declaredField == owner->declaredFields.end() ||
      declaredField->type != field->type ||
      declaredField->dropKind != DropKind::Lexical ||
      !declaredField->requiresActiveCleanup ||
      initializer.kind != ConstructorInitializerTargetKind::Field ||
      initializer.field != declaredField->symbol ||
      initializer.targetType != field->type || initializer.generatedDefault ||
      initializer.constructorTarget || initializer.storesReference ||
      initializer.ownedParameter || initializer.arguments.size() != 1 ||
      producer->hirValue == 0 ||
      initializer.arguments.front() != producer->hirValue ||
      initialize->hirValue != producer->hirValue ||
      !failureConstructorEmptyDefaultFieldType(program, field->type)) {
    return {};
  }

  const MirFullExpression *fullExpression = nullptr;
  for (const MirFullExpression &candidate : body.fullExpressions) {
    if (candidate.constructorInitializer !=
        initialize->constructorInitializer) {
      continue;
    }
    if (fullExpression != nullptr || candidate.roots != initializer.arguments) {
      return {};
    }
    fullExpression = &candidate;
  }
  if (fullExpression == nullptr ||
      valueDrop->fullExpression != fullExpression->id) {
    return {};
  }

  std::size_t fieldPublications = 0;
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      for (const MirLifecycleEvent &event : instruction.lifecycle) {
        if (event.source != fieldDrop->id && event.target != fieldDrop->id) {
          continue;
        }
        if (&instruction == initialize && event == reparent) {
          continue;
        }
        if (event.kind != MirLifecycleEventKind::TransferOut ||
            event.source != fieldDrop->id || event.target != 0 ||
            event.conditional || event.failureCleanup) {
          return {};
        }
        ++fieldPublications;
      }
    }
  }
  if (fieldPublications != 1) {
    return {};
  }

  return {.value = value,
          .producer = producer,
          .initialize = initialize,
          .slot = slot,
          .field = field,
          .valueDrop = valueDrop,
          .fieldDrop = fieldDrop};
}

struct GeneratedSpecialMemberConstruction {
  const MirPlace *source = nullptr;
  const MirPlace *destination = nullptr;
  const MirInstruction *initialize = nullptr;
  bool moved = false;
};

// A generated copy/move has no source constructor body to reopen. This is the
// complete bounded schedule the backend may use instead: one exact source
// place, one generated Construct, immediate publication into an empty lexical
// binding, activation of that binding's drop identity, and an explicit
// cleanup of the identity on an exit path. The copy and move policies come
// from MIR's concrete class row rather than from a C++ spelling heuristic.
[[nodiscard]] std::optional<GeneratedSpecialMemberConstruction>
generatedSpecialMemberConstruction(const MirProgram &program,
                                   const MirBody &body,
                                   const MirInstruction &construct) {
  if (construct.kind != MirInstructionKind::Construct ||
      (construct.constructorKind != ConstructorKind::Copy &&
       construct.constructorKind != ConstructorKind::Move) ||
      construct.callSite != 0 || !construct.result || construct.destination ||
      construct.receiver || construct.operands.size() != 1 ||
      construct.parameterTypes.size() != 1 || construct.functionTarget ||
      construct.constructorTarget || construct.bodyTarget ||
      construct.lambdaTarget || construct.callableInvocation ||
      construct.intrinsic != IntrinsicKind::None ||
      construct.info.type.kind != SemanticType::Class ||
      !construct.definedFailure.empty() ||
      !construct.localFailureSites.empty() || !construct.lifecycle.empty()) {
    return std::nullopt;
  }

  const SemanticType &classType = construct.info.type;
  const SemanticType &parameter = construct.parameterTypes.front();
  const MirClassInstance *owner =
      uniqueClassInstanceForType(program, classType);
  const MirValue *result = body.findValue(*construct.result);
  if (owner == nullptr || result == nullptr ||
      result->definition != construct.id || result->info.type != classType ||
      parameter.kind != SemanticType::Reference ||
      parameter.arguments.size() != 1 ||
      parameter.arguments.front() != classType) {
    return std::nullopt;
  }

  const MirOperand &operand = construct.operands.front();
  const bool moved = construct.constructorKind == ConstructorKind::Move;
  const MirPlace *source = nullptr;
  if (!moved) {
    if (owner->copyConstructor == SpecialMemberStatus::Deleted ||
        !owner->traits.copyable || !result->info.traits.copyable ||
        parameter.referenceAccess != AccessMode::ReadOnly ||
        operand.kind != MirOperandKind::BorrowRead || operand.place == 0 ||
        operand.type != parameter) {
      return std::nullopt;
    }
    source = body.findPlace(operand.place);
  } else {
    if (owner->moveConstructor == SpecialMemberStatus::Deleted ||
        !owner->traits.movable || !result->info.traits.movable ||
        operand.kind != MirOperandKind::Value || operand.value == 0 ||
        operand.type != classType) {
      return std::nullopt;
    }
    source = movedPlaceChainSource(body, operand.value, construct);
  }
  if (source == nullptr || source->root != MirPlaceRootKind::Binding ||
      !source->projections.empty() || source->type != classType) {
    return std::nullopt;
  }

  const std::vector<MirValueUse> uses =
      nonRootRecordUses(body, *construct.result);
  if (uses.size() != 1 ||
      uses.front().kind != MirValueUseKind::InstructionOperand ||
      uses.front().operandIndex != 0) {
    return std::nullopt;
  }
  const MirInstruction *initialize =
      findInstruction(body, uses.front().instruction);
  const MirPlace *destination = initialize != nullptr && initialize->destination
                                    ? body.findPlace(*initialize->destination)
                                    : nullptr;
  if (initialize == nullptr || destination == nullptr ||
      source == destination ||
      initialize->kind != MirInstructionKind::Initialize ||
      initialize->operands.size() != 1 ||
      initialize->operands.front().kind != MirOperandKind::Value ||
      initialize->operands.front().value != *construct.result ||
      initialize->operands.front().type != classType || initialize->result ||
      !initialize->localFailureSites.empty() ||
      !initialize->definedFailure.empty() ||
      destination->root != MirPlaceRootKind::Binding ||
      !destination->projections.empty() || destination->type != classType ||
      destination->traits.drop != DropKind::Lexical ||
      destination->initiallyAvailable || initialize->info.type != classType ||
      initialize->lifecycle.size() != 1) {
    return std::nullopt;
  }

  bool adjacent = false;
  for (const MirBlock &block : body.blocks) {
    for (std::size_t index = 0; index + 1 < block.instructions.size();
         ++index) {
      if (block.instructions[index].id == construct.id &&
          block.instructions[index + 1].id == initialize->id) {
        adjacent = true;
      }
    }
  }
  if (!adjacent) {
    return std::nullopt;
  }

  const MirDropObligation *bindingDrop = nullptr;
  for (const MirDropObligation &drop : body.dropObligations) {
    const MirPlace *dropPlace = body.findPlace(drop.place);
    if (drop.generatedValue == *construct.result ||
        (dropPlace != nullptr && dropPlace->root == MirPlaceRootKind::Value &&
         dropPlace->value == *construct.result)) {
      return std::nullopt;
    }
    if (drop.kind != MirDropObligationKind::Binding ||
        drop.place != destination->id || drop.binding != destination->binding ||
        drop.dropType.type != classType ||
        drop.dropType.classInstance != owner->id) {
      continue;
    }
    if (bindingDrop != nullptr) {
      return std::nullopt;
    }
    bindingDrop = &drop;
  }
  if (bindingDrop == nullptr || bindingDrop->initiallyActive) {
    return std::nullopt;
  }
  const MirLifecycleEvent &activation = initialize->lifecycle.front();
  if (activation.kind != MirLifecycleEventKind::Initialize ||
      activation.source != 0 || activation.target != bindingDrop->id ||
      activation.conditional || activation.failureCleanup) {
    return std::nullopt;
  }

  const bool scheduledBoundary = std::any_of(
      body.cleanupBoundaries.begin(), body.cleanupBoundaries.end(),
      [&](const MirCleanupBoundary &boundary) {
        return std::find(boundary.obligations.begin(),
                         boundary.obligations.end(),
                         bindingDrop->id) != boundary.obligations.end();
      });
  const bool scheduledDrop = std::any_of(
      body.blocks.begin(), body.blocks.end(), [&](const MirBlock &block) {
        return std::any_of(
            block.instructions.begin(), block.instructions.end(),
            [&](const MirInstruction &instruction) {
              return instruction.kind == MirInstructionKind::Drop &&
                     instruction.destination == destination->id &&
                     instruction.info.type == classType &&
                     instruction.lifecycle.size() == 1 &&
                     instruction.lifecycle.front().kind ==
                         MirLifecycleEventKind::Drop &&
                     instruction.lifecycle.front().source == bindingDrop->id &&
                     instruction.lifecycle.front().target == 0 &&
                     !instruction.lifecycle.front().conditional;
            });
      });
  if (!scheduledBoundary || !scheduledDrop) {
    return std::nullopt;
  }

  return GeneratedSpecialMemberConstruction{source, destination, initialize,
                                            moved};
}

// A fixed-array aggregate needs partial-initialization rollback only when an
// element move can fail. This bounded form proves the opposite from MIR:
// every staged element has an exact owning drop identity, every identity
// transfers out in operand order, the result receives one exact identity, and
// the element type's move is structurally defined-failure-free.
[[nodiscard]] bool
failureFreeFixedArrayAggregateMove(const MirProgram &program,
                                   const MirBody &body,
                                   const MirInstruction &instruction) {
  const bool elementMoveDefinedFailureFree =
      instruction.info.type.kind == SemanticType::Array &&
      instruction.info.type.arguments.size() == 1 &&
      mirTypeMoveIsDefinedFailureFree(program,
                                      instruction.info.type.arguments.front());
  if (instruction.kind != MirInstructionKind::Compute ||
      instruction.operation != MirOperation::Aggregate || !instruction.result ||
      instruction.info.type.kind != SemanticType::Array ||
      instruction.info.type.arguments.size() != 1 ||
      instruction.info.type.arrayLengthParameterId != 0 ||
      instruction.info.type.arrayLength == 0 ||
      instruction.operands.size() != instruction.info.type.arrayLength ||
      instruction.lifecycle.size() != instruction.operands.size() + 1 ||
      instruction.literal || !instruction.definedFailure.empty() ||
      !instruction.localFailureSites.empty() ||
      !elementMoveDefinedFailureFree) {
    return false;
  }

  std::unordered_set<MirDropObligationId> transferred;
  const SemanticType &elementType = instruction.info.type.arguments.front();
  for (std::size_t index = 0; index < instruction.operands.size(); ++index) {
    const MirOperand &operand = instruction.operands[index];
    const MirValue *value = operand.kind == MirOperandKind::Value
                                ? body.findValue(operand.value)
                                : nullptr;
    const MirDropObligation *drop =
        value == nullptr ? nullptr
                         : uniqueValueDrop(body, value->id, elementType);
    const MirLifecycleEvent &event = instruction.lifecycle[index];
    if (operand.type != elementType || value == nullptr ||
        value->info.type != elementType || drop == nullptr ||
        !transferred.insert(drop->id).second ||
        event.kind != MirLifecycleEventKind::TransferOut ||
        event.source != drop->id || event.target != 0 || event.conditional ||
        event.failureCleanup) {
      return false;
    }
  }

  const MirDropObligation *resultDrop =
      uniqueValueDrop(body, *instruction.result, instruction.info.type);
  const MirLifecycleEvent &initialize = instruction.lifecycle.back();
  return resultDrop != nullptr &&
         initialize.kind == MirLifecycleEventKind::Initialize &&
         initialize.source == 0 && initialize.target == resultDrop->id &&
         !initialize.conditional && !initialize.failureCleanup;
}

struct PassiveFixedArrayConstructAggregate {
  const MirInstruction *aggregate = nullptr;
  const MirInstruction *initialize = nullptr;
  const MirPlace *destination = nullptr;
  std::vector<const MirInstruction *> elements;

  [[nodiscard]] explicit operator bool() const {
    return aggregate != nullptr && initialize != nullptr &&
           destination != nullptr && !elements.empty();
  }
};

// A fixed array of cleanup-free class prvalues has no partial-drop protocol in
// MIR: each failure-free Construct is consumed once by the Aggregate, and the
// Aggregate initializes one binding. Emit those constructors directly in the
// braced initializer only when doing so crosses pure value staging and keeps
// the exact MIR constructor order.
[[nodiscard]] PassiveFixedArrayConstructAggregate
passiveFixedArrayConstructAggregate(const MirProgram &program,
                                    const MirBody &body,
                                    const MirInstruction &instruction) {
  if (instruction.kind != MirInstructionKind::Compute ||
      instruction.operation != MirOperation::Aggregate || !instruction.result ||
      instruction.destination || instruction.receiver ||
      instruction.info.type.kind != SemanticType::Array ||
      instruction.info.type.arguments.size() != 1 ||
      instruction.info.type.arguments.front().kind != SemanticType::Class ||
      instruction.info.type.arrayLengthParameterId != 0 ||
      instruction.info.type.arrayLength == 0 ||
      instruction.operands.size() != instruction.info.type.arrayLength ||
      instruction.literal || !instruction.localFailureSites.empty() ||
      !instruction.definedFailure.empty() || !instruction.lifecycle.empty() ||
      !mirTypeMoveIsDefinedFailureFree(
          program, instruction.info.type.arguments.front())) {
    return {};
  }

  const MirBlock *aggregateBlock = nullptr;
  std::size_t aggregateIndex = 0;
  for (const MirBlock &block : body.blocks) {
    for (std::size_t index = 0; index < block.instructions.size(); ++index) {
      if (block.instructions[index].id != instruction.id) {
        continue;
      }
      if (aggregateBlock != nullptr) {
        return {};
      }
      aggregateBlock = &block;
      aggregateIndex = index;
    }
  }
  if (aggregateBlock == nullptr) {
    return {};
  }

  const SemanticType &elementType = instruction.info.type.arguments.front();
  std::vector<const MirInstruction *> elements;
  std::vector<std::size_t> elementIndices;
  std::unordered_set<MirInstructionId> elementIds;
  elements.reserve(instruction.operands.size());
  elementIndices.reserve(instruction.operands.size());
  for (std::size_t operandIndex = 0; operandIndex < instruction.operands.size();
       ++operandIndex) {
    const MirOperand &operand = instruction.operands[operandIndex];
    const MirValue *value = operand.kind == MirOperandKind::Value
                                ? body.findValue(operand.value)
                                : nullptr;
    const MirInstruction *construct =
        value == nullptr ? nullptr : findInstruction(body, value->definition);
    const MirConstructorInstance *constructor =
        construct != nullptr && construct->constructorTarget
            ? program.findConstructorInstance(*construct->constructorTarget)
            : nullptr;
    const MirClassInstance *owner =
        constructor == nullptr ? nullptr
                               : program.findClassInstance(constructor->owner);
    if (operand.type != elementType || value == nullptr ||
        value->info.type != elementType || body.usesOf(value->id).size() != 1 ||
        body.usesOf(value->id).front().kind !=
            MirValueUseKind::InstructionOperand ||
        body.usesOf(value->id).front().instruction != instruction.id ||
        body.usesOf(value->id).front().operandIndex != operandIndex ||
        construct == nullptr ||
        construct->kind != MirInstructionKind::Construct ||
        !construct->result || *construct->result != value->id ||
        construct->destination || construct->receiver ||
        construct->info.type != elementType || !construct->constructorTarget ||
        constructor == nullptr || constructor->owner == 0 ||
        constructor->definitionKind != MirDefinitionKind::Source ||
        constructor->mayRaiseDefinedFailure || owner == nullptr ||
        owner->type != elementType ||
        construct->parameterTypes != constructor->parameterTypes ||
        construct->operands.size() != construct->parameterTypes.size() ||
        !construct->localFailureSites.empty() ||
        !construct->definedFailure.empty() || !construct->lifecycle.empty() ||
        construct->fullExpressionEnd != 0 ||
        construct->cleanupBoundaryEnd != 0 ||
        std::any_of(body.places.begin(), body.places.end(),
                    [&](const MirPlace &place) {
                      return place.root == MirPlaceRootKind::Value &&
                             place.value == value->id;
                    }) ||
        std::any_of(body.dropObligations.begin(), body.dropObligations.end(),
                    [&](const MirDropObligation &drop) {
                      return drop.generatedValue == value->id;
                    })) {
      return {};
    }
    for (std::size_t index = 0; index < construct->operands.size(); ++index) {
      const MirOperand &argument = construct->operands[index];
      if (argument.kind != MirOperandKind::Value || argument.value == 0 ||
          argument.type != construct->parameterTypes[index] ||
          argument.type.kind == SemanticType::Class ||
          argument.type.kind == SemanticType::Lambda ||
          argument.type.kind == SemanticType::UniqueOwner ||
          argument.type.kind == SemanticType::SharedPointer ||
          argument.type.kind == SemanticType::Storage ||
          argument.type.kind == SemanticType::PrefixStorage ||
          argument.type.kind == SemanticType::Expected) {
        return {};
      }
    }

    const auto found =
        std::find_if(aggregateBlock->instructions.begin(),
                     aggregateBlock->instructions.begin() + aggregateIndex,
                     [&](const MirInstruction &candidate) {
                       return candidate.id == construct->id;
                     });
    if (found == aggregateBlock->instructions.begin() + aggregateIndex) {
      return {};
    }
    const std::size_t index = static_cast<std::size_t>(
        std::distance(aggregateBlock->instructions.begin(), found));
    if ((!elementIndices.empty() && index <= elementIndices.back()) ||
        !elementIds.insert(construct->id).second) {
      return {};
    }
    elements.push_back(construct);
    elementIndices.push_back(index);
  }

  for (std::size_t index = elementIndices.front(); index < aggregateIndex;
       ++index) {
    const MirInstruction &candidate = aggregateBlock->instructions[index];
    if (elementIds.contains(candidate.id)) {
      continue;
    }
    const bool pureLiteral =
        candidate.kind == MirInstructionKind::Compute && candidate.result &&
        candidate.operation == MirOperation::Literal && candidate.literal &&
        candidate.operands.empty() && candidate.localFailureSites.empty() &&
        candidate.definedFailure.empty() && candidate.lifecycle.empty();
    const bool pureStage =
        candidate.kind == MirInstructionKind::CallInput && candidate.result &&
        !candidate.destination && !candidate.receiver &&
        candidate.operands.size() == 1 &&
        candidate.operands.front().kind == MirOperandKind::Value &&
        candidate.localFailureSites.empty() &&
        candidate.definedFailure.empty() && candidate.lifecycle.empty();
    if (!pureLiteral && !pureStage) {
      return {};
    }
  }

  if (std::any_of(body.places.begin(), body.places.end(),
                  [&](const MirPlace &place) {
                    return place.root == MirPlaceRootKind::Value &&
                           place.value == *instruction.result;
                  }) ||
      std::any_of(body.dropObligations.begin(), body.dropObligations.end(),
                  [&](const MirDropObligation &drop) {
                    return drop.generatedValue == *instruction.result;
                  })) {
    return {};
  }
  const std::vector<MirValueUse> &uses = body.usesOf(*instruction.result);
  if (uses.size() != 1 ||
      uses.front().kind != MirValueUseKind::InstructionOperand ||
      uses.front().operandIndex != 0) {
    return {};
  }
  const MirInstruction *initialize =
      findInstruction(body, uses.front().instruction);
  const MirPlace *destination = initialize != nullptr && initialize->destination
                                    ? body.findPlace(*initialize->destination)
                                    : nullptr;
  const MirDropObligation *bindingDrop = nullptr;
  if (destination != nullptr) {
    for (const MirDropObligation &drop : body.dropObligations) {
      if (drop.kind != MirDropObligationKind::Binding ||
          drop.place != destination->id ||
          drop.dropType.type != instruction.info.type) {
        continue;
      }
      if (bindingDrop != nullptr) {
        return {};
      }
      bindingDrop = &drop;
    }
  }
  if (initialize == nullptr || destination == nullptr ||
      bindingDrop == nullptr || bindingDrop->initiallyActive ||
      initialize->kind != MirInstructionKind::Initialize ||
      initialize->operands.size() != 1 ||
      initialize->operands.front().kind != MirOperandKind::Value ||
      initialize->operands.front().value != *instruction.result ||
      initialize->operands.front().type != instruction.info.type ||
      destination->root != MirPlaceRootKind::Binding ||
      !destination->projections.empty() ||
      destination->type != instruction.info.type ||
      initialize->info.type != instruction.info.type ||
      initialize->lifecycle.size() != 1) {
    return {};
  }
  const MirLifecycleEvent &activation = initialize->lifecycle.front();
  return activation.kind == MirLifecycleEventKind::Initialize &&
                 activation.source == 0 &&
                 activation.target == bindingDrop->id &&
                 !activation.conditional && !activation.failureCleanup
             ? PassiveFixedArrayConstructAggregate{.aggregate = &instruction,
                                                   .initialize = initialize,
                                                   .destination = destination,
                                                   .elements =
                                                       std::move(elements)}
             : PassiveFixedArrayConstructAggregate{};
}

[[nodiscard]] const MirInstruction *
passiveFixedArrayConstructInput(const MirProgram &program, const MirBody &body,
                                MirValueId valueId) {
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      const PassiveFixedArrayConstructAggregate aggregate =
          passiveFixedArrayConstructAggregate(program, body, instruction);
      if (!aggregate) {
        continue;
      }
      const auto found =
          std::find_if(aggregate.elements.begin(), aggregate.elements.end(),
                       [&](const MirInstruction *element) {
                         return element != nullptr && element->result &&
                                *element->result == valueId;
                       });
      if (found != aggregate.elements.end()) {
        return aggregate.aggregate;
      }
    }
  }
  return nullptr;
}

[[nodiscard]] const MirInstruction *passiveFixedArrayConstructDestination(
    const MirProgram &program, const MirBody &body, const MirPlace &place) {
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      const PassiveFixedArrayConstructAggregate aggregate =
          passiveFixedArrayConstructAggregate(program, body, instruction);
      if (aggregate && aggregate.destination->id == place.id) {
        return aggregate.aggregate;
      }
    }
  }
  return nullptr;
}

[[nodiscard]] const MirPlace *
fixedArrayAggregateDestinationSlot(const MirProgram &program,
                                   const MirBody &body,
                                   const MirInstruction &instruction) {
  if (!failureFreeFixedArrayAggregateMove(program, body, instruction) ||
      !instruction.result) {
    return nullptr;
  }
  const std::vector<MirValueUse> uses =
      nonRootRecordUses(body, *instruction.result);
  if (uses.size() != 1 ||
      uses.front().kind != MirValueUseKind::InstructionOperand) {
    return nullptr;
  }
  const MirInstruction *initialize =
      findInstruction(body, uses.front().instruction);
  const MirPlace *destination = initialize != nullptr && initialize->destination
                                    ? body.findPlace(*initialize->destination)
                                    : nullptr;
  const MirDropObligation *valueDrop =
      uniqueValueDrop(body, *instruction.result, instruction.info.type);
  const MirDropObligation *bindingDrop = nullptr;
  if (destination != nullptr) {
    for (const MirDropObligation &drop : body.dropObligations) {
      if (drop.kind != MirDropObligationKind::Binding ||
          drop.place != destination->id) {
        continue;
      }
      if (bindingDrop != nullptr) {
        return nullptr;
      }
      bindingDrop = &drop;
    }
  }
  return initialize != nullptr && destination != nullptr &&
                 valueDrop != nullptr && bindingDrop != nullptr &&
                 uses.front().operandIndex == 0 &&
                 initialize->kind == MirInstructionKind::Initialize &&
                 initialize->operands.size() == 1 &&
                 initialize->operands.front().kind == MirOperandKind::Value &&
                 initialize->operands.front().value == *instruction.result &&
                 destination->root == MirPlaceRootKind::Binding &&
                 destination->projections.empty() &&
                 destination->type == instruction.info.type &&
                 destination->traits.drop == DropKind::Lexical &&
                 initialize->lifecycle.size() == 1 &&
                 initialize->lifecycle.front().kind ==
                     MirLifecycleEventKind::Reparent &&
                 initialize->lifecycle.front().source == valueDrop->id &&
                 initialize->lifecycle.front().target == bindingDrop->id &&
                 !initialize->lifecycle.front().conditional &&
                 !initialize->lifecycle.front().failureCleanup
             ? destination
             : nullptr;
}

// A transformed class-returning call used as one element of the bounded
// aggregate above needs uninitialized SSA storage. The call constructs this
// slot through its out parameter; the aggregate moves from it and destroys
// the moved-from shell before the slot leaves scope.
[[nodiscard]] const MirInstruction *
fixedArrayAggregateInputSlot(const MirProgram &program, const MirBody &body,
                             MirValueId valueId) {
  const MirValue *value = body.findValue(valueId);
  const MirInstruction *definition =
      value == nullptr ? nullptr : findInstruction(body, value->definition);
  const MirFunctionInstance *target =
      definition != nullptr && definition->functionTarget
          ? program.findFunctionInstance(*definition->functionTarget)
          : nullptr;
  const MirDropObligation *valueDrop =
      value == nullptr ? nullptr
                       : uniqueValueDrop(body, valueId, value->info.type);
  if (value == nullptr || value->info.type.kind != SemanticType::Class ||
      value->info.traits.drop != DropKind::Lexical || definition == nullptr ||
      definition->kind != MirInstructionKind::Call ||
      definition->result != valueId || !definition->functionTarget ||
      definition->intrinsic != IntrinsicKind::None || target == nullptr ||
      !target->mayRaiseDefinedFailure ||
      target->definitionKind != MirDefinitionKind::Source ||
      target->linkage != LanguageLinkage::Gti ||
      !invokePairedInstruction(body, definition->id) || valueDrop == nullptr ||
      definition->successResultDrop != valueDrop->id) {
    return nullptr;
  }

  std::vector<MirValueUse> uses;
  bool sawOwningRoot = false;
  for (const MirValueUse &use : body.usesOf(valueId)) {
    if (use.kind != MirValueUseKind::PlaceRoot) {
      uses.push_back(use);
      continue;
    }
    const MirPlace *root = body.findPlace(use.place);
    if (sawOwningRoot || root == nullptr || root->id != valueDrop->place ||
        root->root != MirPlaceRootKind::Value || root->value != valueId ||
        !root->projections.empty() || root->type != value->info.type) {
      return nullptr;
    }
    sawOwningRoot = true;
  }
  if (!sawOwningRoot || uses.size() != 1 ||
      uses.front().kind != MirValueUseKind::InstructionOperand) {
    return nullptr;
  }
  const MirInstruction *aggregate =
      findInstruction(body, uses.front().instruction);
  if (aggregate == nullptr ||
      !failureFreeFixedArrayAggregateMove(program, body, *aggregate) ||
      uses.front().operandIndex >= aggregate->operands.size() ||
      aggregate->operands[uses.front().operandIndex].value != valueId) {
    return nullptr;
  }
  return aggregate;
}

// A resolved Expected<T, E> initialization may consume a transformed
// class-returning call directly. Both the class result and the Expected
// binding need sealed storage: the call constructs the result only on its
// success edge, then this Initialize move-constructs the Expected and retires
// the moved-from result shell. This is a type-directed MIR shape, not a
// conversion inferred by the backend.
[[nodiscard]] const MirInstruction *
expectedPayloadInitialize(const MirProgram &program, const MirBody &body,
                          MirValueId valueId) {
  const MirValue *value = body.findValue(valueId);
  const MirInstruction *definition =
      value == nullptr ? nullptr : findInstruction(body, value->definition);
  const MirFunctionInstance *target =
      definition != nullptr && definition->functionTarget
          ? program.findFunctionInstance(*definition->functionTarget)
          : nullptr;
  const MirDropObligation *valueDrop =
      value == nullptr ? nullptr
                       : uniqueValueDrop(body, valueId, value->info.type);
  if (value == nullptr || value->info.type.kind != SemanticType::Class ||
      value->info.traits.drop != DropKind::Lexical || definition == nullptr ||
      definition->kind != MirInstructionKind::Call ||
      definition->result != valueId || !definition->functionTarget ||
      definition->intrinsic != IntrinsicKind::None || target == nullptr ||
      !target->mayRaiseDefinedFailure ||
      target->definitionKind != MirDefinitionKind::Source ||
      target->linkage != LanguageLinkage::Gti ||
      !invokePairedInstruction(body, definition->id) || valueDrop == nullptr ||
      definition->successResultDrop != valueDrop->id ||
      !mirTypeMoveIsDefinedFailureFree(program, value->info.type)) {
    return nullptr;
  }

  const std::vector<MirValueUse> uses = nonRootRecordUses(body, valueId);
  if (uses.size() != 1 ||
      uses.front().kind != MirValueUseKind::InstructionOperand ||
      uses.front().operandIndex != 0) {
    return nullptr;
  }
  const MirInstruction *initialize =
      findInstruction(body, uses.front().instruction);
  const MirPlace *destination = initialize != nullptr && initialize->destination
                                    ? body.findPlace(*initialize->destination)
                                    : nullptr;
  const MirDropObligation *bindingDrop = nullptr;
  if (destination != nullptr) {
    for (const MirDropObligation &drop : body.dropObligations) {
      if (drop.kind != MirDropObligationKind::Binding ||
          drop.place != destination->id) {
        continue;
      }
      if (bindingDrop != nullptr) {
        return nullptr;
      }
      bindingDrop = &drop;
    }
  }
  if (initialize == nullptr || destination == nullptr ||
      bindingDrop == nullptr ||
      initialize->kind != MirInstructionKind::Initialize ||
      initialize->operands.size() != 1 ||
      initialize->operands.front().kind != MirOperandKind::Value ||
      initialize->operands.front().value != valueId ||
      initialize->operands.front().type != value->info.type ||
      destination->root != MirPlaceRootKind::Binding ||
      !destination->projections.empty() ||
      destination->type.kind != SemanticType::Expected ||
      destination->type.arguments.size() != 2 ||
      destination->type.arguments.front() != value->info.type ||
      destination->traits.drop != DropKind::Lexical ||
      initialize->info.type != destination->type ||
      initialize->lifecycle.size() != 2) {
    return nullptr;
  }
  const MirLifecycleEvent &transfer = initialize->lifecycle.front();
  const MirLifecycleEvent &activate = initialize->lifecycle.back();
  return transfer.kind == MirLifecycleEventKind::TransferOut &&
                 transfer.source == valueDrop->id && transfer.target == 0 &&
                 !transfer.conditional && !transfer.failureCleanup &&
                 activate.kind == MirLifecycleEventKind::Initialize &&
                 activate.source == 0 && activate.target == bindingDrop->id &&
                 !activate.conditional && !activate.failureCleanup
             ? initialize
             : nullptr;
}

struct ExpectedDefaultPayloadInitialization {
  const MirInstruction *producer = nullptr;
  const MirInstruction *initialize = nullptr;
  const MirPlace *destination = nullptr;

  [[nodiscard]] explicit operator bool() const { return producer != nullptr; }
};

// A generated default construction can initialize Expected<T, E> without an
// intermediate payload obligation. The targetless Call has exactly one use,
// and the Expected binding's Initialize activates the destination obligation.
// This remains separate from expectedPayloadInitialize(), whose transformed
// call result owns a payload obligation that must be transferred and retired.
[[nodiscard]] ExpectedDefaultPayloadInitialization
expectedDefaultPayloadInitialization(const MirProgram &program,
                                     const MirBody &body, MirValueId valueId) {
  const MirValue *value = body.findValue(valueId);
  const MirInstruction *producer =
      value == nullptr ? nullptr : findInstruction(body, value->definition);
  if (value == nullptr || value->info.type.kind != SemanticType::Class ||
      value->info.category != ValueCategory::Value || producer == nullptr ||
      producer->kind != MirInstructionKind::Call ||
      producer->result != valueId || producer->functionTarget ||
      producer->constructorTarget || producer->lambdaTarget ||
      producer->bodyTarget || producer->callableInvocation ||
      producer->receiver ||
      (producer->intrinsic != IntrinsicKind::None &&
       producer->intrinsic !=
           IntrinsicKind::DefaultTypeParameterConstruction) ||
      !producer->operands.empty() || !producer->callableArguments.empty() ||
      !producer->localFailureSites.empty() ||
      !producer->definedFailure.empty() || !producer->lifecycle.empty() ||
      producer->successResultDrop || producer->successResultDestination ||
      !mirTypeMoveIsDefinedFailureFree(program, value->info.type) ||
      body.usesOf(valueId).size() != 1) {
    return {};
  }
  const MirValueUse &use = body.usesOf(valueId).front();
  if (use.kind != MirValueUseKind::InstructionOperand ||
      use.operandIndex != 0) {
    return {};
  }
  const MirInstruction *initialize = findInstruction(body, use.instruction);
  const MirPlace *destination = initialize != nullptr && initialize->destination
                                    ? body.findPlace(*initialize->destination)
                                    : nullptr;
  if (initialize == nullptr || destination == nullptr ||
      initialize->kind != MirInstructionKind::Initialize ||
      initialize->result || initialize->receiver ||
      initialize->operands.size() != 1 ||
      initialize->operands.front().kind != MirOperandKind::Value ||
      initialize->operands.front().value != valueId ||
      initialize->operands.front().type != value->info.type ||
      !initialize->localFailureSites.empty() ||
      !initialize->definedFailure.empty() ||
      destination->root != MirPlaceRootKind::Binding ||
      !destination->projections.empty() ||
      destination->type.kind != SemanticType::Expected ||
      destination->type.arguments.size() != 2 ||
      destination->type.arguments.front() != value->info.type ||
      destination->traits.drop != DropKind::Lexical ||
      initialize->info.type != destination->type ||
      initialize->lifecycle.size() != 1) {
    return {};
  }

  const MirDropObligation *bindingDrop = nullptr;
  for (const MirDropObligation &drop : body.dropObligations) {
    if (drop.kind != MirDropObligationKind::Binding ||
        drop.place != destination->id) {
      continue;
    }
    if (bindingDrop != nullptr) {
      return {};
    }
    bindingDrop = &drop;
  }
  const MirLifecycleEvent &activate = initialize->lifecycle.front();
  if (bindingDrop == nullptr || bindingDrop->initiallyActive ||
      bindingDrop->dropType.type != destination->type ||
      activate.kind != MirLifecycleEventKind::Initialize ||
      activate.source != 0 || activate.target != bindingDrop->id ||
      activate.conditional || activate.failureCleanup) {
    return {};
  }
  return {.producer = producer,
          .initialize = initialize,
          .destination = destination};
}

[[nodiscard]] const MirPlace *
expectedPayloadDestinationSlot(const MirProgram &program, const MirBody &body,
                               MirValueId valueId) {
  const MirInstruction *initialize =
      expectedPayloadInitialize(program, body, valueId);
  if (initialize != nullptr && initialize->destination) {
    return body.findPlace(*initialize->destination);
  }
  return expectedDefaultPayloadInitialization(program, body, valueId)
      .destination;
}

[[nodiscard]] bool
expectedClassPlacementStorageEligible(const MirProgram &program,
                                      const SemanticType &type) {
  if (type.kind != SemanticType::Expected || type.arguments.size() != 2 ||
      type.arguments.front().kind != SemanticType::Class) {
    return false;
  }
  const SemanticType &payload = type.arguments.front();
  const MirClassInstance *instance = nullptr;
  for (const MirClassInstance &candidate : program.classInstances()) {
    if (candidate.type != payload) {
      continue;
    }
    if (instance != nullptr) {
      return false;
    }
    instance = &candidate;
  }
  // A user destructor does not make move construction fail. For the exact
  // lifecycle-guarded shape, the compiler-generated move disarms the source
  // before its representation-only destructor runs. Requiring every field
  // move to be independently failure-free keeps this narrower than the
  // general class move proof and excludes custom moves, bases, unions, and
  // polymorphic representation.
  const bool lifecycleGuardedPayloadMove =
      instance != nullptr && instance->destructor.has_value() &&
      instance->requiresActiveDropState &&
      instance->moveConstructor == SpecialMemberStatus::Generated &&
      instance->traits.movable && !instance->abstract &&
      !instance->polymorphic && !instance->unionLayout &&
      instance->bases.empty() && instance->structuralBases.empty() &&
      std::all_of(instance->declaredFields.begin(),
                  instance->declaredFields.end(),
                  [&](const MirClassFieldInfo &field) {
                    return field.field != 0 && field.symbol != 0 &&
                           mirTypeMoveIsDefinedFailureFree(program, field.type);
                  });
  return mirTypeMoveIsDefinedFailureFree(program, type) ||
         lifecycleGuardedPayloadMove;
}

[[nodiscard]] bool
expectedClassPlacementResultType(const MirProgram &program,
                                 const CppMirBodyEmissionMap &representations,
                                 const SemanticType &type) {
  if (type.kind != SemanticType::Expected || type.arguments.size() != 2 ||
      type.arguments.front().kind != SemanticType::Class ||
      !expectedClassPlacementStorageEligible(program, type)) {
    return false;
  }
  const auto hasRow = [&](const SemanticType &candidate) {
    return std::any_of(representations.types().begin(),
                       representations.types().end(),
                       [&](const CppMirTypeRepresentation &row) {
                         return row.type == candidate && !row.spelling.empty();
                       });
  };
  return hasRow(type) && hasRow(type.arguments.front()) &&
         hasRow(type.arguments.back());
}

// A transformed call returning Expected<Class, E> can construct directly in
// the binding that its sole Initialize consumes. The value-root place and
// value drop are then ownership records for that same object, not additional
// C++ storage.
[[nodiscard]] const MirPlace *
expectedClassResultDestinationSlot(const MirProgram &program,
                                   const MirBody &body, MirValueId valueId) {
  const MirValue *value = body.findValue(valueId);
  const MirInstruction *definition =
      value == nullptr ? nullptr : findInstruction(body, value->definition);
  const MirFunctionInstance *target =
      definition != nullptr && definition->functionTarget
          ? program.findFunctionInstance(*definition->functionTarget)
          : nullptr;
  const MirDropObligation *valueDrop =
      value == nullptr ? nullptr
                       : uniqueValueDrop(body, valueId, value->info.type);
  if (value == nullptr || value->info.type.kind != SemanticType::Expected ||
      value->info.type.arguments.size() != 2 ||
      value->info.type.arguments.front().kind != SemanticType::Class ||
      value->info.traits.drop != DropKind::Lexical || definition == nullptr ||
      definition->kind != MirInstructionKind::Call ||
      definition->result != valueId || !definition->functionTarget ||
      definition->constructorTarget || definition->lambdaTarget ||
      definition->bodyTarget || definition->callableInvocation ||
      definition->intrinsic != IntrinsicKind::None || target == nullptr ||
      target->returnType != value->info.type ||
      !target->mayRaiseDefinedFailure ||
      target->definitionKind != MirDefinitionKind::Source ||
      target->linkage != LanguageLinkage::Gti ||
      !invokePairedInstruction(body, definition->id) || valueDrop == nullptr ||
      definition->successResultDrop != valueDrop->id ||
      !expectedClassPlacementStorageEligible(program, value->info.type)) {
    return nullptr;
  }
  const std::vector<MirValueUse> uses = nonRootRecordUses(body, valueId);
  if (uses.size() != 1 ||
      uses.front().kind != MirValueUseKind::InstructionOperand ||
      uses.front().operandIndex != 0) {
    return nullptr;
  }
  const MirInstruction *initialize =
      findInstruction(body, uses.front().instruction);
  const MirPlace *destination = initialize != nullptr && initialize->destination
                                    ? body.findPlace(*initialize->destination)
                                    : nullptr;
  const MirDropObligation *bindingDrop = nullptr;
  if (destination != nullptr) {
    for (const MirDropObligation &drop : body.dropObligations) {
      if (drop.kind != MirDropObligationKind::Binding ||
          drop.place != destination->id) {
        continue;
      }
      if (bindingDrop != nullptr) {
        return nullptr;
      }
      bindingDrop = &drop;
    }
  }
  if (initialize == nullptr || destination == nullptr ||
      bindingDrop == nullptr ||
      initialize->kind != MirInstructionKind::Initialize ||
      initialize->operands.size() != 1 ||
      initialize->operands.front().kind != MirOperandKind::Value ||
      initialize->operands.front().value != valueId ||
      initialize->operands.front().type != value->info.type ||
      destination->root != MirPlaceRootKind::Binding ||
      !destination->projections.empty() ||
      destination->type != value->info.type ||
      destination->traits.drop != DropKind::Lexical ||
      initialize->info.type != destination->type ||
      initialize->lifecycle.size() != 1) {
    return nullptr;
  }
  const MirLifecycleEvent &reparent = initialize->lifecycle.front();
  return reparent.kind == MirLifecycleEventKind::Reparent &&
                 reparent.source == valueDrop->id &&
                 reparent.target == bindingDrop->id && !reparent.conditional &&
                 !reparent.failureCleanup
             ? destination
             : nullptr;
}

// A move of an owning Expected into one binding can construct that binding's
// lifetime slot directly. MIR still records the intermediate value drop and
// the following Initialize reparents it to the binding; neither record needs
// separate C++ storage when this exact move/reparent chain is present.
[[nodiscard]] const MirPlace *
expectedMoveDestinationSlot(const MirProgram &program, const MirBody &body,
                            MirValueId valueId) {
  const MirValue *value = body.findValue(valueId);
  const MirInstruction *move =
      value == nullptr ? nullptr : findInstruction(body, value->definition);
  const MirDropObligation *valueDrop =
      value == nullptr ? nullptr
                       : uniqueValueDrop(body, valueId, value->info.type);
  if (value == nullptr || value->info.type.kind != SemanticType::Expected ||
      value->info.type.arguments.size() != 2 ||
      value->info.traits.drop != DropKind::Lexical || move == nullptr ||
      move->kind != MirInstructionKind::Move || !move->result ||
      *move->result != valueId || move->destination || move->receiver ||
      move->operands.size() != 1 ||
      move->operands.front().kind != MirOperandKind::Move ||
      move->operands.front().place == 0 ||
      move->operands.front().type != value->info.type ||
      !move->localFailureSites.empty() ||
      !move->definedFailure.localOrigins.empty() ||
      move->definedFailure.propagation != FailurePropagationKind::None ||
      move->lifecycle.size() != 1 || valueDrop == nullptr ||
      (!mirTypeMoveIsDefinedFailureFree(program, value->info.type) &&
       !expectedClassPlacementStorageEligible(program, value->info.type))) {
    return nullptr;
  }
  const MirPlace *source = body.findPlace(move->operands.front().place);
  const MirDropObligation *sourceDrop = nullptr;
  if (source != nullptr) {
    for (const MirDropObligation &drop : body.dropObligations) {
      if (drop.place != source->id || drop.dropType.type != source->type) {
        continue;
      }
      if (sourceDrop != nullptr) {
        return nullptr;
      }
      sourceDrop = &drop;
    }
  }
  const MirLifecycleEvent &moveEvent = move->lifecycle.front();
  if (source == nullptr || sourceDrop == nullptr ||
      source->root != MirPlaceRootKind::Binding ||
      !source->projections.empty() || source->type != value->info.type ||
      moveEvent.kind != MirLifecycleEventKind::Move ||
      moveEvent.source != sourceDrop->id || moveEvent.target != valueDrop->id ||
      moveEvent.conditional || moveEvent.failureCleanup) {
    return nullptr;
  }

  const std::vector<MirValueUse> uses = nonRootRecordUses(body, valueId);
  if (uses.size() != 1 ||
      uses.front().kind != MirValueUseKind::InstructionOperand ||
      uses.front().operandIndex != 0) {
    return nullptr;
  }
  const MirInstruction *initialize =
      findInstruction(body, uses.front().instruction);
  const MirPlace *destination = initialize != nullptr && initialize->destination
                                    ? body.findPlace(*initialize->destination)
                                    : nullptr;
  const MirDropObligation *bindingDrop = nullptr;
  if (destination != nullptr) {
    for (const MirDropObligation &drop : body.dropObligations) {
      if (drop.kind != MirDropObligationKind::Binding ||
          drop.place != destination->id ||
          drop.dropType.type != destination->type) {
        continue;
      }
      if (bindingDrop != nullptr) {
        return nullptr;
      }
      bindingDrop = &drop;
    }
  }
  if (initialize == nullptr || destination == nullptr ||
      bindingDrop == nullptr ||
      initialize->kind != MirInstructionKind::Initialize ||
      initialize->operands.size() != 1 ||
      initialize->operands.front().kind != MirOperandKind::Value ||
      initialize->operands.front().value != valueId ||
      initialize->operands.front().type != value->info.type ||
      destination->root != MirPlaceRootKind::Binding ||
      !destination->projections.empty() ||
      destination->type != value->info.type ||
      destination->traits.drop != DropKind::Lexical ||
      initialize->info.type != destination->type ||
      !initialize->localFailureSites.empty() ||
      !initialize->definedFailure.localOrigins.empty() ||
      initialize->definedFailure.propagation != FailurePropagationKind::None ||
      initialize->lifecycle.size() != 1) {
    return nullptr;
  }
  const MirLifecycleEvent &reparent = initialize->lifecycle.front();
  return reparent.kind == MirLifecycleEventKind::Reparent &&
                 reparent.source == valueDrop->id &&
                 reparent.target == bindingDrop->id && !reparent.conditional &&
                 !reparent.failureCleanup
             ? destination
             : nullptr;
}

// A transformed class or Expected<Class, E> call returned unchanged by its
// caller can write directly into that caller's placement result. The
// successful Invoke edge must initialize the call result's drop identity and
// the return block must transfer that exact identity out. Only failure-free
// cleanup may follow the transfer before Return; the MIR value and value-root
// place then remain ownership metadata rather than a second C++ object.
[[nodiscard]] const MirInstruction *
placementDirectReturnCall(const MirProgram &program,
                          const CppMirBodyEmissionMap &representations,
                          const MirBody &body, MirValueId valueId) {
  const MirValue *value = body.findValue(valueId);
  const MirInstruction *call =
      value == nullptr ? nullptr : findInstruction(body, value->definition);
  const MirFunctionInstance *target =
      call != nullptr && call->functionTarget
          ? program.findFunctionInstance(*call->functionTarget)
          : nullptr;
  const MirDropObligation *drop =
      value == nullptr ? nullptr
                       : uniqueValueDrop(body, valueId, value->info.type);
  const bool placementType =
      value != nullptr &&
      (expectedClassPlacementResultType(program, representations,
                                        value->info.type) ||
       (value->info.type.kind == SemanticType::Class &&
        std::any_of(representations.types().begin(),
                    representations.types().end(),
                    [&](const CppMirTypeRepresentation &row) {
                      return row.type == value->info.type &&
                             row.kind == CppMirTypeRepresentationKind::Class &&
                             !row.spelling.empty();
                    })));
  if (value == nullptr || value->info.type != body.returnType ||
      !placementType || value->info.traits.drop != DropKind::Lexical ||
      call == nullptr || call->kind != MirInstructionKind::Call ||
      !call->result || *call->result != valueId || !call->functionTarget ||
      call->constructorTarget || call->lambdaTarget || call->bodyTarget ||
      call->callableInvocation || call->intrinsic != IntrinsicKind::None ||
      call->definedFailure.propagation != FailurePropagationKind::DirectCall ||
      target == nullptr || target->returnType != value->info.type ||
      !target->mayRaiseDefinedFailure ||
      target->definitionKind != MirDefinitionKind::Source ||
      target->linkage != LanguageLinkage::Gti || drop == nullptr ||
      drop->kind != MirDropObligationKind::Value ||
      call->successResultDrop != drop->id) {
    return nullptr;
  }
  const std::vector<MirValueUse> uses = nonRootRecordUses(body, valueId);
  if (uses.size() != 1 || uses.front().kind != MirValueUseKind::Terminator) {
    return nullptr;
  }
  const MirPlace *root = body.findPlace(drop->place);
  if (root == nullptr || root->root != MirPlaceRootKind::Value ||
      root->value != valueId || !root->projections.empty() ||
      root->type != value->info.type) {
    return nullptr;
  }
  const MirBlock *callBlock = nullptr;
  for (const MirBlock &block : body.blocks) {
    if (!block.instructions.empty() &&
        block.instructions.back().id == call->id &&
        block.terminator.kind == MirTerminatorKind::Invoke &&
        block.terminator.invokeInstruction == call->id) {
      if (callBlock != nullptr) {
        return nullptr;
      }
      callBlock = &block;
    }
  }
  if (callBlock == nullptr ||
      callBlock->terminator.successLifecycle.size() != 1) {
    return nullptr;
  }
  const MirLifecycleEvent &activate =
      callBlock->terminator.successLifecycle.front();
  if (activate.kind != MirLifecycleEventKind::Initialize ||
      activate.source != 0 || activate.target != drop->id ||
      activate.conditional || activate.failureCleanup) {
    return nullptr;
  }
  const MirBlock *returnBlock = nullptr;
  for (const MirBlock &block : body.blocks) {
    if (block.id == callBlock->terminator.target) {
      returnBlock = &block;
      break;
    }
  }
  if (returnBlock == nullptr ||
      returnBlock->terminator.kind != MirTerminatorKind::Return ||
      !returnBlock->terminator.value ||
      returnBlock->terminator.value->kind != MirOperandKind::Value ||
      returnBlock->terminator.value->value != valueId) {
    return nullptr;
  }
  std::size_t transfers = 0;
  for (const MirInstruction &instruction : returnBlock->instructions) {
    if ((instruction.kind != MirInstructionKind::Lifecycle &&
         instruction.kind != MirInstructionKind::Drop &&
         instruction.kind != MirInstructionKind::EndBorrow) ||
        !instruction.definedFailure.empty()) {
      return nullptr;
    }
    for (const MirLifecycleEvent &event : instruction.lifecycle) {
      if (event.source == drop->id || event.target == drop->id) {
        if (event.kind != MirLifecycleEventKind::TransferOut ||
            event.source != drop->id || event.target != 0 ||
            event.conditional || event.failureCleanup) {
          return nullptr;
        }
        ++transfers;
      }
    }
  }
  return transfers == 1 ? call : nullptr;
}

[[nodiscard]] const MirFunctionInstance *
transformedFailureCallee(const MirProgram &program,
                         const CppMirBodyEmissionMap &representations,
                         const MirInstruction &instruction);

struct ValueRootedClassCallResultSlot {
  const MirValue *value = nullptr;
  const MirInstruction *producer = nullptr;
  const MirInstruction *consumer = nullptr;
  const MirInstruction *destroy = nullptr;
  const MirPlace *slot = nullptr;
  const MirDropObligation *drop = nullptr;

  [[nodiscard]] explicit operator bool() const {
    return value != nullptr && producer != nullptr && destroy != nullptr &&
           slot != nullptr && drop != nullptr;
  }
};

// A transformed class-returning call may own its result in one value-rooted
// place until the full-expression Drop. The result is either discarded or
// moved once into an existing class binding; in the latter case MIR replaces
// that binding's live state with the result before ending the moved-from
// shell. Preserve that exact slot and schedule instead of inventing an
// initialized C++ SSA local whose destructor would run again.
[[nodiscard]] ValueRootedClassCallResultSlot
valueRootedClassCallResultSlot(const MirProgram &program,
                               const CppMirBodyEmissionMap &representations,
                               const MirBody &body, MirValueId valueId) {
  const MirValue *value = body.findValue(valueId);
  const MirInstruction *producer =
      value == nullptr ? nullptr : findInstruction(body, value->definition);
  const MirFunctionInstance *callee =
      producer != nullptr && producer->functionTarget
          ? program.findFunctionInstance(*producer->functionTarget)
          : nullptr;
  const MirDropObligation *drop =
      value == nullptr ? nullptr
                       : uniqueValueDrop(body, valueId, value->info.type);
  const MirPlace *slot =
      drop == nullptr ? nullptr : body.findPlace(drop->place);
  if (value == nullptr || value->info.type.kind != SemanticType::Class ||
      value->info.category != ValueCategory::Value ||
      value->info.traits.drop != DropKind::Lexical || producer == nullptr ||
      producer->kind != MirInstructionKind::Call || !producer->result ||
      *producer->result != valueId || producer->destination ||
      producer->receiver || !producer->functionTarget ||
      producer->constructorTarget || producer->lambdaTarget ||
      producer->bodyTarget || producer->callableInvocation ||
      producer->intrinsic != IntrinsicKind::None ||
      producer->dispatch != CallDispatch::Static ||
      producer->definedFailure.propagation !=
          FailurePropagationKind::DirectCall ||
      !producer->definedFailure.localOrigins.empty() ||
      !producer->localFailureSites.empty() || !producer->lifecycle.empty() ||
      producer->successResultDestination || callee == nullptr ||
      transformedFailureCallee(program, representations, *producer) != callee ||
      callee->returnType != value->info.type ||
      callee->parameterTypes != producer->parameterTypes ||
      callee->parameterTypes.size() != producer->operands.size() ||
      !hasCompleteCallInputSchedule(body, *producer) || drop == nullptr ||
      drop->kind != MirDropObligationKind::Value || drop->initiallyActive ||
      producer->successResultDrop != drop->id || slot == nullptr ||
      slot->root != MirPlaceRootKind::Value || slot->value != valueId ||
      !slot->projections.empty() || slot->type != value->info.type ||
      slot->traits.drop != DropKind::Lexical || slot->initiallyAvailable ||
      !invokeSuccessActivates(body, *producer, drop->id)) {
    return {};
  }

  const std::vector<MirValueUse> uses = body.usesOf(valueId);
  std::vector<MirValueUse> executableUses;
  std::copy_if(uses.begin(), uses.end(), std::back_inserter(executableUses),
               [](const MirValueUse &use) {
                 return use.kind != MirValueUseKind::PlaceRoot;
               });
  const std::size_t rootUses =
      std::count_if(uses.begin(), uses.end(), [&](const MirValueUse &use) {
        return use.kind == MirValueUseKind::PlaceRoot && use.place == slot->id;
      });
  if (rootUses != 1 || uses.size() != executableUses.size() + 1 ||
      executableUses.size() > 1) {
    return {};
  }

  const MirInstruction *consumer = nullptr;
  const MirDropObligation *destinationDrop = nullptr;
  if (!executableUses.empty()) {
    const MirValueUse &use = executableUses.front();
    consumer = use.kind == MirValueUseKind::InstructionOperand
                   ? findInstruction(body, use.instruction)
                   : nullptr;
    const MirPlace *destination = consumer != nullptr && consumer->destination
                                      ? body.findPlace(*consumer->destination)
                                      : nullptr;
    if (consumer == nullptr || destination == nullptr ||
        consumer->kind != MirInstructionKind::Assign ||
        consumer->operation != MirOperation::Assign || !consumer->result ||
        !discardedAssignmentResult(body, *consumer->result) ||
        consumer->receiver || consumer->operands.size() != 1 ||
        use.operandIndex != 0 ||
        consumer->operands.front().kind != MirOperandKind::Value ||
        consumer->operands.front().value != valueId ||
        consumer->operands.front().type != value->info.type ||
        consumer->info.type != value->info.type ||
        destination->root != MirPlaceRootKind::Binding ||
        !destination->projections.empty() ||
        destination->type != value->info.type ||
        destination->access != AccessMode::Mutable ||
        destination->traits.drop != DropKind::Lexical ||
        !value->info.traits.movable || !consumer->localFailureSites.empty() ||
        !consumer->definedFailure.empty() || consumer->lifecycle.size() != 1) {
      return {};
    }
    const MirLifecycleEvent &replace = consumer->lifecycle.front();
    destinationDrop = body.findDropObligation(replace.target);
    if (replace.kind != MirLifecycleEventKind::Replace ||
        replace.source != drop->id || replace.target == 0 ||
        replace.conditional || replace.failureCleanup ||
        destinationDrop == nullptr ||
        destinationDrop->kind != MirDropObligationKind::Binding ||
        destinationDrop->place != destination->id ||
        destinationDrop->dropType.type != destination->type ||
        !destinationDrop->dropType.requiresActiveCleanup) {
      return {};
    }
  }

  const MirInstruction *destroy = nullptr;
  std::size_t replacementCount = 0;
  for (const MirBlock &block : body.blocks) {
    bool sawConsumer = false;
    for (const MirInstruction &instruction : block.instructions) {
      if (&instruction == consumer) {
        sawConsumer = true;
      }
      if (instruction.destination == slot->id &&
          instruction.kind != MirInstructionKind::Drop) {
        return {};
      }
      for (const MirLifecycleEvent &event : instruction.lifecycle) {
        if (event.source != drop->id && event.target != drop->id) {
          continue;
        }
        if (&instruction == consumer &&
            event.kind == MirLifecycleEventKind::Replace &&
            event.source == drop->id && destinationDrop != nullptr &&
            event.target == destinationDrop->id && !event.conditional &&
            !event.failureCleanup) {
          ++replacementCount;
          continue;
        }
        if (destroy != nullptr ||
            instruction.kind != MirInstructionKind::Drop ||
            instruction.destination != slot->id ||
            event.kind != MirLifecycleEventKind::Drop ||
            event.source != drop->id || event.target != 0 ||
            event.conditional || event.failureCleanup ||
            (consumer != nullptr && !sawConsumer)) {
          return {};
        }
        destroy = &instruction;
      }
    }
    for (const MirLifecycleEvent &event : block.terminator.successLifecycle) {
      if (event.source != drop->id && event.target != drop->id) {
        continue;
      }
      if (block.terminator.kind != MirTerminatorKind::Invoke ||
          block.terminator.invokeInstruction != producer->id ||
          event.kind != MirLifecycleEventKind::Initialize ||
          event.source != 0 || event.target != drop->id || event.conditional ||
          event.failureCleanup) {
        return {};
      }
    }
  }
  return destroy == nullptr || replacementCount != (consumer == nullptr ? 0 : 1)
             ? ValueRootedClassCallResultSlot{}
             : ValueRootedClassCallResultSlot{.value = value,
                                              .producer = producer,
                                              .consumer = consumer,
                                              .destroy = destroy,
                                              .slot = slot,
                                              .drop = drop};
}

struct ExpectedPayloadReturnSlot {
  const MirValue *value = nullptr;
  const MirInstruction *producer = nullptr;
  const MirBlock *returnBlock = nullptr;
  const MirDropObligation *drop = nullptr;

  [[nodiscard]] explicit operator bool() const { return value != nullptr; }
};

// A failure-capable invocation producing the class payload of an
// Expected<Class, E> return first needs payload storage. Its successful
// Invoke edge activates one value drop, and the unique return edge transfers
// that same drop out. The backend can therefore construct into a lifetime
// slot, move the completed payload once into Expected at Return, and destroy
// only the moved-from representation.
[[nodiscard]] ExpectedPayloadReturnSlot
expectedPayloadReturnSlot(const MirProgram &program,
                          const CppMirBodyEmissionMap &representations,
                          const MirBody &body, MirValueId valueId) {
  const MirValue *value = body.findValue(valueId);
  const MirInstruction *producer =
      value == nullptr ? nullptr : findInstruction(body, value->definition);
  const MirConstructorInstance *target =
      producer != nullptr && producer->constructorTarget
          ? program.findConstructorInstance(*producer->constructorTarget)
          : nullptr;
  const MirFunctionInstance *callee =
      producer != nullptr && producer->functionTarget
          ? program.findFunctionInstance(*producer->functionTarget)
          : nullptr;
  const MirDropObligation *drop =
      value == nullptr ? nullptr
                       : uniqueValueDrop(body, valueId, value->info.type);
  const bool constructorProducer =
      producer != nullptr && producer->kind == MirInstructionKind::Construct &&
      producer->constructorKind == ConstructorKind::Ordinary &&
      producer->definedFailure.propagation ==
          FailurePropagationKind::Constructor &&
      target != nullptr && failureConstructorBoundaryEligible(program, *target);
  const bool callProducer =
      producer != nullptr && producer->kind == MirInstructionKind::Call &&
      producer->dispatch == CallDispatch::Static && producer->functionTarget &&
      !producer->constructorTarget && !producer->bodyTarget &&
      !producer->lambdaTarget && !producer->callableInvocation &&
      !producer->receiver && producer->intrinsic == IntrinsicKind::None &&
      producer->localFailureSites.empty() &&
      producer->definedFailure.propagation ==
          FailurePropagationKind::DirectCall &&
      callee != nullptr && callee->returnType == value->info.type &&
      callee->parameterTypes == producer->parameterTypes &&
      callee->mayRaiseDefinedFailure &&
      callee->linkage == LanguageLinkage::Gti &&
      callee->definitionKind == MirDefinitionKind::Source &&
      (!terminallyContainedPlainCallee(program, representations, *producer) ||
       !callee->callableParameters.empty());
  if (value == nullptr ||
      !expectedClassPlacementResultType(program, representations,
                                        body.returnType) ||
      value->info.type != body.returnType.arguments.front() ||
      value->info.category != ValueCategory::Value ||
      value->info.traits.drop != DropKind::Lexical || producer == nullptr ||
      (!constructorProducer && !callProducer) || !producer->result ||
      *producer->result != valueId || producer->destination ||
      producer->receiver || drop == nullptr ||
      drop->kind != MirDropObligationKind::Value ||
      producer->successResultDrop != drop->id) {
    return {};
  }
  const MirPlace *root = body.findPlace(drop->place);
  if (root == nullptr || root->root != MirPlaceRootKind::Value ||
      root->value != valueId || !root->projections.empty() ||
      root->type != value->info.type || body.usesOf(valueId).size() != 2) {
    return {};
  }
  const MirBlock *producerBlock = nullptr;
  for (const MirBlock &block : body.blocks) {
    if (!block.instructions.empty() &&
        block.instructions.back().id == producer->id &&
        block.terminator.kind == MirTerminatorKind::Invoke &&
        block.terminator.invokeInstruction == producer->id) {
      if (producerBlock != nullptr) {
        return {};
      }
      producerBlock = &block;
    }
  }
  if (producerBlock == nullptr ||
      producerBlock->terminator.successLifecycle.size() != 1) {
    return {};
  }
  const MirLifecycleEvent &activate =
      producerBlock->terminator.successLifecycle.front();
  if (activate.kind != MirLifecycleEventKind::Initialize ||
      activate.source != 0 || activate.target != drop->id ||
      activate.conditional || activate.failureCleanup) {
    return {};
  }
  const MirBlock *returnBlock = nullptr;
  for (const MirBlock &block : body.blocks) {
    if (block.id == producerBlock->terminator.target) {
      returnBlock = &block;
      break;
    }
  }
  if (returnBlock == nullptr ||
      returnBlock->terminator.kind != MirTerminatorKind::Return ||
      !returnBlock->terminator.value ||
      returnBlock->terminator.value->kind != MirOperandKind::Value ||
      returnBlock->terminator.value->value != valueId) {
    return {};
  }
  std::size_t transfers = 0;
  for (const MirInstruction &instruction : returnBlock->instructions) {
    if (instruction.kind != MirInstructionKind::Lifecycle) {
      return {};
    }
    for (const MirLifecycleEvent &event : instruction.lifecycle) {
      if (event.kind != MirLifecycleEventKind::TransferOut ||
          event.source != drop->id || event.target != 0 || event.conditional ||
          event.failureCleanup) {
        return {};
      }
      ++transfers;
    }
  }
  return transfers == 1 ? ExpectedPayloadReturnSlot{.value = value,
                                                    .producer = producer,
                                                    .returnBlock = returnBlock,
                                                    .drop = drop}
                        : ExpectedPayloadReturnSlot{};
}

[[nodiscard]] bool
lifetimeSlotPlace(const MirProgram &program,
                  const CppMirBodyEmissionMap &representations,
                  const MirBody &body, const MirPlace &place) {
  if (place.root == MirPlaceRootKind::Value && place.projections.empty() &&
      place.type.kind == SemanticType::Class) {
    if (fixedArrayAggregateInputSlot(program, body, place.value) != nullptr) {
      return true;
    }
    if (directTemporaryReceiverForSlot(body, place)) {
      return true;
    }
    const ValueRootedClassCallResultSlot resultSlot =
        valueRootedClassCallResultSlot(program, representations, body,
                                       place.value);
    if (resultSlot.slot == &place) {
      return true;
    }
    const ConstructorFieldResultSlot fieldResult =
        constructorFieldResultSlot(program, body, place.value);
    return fieldResult.slot != nullptr && fieldResult.slot->id == place.id;
  }
  if (place.root == MirPlaceRootKind::Temporary && place.projections.empty() &&
      place.type.kind == SemanticType::Class) {
    if (directTemporaryReceiverForSlot(body, place)) {
      return true;
    }
    if (conditionalClassReturnJoin(body, place)) {
      return true;
    }
    if (conditionalClassBindingJoin(body, place)) {
      return true;
    }
    return std::any_of(
        body.values.begin(), body.values.end(), [&](const MirValue &value) {
          const StagedClassResult staged =
              stagedClassResultForSource(body, value.id);
          return staged.slot != nullptr && staged.slot->id == place.id;
        });
  }
  if (place.root != MirPlaceRootKind::Binding || !place.projections.empty()) {
    return false;
  }
  if (ownedParameterFieldSourcePlace(program, body, place)) {
    return false;
  }
  if (passiveFixedArrayConstructDestination(program, body, place) != nullptr) {
    return true;
  }
  if (place.type.kind == SemanticType::Class ||
      place.type.kind == SemanticType::Storage ||
      place.type.kind == SemanticType::PrefixStorage ||
      (place.type.kind == SemanticType::Lambda &&
       place.traits.drop == DropKind::Lexical) ||
      (place.type.kind == SemanticType::Array &&
       place.traits.drop == DropKind::Lexical)) {
    return true;
  }
  if (place.type.kind == SemanticType::Lambda &&
      place.traits.drop == DropKind::Trivial) {
    const MaterializedClosure materialized =
        materializedClosureForType(program, body, place.type);
    return materialized &&
           std::any_of(body.values.begin(), body.values.end(),
                       [&](const MirValue &value) {
                         const MirPlace *destination =
                             lambdaValueDestinationSlot(body, value.id);
                         return destination != nullptr &&
                                destination->id == place.id;
                       });
  }
  if (place.type.kind != SemanticType::Expected ||
      place.traits.drop != DropKind::Lexical) {
    return false;
  }
  return std::any_of(
      body.values.begin(), body.values.end(), [&](const MirValue &value) {
        const MirPlace *destination =
            expectedPayloadDestinationSlot(program, body, value.id);
        const MirPlace *resultDestination =
            expectedClassResultDestinationSlot(program, body, value.id);
        const MirPlace *moveDestination =
            expectedMoveDestinationSlot(program, body, value.id);
        return (destination != nullptr && destination->id == place.id) ||
               (resultDestination != nullptr &&
                resultDestination->id == place.id) ||
               (moveDestination != nullptr && moveDestination->id == place.id);
      });
}

// A loan-rooted place projects from the referent, not necessarily from the
// canonical place recorded as the loan's borrow origin. Resolve that referent
// from the unique MIR producer and its exact target before falling back to
// the source carrier's structural pointee.
[[nodiscard]] std::optional<SemanticType>
loanReferentType(const MirProgram &program, const MirBody &body,
                 const MirLoan &loan) {
  const MirPlace *source = body.findPlace(loan.source);
  if (source == nullptr) {
    return std::nullopt;
  }
  if (loan.kind == MirLoanKind::CallResult) {
    const MirInstruction *producer = nullptr;
    for (const MirBlock &block : body.blocks) {
      for (const MirInstruction &instruction : block.instructions) {
        const bool producesLoan =
            instruction.kind == MirInstructionKind::Borrow ||
            instruction.kind == MirInstructionKind::Call ||
            instruction.kind == MirInstructionKind::Construct;
        if (!producesLoan || !instruction.loan ||
            *instruction.loan != loan.id) {
          continue;
        }
        if (producer != nullptr) {
          return std::nullopt;
        }
        producer = &instruction;
      }
    }
    const MirFunctionInstance *target =
        producer != nullptr && producer->functionTarget
            ? program.findFunctionInstance(*producer->functionTarget)
            : nullptr;
    if (target != nullptr &&
        target->returnType.kind == SemanticType::Reference &&
        target->returnType.arguments.size() == 1) {
      return target->returnType.arguments.front();
    }
    if ((source->type.kind == SemanticType::Storage ||
         source->type.kind == SemanticType::PrefixStorage ||
         source->type.kind == SemanticType::UniqueOwner ||
         source->type.kind == SemanticType::SharedPointer) &&
        source->type.arguments.size() == 1) {
      return source->type.arguments.front();
    }
  }
  if ((loan.kind == MirLoanKind::Parameter ||
       loan.kind == MirLoanKind::Return) &&
      source->type.kind == SemanticType::Reference &&
      source->type.arguments.size() == 1) {
    return source->type.arguments.front();
  }
  return source->type;
}

[[nodiscard]] bool exactFieldProjectionRows(
    const MirProgram &program, const CppMirBodyEmissionMap &representations,
    SemanticType currentType,
    const std::vector<MirPlaceProjection> &projections,
    std::size_t firstProjection, const SemanticType &resultType) {
  for (std::size_t index = firstProjection; index < projections.size();
       ++index) {
    const MirPlaceProjection &projection = projections[index];
    if (projection.kind != MirProjectionKind::Field) {
      return false;
    }
    const ResolvedMirField resolved =
        resolveMirField(program, currentType, projection.field);
    if (!resolved) {
      return false;
    }
    const auto row = std::find_if(
        representations.symbols().begin(), representations.symbols().end(),
        [&](const CppMirSymbolRepresentation &candidate) {
          return candidate.kind == CppMirSymbolRepresentationKind::Field &&
                 candidate.owner == resolved.owner->id &&
                 candidate.symbol == projection.field &&
                 candidate.ordinal == 0 &&
                 candidate.type == resolved.field->type;
        });
    if (row == representations.symbols().end() || row->spelling.empty()) {
      return false;
    }
    currentType = resolved.field->type;
  }
  return currentType == resultType;
}

class BodyAnalysisBuilder {
public:
  BodyAnalysisBuilder(const MirProgram &program,
                      const CppMirBodyEmissionMap &representations,
                      MirBodyAddress address)
      : program(program), representations(representations) {
    result.body = address;
    result.readiness = CppMirBodyEmissionReadiness::Ready;
  }

  [[nodiscard]] CppMirBodyEmissionAnalysis run(bool validateProgramAndMap) {
    if (validateProgramAndMap) {
      validateProgram();
      validateRepresentations();
    }

    const MirBody *body = findMirBody(program, result.body);
    if (body == nullptr || body->kind != result.body.kind) {
      add(CppMirBodyEmissionIssueKind::InvalidBodyAddress, 0, 0,
          "MIR body address does not resolve to its exact core owner");
      return std::move(result);
    }
    if (classifyCppMirBodyKind(body->kind) == CppMirEmissionEncoding::Invalid) {
      add(CppMirBodyEmissionIssueKind::InvalidBodyKind, 0, 0,
          "MIR body kind is outside the exhaustive emitter vocabulary");
      return std::move(result);
    }

    scanOwnerMetadata(*body);
    scanBody(*body);
    return std::move(result);
  }

  void validateProgram() {
    const MirVerificationResult verification = verifyMirProgram(program);
    if (!program.valid() || !verification.valid()) {
      if (verification.errors.empty()) {
        add(CppMirBodyEmissionIssueKind::InvalidMirProgram, 0, 0,
            "MIR program is not marked valid");
        return;
      }
      for (const MirVerificationError &error : verification.errors) {
        add(CppMirBodyEmissionIssueKind::InvalidMirProgram, error.block,
            error.instruction, error.message);
      }
    }
  }

  void validateRepresentations() {
    for (std::size_t index = 0; index < representations.types().size();
         ++index) {
      const CppMirTypeRepresentation &row = representations.types()[index];
      const std::optional<CppMirTypeRepresentationKind> expected =
          expectedTypeRepresentation(row.type);
      if (!expected ||
          ordinal(row.kind) >= ordinal(CppMirTypeRepresentationKind::Count)) {
        add(CppMirBodyEmissionIssueKind::InvalidRepresentationEnum, 0, 0,
            "type row has an invalid semantic or representation kind");
      } else if (*expected != row.kind || row.spelling.empty()) {
        add(CppMirBodyEmissionIssueKind::InvalidRepresentationRow, 0, 0,
            "type row disagrees with the exact semantic type");
      }
      if (!row.templateNameSpelling.empty() &&
          (row.kind != CppMirTypeRepresentationKind::Class ||
           (row.type.arguments.empty() && row.type.valueArguments.empty()))) {
        add(CppMirBodyEmissionIssueKind::InvalidRepresentationRow, 0, 0,
            "template-name spelling is not attached to a class template-id");
      }
      if (std::find_if(representations.types().begin(),
                       representations.types().begin() + index,
                       [&](const CppMirTypeRepresentation &prior) {
                         return prior.type == row.type;
                       }) != representations.types().begin() + index) {
        add(CppMirBodyEmissionIssueKind::DuplicateTypeRepresentation, 0, 0,
            "copied map contains duplicate exact type rows");
      }
    }

    for (std::size_t index = 0; index < representations.bodies().size();
         ++index) {
      const CppMirBodyNameRepresentation &row = representations.bodies()[index];
      if (findMirBody(program, row.address) == nullptr ||
          row.spelling.empty()) {
        add(CppMirBodyEmissionIssueKind::InvalidRepresentationRow, 0, 0,
            "body-name row is stale or empty");
      }
      if (std::find_if(representations.bodies().begin(),
                       representations.bodies().begin() + index,
                       [&](const CppMirBodyNameRepresentation &prior) {
                         return prior.address == row.address;
                       }) != representations.bodies().begin() + index) {
        add(CppMirBodyEmissionIssueKind::DuplicateBodyRepresentation, 0, 0,
            "copied map contains duplicate body-name rows");
      }
    }

    for (std::size_t index = 0; index < representations.symbols().size();
         ++index) {
      const CppMirSymbolRepresentation &row = representations.symbols()[index];
      const bool enumValid =
          ordinal(row.kind) < ordinal(CppMirSymbolRepresentationKind::Count);
      const bool ownerValid =
          row.kind == CppMirSymbolRepresentationKind::Storage || row.owner != 0;
      const bool ordinalValid =
          row.kind == CppMirSymbolRepresentationKind::Capture
              ? row.ordinal != 0
              : row.ordinal == 0;
      if (!enumValid) {
        add(CppMirBodyEmissionIssueKind::InvalidRepresentationEnum, 0, 0,
            "symbol row has an invalid representation kind");
      } else if (!ownerValid || !ordinalValid || row.symbol == 0 ||
                 row.type == SemanticType::Unknown || row.spelling.empty()) {
        add(CppMirBodyEmissionIssueKind::InvalidRepresentationRow, 0, 0,
            "symbol row has an invalid owner, identity, type, or spelling");
      }
      if (!row.declarationTypeSpelling.empty() &&
          row.kind != CppMirSymbolRepresentationKind::Field) {
        add(CppMirBodyEmissionIssueKind::InvalidRepresentationRow, 0, 0,
            "dependent declaration type is not attached to a field row");
      }
      if (std::find_if(representations.symbols().begin(),
                       representations.symbols().begin() + index,
                       [&](const CppMirSymbolRepresentation &prior) {
                         return prior.kind == row.kind &&
                                prior.owner == row.owner &&
                                prior.symbol == row.symbol &&
                                prior.ordinal == row.ordinal;
                       }) != representations.symbols().begin() + index) {
        add(CppMirBodyEmissionIssueKind::DuplicateSymbolRepresentation, 0, 0,
            "copied map contains duplicate symbol rows");
      }
    }

    for (std::size_t index = 0; index < representations.enums().size();
         ++index) {
      const CppMirEnumRepresentation &row = representations.enums()[index];
      if (row.owner == 0 || row.spelling.empty() ||
          row.underlyingType == SemanticType::Unknown) {
        add(CppMirBodyEmissionIssueKind::InvalidRepresentationRow, 0, 0,
            "enum row has an invalid owner, underlying type, or spelling");
      }
      if (std::find_if(representations.enums().begin(),
                       representations.enums().begin() + index,
                       [&](const CppMirEnumRepresentation &prior) {
                         return prior.owner == row.owner;
                       }) != representations.enums().begin() + index) {
        add(CppMirBodyEmissionIssueKind::DuplicateEnumRepresentation, 0, 0,
            "copied map contains duplicate enum rows");
      }
      for (std::size_t variant = 0; variant < row.payloadVariants.size();
           ++variant) {
        const CppMirPayloadVariantRepresentation &current =
            row.payloadVariants[variant];
        if (current.spelling.empty() ||
            std::any_of(current.fieldTypes.begin(), current.fieldTypes.end(),
                        [](const SemanticType &type) {
                          return type == SemanticType::Unknown;
                        })) {
          add(CppMirBodyEmissionIssueKind::InvalidRepresentationRow, 0, 0,
              "payload-variant row has an empty spelling or unknown field");
        }
        if (std::find_if(row.payloadVariants.begin(),
                         row.payloadVariants.begin() + variant,
                         [&](const CppMirPayloadVariantRepresentation &prior) {
                           return prior.index == current.index;
                         }) != row.payloadVariants.begin() + variant) {
          add(CppMirBodyEmissionIssueKind::InvalidRepresentationRow, 0, 0,
              "enum row contains a duplicate payload variant index");
        }
      }
    }

    for (std::size_t index = 0; index < representations.capabilities().size();
         ++index) {
      const CppMirEmissionCapabilityRepresentation &row =
          representations.capabilities()[index];
      if (ordinal(row.kind) >= ordinal(CppMirEmissionCapabilityKind::Count)) {
        add(CppMirBodyEmissionIssueKind::InvalidRepresentationEnum, 0, 0,
            "capability row has an invalid representation kind");
      } else if (row.spelling.empty()) {
        add(CppMirBodyEmissionIssueKind::InvalidRepresentationRow, 0, 0,
            "capability row has an empty helper spelling");
      }
      if (std::find_if(
              representations.capabilities().begin(),
              representations.capabilities().begin() + index,
              [&](const CppMirEmissionCapabilityRepresentation &prior) {
                return prior.kind == row.kind;
              }) != representations.capabilities().begin() + index) {
        add(CppMirBodyEmissionIssueKind::DuplicateCapabilityRepresentation, 0,
            0, "copied map contains duplicate capability rows");
      }
    }
  }

  [[nodiscard]] CppMirBodyEmissionAnalysis finishValidation() {
    return std::move(result);
  }

private:
  void add(CppMirBodyEmissionIssueKind kind, MirBlockId block,
           MirInstructionId instruction, std::string detail) {
    const auto duplicate =
        std::find_if(result.issues.begin(), result.issues.end(),
                     [&](const CppMirBodyEmissionIssue &issue) {
                       return issue.kind == kind && issue.block == block &&
                              issue.instruction == instruction &&
                              issue.detail == detail;
                     });
    if (duplicate != result.issues.end()) {
      return;
    }
    result.readiness =
        mergeReadiness(result.readiness, readinessForIssue(kind));
    result.issues.push_back({.kind = kind,
                             .body = result.body,
                             .block = block,
                             .instruction = instruction,
                             .detail = std::move(detail)});
  }

  [[nodiscard]] const CppMirTypeRepresentation *
  findType(const SemanticType &type) const {
    const auto found = std::find_if(
        representations.types().begin(), representations.types().end(),
        [&](const CppMirTypeRepresentation &row) { return row.type == type; });
    return found == representations.types().end() ? nullptr : &*found;
  }

  void requireType(const SemanticType &type, MirBlockId block = 0,
                   MirInstructionId instruction = 0) {
    if (type == SemanticType::Unknown) {
      add(CppMirBodyEmissionIssueKind::InvalidRepresentationRow, block,
          instruction, "executable MIR references an unknown semantic type");
      return;
    }
    // A C++ closure type is unnameable, so Lambda-kind types are row-free
    // by design: the fused closure chain and the deduced-callable
    // template vocabularies own every spelling that touches one.
    if (type.kind == SemanticType::Lambda) {
      return;
    }
    const CppMirTypeRepresentation *row = findType(type);
    if (row == nullptr) {
      add(CppMirBodyEmissionIssueKind::MissingTypeRepresentation, block,
          instruction, "copied map has no row for an exact MIR type");
    } else if (expectedTypeRepresentation(type) != row->kind ||
               row->spelling.empty()) {
      add(CppMirBodyEmissionIssueKind::InvalidRepresentationRow, block,
          instruction, "copied type row is stale or structurally mismatched");
    }
    for (const SemanticType &argument : type.arguments) {
      requireType(argument, block, instruction);
    }
    for (const SemanticType &argument : type.lambdaEnclosingClassTypes) {
      requireType(argument, block, instruction);
    }
    for (const SemanticType &argument : type.lambdaEnclosingFunctionTypes) {
      requireType(argument, block, instruction);
    }
  }

  void requireBody(MirBodyAddress address, MirBlockId block = 0,
                   MirInstructionId instruction = 0) {
    const auto found = std::find_if(
        representations.bodies().begin(), representations.bodies().end(),
        [&](const CppMirBodyNameRepresentation &row) {
          return row.address == address;
        });
    if (found == representations.bodies().end()) {
      add(CppMirBodyEmissionIssueKind::MissingBodyRepresentation, block,
          instruction,
          "copied map has no emitted name for an exact MIR body target");
    } else if (found->spelling.empty()) {
      add(CppMirBodyEmissionIssueKind::InvalidRepresentationRow, block,
          instruction, "body-name row has an empty spelling");
    }
  }

  [[nodiscard]] const CppMirSymbolRepresentation *
  findSymbol(CppMirSymbolRepresentationKind kind, std::size_t owner,
             SymbolId symbol, std::size_t ordinalValue = 0,
             bool *ambiguousStorage = nullptr) const {
    if (ambiguousStorage != nullptr) {
      *ambiguousStorage = false;
    }
    if (kind == CppMirSymbolRepresentationKind::Storage && owner == 0) {
      const CppMirSymbolRepresentation *only = nullptr;
      std::size_t matches = 0;
      for (const CppMirSymbolRepresentation &row : representations.symbols()) {
        if (row.kind != kind || row.symbol != symbol ||
            row.ordinal != ordinalValue) {
          continue;
        }
        only = &row;
        ++matches;
      }
      if (ambiguousStorage != nullptr) {
        *ambiguousStorage = matches > 1;
      }
      return matches == 1 ? only : nullptr;
    }

    const auto exact = std::find_if(
        representations.symbols().begin(), representations.symbols().end(),
        [&](const CppMirSymbolRepresentation &row) {
          return row.kind == kind && row.owner == owner &&
                 row.symbol == symbol && row.ordinal == ordinalValue;
        });
    if (exact != representations.symbols().end()) {
      return &*exact;
    }
    if (kind == CppMirSymbolRepresentationKind::Storage) {
      const auto namespaceStorage = std::find_if(
          representations.symbols().begin(), representations.symbols().end(),
          [&](const CppMirSymbolRepresentation &row) {
            return row.kind == kind && row.owner == 0 && row.symbol == symbol &&
                   row.ordinal == ordinalValue;
          });
      return namespaceStorage == representations.symbols().end()
                 ? nullptr
                 : &*namespaceStorage;
    }
    return nullptr;
  }

  void requireSymbol(CppMirSymbolRepresentationKind kind, std::size_t owner,
                     SymbolId symbol, const SemanticType *type,
                     std::size_t ordinalValue, MirBlockId block = 0,
                     MirInstructionId instruction = 0) {
    bool ambiguousStorage = false;
    const CppMirSymbolRepresentation *row =
        findSymbol(kind, owner, symbol, ordinalValue, &ambiguousStorage);
    if (row == nullptr) {
      if (ambiguousStorage) {
        add(CppMirBodyEmissionIssueKind::MissingProgramInitializationMir, block,
            instruction,
            "MIR does not identify which concrete static-storage owner a "
            "same-symbol representation row denotes");
        return;
      }
      add(CppMirBodyEmissionIssueKind::MissingSymbolRepresentation, block,
          instruction,
          "copied map has no exact storage, field, or capture name row "
          "(kind=" +
              std::to_string(static_cast<int>(kind)) +
              " owner=" + std::to_string(owner) +
              " symbol=" + std::to_string(symbol) + ")");
      return;
    }
    if ((type != nullptr && row->type != *type) || row->spelling.empty()) {
      add(CppMirBodyEmissionIssueKind::InvalidRepresentationRow, block,
          instruction, "symbol row type or spelling disagrees with MIR");
    }
  }

  [[nodiscard]] const CppMirEnumRepresentation *findEnum(EnumId owner) const {
    const auto found = std::find_if(
        representations.enums().begin(), representations.enums().end(),
        [owner](const CppMirEnumRepresentation &row) {
          return row.owner == owner;
        });
    return found == representations.enums().end() ? nullptr : &*found;
  }

  const CppMirEnumRepresentation *
  requireEnum(EnumId owner, MirBlockId block = 0,
              MirInstructionId instruction = 0) {
    const CppMirEnumRepresentation *row = findEnum(owner);
    if (row == nullptr) {
      add(CppMirBodyEmissionIssueKind::MissingEnumRepresentation, block,
          instruction,
          "copied map has no declaration row for an exact MIR enum");
    } else {
      requireType(row->underlyingType, block, instruction);
    }
    return row;
  }

  void requireCapability(CppMirEmissionCapabilityKind kind,
                         MirBlockId block = 0,
                         MirInstructionId instruction = 0) {
    const auto found =
        std::find_if(representations.capabilities().begin(),
                     representations.capabilities().end(),
                     [kind](const CppMirEmissionCapabilityRepresentation &row) {
                       return row.kind == kind;
                     });
    if (found == representations.capabilities().end()) {
      add(CppMirBodyEmissionIssueKind::MissingCapabilityRepresentation, block,
          instruction,
          "copied map lacks a required sealed representation helper (kind " +
              std::to_string(static_cast<unsigned>(kind)) + ")");
    }
  }

  [[nodiscard]] std::optional<HirClassInstanceId>
  classInstanceForType(const SemanticType &type) const {
    std::optional<HirClassInstanceId> resultId;
    for (const MirClassInstance &instance : program.classInstances()) {
      if (instance.type != type) {
        continue;
      }
      if (resultId) {
        return std::nullopt;
      }
      resultId = instance.id;
    }
    return resultId;
  }

  [[nodiscard]] std::optional<SemanticType> thisType() const {
    switch (result.body.kind) {
    case MirBodyKind::FieldInitializers:
    case MirBodyKind::StaticFieldInitializers:
      if (const MirClassInstance *instance =
              program.findClassInstance(result.body.owner)) {
        return instance->type;
      }
      return std::nullopt;
    case MirBodyKind::Function:
      if (const MirFunctionInstance *function =
              program.findFunctionInstance(result.body.owner);
          function != nullptr && function->owner) {
        if (const MirClassInstance *instance =
                program.findClassInstance(*function->owner)) {
          return instance->type;
        }
      }
      return std::nullopt;
    case MirBodyKind::Constructor:
      if (const MirConstructorInstance *constructor =
              program.findConstructorInstance(result.body.owner)) {
        if (const MirClassInstance *instance =
                program.findClassInstance(constructor->owner)) {
          return instance->type;
        }
      }
      return std::nullopt;
    case MirBodyKind::Destructor:
      if (const MirDestructorInstance *destructor =
              program.findDestructorInstance(result.body.owner)) {
        if (const MirClassInstance *instance =
                program.findClassInstance(destructor->owner)) {
          return instance->type;
        }
      }
      return std::nullopt;
    case MirBodyKind::Module:
    case MirBodyKind::Lambda:
    case MirBodyKind::HostedStartup:
      return std::nullopt;
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<HirClassInstanceId> concreteClassOwner() const {
    switch (result.body.kind) {
    case MirBodyKind::FieldInitializers:
    case MirBodyKind::StaticFieldInitializers:
      return result.body.owner;
    case MirBodyKind::Function:
      if (const MirFunctionInstance *function =
              program.findFunctionInstance(result.body.owner)) {
        return function->owner;
      }
      return std::nullopt;
    case MirBodyKind::Constructor:
      if (const MirConstructorInstance *constructor =
              program.findConstructorInstance(result.body.owner)) {
        return constructor->owner;
      }
      return std::nullopt;
    case MirBodyKind::Destructor:
      if (const MirDestructorInstance *destructor =
              program.findDestructorInstance(result.body.owner)) {
        return destructor->owner;
      }
      return std::nullopt;
    case MirBodyKind::Module:
    case MirBodyKind::Lambda:
    case MirBodyKind::HostedStartup:
      return std::nullopt;
    }
    return std::nullopt;
  }

  [[nodiscard]] std::size_t storageOwner(SymbolId symbol) const {
    const CppMirSymbolRepresentation *row = storageRepresentationForBody(
        program, representations, concreteClassOwner(), symbol);
    return row == nullptr ? 0 : row->owner;
  }

  [[nodiscard]] static bool sameRoot(const MirPlace &left,
                                     const MirPlace &right) {
    if (left.root != right.root) {
      return false;
    }
    switch (left.root) {
    case MirPlaceRootKind::Binding:
      return left.binding == right.binding;
    case MirPlaceRootKind::Symbol:
      return left.symbol == right.symbol && left.capture == right.capture;
    case MirPlaceRootKind::This:
      return true;
    case MirPlaceRootKind::Temporary:
      return left.temporary == right.temporary;
    case MirPlaceRootKind::Value:
      return left.value == right.value;
    case MirPlaceRootKind::Loan:
      return left.loan == right.loan;
    }
    return false;
  }

  [[nodiscard]] std::optional<SemanticType>
  rootType(const MirBody &body, const MirPlace &place) const {
    if (place.projections.empty()) {
      return place.type;
    }
    if (place.root == MirPlaceRootKind::This) {
      return thisType();
    }
    if (place.root == MirPlaceRootKind::Value) {
      const MirValue *value = body.findValue(place.value);
      return value == nullptr ? std::nullopt
                              : std::optional<SemanticType>{value->info.type};
    }
    if (place.root == MirPlaceRootKind::Loan) {
      const MirLoan *loan = body.findLoan(place.loan);
      return loan == nullptr ? std::nullopt
                             : loanReferentType(program, body, *loan);
    }
    const auto root = std::find_if(
        body.places.begin(), body.places.end(), [&](const MirPlace &candidate) {
          return candidate.projections.empty() && sameRoot(candidate, place);
        });
    if (root != body.places.end()) {
      return root->type;
    }
    if (place.root == MirPlaceRootKind::Symbol) {
      const CppMirSymbolRepresentationKind kind =
          place.capture == 0 ? CppMirSymbolRepresentationKind::Storage
                             : CppMirSymbolRepresentationKind::Capture;
      const std::size_t owner =
          place.capture == 0 ? storageOwner(place.symbol) : result.body.owner;
      if (const CppMirSymbolRepresentation *row =
              findSymbol(kind, owner, place.symbol, place.capture)) {
        return row->type;
      }
    }
    return std::nullopt;
  }

  void scanOwnerMetadata(const MirBody &body) {
    const bool executableBody =
        result.body.kind == MirBodyKind::Module
            ? hasExecutableProgramInitialization(program)
            : !isCanonicalNoExecutionInitializer(body);
    if (executableBody) {
      requireBody(result.body);
    }
    requireType(body.returnType);

    switch (result.body.kind) {
    case MirBodyKind::Module:
      if (hasExecutableProgramInitialization(program)) {
        requireCapability(CppMirEmissionCapabilityKind::ProgramInitialization);
      }
      return;
    case MirBodyKind::FieldInitializers:
      // A field-initializer body whose every owning transfer armed rollback
      // routes failure edges and carries the complete construction
      // schedule; only a body with an unarmed transfer keeps the issue.
      if (!isCanonicalNoExecutionInitializer(body) &&
          !mirBodyRoutesFailureEdges(body)) {
        add(CppMirBodyEmissionIssueKind::MissingConstructionScheduleMir, 0, 0,
            "declaration field initializers lack a complete constructor "
            "destination and partial-construction schedule");
      }
      if (const MirClassInstance *owner =
              program.findClassInstance(result.body.owner)) {
        requireType(owner->type);
      }
      return;
    case MirBodyKind::StaticFieldInitializers:
      if (!isCanonicalNoExecutionInitializer(body)) {
        requireCapability(CppMirEmissionCapabilityKind::ProgramInitialization);
        add(CppMirBodyEmissionIssueKind::MissingProgramInitializationMir, 0, 0,
            "static-field initialization is not yet merged into the verified "
            "program initialization walk");
      }
      if (const MirClassInstance *owner =
              program.findClassInstance(result.body.owner)) {
        requireType(owner->type);
      }
      return;
    case MirBodyKind::Function: {
      const MirFunctionInstance *function =
          program.findFunctionInstance(result.body.owner);
      if (function == nullptr) {
        return;
      }
      requireType(function->returnType);
      for (const SemanticType &type : function->parameterTypes) {
        requireType(type);
      }
      if (function->owner) {
        const MirClassInstance *owner =
            program.findClassInstance(*function->owner);
        if (owner != nullptr) {
          requireType(owner->type);
        }
      }
      if (function->entryKind != ProgramEntryKind::None) {
        requireCapability(CppMirEmissionCapabilityKind::HostedEntry);
      }
      if (function->linkage == LanguageLinkage::C ||
          function->definitionKind == MirDefinitionKind::RuntimeBinding) {
        requireCapability(CppMirEmissionCapabilityKind::NativeInterop);
      }
      if (function->virtualMethod || function->pureVirtual ||
          function->overrideMethod) {
        requireCapability(CppMirEmissionCapabilityKind::VirtualDispatch);
      }
      if (!function->callableParameters.empty()) {
        requireCapability(CppMirEmissionCapabilityKind::CallableDispatch);
      }
      for (const MirCallableParameter &parameter :
           function->callableParameters) {
        requireType(parameter.callableType);
        for (const MirCallableSignature &signature : parameter.signatures) {
          requireType(signature.returnType);
          for (const SemanticType &type : signature.parameterTypes) {
            requireType(type);
          }
          if (signature.functionTarget) {
            requireBody({.kind = MirBodyKind::Function,
                         .owner = *signature.functionTarget});
          }
          if (signature.lambdaTarget) {
            requireBody({.kind = MirBodyKind::Lambda,
                         .owner = *signature.lambdaTarget});
          }
        }
      }
      return;
    }
    case MirBodyKind::Constructor: {
      const MirConstructorInstance *constructor =
          program.findConstructorInstance(result.body.owner);
      if (constructor == nullptr) {
        return;
      }
      const MirClassInstance *owner =
          program.findClassInstance(constructor->owner);
      if (owner != nullptr) {
        requireType(owner->type);
      }
      for (const SemanticType &type : constructor->parameterTypes) {
        requireType(type);
      }
      for (const MirConstructorInitializer &initializer :
           constructor->initializers) {
        requireType(initializer.targetType);
        if (initializer.constructorTarget) {
          requireBody({.kind = MirBodyKind::Constructor,
                       .owner = *initializer.constructorTarget});
        }
      }
      constructorRollbackAuthority =
          constructorRollbackCovered(*constructor, owner) ||
          (owner != nullptr &&
           constructorBodyFailureEdgeFree(constructor->body) &&
           passiveDefaultBaseSurface(program, *owner, *constructor)) ||
          (owner != nullptr &&
           nativeContainedBaseConstruction(program, representations, *owner,
                                           *constructor)) ||
          cppMirConstructorStatusCannotFail(program, constructor->id);
      if (constructor->definitionKind == MirDefinitionKind::Source &&
          !constructorRollbackAuthority &&
          (constructor->mayRaiseDefinedFailure ||
           (owner != nullptr && (owner->requiresActiveCleanup ||
                                 classHasStateBearingBase(*owner))))) {
        add(CppMirBodyEmissionIssueKind::MissingPartialConstructionRollbackMir,
            0, 0,
            "constructor lacks rollback authority: " +
                std::string(constructorRollbackGap(*constructor, owner)));
      }
      return;
    }
    case MirBodyKind::Destructor: {
      const MirDestructorInstance *destructor =
          program.findDestructorInstance(result.body.owner);
      if (destructor == nullptr) {
        return;
      }
      const MirClassInstance *owner =
          program.findClassInstance(destructor->owner);
      if (owner != nullptr) {
        requireType(owner->type);
      }
      if (owner != nullptr &&
          !nativeDestructorCompositionCovered(program, *owner, *destructor)) {
        add(CppMirBodyEmissionIssueKind::MissingConstructionScheduleMir, 0, 0,
            "declared destructor lacks an exact MIR field/base destruction "
            "composition");
      }
      if (owner != nullptr) {
        for (const HirBaseInstance &base : owner->structuralBases) {
          requireType(base.type);
          const MirClassInstance *baseInstance =
              program.findClassInstance(base.instance);
          if (baseInstance != nullptr && baseInstance->destructor) {
            requireBody({.kind = MirBodyKind::Destructor,
                         .owner = *baseInstance->destructor});
          }
        }
        for (const MirClassFieldInfo &field : owner->declaredFields) {
          requireType(field.type);
          requireSymbol(CppMirSymbolRepresentationKind::Field, owner->id,
                        field.symbol, &field.type, 0);
          if (field.type.kind != SemanticType::Class ||
              !field.requiresActiveCleanup) {
            continue;
          }
          const std::optional<HirClassInstanceId> fieldOwner =
              classInstanceForType(field.type);
          const MirClassInstance *fieldInstance =
              fieldOwner ? program.findClassInstance(*fieldOwner) : nullptr;
          if (fieldInstance != nullptr && fieldInstance->destructor) {
            requireBody({.kind = MirBodyKind::Destructor,
                         .owner = *fieldInstance->destructor});
          }
        }
      }
      return;
    }
    case MirBodyKind::Lambda: {
      const MirLambdaInstance *lambda = program.findLambda(result.body.owner);
      if (lambda == nullptr) {
        return;
      }
      requireCapability(CppMirEmissionCapabilityKind::Closure);
      requireType(lambda->returnType);
      for (const SemanticType &type : lambda->parameterTypes) {
        requireType(type);
      }
      for (std::size_t index = 0; index < lambda->captureTypes.size();
           ++index) {
        requireType(lambda->captureTypes[index]);
        if (index < lambda->captureSymbols.size() &&
            lambda->captureSymbols[index] != 0) {
          requireSymbol(CppMirSymbolRepresentationKind::Capture, lambda->id,
                        lambda->captureSymbols[index],
                        &lambda->captureTypes[index], index + 1);
        }
      }
      return;
    }
    case MirBodyKind::HostedStartup: {
      requireCapability(CppMirEmissionCapabilityKind::HostedEntry);
      const bool ownedArgumentsSchedule =
          cppMirHostedStartupOwnedArgumentsSchedule(program);
      if (!cppMirHostedStartupNoArgumentsSchedule(program) &&
          !cppMirHostedStartupFailureFreeSchedule(program) &&
          !ownedArgumentsSchedule) {
        add(CppMirBodyEmissionIssueKind::MissingFailureCleanupMir, 0, 0,
            "compiler-generated hosted startup lacks the Stage-E terminal "
            "failure-containment path");
      }
      // The verified owned-arguments schedule carries its own
      // drop/end failure-cleanup envelope, so its owned marshaling
      // obligations are covered by the plan itself.
      if (!ownedArgumentsSchedule &&
          std::any_of(body.dropObligations.begin(), body.dropObligations.end(),
                      [](const MirDropObligation &obligation) {
                        return obligation.dropType.requiresActiveCleanup;
                      })) {
        add(CppMirBodyEmissionIssueKind::MissingPartialConstructionRollbackMir,
            0, 0,
            "owned hosted arguments lack the Stage-E partial-construction "
            "rollback and transfer envelope");
      }
      return;
    }
    }
  }

  void scanBody(const MirBody &body) {
    for (const MirPlace &place : body.places) {
      scanPlace(body, place);
    }
    for (const MirDropObligation &obligation : body.dropObligations) {
      requireType(obligation.dropType.type);
      if (obligation.dropType.destructor) {
        requireBody({.kind = MirBodyKind::Destructor,
                     .owner = *obligation.dropType.destructor});
      }
      if (obligation.dropType.requiresActiveCleanup) {
        requireCapability(CppMirEmissionCapabilityKind::LifetimeStorage);
      }
    }
    for (const MirValue &value : body.values) {
      requireType(value.info.type);
    }

    for (const MirBlock &block : body.blocks) {
      for (const MirInstruction &instruction : block.instructions) {
        scanInstruction(body, block, instruction);
      }
      scanTerminator(block);
    }
  }

  void scanPlace(const MirBody &body, const MirPlace &place) {
    requireType(place.type);
    const CppMirEmissionEncoding root = classifyCppMirPlaceRootKind(place.root);
    if (root == CppMirEmissionEncoding::Invalid) {
      add(CppMirBodyEmissionIssueKind::InvalidPlaceRootKind, 0, 0,
          "place root is outside the exhaustive emitter vocabulary");
    }
    std::optional<SemanticType> currentType = rootType(body, place);
    if (place.root == MirPlaceRootKind::Binding &&
        result.body.kind == MirBodyKind::Module) {
      const MirProgramInitializationStep *step =
          program.programInitializationPlan().findStepForSymbol(place.symbol);
      if (step == nullptr || step->binding != place.binding ||
          step->storagePlace != place.id) {
        add(CppMirBodyEmissionIssueKind::InvalidMirProgram, 0, 0,
            "Module binding place has no exact program-initialization row");
      } else {
        requireSymbol(CppMirSymbolRepresentationKind::Storage, step->ownerClass,
                      place.symbol, currentType ? &*currentType : nullptr, 0);
      }
    } else if (place.root == MirPlaceRootKind::Symbol) {
      if (place.capture != 0 && result.body.kind == MirBodyKind::Lambda) {
        requireSymbol(CppMirSymbolRepresentationKind::Capture,
                      result.body.owner, place.symbol,
                      currentType ? &*currentType : nullptr, place.capture);
      } else {
        requireSymbol(CppMirSymbolRepresentationKind::Storage,
                      storageOwner(place.symbol), place.symbol,
                      currentType ? &*currentType : nullptr, 0);
      }
    } else if (place.root == MirPlaceRootKind::This) {
      if (result.body.kind == MirBodyKind::Function) {
        const MirFunctionInstance *function =
            program.findFunctionInstance(result.body.owner);
        if (function != nullptr && function->owner) {
          const MirClassInstance *owner =
              program.findClassInstance(*function->owner);
          if (owner != nullptr) {
            requireType(owner->type);
          }
        }
      }
    } else if (place.root == MirPlaceRootKind::Loan) {
      requireCapability(CppMirEmissionCapabilityKind::Borrow);
    }

    if (!currentType) {
      add(CppMirBodyEmissionIssueKind::MissingTypeRepresentation, 0, 0,
          "projected place needs an explicit copied root-type row; MIR does "
          "not identify a unique concrete root type");
    }

    for (const MirPlaceProjection &projection : place.projections) {
      const CppMirEmissionEncoding encoding =
          classifyCppMirProjectionKind(projection.kind);
      if (encoding == CppMirEmissionEncoding::Invalid) {
        add(CppMirBodyEmissionIssueKind::InvalidProjectionKind, 0, 0,
            "place projection is outside the exhaustive emitter vocabulary");
        continue;
      }
      switch (projection.kind) {
      case MirProjectionKind::Field: {
        const ResolvedMirField resolved =
            currentType
                ? resolveMirField(program, *currentType, projection.field)
                : ResolvedMirField{};
        if (!resolved) {
          add(CppMirBodyEmissionIssueKind::MissingSymbolRepresentation, 0, 0,
              "field projection cannot be keyed to one exact concrete class "
              "instance from its evolving place type (place p" +
                  std::to_string(place.id) + ", field symbol " +
                  std::to_string(projection.field) + ")");
          currentType.reset();
        } else {
          requireSymbol(CppMirSymbolRepresentationKind::Field,
                        resolved.owner->id, projection.field,
                        &resolved.field->type, 0);
          currentType = resolved.field->type;
        }
        break;
      }
      case MirProjectionKind::Index:
        // A dynamic index is representable since the checked fixed-array
        // access family shipped: a site-carrying access spells the checked
        // helper and its record, and a proven-safe access spells a plain
        // subscription. The Bounds capability row names that family; the
        // text vocabulary decides each instruction.
        requireCapability(CppMirEmissionCapabilityKind::Bounds);
        if (currentType && currentType->kind == SemanticType::Array &&
            currentType->arguments.size() == 1) {
          // Copy before assignment: the element lives inside the vector the
          // assignment replaces, so a direct self-assign reads freed storage.
          SemanticType element = currentType->arguments.front();
          currentType = std::move(element);
        } else {
          currentType.reset();
        }
        break;
      case MirProjectionKind::Dereference:
        requireCapability(CppMirEmissionCapabilityKind::Borrow);
        if (currentType && currentType->arguments.size() == 1 &&
            (currentType->kind == SemanticType::Reference ||
             currentType->kind == SemanticType::UniqueOwner ||
             currentType->kind == SemanticType::SharedPointer)) {
          SemanticType pointee = currentType->arguments.front();
          currentType = std::move(pointee);
        } else {
          currentType.reset();
        }
        break;
      case MirProjectionKind::RawIndex:
      case MirProjectionKind::RawDereference:
        requireCapability(CppMirEmissionCapabilityKind::RawMemory);
        if (currentType && currentType->kind == SemanticType::RawPointer &&
            currentType->arguments.size() == 1) {
          SemanticType pointee = currentType->arguments.front();
          currentType = std::move(pointee);
        } else {
          currentType.reset();
        }
        break;
      case MirProjectionKind::PackElement: {
        if (!currentType || currentType->kind != SemanticType::TypePack ||
            !currentType->concretePack || !projection.constantIndex ||
            *projection.constantIndex >= currentType->arguments.size()) {
          currentType.reset();
          break;
        }
        SemanticType element = currentType->arguments[static_cast<std::size_t>(
            *projection.constantIndex)];
        currentType = std::move(element);
        break;
      }
      }
    }
    if (currentType && *currentType != place.type) {
      add(CppMirBodyEmissionIssueKind::InvalidMirProgram, 0, 0,
          "place projection result type disagrees with its concrete root "
          "and projection chain");
    }
  }

  void scanOperand(const MirOperand &operand, MirBlockId block,
                   MirInstructionId instruction) {
    const CppMirEmissionEncoding encoding =
        classifyCppMirOperandKind(operand.kind);
    if (encoding == CppMirEmissionEncoding::Invalid) {
      add(CppMirBodyEmissionIssueKind::InvalidOperandKind, block, instruction,
          "operand kind is outside the exhaustive emitter vocabulary");
      return;
    }
    requireType(operand.type, block, instruction);
    switch (operand.kind) {
    case MirOperandKind::Address:
      requireCapability(CppMirEmissionCapabilityKind::RawMemory, block,
                        instruction);
      break;
    case MirOperandKind::BorrowRead:
    case MirOperandKind::BorrowWrite:
    case MirOperandKind::Loan:
      requireCapability(CppMirEmissionCapabilityKind::Borrow, block,
                        instruction);
      break;
    case MirOperandKind::Value:
    case MirOperandKind::Constant:
    case MirOperandKind::Copy:
    case MirOperandKind::Move:
      break;
    }
  }

  void scanOperation(const MirBody &body, const MirBlock &block,
                     const MirInstruction &instruction) {
    const CppMirEmissionEncoding encoding =
        classifyCppMirOperation(instruction.operation);
    if (encoding == CppMirEmissionEncoding::Invalid) {
      add(CppMirBodyEmissionIssueKind::InvalidOperation, block.id,
          instruction.id,
          "MIR operation is outside the exhaustive emitter vocabulary");
      return;
    }
    if (encoding == CppMirEmissionEncoding::MissingMirAuthority &&
        !isHostedStartupArgumentIndexAdvance(body, instruction)) {
      // The closed narrowing compound assignment keeps its fused origin
      // by design; the terminal compatibility helper spells it whole
      // and contains the failure at the site. The per-form walks own
      // the invoke-edge decision.
      const bool closedCompoundAssign =
          instruction.kind == MirInstructionKind::Assign &&
          !cppMirCompoundAssignHelperSpelling(instruction.operation).empty() &&
          instruction.destination && instruction.operands.size() == 1 &&
          instruction.operands.front().kind == MirOperandKind::Value &&
          instruction.operands.front().value != 0 &&
          instruction.localFailureSites.size() <= 1 &&
          (instruction.localFailureSites.empty() ||
           program.failureMetadata().findSite(
               instruction.localFailureSites.front()) != nullptr);
      if (!closedCompoundAssign) {
        add(CppMirBodyEmissionIssueKind::MissingOrderedCompoundMir, block.id,
            instruction.id,
            "compound operation lacks the complete verified target/operand/"
            "commit schedule required for generic emission");
      }
    }

    switch (instruction.operation) {
    case MirOperation::EnumConstant:
      if (instruction.enumOwner) {
        requireEnum(*instruction.enumOwner, block.id, instruction.id);
      }
      break;
    case MirOperation::Aggregate:
      requireCapability(CppMirEmissionCapabilityKind::Aggregate, block.id,
                        instruction.id);
      if (instruction.info.traits.drop != DropKind::Trivial &&
          !failureFreeFixedArrayAggregateMove(program, body, instruction) &&
          !passiveFixedArrayConstructAggregate(program, body, instruction)) {
        add(CppMirBodyEmissionIssueKind::MissingAggregateRollbackMir, block.id,
            instruction.id,
            "cleanup-owning aggregate construction lacks per-element partial "
            "initialization and rollback state");
      }
      break;
    case MirOperation::Index:
      requireCapability(CppMirEmissionCapabilityKind::Bounds, block.id,
                        instruction.id);
      break;
    case MirOperation::AddressOf:
    case MirOperation::PointerAdd:
    case MirOperation::PointerSubtract:
    case MirOperation::PointerDifference:
      requireCapability(CppMirEmissionCapabilityKind::RawMemory, block.id,
                        instruction.id);
      break;
    case MirOperation::ExpectedHasValue:
    case MirOperation::Unexpected:
      requireCapability(CppMirEmissionCapabilityKind::Expected, block.id,
                        instruction.id);
      break;
    case MirOperation::Closure:
      requireCapability(CppMirEmissionCapabilityKind::Closure, block.id,
                        instruction.id);
      if (instruction.lambdaTarget) {
        requireBody(
            {.kind = MirBodyKind::Lambda, .owner = *instruction.lambdaTarget},
            block.id, instruction.id);
      }
      break;
    case MirOperation::PayloadConstruct:
    case MirOperation::PayloadExtract: {
      requireCapability(CppMirEmissionCapabilityKind::Payload, block.id,
                        instruction.id);
      const CppMirEnumRepresentation *enumeration =
          instruction.enumOwner
              ? requireEnum(*instruction.enumOwner, block.id, instruction.id)
              : nullptr;
      if (enumeration != nullptr && instruction.enumVariant) {
        const auto variant =
            std::find_if(enumeration->payloadVariants.begin(),
                         enumeration->payloadVariants.end(),
                         [&](const CppMirPayloadVariantRepresentation &row) {
                           return row.index == *instruction.enumVariant;
                         });
        if (variant == enumeration->payloadVariants.end()) {
          add(CppMirBodyEmissionIssueKind::InvalidRepresentationRow, block.id,
              instruction.id,
              "payload operation names a variant absent from the copied map");
        } else if (instruction.operation == MirOperation::PayloadConstruct) {
          std::vector<SemanticType> operands;
          operands.reserve(instruction.operands.size());
          for (const MirOperand &operand : instruction.operands) {
            operands.push_back(operand.type);
          }
          if (variant->fieldTypes != operands) {
            add(CppMirBodyEmissionIssueKind::InvalidRepresentationRow, block.id,
                instruction.id,
                "payload constructor fields disagree with the copied enum "
                "variant");
          }
        } else if (!instruction.payloadIndex ||
                   *instruction.payloadIndex >= variant->fieldTypes.size() ||
                   variant->fieldTypes[*instruction.payloadIndex] !=
                       instruction.info.type) {
          add(CppMirBodyEmissionIssueKind::InvalidRepresentationRow, block.id,
              instruction.id,
              "payload extraction index or type disagrees with the copied "
              "enum variant");
        }
      }
      break;
    }
    case MirOperation::Comma:
    case MirOperation::AddAssign:
    case MirOperation::SubtractAssign:
    case MirOperation::MultiplyAssign:
    case MirOperation::DivideAssign:
    case MirOperation::RemainderAssign:
    case MirOperation::BitwiseAndAssign:
    case MirOperation::BitwiseOrAssign:
    case MirOperation::BitwiseXorAssign:
    case MirOperation::ShiftLeftAssign:
    case MirOperation::ShiftRightAssign:
    case MirOperation::PreIncrement:
    case MirOperation::PreDecrement:
    case MirOperation::PostIncrement:
    case MirOperation::PostDecrement:
      break;
    case MirOperation::None:
    case MirOperation::Literal:
    case MirOperation::Identity:
    case MirOperation::Convert:
    case MirOperation::Add:
    case MirOperation::Subtract:
    case MirOperation::Multiply:
    case MirOperation::Divide:
    case MirOperation::Remainder:
    case MirOperation::BitwiseAnd:
    case MirOperation::BitwiseOr:
    case MirOperation::BitwiseXor:
    case MirOperation::ShiftLeft:
    case MirOperation::ShiftRight:
    case MirOperation::Equal:
    case MirOperation::NotEqual:
    case MirOperation::Less:
    case MirOperation::LessEqual:
    case MirOperation::Greater:
    case MirOperation::GreaterEqual:
    case MirOperation::Positive:
    case MirOperation::Negate:
    case MirOperation::LogicalNot:
    case MirOperation::BitwiseNot:
    case MirOperation::Assign:
      break;
    case MirOperation::Count:
      add(CppMirBodyEmissionIssueKind::InvalidOperation, block.id,
          instruction.id, "MirOperation::Count is not executable");
      break;
    }

    (void)body;
  }

  void scanInstruction(const MirBody &body, const MirBlock &block,
                       const MirInstruction &instruction) {
    const CppMirEmissionEncoding kind =
        classifyCppMirInstructionKind(instruction.kind);
    if (kind == CppMirEmissionEncoding::Invalid) {
      add(CppMirBodyEmissionIssueKind::InvalidInstructionKind, block.id,
          instruction.id,
          "instruction kind is outside the exhaustive emitter vocabulary");
      return;
    }
    if (kind == CppMirEmissionEncoding::MissingMirAuthority &&
        !isHostedStartupArgumentIndexAdvance(body, instruction)) {
      add(CppMirBodyEmissionIssueKind::MissingOrderedCompoundMir, block.id,
          instruction.id,
          "Modify lacks the verified read/check/convert/commit schedule");
    }

    switch (instruction.kind) {
    case MirInstructionKind::Drop:
    case MirInstructionKind::EndBorrow:
    case MirInstructionKind::Lifecycle:
      if (instruction.info.type != SemanticType::Unknown) {
        requireType(instruction.info.type, block.id, instruction.id);
      }
      break;
    case MirInstructionKind::Compute:
    case MirInstructionKind::Load:
    case MirInstructionKind::Initialize:
    case MirInstructionKind::Assign:
    case MirInstructionKind::Modify:
    case MirInstructionKind::Move:
    case MirInstructionKind::Borrow:
    case MirInstructionKind::CallInput:
    case MirInstructionKind::Call:
    case MirInstructionKind::Construct:
      requireType(instruction.info.type, block.id, instruction.id);
      break;
    case MirInstructionKind::CallBody:
      requireType(instruction.info.type, block.id, instruction.id);
      break;
    case MirInstructionKind::Count:
      break;
    }
    for (const SemanticType &type : instruction.parameterTypes) {
      requireType(type, block.id, instruction.id);
    }
    for (const SemanticType &type : instruction.closureCaptureTypes) {
      requireType(type, block.id, instruction.id);
    }
    if (instruction.receiver) {
      scanOperand(*instruction.receiver, block.id, instruction.id);
    }
    for (const MirOperand &operand : instruction.operands) {
      scanOperand(operand, block.id, instruction.id);
    }
    scanOperation(body, block, instruction);

    if (instruction.functionTarget) {
      requireBody(
          {.kind = MirBodyKind::Function, .owner = *instruction.functionTarget},
          block.id, instruction.id);
      const MirFunctionInstance *target =
          program.findFunctionInstance(*instruction.functionTarget);
      if (target != nullptr && target->linkage == LanguageLinkage::C) {
        requireCapability(CppMirEmissionCapabilityKind::NativeInterop, block.id,
                          instruction.id);
      }
    }
    if (instruction.constructorTarget) {
      requireBody({.kind = MirBodyKind::Constructor,
                   .owner = *instruction.constructorTarget},
                  block.id, instruction.id);
    }
    if (instruction.lambdaTarget) {
      requireBody(
          {.kind = MirBodyKind::Lambda, .owner = *instruction.lambdaTarget},
          block.id, instruction.id);
    }
    if (instruction.bodyTarget) {
      requireBody(*instruction.bodyTarget, block.id, instruction.id);
    }
    if (instruction.dispatch == CallDispatch::Virtual) {
      requireCapability(CppMirEmissionCapabilityKind::VirtualDispatch, block.id,
                        instruction.id);
    }
    if (instruction.callableInvocation || instruction.callableBoundary ||
        !instruction.callableArguments.empty()) {
      requireCapability(CppMirEmissionCapabilityKind::CallableDispatch,
                        block.id, instruction.id);
    }
    if (instruction.intrinsic != IntrinsicKind::None) {
      if (ordinal(instruction.intrinsic) >= ordinal(IntrinsicKind::Count)) {
        add(CppMirBodyEmissionIssueKind::InvalidRepresentationEnum, block.id,
            instruction.id, "call has an invalid intrinsic identity");
      }
      requireCapability(CppMirEmissionCapabilityKind::Intrinsic, block.id,
                        instruction.id);
    }
    if (instruction.synchronization.kind !=
        SynchronizationOperationKind::None) {
      if (ordinal(instruction.synchronization.kind) >=
          ordinal(SynchronizationOperationKind::Count)) {
        add(CppMirBodyEmissionIssueKind::InvalidRepresentationEnum, block.id,
            instruction.id, "call has an invalid synchronization identity");
      }
      requireCapability(CppMirEmissionCapabilityKind::Synchronization, block.id,
                        instruction.id);
    }
    if (instruction.unsafeOperation != UnsafeOperationKind::None ||
        instruction.rawMemoryAccess) {
      requireCapability(CppMirEmissionCapabilityKind::RawMemory, block.id,
                        instruction.id);
    }
    if (!instruction.definedFailure.empty()) {
      requireCapability(CppMirEmissionCapabilityKind::DefinedFailure, block.id,
                        instruction.id);
      if (result.body.kind == MirBodyKind::HostedStartup) {
        if (!cppMirHostedStartupNoArgumentsSchedule(program) &&
            !cppMirHostedStartupOwnedArgumentsSchedule(program)) {
          add(CppMirBodyEmissionIssueKind::MissingFailureCleanupMir, block.id,
              instruction.id,
              instruction.definedFailure.propagation ==
                      FailurePropagationKind::BodyCall
                  ? "compiler-generated body-call propagation lacks the "
                    "Stage-E hosted cleanup and terminal containment path"
                  : "compiler-generated hosted failure propagation lacks the "
                    "Stage-E cleanup and terminal containment path");
        }
      } else if (!instructionHasInvoke(block, instruction)) {
        // A proven-safe element access records its site without a failure
        // edge: flow analysis discharged the bounds check, so no Invoke,
        // record, or cleanup successor exists to demand.
        const MirPlace *elementPlace = nullptr;
        if ((instruction.kind == MirInstructionKind::Load ||
             instruction.kind == MirInstructionKind::Move) &&
            instruction.operands.size() == 1) {
          elementPlace = body.findPlace(instruction.operands.front().place);
        } else if (instruction.kind == MirInstructionKind::Assign &&
                   instruction.destination) {
          elementPlace = body.findPlace(*instruction.destination);
        }
        // A storage read whose site carries no failure edge is terminally
        // contained by the sealed runtime guard. Prefix storage reaches this
        // shape after the enclosing container proves its logical bound;
        // sparse storage retains its slot-state guard as defense in depth.
        const bool dischargedStorageRead =
            instruction.kind == MirInstructionKind::Call &&
            (instruction.intrinsic == IntrinsicKind::StorageRead ||
             instruction.intrinsic == IntrinsicKind::StorageReadMut ||
             instruction.intrinsic == IntrinsicKind::PrefixStorageRead ||
             instruction.intrinsic == IntrinsicKind::PrefixStorageReadMut) &&
            !instruction.definedFailure.localOrigins.empty() &&
            instruction.definedFailure.propagation ==
                FailurePropagationKind::None;
        // A direct call whose callee may raise carries the propagation
        // dimension with no local origin and no failure edge: the callee
        // writes the caller's forwarded record and the caller returns
        // false transparently. MIR itself asserts the caller owns no
        // cleanup here, so there is no Invoke/record/cleanup successor
        // to demand.
        const bool transparentCallPropagation =
            (instruction.kind == MirInstructionKind::Call &&
             instruction.intrinsic == IntrinsicKind::None &&
             instruction.definedFailure.propagation ==
                 FailurePropagationKind::DirectCall &&
             instruction.definedFailure.localOrigins.empty()) ||
            // A propagating construction has no failure edge because the
            // constructor's failure terminates at its own site on every
            // shipped path — the untransformed constructor and the
            // compatibility one behave identically — so the caller owns
            // nothing here until the constructor failure ABI exists.
            (instruction.kind == MirInstructionKind::Construct &&
             instruction.definedFailure.propagation ==
                 FailurePropagationKind::Constructor &&
             instruction.definedFailure.localOrigins.empty());
        // A unique-owner allocation contains its failure terminally
        // inside the backend helper — the compatibility call site carries
        // no handling either — so no Invoke, record, or cleanup successor
        // exists to demand.
        const bool terminalAllocation =
            instruction.kind == MirInstructionKind::Call &&
            (instruction.intrinsic == IntrinsicKind::AllocateUniqueOwner ||
             // The owner borrow contains its null-state failure terminally
             // inside the backend accessor, exactly like the compatibility
             // call site.
             instruction.intrinsic == IntrinsicKind::UniqueOwnerBorrow ||
             instruction.intrinsic == IntrinsicKind::UniqueOwnerBorrowMut ||
             // Prefix-storage allocation aborts inside the backend helper
             // on exhaustion, exactly like the compatibility call site.
             instruction.intrinsic == IntrinsicKind::AllocatePrefixStorage);
        // An unconsumed constructor-initializer argument publishes into
        // its field: the binding resolves through the initializer's HIR
        // argument, and the field row must exist or the body fails
        // closed — a dropped publication would leave the field
        // default-constructed.
        if (instruction.hirValue != 0 && instruction.result &&
            result.body.kind == MirBodyKind::Constructor &&
            onlyRootRecordUses(body, *instruction.result)) {
          const MirConstructorInstance *constructorInstance =
              program.findConstructorInstance(result.body.owner);
          if (constructorInstance != nullptr) {
            for (const MirConstructorInitializer &initializer :
                 constructorInstance->initializers) {
              if (initializer.arguments.size() != 1 ||
                  initializer.arguments.front() != instruction.hirValue ||
                  initializer.storesReference) {
                continue;
              }
              if (initializer.kind != ConstructorInitializerTargetKind::Field ||
                  initializer.field == 0) {
                add(CppMirBodyEmissionIssueKind::MissingConstructionScheduleMir,
                    block.id, instruction.id,
                    "unconsumed initializer argument has no exact field "
                    "binding");
              } else {
                requireSymbol(CppMirSymbolRepresentationKind::Field,
                              constructorInstance->owner, initializer.field,
                              nullptr, 0, block.id, instruction.id);
              }
            }
          }
        }
        // A checked compute or storage append with no failure edge spells
        // the terminal compatibility helper, which contains the failure
        // at the site: no edge in MIR means no propagation semantics to
        // preserve, exactly like the compatibility spelling.
        const bool terminalConversion =
            instruction.kind == MirInstructionKind::Compute &&
            instruction.localFailureSites.size() == 1 &&
            !instructionHasInvoke(block, instruction) &&
            (instruction.operation == MirOperation::Convert ||
             instruction.operation == MirOperation::Index ||
             !cppMirTerminalCheckedHelperSpelling(instruction.operation)
                  .empty());
        const bool terminalAppendCall =
            instruction.kind == MirInstructionKind::Call &&
            instruction.intrinsic == IntrinsicKind::PrefixStorageAppend &&
            instruction.localFailureSites.size() == 1 &&
            !instructionHasInvoke(block, instruction);
        // The expected extraction's spelled member contains the
        // wrong-state failure terminally; a staging call-input's site is
        // proof metadata with no compatibility runtime manifestation.
        const bool terminalExtraction =
            instruction.kind == MirInstructionKind::Call &&
            (instruction.intrinsic == IntrinsicKind::ExpectedValue ||
             instruction.intrinsic == IntrinsicKind::ExpectedError) &&
            !instructionHasInvoke(block, instruction);
        const bool stagedProofSite =
            instruction.kind == MirInstructionKind::CallInput &&
            instruction.localFailureSites.size() == 1 &&
            !instructionHasInvoke(block, instruction);
        // The closed compound assignment contains its fused narrowing
        // failure inside the terminal helper.
        const bool terminalCompoundAssign =
            instruction.kind == MirInstructionKind::Assign &&
            !cppMirCompoundAssignHelperSpelling(instruction.operation)
                 .empty() &&
            instruction.localFailureSites.size() == 1 &&
            !instructionHasInvoke(block, instruction);
        // A bounds-checked element borrow publishing the return loan
        // contains its failure terminally inside the array_at accessor,
        // exactly like the compatibility subscript body.
        bool terminalElementBorrow = false;
        if (instruction.kind == MirInstructionKind::Borrow &&
            instruction.loan) {
          const MirLoan *borrowLoan = body.findLoan(*instruction.loan);
          terminalElementBorrow =
              borrowLoan != nullptr &&
              elementBorrowLoanProducer(body, *borrowLoan) == &instruction;
        }
        if (!dischargedStorageRead && !transparentCallPropagation &&
            !terminalAllocation && !terminalElementBorrow &&
            !terminalConversion && !terminalAppendCall && !terminalExtraction &&
            !stagedProofSite && !terminalCompoundAssign &&
            (elementPlace == nullptr ||
             (!arrayElementAccess(body, *elementPlace) &&
              !bindingArrayFieldElementAccess(body, *elementPlace) &&
              !receiverArrayElementAccess(body, *elementPlace)))) {
          add(CppMirBodyEmissionIssueKind::MissingCheckedFailureControlFlow,
              block.id, instruction.id,
              "checked operation has no exact Invoke/record/cleanup "
              "successor");
        }
      }
      if (result.body.kind == MirBodyKind::Constructor &&
          !constructorRollbackAuthority) {
        add(CppMirBodyEmissionIssueKind::MissingPartialConstructionRollbackMir,
            block.id, instruction.id,
            "failure-capable construction has no general subobject rollback");
      }
    }

    const MirPlace *expectedValueOrReceiverPlace =
        instruction.intrinsic == IntrinsicKind::ExpectedValueOr &&
                instruction.receiver &&
                instruction.receiver->kind == MirOperandKind::BorrowRead &&
                instruction.receiver->place != 0
            ? body.findPlace(instruction.receiver->place)
            : nullptr;
    const bool expectedValueOrValueReceiver =
        instruction.intrinsic == IntrinsicKind::ExpectedValueOr &&
        instruction.receiver &&
        instruction.receiver->kind == MirOperandKind::Value &&
        instruction.receiver->value != 0 &&
        body.findValue(instruction.receiver->value) != nullptr;
    if (instruction.intrinsic == IntrinsicKind::ExpectedValueOr &&
        (instruction.kind != MirInstructionKind::Call ||
         !instruction.receiver || !instruction.result ||
         instruction.operands.size() != 1 ||
         instruction.receiver->type.kind != SemanticType::Expected ||
         instruction.receiver->type.arguments.size() != 2 ||
         instruction.info.type != instruction.receiver->type.arguments[0] ||
         instruction.operands.front().kind != MirOperandKind::Value ||
         instruction.operands.front().value == 0 ||
         body.findValue(instruction.operands.front().value) == nullptr ||
         instruction.operands.front().type != instruction.info.type ||
         (!expectedValueOrValueReceiver &&
          (expectedValueOrReceiverPlace == nullptr ||
           expectedValueOrReceiverPlace->type !=
               instruction.receiver->type)))) {
      add(CppMirBodyEmissionIssueKind::InvalidInstructionKind, block.id,
          instruction.id,
          "expected value_or intrinsic lost its exact receiver or fallback");
    }
    if ((instruction.kind == MirInstructionKind::Call ||
         instruction.kind == MirInstructionKind::Construct) &&
        instruction.intrinsic == IntrinsicKind::None &&
        // A callable-value invocation has no CallInput schedule: its
        // receiver is the fused closure literal and its arguments pass as
        // plain staged values, exactly like the compatibility call.
        !callableValueInvocation(instruction) &&
        !hasCompleteCallInputSchedule(body, instruction)) {
      add(CppMirBodyEmissionIssueKind::MissingCallInputScheduleMir, block.id,
          instruction.id,
          "call or construction operands do not all come from exact ordered "
          "CallInput stages");
    }
    if (instruction.kind == MirInstructionKind::Construct &&
        instruction.constructorKind != ConstructorKind::Ordinary &&
        !generatedSpecialMemberConstruction(program, body, instruction)) {
      add(CppMirBodyEmissionIssueKind::MissingConstructionScheduleMir, block.id,
          instruction.id,
          "generated copy/move construction lacks the complete generic "
          "destination and cleanup schedule");
    }
    if (instruction.kind == MirInstructionKind::Borrow ||
        instruction.kind == MirInstructionKind::EndBorrow) {
      requireCapability(CppMirEmissionCapabilityKind::Borrow, block.id,
                        instruction.id);
    }
    if (instruction.kind == MirInstructionKind::Drop ||
        !instruction.lifecycle.empty()) {
      requireCapability(CppMirEmissionCapabilityKind::LifetimeStorage, block.id,
                        instruction.id);
    }
  }

  void scanTerminator(const MirBlock &block) {
    const MirTerminator &terminator = block.terminator;
    const CppMirEmissionEncoding encoding =
        classifyCppMirTerminatorKind(terminator.kind);
    if (encoding == CppMirEmissionEncoding::Invalid) {
      add(CppMirBodyEmissionIssueKind::InvalidTerminatorKind, block.id, 0,
          "terminator kind is not executable");
      return;
    }
    if (terminator.value) {
      scanOperand(*terminator.value, block.id, 0);
    }
    for (const MirSwitchTarget &target : terminator.switchTargets) {
      if (!target.value) {
        continue;
      }
      requireType(target.value->type, block.id, 0);
      if (target.value->enumOwner != 0) {
        requireEnum(target.value->enumOwner, block.id, 0);
      }
    }
    if (terminator.kind == MirTerminatorKind::Invoke ||
        terminator.kind == MirTerminatorKind::PropagateFailure ||
        terminator.kind == MirTerminatorKind::ContainFailure ||
        terminator.kind == MirTerminatorKind::TerminateCleanupFailure) {
      requireCapability(CppMirEmissionCapabilityKind::DefinedFailure, block.id,
                        0);
    }
    if (terminator.kind == MirTerminatorKind::Exit &&
        !isInitializerBody(result.body.kind)) {
      add(CppMirBodyEmissionIssueKind::InvalidTerminatorKind, block.id, 0,
          "Exit is reserved for module/field initializer bodies");
    }
  }

  const MirProgram &program;
  const CppMirBodyEmissionMap &representations;
  CppMirBodyEmissionAnalysis result;
  // Set by the Constructor owner-metadata scan: the body's verified MIR
  // carries complete rollback authority, so the categorical rollback issues
  // do not apply.
  bool constructorRollbackAuthority = false;
};

} // namespace

bool cppMirModuleMayRaiseDefinedFailure(const MirProgram &program) {
  return moduleMayRaiseDefinedFailure(program);
}

CppMirBodyEmissionMap::CppMirBodyEmissionMap(CppMirBodyEmissionMapRows rows)
    : types_(std::move(rows.types)), bodies_(std::move(rows.bodies)),
      symbols_(std::move(rows.symbols)), enums_(std::move(rows.enums)),
      capabilities_(std::move(rows.capabilities)) {}

bool cppMirFailureConstructorEmptyStateEligible(const MirProgram &program,
                                                HirClassInstanceId instanceId) {
  const MirClassInstance *instance = program.findClassInstance(instanceId);
  return instance != nullptr &&
         failureConstructorEmptyDefaultFieldType(program, instance->type);
}

std::optional<std::vector<SemanticType>> cppMirFlattenConcreteParameterTypes(
    std::span<const SemanticType> parameterTypes) {
  std::vector<SemanticType> flattened;
  flattened.reserve(parameterTypes.size());
  for (std::size_t index = 0; index < parameterTypes.size(); ++index) {
    const SemanticType &type = parameterTypes[index];
    if (type.kind != SemanticType::TypePack) {
      flattened.push_back(type);
      continue;
    }
    if (index + 1 != parameterTypes.size() || !type.concretePack) {
      return std::nullopt;
    }
    flattened.insert(flattened.end(), type.arguments.begin(),
                     type.arguments.end());
  }
  return flattened;
}

CppMirEmissionEncoding classifyCppMirInstructionKind(MirInstructionKind kind) {
  switch (kind) {
  case MirInstructionKind::Compute:
  case MirInstructionKind::Load:
  case MirInstructionKind::Initialize:
  case MirInstructionKind::Assign:
  case MirInstructionKind::Move:
  case MirInstructionKind::Lifecycle:
    return CppMirEmissionEncoding::RepresentedByMir;
  case MirInstructionKind::Borrow:
  case MirInstructionKind::CallInput:
  case MirInstructionKind::Call:
  case MirInstructionKind::Construct:
  case MirInstructionKind::Drop:
  case MirInstructionKind::EndBorrow:
  case MirInstructionKind::CallBody:
    return CppMirEmissionEncoding::NeedsCopiedRepresentation;
  case MirInstructionKind::Modify:
    return CppMirEmissionEncoding::MissingMirAuthority;
  case MirInstructionKind::Count:
    return CppMirEmissionEncoding::Invalid;
  }
  return CppMirEmissionEncoding::Invalid;
}

CppMirEmissionEncoding classifyCppMirOperation(MirOperation operation) {
  switch (operation) {
  case MirOperation::None:
  case MirOperation::Literal:
  case MirOperation::Identity:
  case MirOperation::Convert:
  case MirOperation::Add:
  case MirOperation::Subtract:
  case MirOperation::Multiply:
  case MirOperation::Divide:
  case MirOperation::Remainder:
  case MirOperation::BitwiseAnd:
  case MirOperation::BitwiseOr:
  case MirOperation::BitwiseXor:
  case MirOperation::ShiftLeft:
  case MirOperation::ShiftRight:
  case MirOperation::Equal:
  case MirOperation::NotEqual:
  case MirOperation::Less:
  case MirOperation::LessEqual:
  case MirOperation::Greater:
  case MirOperation::GreaterEqual:
  case MirOperation::Positive:
  case MirOperation::Negate:
  case MirOperation::LogicalNot:
  case MirOperation::BitwiseNot:
  case MirOperation::Assign:
  case MirOperation::Comma:
    return CppMirEmissionEncoding::RepresentedByMir;
  case MirOperation::EnumConstant:
  case MirOperation::Aggregate:
  case MirOperation::Index:
  case MirOperation::ExpectedHasValue:
  case MirOperation::Closure:
  case MirOperation::PayloadConstruct:
  case MirOperation::PayloadExtract:
  case MirOperation::Unexpected:
  case MirOperation::AddressOf:
  case MirOperation::PointerAdd:
  case MirOperation::PointerSubtract:
  case MirOperation::PointerDifference:
    return CppMirEmissionEncoding::NeedsCopiedRepresentation;
  case MirOperation::AddAssign:
  case MirOperation::SubtractAssign:
  case MirOperation::MultiplyAssign:
  case MirOperation::DivideAssign:
  case MirOperation::RemainderAssign:
  case MirOperation::BitwiseAndAssign:
  case MirOperation::BitwiseOrAssign:
  case MirOperation::BitwiseXorAssign:
  case MirOperation::ShiftLeftAssign:
  case MirOperation::ShiftRightAssign:
  case MirOperation::PreIncrement:
  case MirOperation::PreDecrement:
  case MirOperation::PostIncrement:
  case MirOperation::PostDecrement:
    return CppMirEmissionEncoding::MissingMirAuthority;
  case MirOperation::Count:
    return CppMirEmissionEncoding::Invalid;
  }
  return CppMirEmissionEncoding::Invalid;
}

CppMirEmissionEncoding classifyCppMirOperandKind(MirOperandKind kind) {
  switch (kind) {
  case MirOperandKind::Value:
  case MirOperandKind::Constant:
  case MirOperandKind::Copy:
  case MirOperandKind::Move:
    return CppMirEmissionEncoding::RepresentedByMir;
  case MirOperandKind::Address:
  case MirOperandKind::BorrowRead:
  case MirOperandKind::BorrowWrite:
  case MirOperandKind::Loan:
    return CppMirEmissionEncoding::NeedsCopiedRepresentation;
  }
  return CppMirEmissionEncoding::Invalid;
}

CppMirEmissionEncoding classifyCppMirPlaceRootKind(MirPlaceRootKind kind) {
  switch (kind) {
  case MirPlaceRootKind::Binding:
  case MirPlaceRootKind::Temporary:
  case MirPlaceRootKind::Value:
    return CppMirEmissionEncoding::RepresentedByMir;
  case MirPlaceRootKind::Symbol:
  case MirPlaceRootKind::This:
  case MirPlaceRootKind::Loan:
    return CppMirEmissionEncoding::NeedsCopiedRepresentation;
  }
  return CppMirEmissionEncoding::Invalid;
}

CppMirEmissionEncoding classifyCppMirProjectionKind(MirProjectionKind kind) {
  switch (kind) {
  case MirProjectionKind::Field:
  case MirProjectionKind::Index:
  case MirProjectionKind::Dereference:
  case MirProjectionKind::RawIndex:
  case MirProjectionKind::RawDereference:
  case MirProjectionKind::PackElement:
    return CppMirEmissionEncoding::NeedsCopiedRepresentation;
  }
  return CppMirEmissionEncoding::Invalid;
}

CppMirEmissionEncoding classifyCppMirTerminatorKind(MirTerminatorKind kind) {
  switch (kind) {
  case MirTerminatorKind::Goto:
  case MirTerminatorKind::Branch:
  case MirTerminatorKind::Switch:
  case MirTerminatorKind::Return:
  case MirTerminatorKind::Unreachable:
  case MirTerminatorKind::Exit:
    return CppMirEmissionEncoding::RepresentedByMir;
  case MirTerminatorKind::Invoke:
  case MirTerminatorKind::PropagateFailure:
  case MirTerminatorKind::ContainFailure:
  case MirTerminatorKind::TerminateCleanupFailure:
    return CppMirEmissionEncoding::NeedsCopiedRepresentation;
  case MirTerminatorKind::None:
    return CppMirEmissionEncoding::Invalid;
  }
  return CppMirEmissionEncoding::Invalid;
}

CppMirEmissionEncoding classifyCppMirBodyKind(MirBodyKind kind) {
  switch (kind) {
  case MirBodyKind::Module:
  case MirBodyKind::FieldInitializers:
  case MirBodyKind::StaticFieldInitializers:
  case MirBodyKind::Function:
  case MirBodyKind::Constructor:
  case MirBodyKind::Destructor:
  case MirBodyKind::Lambda:
  case MirBodyKind::HostedStartup:
    return CppMirEmissionEncoding::NeedsCopiedRepresentation;
  }
  return CppMirEmissionEncoding::Invalid;
}

namespace {

// General per-instance text step (ADR 016 phase 5). Ported verbatim from the
// transitional emitter's scalar-cfg/scalar-direct-call body emission so
// production text is byte-identical across the delegation; every naming or
// type consultation is replaced by a copied representation row. Constructs
// outside this vocabulary after a Ready analysis are emission drift and
// throw, exactly as the transitional emitter throws.
// The vocabulary spells only the wrapping/saturating kinds: their helpers
// take and return the operand scalar directly, while a checked variant
// produces an `Expected` payload the scalar vocabulary cannot represent.
// The prefix-storage intrinsic family the failure form spells through the
// shipped mir_prefix_*_v1 checked helpers.
[[nodiscard]] bool prefixStorageIntrinsic(IntrinsicKind intrinsic) {
  switch (intrinsic) {
  case IntrinsicKind::AllocatePrefixStorage:
  case IntrinsicKind::PrefixStorageAppend:
  case IntrinsicKind::PrefixStoragePop:
  case IntrinsicKind::PrefixStorageInsert:
  case IntrinsicKind::PrefixStorageErase:
  case IntrinsicKind::PrefixStorageRelocate:
    return true;
  default:
    return false;
  }
}

[[nodiscard]] bool sparseStorageIntrinsic(IntrinsicKind intrinsic) {
  switch (intrinsic) {
  case IntrinsicKind::StorageConstruct:
  case IntrinsicKind::StorageDestroy:
  case IntrinsicKind::StorageRelocate:
  case IntrinsicKind::StorageShiftRight:
  case IntrinsicKind::StorageShiftLeft:
    return true;
  default:
    return false;
  }
}

[[nodiscard]] bool storageReadIntrinsic(IntrinsicKind intrinsic) {
  return intrinsic == IntrinsicKind::StorageRead ||
         intrinsic == IntrinsicKind::StorageReadMut ||
         intrinsic == IntrinsicKind::PrefixStorageRead ||
         intrinsic == IntrinsicKind::PrefixStorageReadMut;
}

[[nodiscard]] bool
storageAllocationFailureCall(const MirProgram &program,
                             const MirInstruction &instruction) {
  const bool rawStorage =
      instruction.intrinsic == IntrinsicKind::AllocateStorage;
  const bool prefixStorage =
      instruction.intrinsic == IntrinsicKind::AllocatePrefixStorage;
  if (instruction.kind != MirInstructionKind::Call ||
      (!rawStorage && !prefixStorage) || instruction.functionTarget ||
      instruction.constructorTarget || instruction.bodyTarget ||
      instruction.lambdaTarget || instruction.callableInvocation ||
      instruction.receiver || !instruction.result ||
      instruction.operands.size() != 1 ||
      instruction.operands.front().kind != MirOperandKind::Value ||
      instruction.operands.front().type != SemanticType::UInt64 ||
      instruction.info.type.kind !=
          (rawStorage ? SemanticType::Storage : SemanticType::PrefixStorage) ||
      instruction.info.type.arguments.size() != 1 ||
      instruction.parameterTypes !=
          std::vector<SemanticType>{SemanticType::UInt64} ||
      instruction.callableArguments.size() != 0 ||
      instruction.localFailureSites.size() != 1 ||
      instruction.definedFailure.localOrigins.size() != 1 ||
      instruction.definedFailure.propagation != FailurePropagationKind::None ||
      !instruction.lifecycle.empty()) {
    return false;
  }
  const FailureSiteDescriptor *site =
      program.failureMetadata().findSite(instruction.localFailureSites.front());
  return site != nullptr && site->outcomes.size() == 1 &&
         site->outcomes.front().code == DefinedFailureCode::AllocationFailure &&
         site->outcomes.front().detail == DefinedFailureDetail::PrivateStorage;
}

// A storage read with no Invoke is terminally contained by the sealed helper.
// Prefix storage reaches this after a logical-bound proof; sparse storage also
// retains its initialized-slot check. The element address feeds the loan
// directly in either case.
[[nodiscard]] bool storageReferenceReadCall(const MirBody &body,
                                            const MirInstruction &instruction) {
  return instruction.kind == MirInstructionKind::Call &&
         storageReadIntrinsic(instruction.intrinsic) &&
         !instruction.definedFailure.localOrigins.empty() &&
         instruction.definedFailure.propagation ==
             FailurePropagationKind::None &&
         !invokePairedInstruction(body, instruction.id);
}

// The identity-bound public logical-size check (P-STORAGE-01). Its plain form
// contains failure in the compatibility helper; the transformed form returns
// a status carrying the exact container-specific detail from MIR metadata.
[[nodiscard]] bool storageBoundsCheckCall(const MirInstruction &instruction) {
  return instruction.kind == MirInstructionKind::Call &&
         instruction.intrinsic == IntrinsicKind::StorageBoundsCheck &&
         !instruction.functionTarget;
}

[[nodiscard]] std::optional<std::string_view>
storageBoundsDetailSpelling(const MirProgram &program,
                            const MirInstruction &instruction) {
  if (!storageBoundsCheckCall(instruction) ||
      instruction.localFailureSites.size() != 1 ||
      instruction.definedFailure.localOrigins.size() != 1) {
    return std::nullopt;
  }
  const FailureSiteDescriptor *site =
      program.failureMetadata().findSite(instruction.localFailureSites.front());
  if (site == nullptr || site->outcomes.size() != 1 ||
      site->outcomes.front().code != DefinedFailureCode::IndexOutOfBounds) {
    return std::nullopt;
  }
  switch (site->outcomes.front().detail) {
  case DefinedFailureDetail::Vector:
    return "GTI_FAILURE_DETAIL_VECTOR_V1";
  case DefinedFailureDetail::String:
    return "GTI_FAILURE_DETAIL_STRING_V1";
  case DefinedFailureDetail::PrivateStorage:
    return "GTI_FAILURE_DETAIL_PRIVATE_STORAGE_V1";
  default:
    return std::nullopt;
  }
}

[[nodiscard]] bool
stringViewIndexFailureSite(const MirProgram &program,
                           const MirInstruction &instruction) {
  if (instruction.kind != MirInstructionKind::Compute ||
      instruction.operation != MirOperation::Index ||
      instruction.localFailureSites.size() != 1 ||
      instruction.definedFailure.localOrigins.size() != 1) {
    return false;
  }
  const FailureSiteDescriptor *site =
      program.failureMetadata().findSite(instruction.localFailureSites.front());
  return site != nullptr && site->outcomes.size() == 1 &&
         site->outcomes.front() ==
             DefinedFailureOutcome{.code = DefinedFailureCode::IndexOutOfBounds,
                                   .detail = DefinedFailureDetail::StringView};
}

// A site-carrying numeric conversion arriving as an intrinsic call is a
// checked detector exactly like the Convert compute: the status helper
// writes the converted value and the paired invoke branches on it.
[[nodiscard]] bool
checkedConversionIntrinsicCall(const MirInstruction &instruction) {
  return instruction.kind == MirInstructionKind::Call &&
         (instruction.intrinsic == IntrinsicKind::NumericAliasConversion ||
          instruction.intrinsic ==
              IntrinsicKind::NumericTypeParameterConversion) &&
         !instruction.functionTarget &&
         instruction.localFailureSites.size() == 1;
}

// A class-valued failure Return publishes its constructor call inline
// through the `T *` out-parameter: the construct's value result has
// exactly the Return as its consumer and never declares a local.
[[nodiscard]] const MirInstruction *
returnConstructDefinition(const MirBody &body, MirValueId value) {
  const MirValue *record = body.findValue(value);
  const MirInstruction *definition =
      record == nullptr ? nullptr : findInstruction(body, record->definition);
  if (definition == nullptr ||
      definition->kind != MirInstructionKind::Construct ||
      !definition->result || definition->receiver ||
      definition->constructorKind != ConstructorKind::Ordinary ||
      nonRootRecordUses(body, value).size() != 1 ||
      nonRootRecordUses(body, value).front().kind !=
          MirValueUseKind::Terminator) {
    return nullptr;
  }
  return definition;
}

// An explicit default construction fused into its consuming Return. This
// covers both a concrete class with no declared constructor and a substituted
// type parameter; the value publishes at the Return and no local declares.
[[nodiscard]] const MirInstruction *
returnDefaultConstructionDefinition(const MirBody &body, MirValueId value) {
  const MirValue *record = body.findValue(value);
  const MirInstruction *definition =
      record == nullptr ? nullptr : findInstruction(body, record->definition);
  if (definition == nullptr || definition->kind != MirInstructionKind::Call ||
      definition->functionTarget || definition->constructorTarget ||
      definition->lambdaTarget || definition->bodyTarget ||
      definition->callableInvocation || definition->receiver ||
      (definition->intrinsic != IntrinsicKind::None &&
       definition->intrinsic !=
           IntrinsicKind::DefaultTypeParameterConstruction) ||
      !definition->operands.empty() || !definition->callableArguments.empty() ||
      !definition->localFailureSites.empty() || !definition->result ||
      nonRootRecordUses(body, value).size() != 1 ||
      nonRootRecordUses(body, value).front().kind !=
          MirValueUseKind::Terminator) {
    return nullptr;
  }
  return definition;
}

// A copy-loaded class value whose sole executable use is Return publishes
// directly into the failure ABI's uninitialized result storage. This preserves
// the source-level copy while avoiding an impossible default-constructed SSA
// local for T.
[[nodiscard]] const MirInstruction *
returnCopyLoadDefinition(const MirBody &body, MirValueId value) {
  const MirValue *record = body.findValue(value);
  const MirInstruction *definition =
      record == nullptr ? nullptr : findInstruction(body, record->definition);
  if (record == nullptr || record->info.type.kind != SemanticType::Class ||
      record->info.type != body.returnType || definition == nullptr ||
      definition->kind != MirInstructionKind::Load || !definition->result ||
      *definition->result != value || definition->operands.size() != 1 ||
      definition->operands.front().kind != MirOperandKind::Copy ||
      definition->operands.front().place == 0 ||
      !definition->localFailureSites.empty() ||
      nonRootRecordUses(body, value).size() != 1 ||
      nonRootRecordUses(body, value).front().kind !=
          MirValueUseKind::Terminator) {
    return nullptr;
  }
  const MirPlace *source = body.findPlace(definition->operands.front().place);
  return source != nullptr && source->type == record->info.type ? definition
                                                                : nullptr;
}

// MIR keeps an Expected observer's place read explicit, but copying the
// Expected into an SSA local would strengthen that read into a class copy (and
// is ill-formed for move-only payloads). Fuse this exact single-use
// Load -> ExpectedHasValue chain into a direct observer on the live place.
[[nodiscard]] const MirInstruction *
expectedObserverLoadConsumer(const MirBody &body, const MirInstruction &load) {
  if (load.kind != MirInstructionKind::Load || !load.result ||
      load.operands.size() != 1 || load.operands.front().place == 0 ||
      load.info.type.kind != SemanticType::Expected ||
      !load.localFailureSites.empty()) {
    return nullptr;
  }
  const MirPlace *source = body.findPlace(load.operands.front().place);
  const std::vector<MirValueUse> uses = nonRootRecordUses(body, *load.result);
  const MirInstruction *consumer =
      uses.size() == 1 &&
              uses.front().kind == MirValueUseKind::InstructionOperand
          ? findInstruction(body, uses.front().instruction)
          : nullptr;
  if (source == nullptr || source->type != load.info.type ||
      consumer == nullptr || consumer->kind != MirInstructionKind::Compute ||
      consumer->operation != MirOperation::ExpectedHasValue ||
      consumer->operands.size() != 1 ||
      consumer->operands.front().kind != MirOperandKind::Value ||
      consumer->operands.front().value != *load.result ||
      consumer->operands.front().type != load.info.type ||
      !consumer->localFailureSites.empty()) {
    return nullptr;
  }
  return consumer;
}

// A Return loan erased into a fused construct return (ADR 018): the
// constructed value carries the borrowed reference, so the loan never
// binds a pointer local and nothing spells it.
[[nodiscard]] bool returnLoanErasedByConstruct(const MirBody &body,
                                               const MirLoan &loan) {
  if (loan.kind != MirLoanKind::Return) {
    return false;
  }
  for (const MirBlock &block : body.blocks) {
    if (block.terminator.kind == MirTerminatorKind::Return &&
        block.terminator.returnLoan &&
        *block.terminator.returnLoan == loan.id) {
      return block.terminator.value &&
             block.terminator.value->kind == MirOperandKind::Value &&
             returnConstructDefinition(body, block.terminator.value->value) !=
                 nullptr;
    }
  }
  return false;
}

// A class-valued plain Return may publish its defining call directly:
// only metadata-only full-expression/cleanup boundary records may follow the
// call in the Return's own block, and the value's only use is the Return, so
// the call spells `return <call>` and the class result never materializes as a
// local.
[[nodiscard]] const MirInstruction *returnCallDefinition(const MirBody &body,
                                                         MirValueId value) {
  const MirValue *record = body.findValue(value);
  const MirInstruction *definition =
      record == nullptr ? nullptr : findInstruction(body, record->definition);
  if (record == nullptr || record->info.type.kind != SemanticType::Class ||
      definition == nullptr || definition->kind != MirInstructionKind::Call ||
      !definition->result || nonRootRecordUses(body, value).size() != 1 ||
      nonRootRecordUses(body, value).front().kind !=
          MirValueUseKind::Terminator) {
    return nullptr;
  }
  for (const MirBlock &block : body.blocks) {
    const auto found =
        std::find_if(block.instructions.begin(), block.instructions.end(),
                     [&](const MirInstruction &instruction) {
                       return instruction.id == definition->id;
                     });
    if (found == block.instructions.end()) {
      continue;
    }
    const bool inertTail = std::all_of(
        std::next(found), block.instructions.end(),
        [](const MirInstruction &instruction) {
          return instruction.kind == MirInstructionKind::Lifecycle &&
                 !instruction.result && !instruction.destination &&
                 !instruction.receiver && instruction.operands.empty() &&
                 instruction.parameterTypes.empty() &&
                 instruction.lifecycle.empty() &&
                 instruction.fullExpressionEnd != 0 &&
                 instruction.cleanupBoundaryEnd == 0;
        });
    return inertTail && block.terminator.kind == MirTerminatorKind::Return &&
                   block.terminator.value &&
                   block.terminator.value->kind == MirOperandKind::Value &&
                   block.terminator.value->value == value
               ? definition
               : nullptr;
  }
  return nullptr;
}

struct PassiveCAbiCallAssignmentResult {
  const MirInstruction *call = nullptr;
  const MirInstruction *assignment = nullptr;
  const MirPlace *destination = nullptr;

  [[nodiscard]] explicit operator bool() const {
    return call != nullptr && assignment != nullptr && destination != nullptr;
  }
};

// A passive native record returned by a failure-free call and consumed by the
// immediately following bare assignment has an ordinary value-semantic C++
// temporary. Unlike an owning class value, the `[[c_abi]]` contract proves
// trivial transport and no cleanup identity, so this exact schedule may use a
// typed local between the call and assignment.
[[nodiscard]] PassiveCAbiCallAssignmentResult
passiveCAbiCallAssignmentResult(const MirProgram &program, const MirBody &body,
                                MirValueId valueId) {
  const MirValue *value = body.findValue(valueId);
  const MirInstruction *call =
      value == nullptr ? nullptr : findInstruction(body, value->definition);
  const MirFunctionInstance *target =
      call != nullptr && call->functionTarget
          ? program.findFunctionInstance(*call->functionTarget)
          : nullptr;
  if (value == nullptr ||
      passiveCAbiRecordInstance(program, value->info.type) == nullptr ||
      value->info.traits.drop != DropKind::Trivial ||
      value->info.traits.containsBorrowedState || call == nullptr ||
      call->kind != MirInstructionKind::Call || !call->result ||
      *call->result != valueId || call->destination || call->receiver ||
      call->constructorTarget || call->lambdaTarget || call->bodyTarget ||
      call->callableInvocation || call->intrinsic != IntrinsicKind::None ||
      !call->localFailureSites.empty() ||
      !call->definedFailure.localOrigins.empty() ||
      call->definedFailure.propagation != FailurePropagationKind::None ||
      target == nullptr || target->returnType != value->info.type ||
      !((target->linkage == LanguageLinkage::C) ||
        (target->linkage == LanguageLinkage::Gti &&
         target->definitionKind == MirDefinitionKind::Source &&
         !target->mayRaiseDefinedFailure))) {
    return {};
  }

  const std::vector<MirValueUse> uses = nonRootRecordUses(body, valueId);
  if (uses.size() != 1 ||
      uses.front().kind != MirValueUseKind::InstructionOperand ||
      uses.front().operandIndex != 0) {
    return {};
  }
  const MirInstruction *assignment =
      findInstruction(body, uses.front().instruction);
  const MirPlace *destination = assignment != nullptr && assignment->destination
                                    ? body.findPlace(*assignment->destination)
                                    : nullptr;
  const MirValue *assignmentResult = assignment != nullptr && assignment->result
                                         ? body.findValue(*assignment->result)
                                         : nullptr;
  if (assignment == nullptr || destination == nullptr ||
      assignmentResult == nullptr ||
      assignment->kind != MirInstructionKind::Assign ||
      assignment->operation != MirOperation::Assign || assignment->receiver ||
      assignment->operands.size() != 1 ||
      assignment->operands.front().kind != MirOperandKind::Value ||
      assignment->operands.front().value != valueId ||
      assignment->operands.front().type != value->info.type ||
      assignment->info.type != value->info.type ||
      assignmentResult->definition != assignment->id ||
      assignmentResult->info.type != value->info.type ||
      destination->root != MirPlaceRootKind::Binding ||
      !destination->projections.empty() ||
      destination->type != value->info.type ||
      destination->access != AccessMode::Mutable ||
      !destination->traits.copyAssignable ||
      !assignment->localFailureSites.empty() ||
      !assignment->definedFailure.empty() || !assignment->lifecycle.empty() ||
      assignment->fullExpressionEnd != 0 ||
      assignment->cleanupBoundaryEnd != 0 ||
      !nonRootRecordUses(body, assignmentResult->id).empty()) {
    return {};
  }
  if (std::any_of(body.dropObligations.begin(), body.dropObligations.end(),
                  [&](const MirDropObligation &drop) {
                    return drop.generatedValue == valueId ||
                           drop.generatedValue == assignmentResult->id;
                  })) {
    return {};
  }
  for (const MirBlock &block : body.blocks) {
    for (std::size_t index = 0; index + 1 < block.instructions.size();
         ++index) {
      if (block.instructions[index].id == call->id &&
          block.instructions[index + 1].id == assignment->id) {
        return {
            .call = call, .assignment = assignment, .destination = destination};
      }
    }
  }
  return {};
}

// A class-valued failure Return, or an Expected<Class, E> return, may publish
// a moved local instead: the
// Move sits in the Return's own block with nothing between them touching
// the source place, the value's uses are exactly the Return plus at most
// one Value-rooted place that only a Drop touches, and no instruction or
// terminator anywhere else references the source place after the Move.
// The out-parameter's move-assignment then consumes the source directly
// at the point the Move proved it live.
[[nodiscard]] const MirInstruction *returnMoveDefinition(const MirBody &body,
                                                         MirValueId value) {
  const MirValue *record = body.findValue(value);
  const MirInstruction *definition =
      record == nullptr ? nullptr : findInstruction(body, record->definition);
  const bool placementValue =
      record != nullptr &&
      (record->info.type.kind == SemanticType::Class ||
       (record->info.type == body.returnType &&
        record->info.type.kind == SemanticType::Lambda) ||
       (record->info.type == body.returnType &&
        record->info.type.kind == SemanticType::Expected &&
        record->info.type.arguments.size() == 2 &&
        record->info.type.arguments.front().kind == SemanticType::Class));
  if (!placementValue || definition == nullptr ||
      definition->kind != MirInstructionKind::Move || !definition->result ||
      definition->operands.size() != 1 ||
      definition->operands.front().kind != MirOperandKind::Move ||
      definition->operands.front().place == 0) {
    return nullptr;
  }
  const MirPlaceId source = definition->operands.front().place;
  // Same-block adjacency only: the Return's block contains the Move and
  // nothing after it references the source place. The cross-block form
  // (Move before an Invoke whose success target returns the value)
  // proved unsound as a simple allowance — the slot-state flow needs a
  // real proof before that widening returns.
  const MirBlock *home = nullptr;
  for (const MirBlock &candidate : body.blocks) {
    for (const MirInstruction &member : candidate.instructions) {
      if (member.id == definition->id) {
        home = &candidate;
      }
    }
  }
  if (home == nullptr) {
    return nullptr;
  }
  const auto returnsValue = [&](const MirBlock &block) {
    return block.terminator.kind == MirTerminatorKind::Return &&
           block.terminator.value &&
           block.terminator.value->kind == MirOperandKind::Value &&
           block.terminator.value->value == value;
  };
  const auto touchesSource = [&](const MirInstruction &member) {
    if (member.kind == MirInstructionKind::Drop) {
      // A drop of the moved-from source is a no-op by representation.
      return false;
    }
    if ((member.destination && *member.destination == source) ||
        (member.receiver && member.receiver->place == source)) {
      return true;
    }
    for (const MirOperand &operand : member.operands) {
      if (operand.place == source) {
        return true;
      }
    }
    return false;
  };
  const MirBlock *returnBlock = nullptr;
  // Every path leaving the Move's block may touch the source only via
  // drops — this single scan vets same-block secondary Returns and the
  // cross-block form alike, because def-dominance routes every
  // value-consuming Return through blocks reachable from here.
  {
    const auto successorsOf = [&](MirBlockId id, std::vector<MirBlockId> &out) {
      for (const MirBlock &candidate : body.blocks) {
        if (candidate.id != id) {
          continue;
        }
        const MirTerminator &edge = candidate.terminator;
        if (edge.kind == MirTerminatorKind::Goto ||
            edge.kind == MirTerminatorKind::Branch ||
            edge.kind == MirTerminatorKind::Invoke) {
          out.push_back(edge.target);
        }
        if (edge.kind == MirTerminatorKind::Branch ||
            edge.kind == MirTerminatorKind::Invoke) {
          out.push_back(edge.elseTarget);
        }
      }
    };
    std::vector<MirBlockId> stack;
    std::vector<MirBlockId> seen;
    successorsOf(home->id, stack);
    while (!stack.empty()) {
      const MirBlockId id = stack.back();
      stack.pop_back();
      if (id == 0 || id == home->id ||
          std::find(seen.begin(), seen.end(), id) != seen.end()) {
        continue;
      }
      seen.push_back(id);
      for (const MirBlock &candidate : body.blocks) {
        if (candidate.id != id) {
          continue;
        }
        for (const MirInstruction &member : candidate.instructions) {
          if (touchesSource(member)) {
            return nullptr;
          }
        }
      }
      successorsOf(id, stack);
    }
  }
  if (returnsValue(*home)) {
    returnBlock = home;
  } else if (home->terminator.kind == MirTerminatorKind::Invoke &&
             home->terminator.target != 0) {
    // Cross-block form: the invoke's success target returns the value,
    // the Move's block dominates it — so no merge path reaches the
    // Return without executing the Move — and every block reachable
    // after the Move touches the source only through drops (a drop of
    // the moved-from local is a no-op by representation).
    const auto successors = [&](MirBlockId id, std::vector<MirBlockId> &out) {
      for (const MirBlock &candidate : body.blocks) {
        if (candidate.id != id) {
          continue;
        }
        const MirTerminator &edge = candidate.terminator;
        if (edge.kind == MirTerminatorKind::Goto ||
            edge.kind == MirTerminatorKind::Branch ||
            edge.kind == MirTerminatorKind::Invoke) {
          out.push_back(edge.target);
        }
        if (edge.kind == MirTerminatorKind::Branch ||
            edge.kind == MirTerminatorKind::Invoke) {
          out.push_back(edge.elseTarget);
        }
      }
    };
    // Dominance by removal: the target is dominated by `home` when the
    // entry cannot reach it without passing through `home`.
    bool dominated = true;
    {
      std::vector<MirBlockId> stack{body.entry};
      std::vector<MirBlockId> seen;
      while (!stack.empty()) {
        const MirBlockId id = stack.back();
        stack.pop_back();
        if (id == 0 || id == home->id ||
            std::find(seen.begin(), seen.end(), id) != seen.end()) {
          continue;
        }
        seen.push_back(id);
        if (id == home->terminator.target) {
          dominated = false;
          break;
        }
        successors(id, stack);
      }
    }
    const MirBlock *target = nullptr;
    for (const MirBlock &candidate : body.blocks) {
      if (candidate.id == home->terminator.target) {
        target = &candidate;
      }
    }
    bool clean = dominated && target != nullptr && returnsValue(*target);
    if (clean) {
      // Every block reachable from the Move's block may only drop the
      // source.
      std::vector<MirBlockId> stack;
      std::vector<MirBlockId> seen;
      successors(home->id, stack);
      while (!stack.empty() && clean) {
        const MirBlockId id = stack.back();
        stack.pop_back();
        if (id == 0 || id == home->id ||
            std::find(seen.begin(), seen.end(), id) != seen.end()) {
          continue;
        }
        seen.push_back(id);
        for (const MirBlock &candidate : body.blocks) {
          if (candidate.id != id) {
            continue;
          }
          for (const MirInstruction &member : candidate.instructions) {
            if (touchesSource(member)) {
              clean = false;
            }
          }
        }
        successors(id, stack);
      }
    }
    if (clean) {
      returnBlock = target;
    }
  }
  if (returnBlock == nullptr) {
    return nullptr;
  }
  bool afterMove = false;
  for (const MirInstruction &member : home->instructions) {
    if (member.id == definition->id) {
      afterMove = true;
      continue;
    }
    if (afterMove && touchesSource(member)) {
      // Publication happens at the Move itself, so a later drop of the
      // moved-from source is the only admissible touch.
      return nullptr;
    }
  }
  const MirPlace *rooted = nullptr;
  std::size_t terminatorUses = 0;
  for (const MirValueUse &use : body.usesOf(value)) {
    if (use.kind == MirValueUseKind::Terminator) {
      // Every consuming terminator must be a Return of this value; the
      // universal reachable-only-drops scan above proved the source
      // untouched on every path to each publication.
      const MirBlock *user = nullptr;
      for (const MirBlock &candidate : body.blocks) {
        if (candidate.id == use.block) {
          user = &candidate;
        }
      }
      if (user == nullptr || !returnsValue(*user)) {
        return nullptr;
      }
      ++terminatorUses;
      continue;
    }
    if (use.kind == MirValueUseKind::PlaceRoot && rooted == nullptr) {
      rooted = body.findPlace(use.place);
      if (rooted == nullptr) {
        return nullptr;
      }
      continue;
    }
    return nullptr;
  }
  if (terminatorUses == 0) {
    return nullptr;
  }
  if (rooted != nullptr) {
    for (const MirBlock &block : body.blocks) {
      for (const MirInstruction &instruction : block.instructions) {
        const bool dropsIt = instruction.kind == MirInstructionKind::Drop &&
                             instruction.destination &&
                             *instruction.destination == rooted->id;
        if (dropsIt) {
          continue;
        }
        if (instruction.destination && *instruction.destination == rooted->id) {
          return nullptr;
        }
        if (instruction.receiver && instruction.receiver->place == rooted->id) {
          return nullptr;
        }
        for (const MirOperand &operand : instruction.operands) {
          if (operand.place == rooted->id) {
            return nullptr;
          }
        }
      }
    }
  }
  return definition;
}

// An Unexpected value never materializes: std::unexpected has no default
// construction, so the SSA declare-then-assign pattern cannot hold it. Its
// single consumer (the Return that converts it into the expected-typed
// result) spells the construction inline instead.
[[nodiscard]] const MirInstruction *unexpectedDefinition(const MirBody &body,
                                                         MirValueId value) {
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      if (instruction.kind == MirInstructionKind::Compute &&
          instruction.operation == MirOperation::Unexpected &&
          instruction.result && *instruction.result == value) {
        return &instruction;
      }
    }
  }
  return nullptr;
}

// A call-result loan pairs to its producing call through the shared HIR
// value; ambiguity declines exactly like the Borrow pairing below.
[[nodiscard]] const MirLoan *
producedCallResultLoan(const MirBody &body, const MirInstruction &producer) {
  if (producer.hirValue == 0) {
    return nullptr;
  }
  const MirLoan *found = nullptr;
  for (const MirLoan &loan : body.loans) {
    if (loan.kind == MirLoanKind::CallResult &&
        loan.producedBy == producer.hirValue) {
      if (found != nullptr) {
        return nullptr;
      }
      found = &loan;
    }
  }
  return found;
}

// A reference-returning call's SSA result is only the semantic identity of
// the referent. The paired CallResult loan is the physical pointer carrier,
// so an otherwise unused class-typed value must not materialize storage.
[[nodiscard]] bool callResultLoanIdentity(const MirBody &body,
                                          const MirValue &value) {
  const MirInstruction *definition = findInstruction(body, value.definition);
  return definition != nullptr &&
         definition->kind == MirInstructionKind::Call && definition->result &&
         *definition->result == value.id && definition->loan &&
         producedCallResultLoan(body, *definition) ==
             body.findLoan(*definition->loan) &&
         nonRootRecordUses(body, value.id).empty();
}

// A reference binding is a pointer carrier in the C++ representation. MIR
// names the retained loan explicitly, so recover it from the binding-carrier
// relation rather than inferring an address from the source-level expression.
// Ambiguous carrier identities are outside this bounded vocabulary.
[[nodiscard]] const MirLoan *loanCarriedByBinding(const MirBody &body,
                                                  HirBindingId binding) {
  if (binding == 0) {
    return nullptr;
  }
  const MirLoan *found = nullptr;
  for (const MirLoan &loan : body.loans) {
    if (std::find(loan.carriers.begin(), loan.carriers.end(), binding) ==
        loan.carriers.end()) {
      continue;
    }
    if (found != nullptr) {
      return nullptr;
    }
    found = &loan;
  }
  return found;
}

[[nodiscard]] const MirFunctionInstance *
virtualFailureCallee(const MirProgram &program,
                     const MirInstruction &instruction) {
  if (instruction.kind != MirInstructionKind::Call ||
      instruction.dispatch != CallDispatch::Virtual ||
      instruction.definedFailure.propagation !=
          FailurePropagationKind::VirtualCall ||
      !instruction.functionTarget || !instruction.receiver ||
      instruction.intrinsic != IntrinsicKind::None) {
    return nullptr;
  }
  const MirFunctionInstance *target =
      program.findFunctionInstance(*instruction.functionTarget);
  const MirClassInstance *owner =
      target == nullptr || !target->owner
          ? nullptr
          : program.findClassInstance(*target->owner);
  const MirFunctionInstance *root =
      target == nullptr ? nullptr
                        : cppMirVirtualFailureContractRoot(program, *target);
  if (target == nullptr || owner == nullptr || root == nullptr ||
      owner->type != instruction.dispatchOwner ||
      target->parameterTypes != instruction.parameterTypes) {
    return nullptr;
  }
  return target;
}

[[nodiscard]] const MirFunctionInstance *
transformedFailureCallee(const MirProgram &program,
                         const CppMirBodyEmissionMap &representations,
                         const MirInstruction &instruction) {
  if (instruction.kind != MirInstructionKind::Call ||
      !instruction.functionTarget ||
      instruction.intrinsic != IntrinsicKind::None) {
    return nullptr;
  }
  const MirFunctionInstance *target =
      program.findFunctionInstance(*instruction.functionTarget);
  if (target != nullptr &&
      virtualFailureCallee(program, instruction) != nullptr) {
    return target;
  }
  // A deduced-callable template callee keeps the compatibility plain
  // convention. Every other source GTI callee that may raise uses the
  // transformed failure-record ABI.
  return target != nullptr && target->mayRaiseDefinedFailure &&
                 target->linkage == LanguageLinkage::Gti &&
                 target->definitionKind ==
                     MirFunctionInstance::DefinitionKind::Source &&
                 (!terminallyContainedPlainCallee(program, representations,
                                                  instruction) ||
                  !target->callableParameters.empty())
             ? target
             : nullptr;
}

enum class ClassSsaSlotConsumerKind {
  None,
  Return,
  CallArgument,
};

struct ClassSsaLifetimeSlot {
  const MirValue *value = nullptr;
  const MirInstruction *producer = nullptr;
  const MirInstruction *consumer = nullptr;
  const MirBlock *returnBlock = nullptr;
  ClassSsaSlotConsumerKind consumerKind = ClassSsaSlotConsumerKind::None;

  [[nodiscard]] explicit operator bool() const { return value != nullptr; }
};

// Some class SSA producers cannot use an ordinary initialized C++ local. This
// exact no-alias, single-consumer shape gives the result uninitialized
// representation storage. It is not a semantic drop: the value is transferred
// immediately to a Return or one by-value call parameter, and only a generated
// cleanup-free moved-from representation remains to be ended by the slot.
[[nodiscard]] ClassSsaLifetimeSlot
classSsaLifetimeSlot(const MirProgram &program,
                     const CppMirBodyEmissionMap &representations,
                     const MirBody &body, MirValueId valueId) {
  const MirValue *value = body.findValue(valueId);
  const MirInstruction *producer =
      value == nullptr ? nullptr : findInstruction(body, value->definition);
  const MirFunctionInstance *callee =
      producer == nullptr
          ? nullptr
          : transformedFailureCallee(program, representations, *producer);
  const bool defaultConstructionProducer =
      producer != nullptr && producer->kind == MirInstructionKind::Call &&
      !producer->functionTarget && !producer->constructorTarget &&
      !producer->lambdaTarget && !producer->bodyTarget &&
      !producer->callableInvocation && !producer->receiver &&
      (producer->intrinsic == IntrinsicKind::None ||
       producer->intrinsic ==
           IntrinsicKind::DefaultTypeParameterConstruction) &&
      producer->operands.empty() && producer->callableArguments.empty() &&
      producer->localFailureSites.empty() &&
      producer->definedFailure.propagation == FailurePropagationKind::None;
  if (value == nullptr || value->info.type.kind != SemanticType::Class ||
      value->info.category != ValueCategory::Value ||
      !value->info.traits.movable || producer == nullptr ||
      (callee == nullptr && !defaultConstructionProducer) ||
      !producer->result || *producer->result != valueId ||
      producer->info.type != value->info.type ||
      (callee != nullptr && callee->returnType != value->info.type) ||
      producer->successResultDrop || producer->successResultDestination) {
    return {};
  }

  const MirClassInstance *classInstance = nullptr;
  for (const MirClassInstance &candidate : program.classInstances()) {
    if (candidate.type != value->info.type) {
      continue;
    }
    if (classInstance != nullptr) {
      return {};
    }
    classInstance = &candidate;
  }
  if (classInstance == nullptr ||
      classInstance->destructorStatus != SpecialMemberStatus::Generated ||
      classInstance->destructor || classInstance->requiresActiveCleanup ||
      !classInstance->traits.movable) {
    return {};
  }

  // A value-root place or drop obligation would make this semantic storage;
  // that must stay under the ordinary MIR place/drop protocol instead.
  if (std::any_of(body.places.begin(), body.places.end(),
                  [&](const MirPlace &place) {
                    return place.root == MirPlaceRootKind::Value &&
                           place.value == valueId;
                  }) ||
      std::any_of(body.dropObligations.begin(), body.dropObligations.end(),
                  [&](const MirDropObligation &drop) {
                    return drop.generatedValue == valueId;
                  })) {
    return {};
  }

  const std::vector<MirValueUse> uses = nonRootRecordUses(body, valueId);
  if (uses.size() != 1 || body.usesOf(valueId).size() != 1) {
    return {};
  }
  if (uses.front().kind == MirValueUseKind::Terminator &&
      body.returnType == value->info.type) {
    // A targetless default construction already publishes directly as T{} at
    // Return and needs no intermediate representation storage.
    if (defaultConstructionProducer) {
      return {};
    }
    const MirBlock *returnBlock = nullptr;
    for (const MirBlock &block : body.blocks) {
      if (block.terminator.kind != MirTerminatorKind::Return ||
          !block.terminator.value ||
          block.terminator.value->kind != MirOperandKind::Value ||
          block.terminator.value->value != valueId) {
        continue;
      }
      if (returnBlock != nullptr) {
        return {};
      }
      returnBlock = &block;
    }
    return returnBlock == nullptr
               ? ClassSsaLifetimeSlot{}
               : ClassSsaLifetimeSlot{.value = value,
                                      .producer = producer,
                                      .returnBlock = returnBlock,
                                      .consumerKind =
                                          ClassSsaSlotConsumerKind::Return};
  }

  // Owning values with semantic cleanup use PreparedParameter/drop staging.
  // A cleanup-free value may instead transfer directly to one exact by-value
  // parameter; the generated moved-from representation is all this slot ends.
  if (uses.front().kind != MirValueUseKind::InstructionOperand) {
    return {};
  }
  const MirInstruction *consumer =
      findInstruction(body, uses.front().instruction);
  if (consumer == nullptr || consumer->kind != MirInstructionKind::Call ||
      consumer->parameterTypes.size() != consumer->operands.size()) {
    return {};
  }
  std::size_t matchingOperand = consumer->operands.size();
  for (std::size_t index = 0; index < consumer->operands.size(); ++index) {
    const MirOperand &operand = consumer->operands[index];
    if (operand.kind != MirOperandKind::Value || operand.value != valueId) {
      continue;
    }
    if (matchingOperand != consumer->operands.size() ||
        operand.type != value->info.type ||
        consumer->parameterTypes[index] != value->info.type) {
      return {};
    }
    matchingOperand = index;
  }
  return matchingOperand == consumer->operands.size()
             ? ClassSsaLifetimeSlot{}
             : ClassSsaLifetimeSlot{.value = value,
                                    .producer = producer,
                                    .consumer = consumer,
                                    .consumerKind =
                                        ClassSsaSlotConsumerKind::CallArgument};
}

// A Return loan on a slot-backed class result is dependency metadata. The
// returned object carries that borrow; no independent referent pointer exists
// in the C++ representation.
[[nodiscard]] bool
returnLoanErasedByClassSsaSlot(const MirProgram &program,
                               const CppMirBodyEmissionMap &representations,
                               const MirBody &body, const MirLoan &loan) {
  if (loan.kind != MirLoanKind::Return) {
    return false;
  }
  for (const MirBlock &block : body.blocks) {
    if (block.terminator.kind != MirTerminatorKind::Return ||
        !block.terminator.returnLoan ||
        *block.terminator.returnLoan != loan.id || !block.terminator.value ||
        block.terminator.value->kind != MirOperandKind::Value) {
      continue;
    }
    const ClassSsaLifetimeSlot slot = classSsaLifetimeSlot(
        program, representations, body, block.terminator.value->value);
    return slot.returnBlock == &block &&
           slot.consumerKind == ClassSsaSlotConsumerKind::Return;
  }
  return false;
}

[[nodiscard]] std::string_view
unqualifiedBodySpelling(std::string_view spelling) {
  std::size_t scope = std::string_view::npos;
  std::size_t depth = 0;
  for (std::size_t index = 0; index + 1 < spelling.size(); ++index) {
    if (spelling[index] == '<') {
      ++depth;
    } else if (spelling[index] == '>' && depth != 0) {
      --depth;
    } else if (depth == 0 && spelling[index] == ':' &&
               spelling[index + 1] == ':') {
      scope = index;
      ++index;
    }
  }
  return scope == std::string_view::npos ? spelling
                                         : spelling.substr(scope + 2);
}

// The transformed reference-returning call that produces a call-result
// loan (ADR 018 §5): the callee publishes its loan pointer through the
// caller's `T **` out-argument, which is exactly the caller's loan local.
[[nodiscard]] const MirInstruction *
loanProducingReferenceCall(const MirProgram &program, const MirBody &body,
                           const MirLoan &loan) {
  if (loan.producedBy == 0) {
    return nullptr;
  }
  const MirInstruction *found = nullptr;
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      if (instruction.kind != MirInstructionKind::Call ||
          instruction.hirValue != loan.producedBy ||
          !instruction.functionTarget ||
          instruction.intrinsic != IntrinsicKind::None) {
        continue;
      }
      if (found != nullptr) {
        return nullptr;
      }
      found = &instruction;
    }
  }
  if (found == nullptr) {
    return nullptr;
  }
  const MirFunctionInstance *target =
      program.findFunctionInstance(*found->functionTarget);
  if (target == nullptr || !target->mayRaiseDefinedFailure ||
      target->returnType.kind != SemanticType::Reference ||
      target->linkage != LanguageLinkage::Gti ||
      (target->definitionKind != MirFunctionInstance::DefinitionKind::Source &&
       virtualFailureCallee(program, *found) == nullptr)) {
    return nullptr;
  }
  return found;
}

// The owner-borrow call feeding a loan, resolved through the loan's own
// producedBy or its parent chain (a Return loan parents the producing
// CallResult loan), with a final source-place match for return loans that
// alias the borrowed owner-field place directly. Nullptr when the loan is
// not owner-borrow-fed.
[[nodiscard]] const MirInstruction *
ownerBorrowLoanProducer(const MirBody &body, const MirLoan &loan) {
  const MirLoan *current = &loan;
  for (int depth = 0; current != nullptr && depth < 4; ++depth) {
    if (current->producedBy != 0) {
      for (const MirBlock &block : body.blocks) {
        for (const MirInstruction &instruction : block.instructions) {
          if (instruction.kind == MirInstructionKind::Call &&
              instruction.hirValue == current->producedBy &&
              (instruction.intrinsic == IntrinsicKind::UniqueOwnerBorrow ||
               instruction.intrinsic == IntrinsicKind::UniqueOwnerBorrowMut)) {
            return &instruction;
          }
        }
      }
    }
    current = current->parent != 0 ? body.findLoan(current->parent) : nullptr;
  }
  if (loan.kind == MirLoanKind::Return && loan.source != 0) {
    for (const MirBlock &block : body.blocks) {
      for (const MirInstruction &instruction : block.instructions) {
        if (instruction.kind != MirInstructionKind::Call ||
            (instruction.intrinsic != IntrinsicKind::UniqueOwnerBorrow &&
             instruction.intrinsic != IntrinsicKind::UniqueOwnerBorrowMut) ||
            instruction.operands.size() != 1) {
          continue;
        }
        const MirValue *operandValue =
            body.findValue(instruction.operands.front().value);
        const MirInstruction *fieldLoad =
            operandValue == nullptr
                ? nullptr
                : findInstruction(body, operandValue->definition);
        if (fieldLoad != nullptr &&
            fieldLoad->kind == MirInstructionKind::Load &&
            fieldLoad->operands.size() == 1 &&
            fieldLoad->operands.front().place == loan.source) {
          return &instruction;
        }
      }
    }
  }
  return nullptr;
}

// A local reference binding retains a provenance loan, but a reference-
// returning call may publish the physical address through a shorter-lived
// CallResult child. Pair that child only when it is the unique matching root
// of the binding's full expression; the parent remains the borrow-checker's
// retained identity while the child supplies the C++ pointer value.
[[nodiscard]] const MirLoan *
referenceBindingAddressLoan(const MirProgram &program, const MirBody &body,
                            const MirInstruction &initialize) {
  if (initialize.kind != MirInstructionKind::Initialize ||
      !initialize.destination || initialize.operands.size() != 1 ||
      initialize.operands.front().kind != MirOperandKind::Loan) {
    return nullptr;
  }
  const MirPlace *destination = body.findPlace(*initialize.destination);
  const MirLoan *retained = body.findLoan(initialize.operands.front().loan);
  if (destination == nullptr ||
      destination->root != MirPlaceRootKind::Binding ||
      !destination->projections.empty() ||
      destination->type.kind != SemanticType::Reference ||
      destination->type.arguments.size() != 1 || retained == nullptr ||
      loanCarriedByBinding(body, destination->binding) != retained) {
    return nullptr;
  }

  const SemanticType &referent = destination->type.arguments.front();
  const auto referentCompatible = [&](const SemanticType &source) {
    if (source == referent) {
      return true;
    }
    if (source.kind != SemanticType::Class ||
        referent.kind != SemanticType::Class) {
      return false;
    }
    const MirClassInstance *sourceClass =
        uniqueClassInstanceForType(program, source);
    const MirClassInstance *targetClass =
        uniqueClassInstanceForType(program, referent);
    if (sourceClass == nullptr || targetClass == nullptr) {
      return false;
    }
    std::unordered_set<HirClassInstanceId> seen;
    const auto derivesFrom = [&](const auto &self,
                                 const MirClassInstance &candidate) -> bool {
      if (!seen.insert(candidate.id).second) {
        return false;
      }
      for (const HirBaseInstance &base : candidate.structuralBases) {
        if (base.instance == targetClass->id) {
          return true;
        }
        const MirClassInstance *baseClass =
            program.findClassInstance(base.instance);
        if (baseClass != nullptr && self(self, *baseClass)) {
          return true;
        }
      }
      return false;
    };
    return derivesFrom(derivesFrom, *sourceClass);
  };
  const std::optional<SemanticType> retainedReferent =
      loanReferentType(program, body, *retained);
  const bool retainedHasPointer =
      retained->kind != MirLoanKind::Stored &&
      ownerBorrowLoanProducer(body, *retained) == nullptr &&
      !returnLoanErasedByConstruct(body, *retained) &&
      elementBorrowLoanProducer(body, *retained) == nullptr;
  if (retainedHasPointer && retainedReferent &&
      referentCompatible(*retainedReferent) &&
      (destination->type.referenceAccess != AccessMode::Mutable ||
       retained->access == AccessMode::Mutable)) {
    return retained;
  }

  const MirLoan *address = nullptr;
  for (const MirLoan &candidate : body.loans) {
    if (candidate.parent != retained->id ||
        candidate.kind != MirLoanKind::CallResult ||
        loanProducingReferenceCall(program, body, candidate) == nullptr) {
      continue;
    }
    const std::optional<SemanticType> candidateReferent =
        loanReferentType(program, body, candidate);
    const bool sameFullExpression =
        initialize.hirStatement != 0 && candidate.producedBy != 0 &&
        std::any_of(
            body.fullExpressions.begin(), body.fullExpressions.end(),
            [&](const MirFullExpression &expression) {
              return expression.statement == initialize.hirStatement &&
                     std::find(expression.roots.begin(), expression.roots.end(),
                               candidate.producedBy) != expression.roots.end();
            });
    const bool accessCompatible =
        destination->type.referenceAccess != AccessMode::Mutable ||
        candidate.access == AccessMode::Mutable;
    if (!candidateReferent || !referentCompatible(*candidateReferent) ||
        !accessCompatible || !sameFullExpression || address != nullptr) {
      if (candidateReferent && referentCompatible(*candidateReferent) &&
          accessCompatible && sameFullExpression) {
        return nullptr;
      }
      continue;
    }
    address = &candidate;
  }
  return address;
}

// The Borrow that publishes a discharged read's element pairs with its
// producing call through the shared HIR value; ambiguity declines.
[[nodiscard]] const MirInstruction *pairedDischargedRead(const MirBody &body,
                                                         HirValueId produced) {
  const MirInstruction *found = nullptr;
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      if (storageReferenceReadCall(body, instruction) &&
          instruction.hirValue == produced) {
        if (found != nullptr) {
          return nullptr;
        }
        found = &instruction;
      }
    }
  }
  return found;
}

[[nodiscard]] std::string_view
prefixStorageHelperSpelling(IntrinsicKind intrinsic) {
  switch (intrinsic) {
  case IntrinsicKind::AllocatePrefixStorage:
    return "::gti_internal::backend::mir_prefix_allocate_v1";
  case IntrinsicKind::PrefixStorageAppend:
    return "::gti_internal::backend::mir_prefix_append_v1";
  case IntrinsicKind::PrefixStoragePop:
    return "::gti_internal::backend::mir_prefix_pop_v1";
  case IntrinsicKind::PrefixStorageInsert:
    return "::gti_internal::backend::mir_prefix_insert_v1";
  case IntrinsicKind::PrefixStorageErase:
    return "::gti_internal::backend::mir_prefix_erase_v1";
  case IntrinsicKind::PrefixStorageRelocate:
    return "::gti_internal::backend::mir_prefix_relocate_v1";
  default:
    return "";
  }
}

[[nodiscard]] std::string_view
sparseStorageHelperSpelling(IntrinsicKind intrinsic) {
  switch (intrinsic) {
  case IntrinsicKind::StorageConstruct:
    return "::gti_internal::backend::mir_storage_construct_v1";
  case IntrinsicKind::StorageDestroy:
    return "::gti_internal::backend::mir_storage_destroy_v1";
  case IntrinsicKind::StorageRelocate:
    return "::gti_internal::backend::mir_storage_relocate_v1";
  case IntrinsicKind::StorageShiftRight:
    return "::gti_internal::backend::mir_storage_shift_right_v1";
  case IntrinsicKind::StorageShiftLeft:
    return "::gti_internal::backend::mir_storage_shift_left_v1";
  default:
    return "";
  }
}

[[nodiscard]] std::string_view
sparseStoragePlainHelperSpelling(IntrinsicKind intrinsic) {
  switch (intrinsic) {
  case IntrinsicKind::StorageConstruct:
    return "::gti_internal::backend::storage_construct";
  case IntrinsicKind::StorageDestroy:
    return "::gti_internal::backend::storage_destroy";
  case IntrinsicKind::StorageRelocate:
    return "::gti_internal::backend::storage_relocate";
  case IntrinsicKind::StorageShiftRight:
    return "::gti_internal::backend::storage_shift_right";
  case IntrinsicKind::StorageShiftLeft:
    return "::gti_internal::backend::storage_shift_left";
  default:
    return "";
  }
}

[[nodiscard]] std::string_view
storageReadHelperSpelling(IntrinsicKind intrinsic) {
  switch (intrinsic) {
  case IntrinsicKind::StorageRead:
    return "storage_read";
  case IntrinsicKind::StorageReadMut:
    return "storage_read_mut";
  case IntrinsicKind::PrefixStorageRead:
    return "prefix_storage_read";
  case IntrinsicKind::PrefixStorageReadMut:
    return "prefix_storage_read_mut";
  default:
    return "";
  }
}

// A value produced by loading or explicitly borrow-staging a storage-typed
// place stages the storage for exactly one storage-intrinsic call. It never
// materializes as a local, and the call spells the place lvalue directly.
[[nodiscard]] const MirPlace *storageStagedPlace(const MirBody &body,
                                                 const MirOperand &operand) {
  const MirInstruction *definition = definitionFor(body, operand);
  if (definition == nullptr || definition->operands.size() != 1 ||
      definition->operands.front().place == 0) {
    return nullptr;
  }
  const bool storageLoad = definition->kind == MirInstructionKind::Load;
  const bool storageBorrowStage =
      definition->kind == MirInstructionKind::CallInput &&
      (definition->operands.front().kind == MirOperandKind::BorrowRead ||
       definition->operands.front().kind == MirOperandKind::BorrowWrite);
  if (!storageLoad && !storageBorrowStage) {
    return nullptr;
  }
  const MirPlace *place = body.findPlace(definition->operands.front().place);
  return place != nullptr && (place->type.kind == SemanticType::Storage ||
                              place->type.kind == SemanticType::PrefixStorage)
             ? place
             : nullptr;
}

[[nodiscard]] bool isStorageStagedResult(const MirBody &body,
                                         const MirValue &value) {
  const MirInstruction *definition = findInstruction(body, value.definition);
  if (definition == nullptr || definition->kind != MirInstructionKind::Load ||
      definition->operands.size() != 1 ||
      definition->operands.front().place == 0) {
    return false;
  }
  const MirPlace *place = body.findPlace(definition->operands.front().place);
  return place != nullptr && (place->type.kind == SemanticType::Storage ||
                              place->type.kind == SemanticType::PrefixStorage);
}

[[nodiscard]] bool scalarSpellableArithmeticIntrinsic(IntrinsicKind intrinsic) {
  switch (intrinsic) {
  case IntrinsicKind::IntegerWrappingAdd:
  case IntrinsicKind::IntegerWrappingSubtract:
  case IntrinsicKind::IntegerWrappingMultiply:
  case IntrinsicKind::IntegerSaturatingAdd:
  case IntrinsicKind::IntegerSaturatingSubtract:
  case IntrinsicKind::IntegerSaturatingMultiply:
    return true;
  default:
    return false;
  }
}

// The per-body facts the scalar text step spells from. Function and
// destructor instances project onto the same shape: a destructor has no
// parameters, its receiver is inherently mutable, and its banner names a
// destructor-instance.
struct ScalarBodyFacts {
  const MirBody &body;
  MirBodyKind kind;
  std::uint64_t id;
  std::string_view instanceLabel;
  std::optional<HirClassInstanceId> owner;
  const std::vector<HirBindingId> &parameterBindings;
  ReceiverMutability receiverMutability;
};

class ScalarBodyTextEmitter {
public:
  ScalarBodyTextEmitter(const MirProgram &program,
                        const CppMirBodyEmissionMap &representations,
                        std::size_t indentation, bool failureForm = false)
      : program(program), representations(representations),
        indentation(indentation), failureForm(failureForm) {}

  [[nodiscard]] std::string emit(const MirFunctionInstance &function,
                                 std::string_view familyLabel) {
    return emit(
        ScalarBodyFacts{.body = function.body,
                        .kind = MirBodyKind::Function,
                        .id = function.id,
                        .instanceLabel = "function-instance",
                        .owner = function.owner,
                        .parameterBindings = function.parameterBindings,
                        .receiverMutability = function.receiverMutability},
        familyLabel);
  }

  [[nodiscard]] std::string emit(const MirDestructorInstance &destructor,
                                 std::string_view familyLabel) {
    return emit(
        ScalarBodyFacts{.body = destructor.body,
                        .kind = MirBodyKind::Destructor,
                        .id = destructor.id,
                        .instanceLabel = "destructor-instance",
                        .owner = destructor.owner == 0
                                     ? std::optional<HirClassInstanceId>()
                                     : std::optional(destructor.owner),
                        .parameterBindings = emptyParameterBindings(),
                        .receiverMutability = ReceiverMutability::Mutable},
        familyLabel);
  }

  // Spells one literal value through the shared literal writer so the
  // initializer-schedule surface reuses the exact range assertions and type
  // spellings of the body text step.
  [[nodiscard]] std::string literalSpelling(const Literal &literal,
                                            const SemanticType &type) {
    output.str("");
    emitLiteral(literal, type);
    return output.str();
  }

  // Mirrors emitLiteral's dispatch without throwing, so a probing caller
  // can decline unsupported literal representations fail-closed.
  [[nodiscard]] static bool spellableLiteral(const Literal &literal,
                                             const SemanticType &type) {
    if (std::holds_alternative<std::nullptr_t>(literal)) {
      return type.kind == SemanticType::NullPtr ||
             type.kind == SemanticType::RawPointer ||
             type.kind == SemanticType::CString;
    }
    if (const auto *integer = std::get_if<std::uint64_t>(&literal)) {
      return integerFitsType(*integer, type);
    }
    if (std::holds_alternative<CharacterLiteral>(literal) ||
        std::holds_alternative<bool>(literal)) {
      return true;
    }
    if (std::holds_alternative<std::string>(literal)) {
      return type.kind == SemanticType::StringView;
    }
    if (const auto *value = std::get_if<BinaryFloat>(&literal)) {
      return validBinaryFloat(*value) &&
             (value->format == BinaryFloatFormat::Binary64
                  ? type == SemanticType::Double
                  : type == SemanticType::Float);
    }
    return false;
  }

  [[nodiscard]] std::string emit(const MirConstructorInstance &constructor,
                                 std::string_view familyLabel) {
    // A constructor projects like a mutable-receiver member: its receiver
    // is inherently mutable while the object is under construction, and
    // its banner names a constructor-instance.
    return emit(
        ScalarBodyFacts{.body = constructor.body,
                        .kind = MirBodyKind::Constructor,
                        .id = constructor.id,
                        .instanceLabel = "constructor-instance",
                        .owner = constructor.owner == 0
                                     ? std::optional<HirClassInstanceId>()
                                     : std::optional(constructor.owner),
                        .parameterBindings = constructor.parameterBindings,
                        .receiverMutability = ReceiverMutability::Mutable},
        familyLabel);
  }

  [[nodiscard]] std::string emit(const MirLambdaInstance &lambda,
                                 std::string_view familyLabel) {
    // A lambda body spells only nested inside its closure literal: the
    // receiver is the immutable closure object and capture places spell
    // through their Capture rows rather than storage rows.
    return emit(
        ScalarBodyFacts{.body = lambda.body,
                        .kind = MirBodyKind::Lambda,
                        .id = lambda.id,
                        .instanceLabel = "lambda-instance",
                        .owner = std::optional<HirClassInstanceId>(),
                        .parameterBindings = lambda.parameterBindings,
                        .receiverMutability = ReceiverMutability::ReadOnly},
        familyLabel);
  }

  [[nodiscard]] std::string emitModule(std::string_view familyLabel) {
    return emit(
        ScalarBodyFacts{.body = program.module(),
                        .kind = MirBodyKind::Module,
                        .id = 0,
                        .instanceLabel = "module-instance",
                        .owner = std::nullopt,
                        .parameterBindings = emptyParameterBindings(),
                        .receiverMutability = ReceiverMutability::ReadOnly},
        familyLabel);
  }

  [[nodiscard]] std::string emit(const ScalarBodyFacts &facts,
                                 std::string_view familyLabel) {
    currentFamilyLabel = familyLabel;
    output.str("");
    output << "{\n";
    ++indentation;
    writeIndent();
    output << "// GTI verified-MIR body: " << familyLabel << " "
           << facts.instanceLabel << " " << facts.id << "\n";
    if (failureForm && facts.kind == MirBodyKind::Constructor) {
      writeIndent();
      output << "*__gti_mir_constructor_success = false;\n";
      if (const MirClassInstance *owner =
              disarmedConstructorLifecycleOwner(facts)) {
        writeIndent();
        output << "(*this).__gti_lifecycle_active_" << owner->declaration
               << " = false;\n";
      }
    }
    for (const MirBlock &block : facts.body.blocks) {
      for (const MirInstruction &instruction : block.instructions) {
        if (instruction.kind != MirInstructionKind::Compute ||
            instruction.operation != MirOperation::Closure ||
            closureChainAdmits(program, facts.body, instruction)) {
          continue;
        }
        const MaterializedClosure closure =
            materializedClosure(program, facts.body, instruction);
        if (closure) {
          emitMaterializedClosureFactory(facts.body, closure);
        }
      }
    }
    for (const MirPlace &place : facts.body.places) {
      // Module binding roots are the exact program-storage places named by
      // the initialization plan. Their C++ definitions live outside this
      // function; declaring a body-local carrier would initialize the wrong
      // object and leave the real global untouched.
      if (facts.kind == MirBodyKind::Module &&
          place.root == MirPlaceRootKind::Binding &&
          place.projections.empty() && place.symbol != 0) {
        continue;
      }
      // An owning class local lives in a sealed lifetime slot so failure
      // and scope cleanup can destroy it exactly once from verified MIR.
      if (slotPlace(facts.body, place)) {
        if (canonicalSlotPlaceId(facts.body, place) != place.id) {
          // The duplicate view shares its binding's canonical slot.
          continue;
        }
        writeIndent();
        output << lifetimeSlotSpelling() << '<'
               << bodyLocalTypeSpelling(facts.body, place.type);
        if (representationOnlySlotPlace(facts.body, place)) {
          output << ", true";
        }
        output << "> __gti_mir_p_" << place.id << ";\n";
        // A slot-allocated parameter engages its slot from the argument;
        // MIR models the parameter as initialized at entry.
        if (const std::optional<std::size_t> parameter =
                parameterIndex(place, facts)) {
          writeIndent();
          output << "__gti_mir_p_" << place.id
                 << ".construct(std::move(__gti_mir_arg_" << *parameter
                 << "));\n";
        }
        continue;
      }
      if (facts.kind == MirBodyKind::Constructor &&
          ownedParameterFieldSourcePlace(program, facts.body, place)) {
        // The native member-initializer list consumes this parameter before
        // the body starts; declaring a second carrier would copy or move the
        // already-consumed argument again.
        continue;
      }
      // A storage-rooted place reads or writes its named global directly.
      if (place.root == MirPlaceRootKind::Symbol) {
        continue;
      }
      // An element place spells as subscription on its sibling array.
      if (arrayElementAccess(facts.body, place)) {
        continue;
      }
      if (bindingArrayFieldElementAccess(facts.body, place)) {
        continue;
      }
      if (constantArrayElementFieldAccess(facts.body, place)) {
        continue;
      }
      // A view element spells through the terminal checked helper.
      if (viewElementAccess(facts.body, place)) {
        continue;
      }
      // An unchecked raw-memory projection is an lvalue view over its
      // pointer SSA root, not independent storage.
      if (rawMemoryPlaceAccess(facts.body, place)) {
        continue;
      }
      // A loan carrier place spells through its loan pointer (ADR 018).
      if (place.root == MirPlaceRootKind::Loan) {
        continue;
      }
      // A by-value argument staging temporary never materializes; the
      // consuming call spells the source place.
      if (copyStageForTemporary(facts.body, place) != nullptr) {
        continue;
      }
      if (place.root == MirPlaceRootKind::Value &&
          expectedClassResultDestinationSlot(program, facts.body,
                                             place.value) != nullptr) {
        continue;
      }
      if (place.root == MirPlaceRootKind::Value &&
          placementDirectReturnCall(program, representations, facts.body,
                                    place.value) != nullptr) {
        continue;
      }
      if (place.root == MirPlaceRootKind::Value &&
          expectedPayloadReturnSlot(program, representations, facts.body,
                                    place.value)) {
        continue;
      }
      if (place.root == MirPlaceRootKind::Value &&
          expectedClassExtractionPlace(facts.body, place.value) == &place) {
        continue;
      }
      // A pure root record spells nothing.
      if (unreferencedValueRootedPlace(facts.body, place)) {
        continue;
      }
      // The trailing parameter pack's flattened parameters spell at the
      // one forwarding call; the pack place itself never declares.
      if (place.root == MirPlaceRootKind::Binding &&
          place.projections.empty() &&
          place.type.kind == SemanticType::TypePack) {
        continue;
      }
      // A concrete pack-element place is an ABI view over one flattened
      // parameter. It has no independent local storage.
      if (packElementParameterIndex(program, facts.body,
                                    {.kind = facts.kind, .owner = facts.id},
                                    place)) {
        continue;
      }
      // A field projected from a local binding is an lvalue view of that
      // binding. It never owns separate storage or cleanup.
      if (place.root == MirPlaceRootKind::Binding &&
          !place.projections.empty() &&
          std::all_of(place.projections.begin(), place.projections.end(),
                      [](const MirPlaceProjection &projection) {
                        return projection.kind == MirProjectionKind::Field;
                      })) {
        continue;
      }
      // A This-rooted field element spells through the live member.
      if (place.root == MirPlaceRootKind::This &&
          place.projections.size() == 2 &&
          place.projections[0].kind == MirProjectionKind::Field &&
          place.projections[1].kind == MirProjectionKind::Index) {
        continue;
      }
      // A dereference-projected place spells through its base carrier.
      if (place.root == MirPlaceRootKind::Binding &&
          !place.projections.empty() &&
          place.projections[0].kind == MirProjectionKind::Dereference) {
        continue;
      }
      // A reference parameter keeps its C++ reference at the signature. A
      // local reference is an initially-empty pointer carrier populated by
      // its MIR Initialize from the exact retained loan (ADR 018).
      if (place.root == MirPlaceRootKind::Binding &&
          place.projections.empty() &&
          place.type.kind == SemanticType::Reference) {
        const std::optional<std::size_t> parameter =
            parameterIndex(place, facts);
        if (place.type.arguments.size() != 1) {
          throw std::logic_error(
              "verified reference carrier lost its referent type");
        }
        writeIndent();
        if (place.type.referenceAccess == AccessMode::ReadOnly) {
          output << "const ";
        }
        if (parameter) {
          output << "auto *__gti_mir_p_" << place.id << " = &__gti_mir_arg_"
                 << *parameter << ";\n";
        } else {
          output << typeSpelling(place.type.arguments.front())
                 << " *__gti_mir_p_" << place.id << "{};\n";
        }
        continue;
      }
      // Receiver-place handling is derived from MIR, not selected by the
      // caller: a This-rooted place is the projection carrier (skipped) or
      // one projected field bound by reference to the live member. No
      // admitted body declares a receiver as an ordinary local.
      if (place.root == MirPlaceRootKind::This) {
        // The bare receiver place is only the projection carrier and is never
        // referenced. A field place binds by reference so every load reads
        // the live member and every store lands in it. Constness follows the
        // receiver, not the per-place access mode: a store destination and a
        // read of the same field share one binding, and the probe rejects
        // stores to This-rooted places under a read-only receiver.
        if (place.projections.empty()) {
          continue;
        }
        // A field element place spells only inside its fused array_at
        // accessor; no alias binds (its index value assigns later).
        if (place.projections.size() == 2 &&
            place.projections[1].kind == MirProjectionKind::Index) {
          continue;
        }
        writeIndent();
        output << (facts.receiverMutability == ReceiverMutability::Mutable
                       ? "auto &__gti_mir_p_"
                       : "const auto &__gti_mir_p_")
               << place.id << " = (*this)."
               << fieldSpelling(facts, place.projections.front().field)
               << ";\n";
        continue;
      }
      // A lambda-typed local declares only under a template emission's
      // overlay row (spelling its template parameter name); otherwise its
      // C++ closure type is unnameable and every consumer spells the
      // fused literal inline.
      if (place.type.kind == SemanticType::Lambda &&
          !typeRowExists(place.type)) {
        continue;
      }
      writeIndent();
      output << typeSpelling(place.type) << " __gti_mir_p_" << place.id;
      if (const std::optional<std::size_t> parameter =
              parameterIndex(place, facts)) {
        // A move-only owner parameter cannot copy-initialize its local.
        if (place.type.kind == SemanticType::UniqueOwner) {
          output << " = std::move(__gti_mir_arg_" << *parameter << ')';
        } else {
          output << " = __gti_mir_arg_" << *parameter;
        }
      } else {
        output << "{}";
      }
      output << ";\n";
    }
    for (const MirValue &value : facts.body.values) {
      if (constructorFieldResultSlot(program, facts.body, value.id)) {
        // The Value-rooted place declares the call's exact lifetime slot; the
        // successful constructor-field Initialize moves from that slot.
        continue;
      }
      if (valueRootedClassCallResultSlot(program, representations, facts.body,
                                         value.id)) {
        // The Value-rooted place declares the successful call result's slot;
        // its full-expression Drop remains the sole destructor authority.
        continue;
      }
      if (const ClassSsaLifetimeSlot slot = classSsaLifetimeSlot(
              program, representations, facts.body, value.id)) {
        writeIndent();
        output << lifetimeSlotSpelling() << '<' << typeSpelling(value.info.type)
               << ", true> __gti_mir_v_" << value.id << ";\n";
        continue;
      }
      if (passiveFixedArrayConstructInput(program, facts.body, value.id) !=
          nullptr) {
        continue;
      }
      if (expectedPayloadReturnSlot(program, representations, facts.body,
                                    value.id)) {
        writeIndent();
        output << lifetimeSlotSpelling() << '<' << typeSpelling(value.info.type)
               << ", true> __gti_mir_v_" << value.id << ";\n";
        continue;
      }
      if (expectedClassResultDestinationSlot(program, facts.body, value.id) !=
          nullptr) {
        continue;
      }
      if (expectedDefaultPayloadInitialization(program, facts.body, value.id)) {
        continue;
      }
      if (expectedMoveDestinationSlot(program, facts.body, value.id) !=
          nullptr) {
        continue;
      }
      if (placementDirectReturnCall(program, representations, facts.body,
                                    value.id) != nullptr) {
        continue;
      }
      if (fixedArrayAggregateInputSlot(program, facts.body, value.id) !=
              nullptr ||
          expectedPayloadInitialize(program, facts.body, value.id) != nullptr) {
        writeIndent();
        output << lifetimeSlotSpelling() << '<' << typeSpelling(value.info.type)
               << "> __gti_mir_v_" << value.id << ";\n";
        continue;
      }
      const MirInstruction *valueDefinition =
          findInstruction(facts.body, value.definition);
      if (valueDefinition != nullptr &&
          valueDefinition->kind == MirInstructionKind::Move &&
          valueDefinition->result && *valueDefinition->result == value.id) {
        const bool declaresAtMove =
            constructorFieldMoveInitialize(facts.body, value.id) != nullptr ||
            stagedConstructorFieldPublication(program, facts.body, facts.id,
                                              *valueDefinition) != nullptr ||
            sequencedMovedArgument(facts.body, value.id) == valueDefinition;
        const MovedChainTerminal terminal =
            movedChainTerminal(facts.body, value.id);
        const bool fusesIntoConstruction =
            terminal.consumer != nullptr &&
            terminal.consumer->kind == MirInstructionKind::Construct &&
            movedPlaceChainSource(facts.body, terminal.top,
                                  *terminal.consumer) != nullptr;
        if (declaresAtMove || fusesIntoConstruction) {
          continue;
        }
      }
      if (discardedAssignmentResult(facts.body, value.id)) {
        // A discarded assignment result is only expression metadata. Reading
        // the destination back would invent a copy, which is ill-formed for
        // move-only storage and not present in MIR.
        continue;
      }
      if (valueDefinition != nullptr &&
          fusedSignedMinimumNegation(facts.body, *valueDefinition) != nullptr) {
        continue;
      }
      if (valueDefinition != nullptr &&
          expectedObserverLoadConsumer(facts.body, *valueDefinition) !=
              nullptr) {
        continue;
      }
      if (valueDefinition != nullptr &&
          (fixedArrayAggregateDestinationSlot(program, facts.body,
                                              *valueDefinition) != nullptr ||
           passiveFixedArrayConstructAggregate(program, facts.body,
                                               *valueDefinition))) {
        // The Aggregate constructs its unique consuming binding slot in place.
        continue;
      }
      if (value.info.type.kind == SemanticType::Class) {
        if (valueDefinition != nullptr &&
            stagedClassResultForSource(facts.body, value.id).producer ==
                valueDefinition) {
          // The prepared-parameter slot is also the source construction's
          // storage; neither SSA identity declares independently.
          continue;
        }
        if (valueDefinition != nullptr &&
            storedReferenceFieldPublication(program, facts.body, facts.id,
                                            *valueDefinition) != nullptr) {
          // The member-initializer list binds the referent directly from the
          // reference parameter; this initializer-argument load is metadata.
          continue;
        }
        if (expectedClassExtractionPlace(facts.body, value.id) != nullptr) {
          writeIndent();
          if (value.info.access == AccessMode::ReadOnly) {
            output << "const ";
          }
          output << typeSpelling(value.info.type) << " *__gti_mir_ref_v_"
                 << value.id << "{};\n";
          continue;
        }
        if (callResultLoanIdentity(facts.body, value)) {
          continue;
        }
        if (directTemporaryReceiverForValue(facts.body, value.id)) {
          // The Construct owns the temporary's slot and its sole member call
          // reads that slot; the SSA receiver identity has no storage.
          continue;
        }
        if (valueDefinition != nullptr &&
            classCopyAssignmentFusion(facts.body, *valueDefinition)) {
          // The adjacent assignment consumes this passive copy directly; MIR
          // gives the loaded SSA record no independent lifetime.
          continue;
        }
        if (valueDefinition != nullptr &&
            classMoveArrayAssignmentFusion(facts.body, *valueDefinition)) {
          // The adjacent fixed-array store consumes this move directly. Its
          // by-value checked-write parameter is the MIR staging value.
          continue;
        }
        if (classValuePublicationSlot(facts.body, value.id) != nullptr) {
          // The defining call, move, or construct publishes directly into
          // the unique consuming binding's lifetime slot.
          continue;
        }
        if (conditionalClassReturnJoinSlotForValue(facts.body, value.id) !=
                nullptr ||
            conditionalClassBindingJoinSlotForValue(facts.body, value.id) !=
                nullptr) {
          // Each conditional arm publishes directly into the shared join
          // slot; its class SSA record never owns independent storage.
          continue;
        }
        if (stagedClassResultForResult(facts.body, value.id).slot != nullptr) {
          // The prepared-parameter slot is the staged value's storage.
          continue;
        }
        if (inlineFailureConstructorArgument(program, facts.body, value.id)) {
          // The failure-free inner construction and its MoveValue stage spell
          // together as the outer failure constructor's prvalue argument.
          continue;
        }
        if (failureForm &&
            (returnCopyLoadDefinition(facts.body, value.id) != nullptr ||
             returnDefaultConstructionDefinition(facts.body, value.id) !=
                 nullptr)) {
          // These definitions publish directly into the transformed
          // function's uninitialized result storage.
          continue;
        }
        if (passiveCAbiCallAssignmentResult(program, facts.body, value.id)) {
          const CppMirTypeRepresentation *row =
              representationTypeRow(representations, value.info.type);
          if (row == nullptr || row->spelling.empty() ||
              !row->boundaryConstructible || !row->copyable) {
            throw std::logic_error(
                "verified MIR passive C ABI call result lost its value "
                "boundary row");
          }
          writeIndent();
          output << row->spelling << " __gti_mir_v_" << value.id << "{};\n";
          continue;
        }
        // A class value declares only when its row carries the 0.215
        // boundary proof AND its definition is the value-producing
        // construction that assigns into it — a blanket declaration would
        // run default constructors for values other vocabularies spell
        // without a local.
        const MirInstruction *definition =
            findInstruction(facts.body, value.definition);
        const bool valueProducingConstruct =
            definition != nullptr &&
            definition->kind == MirInstructionKind::Construct &&
            definition->result && *definition->result == value.id &&
            !definition->destination && !definition->receiver &&
            returnConstructDefinition(facts.body, value.id) != definition &&
            !slotConsumedConstruct(facts, *definition);
        // A transformed callee's class result lands in the declared
        // receiving local through the `T *` out-parameter.
        const bool transformedClassResult =
            !valueProducingConstruct && definition != nullptr &&
            definition->kind == MirInstructionKind::Call &&
            definition->result && *definition->result == value.id &&
            transformedCallee(*definition) != nullptr;
        // A terminally contained plain callee's class result assigns into
        // the declared local exactly like the compatibility call site;
        // return-fused results keep their no-local spelling.
        const bool containedPlainResult =
            !valueProducingConstruct && !transformedClassResult &&
            definition != nullptr &&
            definition->kind == MirInstructionKind::Call &&
            definition->result && *definition->result == value.id &&
            definition->functionTarget &&
            definition->intrinsic == IntrinsicKind::None &&
            returnCallDefinition(facts.body, value.id) == nullptr &&
            terminallyContainedPlainCallee(program, representations,
                                           *definition);
        // An expected extraction's class payload lands in the declared
        // local through the spelled member read.
        // A call-input stage sourced from a declared class value copies
        // it into its own declared local under the copyable row.
        const bool valueStagedCopyResult =
            !valueProducingConstruct && !transformedClassResult &&
            definition != nullptr &&
            definition->kind == MirInstructionKind::CallInput &&
            definition->result && *definition->result == value.id &&
            definition->operands.size() == 1 &&
            definition->operands.front().kind == MirOperandKind::Value &&
            definition->localFailureSites.empty() &&
            copyStagedCallInput(facts.body, value.id) == nullptr &&
            std::any_of(representations.types().begin(),
                        representations.types().end(),
                        [&](const CppMirTypeRepresentation &candidate) {
                          return candidate.type == value.info.type &&
                                 candidate.copyable;
                        });
        // A copy load assigns the place into the declared local when the
        // copied row proves both copy members usable.
        const bool copyLoadResult =
            !valueProducingConstruct && !transformedClassResult &&
            definition != nullptr &&
            definition->kind == MirInstructionKind::Load &&
            definition->result && *definition->result == value.id &&
            definition->operands.size() == 1 &&
            definition->operands.front().kind == MirOperandKind::Copy &&
            definition->operands.front().place != 0 &&
            facts.body.findPlace(definition->operands.front().place) !=
                nullptr &&
            definition->localFailureSites.empty() &&
            std::any_of(representations.types().begin(),
                        representations.types().end(),
                        [&](const CppMirTypeRepresentation &candidate) {
                          return candidate.type == value.info.type &&
                                 candidate.copyable;
                        });
        // The explicit default construction of a class with no declared
        // constructor assigns the value-initialized temporary into the
        // declared local.
        const bool defaultConstructionResult =
            !valueProducingConstruct && !transformedClassResult &&
            definition != nullptr &&
            definition->kind == MirInstructionKind::Call &&
            definition->result && *definition->result == value.id &&
            !definition->functionTarget && !definition->constructorTarget &&
            !definition->lambdaTarget && !definition->bodyTarget &&
            !definition->callableInvocation && !definition->receiver &&
            (definition->intrinsic == IntrinsicKind::None ||
             definition->intrinsic ==
                 IntrinsicKind::DefaultTypeParameterConstruction) &&
            definition->operands.empty() &&
            definition->callableArguments.empty() &&
            definition->localFailureSites.empty();
        // A constructor invocation's class result assigns the
        // constructed temporary into the declared local.
        const bool constructorInvocationResult =
            !valueProducingConstruct && !transformedClassResult &&
            definition != nullptr &&
            definition->kind == MirInstructionKind::Call &&
            definition->result && *definition->result == value.id &&
            definition->constructorTarget.has_value() &&
            definition->intrinsic == IntrinsicKind::None &&
            definition->localFailureSites.empty();
        // A never-raising callee's class result assigns into the
        // declared local; the call has no failure dimension at all.
        const bool plainNoRaiseCallResult =
            !valueProducingConstruct && !transformedClassResult &&
            !containedPlainResult && definition != nullptr &&
            definition->kind == MirInstructionKind::Call &&
            definition->result && *definition->result == value.id &&
            definition->functionTarget &&
            definition->intrinsic == IntrinsicKind::None &&
            returnCallDefinition(facts.body, value.id) == nullptr && [&]() {
              const MirFunctionInstance *target =
                  program.findFunctionInstance(*definition->functionTarget);
              return target != nullptr && !target->mayRaiseDefinedFailure &&
                     target->linkage == LanguageLinkage::Gti &&
                     target->definitionKind ==
                         MirFunctionInstance::DefinitionKind::Source;
            }();
        const bool extractionClassResult =
            !valueProducingConstruct && !transformedClassResult &&
            !containedPlainResult && !plainNoRaiseCallResult &&
            definition != nullptr &&
            definition->kind == MirInstructionKind::Call &&
            definition->result && *definition->result == value.id &&
            (definition->intrinsic == IntrinsicKind::ExpectedValue ||
             definition->intrinsic == IntrinsicKind::ExpectedError);
        const auto row = std::find_if(
            representations.types().begin(), representations.types().end(),
            [&](const CppMirTypeRepresentation &candidate) {
              return candidate.type == value.info.type;
            });
        if ((!valueProducingConstruct && !transformedClassResult &&
             !containedPlainResult && !plainNoRaiseCallResult &&
             !constructorInvocationResult && !defaultConstructionResult &&
             !copyLoadResult && !valueStagedCopyResult &&
             !extractionClassResult) ||
            row == representations.types().end() || row->spelling.empty() ||
            !row->boundaryConstructible) {
          continue;
        }
        throw std::logic_error(
            "verified MIR class SSA value reached emission without exact "
            "uninitialized value storage");
      }
      // A borrow-staged call input never materializes: the call spells its
      // place expression directly, so the staged value has no local.
      if (isBorrowStagedResult(facts.body, value)) {
        continue;
      }
      // A storage-staged load never materializes either: the storage
      // intrinsic call spells the storage place lvalue directly.
      if (isStorageStagedResult(facts.body, value)) {
        continue;
      }
      // An owner field staged by a Load for an owner operation never copies;
      // the consuming intrinsic spells the field lvalue directly.
      {
        const MirInstruction *definition =
            findInstruction(facts.body, value.definition);
        const std::vector<MirValueUse> stagedUses =
            nonRootRecordUses(facts.body, value.id);
        const MirInstruction *user =
            stagedUses.size() == 1
                ? findInstruction(facts.body, stagedUses.front().instruction)
                : nullptr;
        if (definition != nullptr &&
            definition->kind == MirInstructionKind::Load && user != nullptr &&
            user->kind == MirInstructionKind::Call &&
            (user->intrinsic == IntrinsicKind::UniqueOwnerBorrow ||
             user->intrinsic == IntrinsicKind::UniqueOwnerBorrowMut ||
             user->intrinsic == IntrinsicKind::UniqueOwnerIsNull)) {
          continue;
        }
      }
      // An Unexpected result spells inline at its consuming Return and
      // never declares: std::unexpected has no default construction.
      if (unexpectedDefinition(facts.body, value.id) != nullptr) {
        continue;
      }
      // A lambda-typed value never declares either; the fused closure
      // chain spells the literal at each consuming invocation.
      if (value.info.type.kind == SemanticType::Lambda) {
        continue;
      }
      // The pack value spells nothing: its flattened parameters spell at
      // the one forwarding call the bounded shape proved.
      if (value.info.type.kind == SemanticType::TypePack) {
        continue;
      }
      // A reference-typed value never declares; its paired call-result
      // loan pointer carries the referent.
      if (value.info.type.kind == SemanticType::Reference) {
        continue;
      }
      // A discharged read's element is published through its loan
      // pointer, never copied into a local.
      {
        bool dischargedReadResult = false;
        for (const MirBlock &block : facts.body.blocks) {
          for (const MirInstruction &instruction : block.instructions) {
            if (storageReferenceReadCall(facts.body, instruction) &&
                instruction.result && *instruction.result == value.id) {
              dischargedReadResult = true;
            }
          }
        }
        if (dischargedReadResult) {
          continue;
        }
      }
      writeIndent();
      output << typeSpelling(value.info.type) << " __gti_mir_v_" << value.id
             << "{};\n";
    }
    for (const MirLoan &loan : facts.body.loans) {
      // A Stored loan binds its reference field in the member initializer
      // list; no pointer local exists in the body.
      if (loan.kind == MirLoanKind::Stored) {
        continue;
      }
      // An owner borrow's loans publish as the fused accessor spelling at
      // the consuming return; no pointer local exists.
      if (ownerBorrowLoanProducer(facts.body, loan) != nullptr) {
        continue;
      }
      // A Return loan erased into a fused construct return binds no
      // pointer local either (ADR 018).
      if (returnLoanErasedByConstruct(facts.body, loan)) {
        continue;
      }
      if (returnLoanErasedByClassSsaSlot(program, representations, facts.body,
                                         loan)) {
        continue;
      }
      // A bounds-checked element borrow's return loan publishes as the
      // fused array_at spelling at the consuming return.
      if (elementBorrowLoanProducer(facts.body, loan) != nullptr) {
        continue;
      }
      const MirPlace *source = facts.body.findPlace(loan.source);
      if (source == nullptr) {
        throw std::logic_error("verified MIR loan lost its source place");
      }
      writeIndent();
      if (loan.access == AccessMode::ReadOnly) {
        output << "const ";
      }
      // A loan published by a discharged storage read points at the
      // element, not the storage that sources it.
      if ((source->type.kind == SemanticType::Storage ||
           source->type.kind == SemanticType::PrefixStorage) &&
          pairedDischargedRead(facts.body, loan.producedBy) != nullptr) {
        if (source->type.arguments.empty()) {
          throw std::logic_error(
              "verified MIR storage loan lost its element type");
        }
        output << typeSpelling(source->type.arguments.front())
               << " *__gti_mir_loan_" << loan.id << "{};\n";
        continue;
      }
      if (loan.kind == MirLoanKind::Parameter) {
        // The entry loan aliases the reference parameter's pointer
        // carrier (ADR 018 §4): the place prelude above already bound
        // the carrier from the argument, so the loan pointer copies it.
        writeIndent();
        output << "auto *__gti_mir_loan_" << loan.id << " = __gti_mir_p_"
               << loan.source << ";\n";
        continue;
      }
      // A loan published by a transformed reference-returning callee
      // points at the callee's return element, not the receiver that
      // sources it (ADR 018 §5).
      if (loan.kind == MirLoanKind::CallResult) {
        const MirInstruction *call =
            loanProducingReferenceCall(program, facts.body, loan);
        const MirFunctionInstance *target =
            call != nullptr && call->functionTarget
                ? program.findFunctionInstance(*call->functionTarget)
                : nullptr;
        if (target == nullptr || target->returnType.arguments.empty()) {
          throw std::logic_error(
              "verified MIR call-result loan lost its element type");
        }
        output << typeSpelling(target->returnType.arguments.front())
               << " *__gti_mir_loan_" << loan.id << "{};\n";
        continue;
      }
      output << typeSpelling(source->type) << " *__gti_mir_loan_" << loan.id
             << "{};\n";
    }
    if (failureForm) {
      for (const MirBlock &block : facts.body.blocks) {
        for (const MirInstruction &instruction : block.instructions) {
          if (instruction.kind == MirInstructionKind::Compute &&
              (!cppMirCheckedOperationHelperSpelling(instruction.operation)
                    .empty() ||
               instruction.operation == MirOperation::Index) &&
              !instruction.localFailureSites.empty() &&
              instruction.info.type.kind != SemanticType::Float &&
              instruction.info.type.kind != SemanticType::Double) {
            writeIndent();
            output << "::gti_internal::backend::mir_failure_status_v1 "
                      "__gti_mir_failure_status_"
                   << instruction.id
                   << " = ::gti_internal::backend::mir_failure_success_v1;\n";
          }
          if ((instruction.kind == MirInstructionKind::Load ||
               instruction.kind == MirInstructionKind::Assign) &&
              instruction.localFailureSites.size() == 1) {
            writeIndent();
            output << "::gti_internal::backend::mir_failure_status_v1 "
                      "__gti_mir_failure_status_"
                   << instruction.id
                   << " = ::gti_internal::backend::mir_failure_success_v1;\n";
          }
          if (instruction.kind == MirInstructionKind::Call &&
              (storageBoundsCheckCall(instruction) ||
               storageAllocationFailureCall(program, instruction) ||
               checkedConversionIntrinsicCall(instruction) ||
               ((instruction.intrinsic == IntrinsicKind::ExpectedValue ||
                 instruction.intrinsic == IntrinsicKind::ExpectedError) &&
                !instruction.localFailureSites.empty()) ||
               (sparseStorageIntrinsic(instruction.intrinsic) &&
                !instruction.localFailureSites.empty() &&
                invokePairedInstruction(facts.body, instruction.id)) ||
               (prefixStorageIntrinsic(instruction.intrinsic) &&
                !instruction.localFailureSites.empty()))) {
            writeIndent();
            output << "::gti_internal::backend::mir_failure_status_v1 "
                      "__gti_mir_failure_status_"
                   << instruction.id
                   << " = ::gti_internal::backend::mir_failure_success_v1;\n";
          }
          if (instruction.kind == MirInstructionKind::Call &&
              transformedCallee(instruction) != nullptr) {
            writeIndent();
            output << "bool __gti_mir_call_success_" << instruction.id
                   << " = false;\n";
            const SemanticType &calleeReturn =
                transformedCallee(instruction)->returnType;
            if (calleeReturn.kind == SemanticType::Reference &&
                producedCallResultLoan(facts.body, instruction) == nullptr) {
              writeIndent();
              if (calleeReturn.referenceAccess == AccessMode::ReadOnly) {
                output << "const ";
              }
              output << typeSpelling(calleeReturn.arguments.front())
                     << " *__gti_mir_discard_" << instruction.id << "{};\n";
            } else if (!instruction.result &&
                       (calleeReturn.kind == SemanticType::Class ||
                        calleeReturn.kind == SemanticType::Lambda ||
                        expectedClassPlacementResultType(
                            program, representations, calleeReturn))) {
              writeIndent();
              output << lifetimeSlotSpelling() << '<'
                     << (calleeReturn.kind == SemanticType::Lambda
                             ? lambdaSlotTypeSpelling(facts.body, calleeReturn)
                             : typeSpelling(calleeReturn))
                     << "> __gti_mir_discard_" << instruction.id << ";\n";
            } else if (!instruction.result &&
                       calleeReturn != SemanticType::Void &&
                       calleeReturn.kind != SemanticType::Reference) {
              writeIndent();
              output << typeSpelling(calleeReturn) << " __gti_mir_discard_"
                     << instruction.id << "{};\n";
            }
          } else if (instruction.kind == MirInstructionKind::Call &&
                     callableValueInvocation(instruction)) {
            const MirInstruction *closure =
                closureChainDefinition(facts.body, instruction.receiver->value);
            const MirLambdaInstance *lambda =
                closure != nullptr && closure->lambdaTarget
                    ? program.findLambda(*closure->lambdaTarget)
                    : nullptr;
            const MirInstruction *staged =
                callableReceiverStage(facts.body, instruction.receiver->value);
            const MirPlace *materialized = materializedCallableReceiverPlace(
                program, facts.body, instruction.receiver->value);
            if (lambda != nullptr || staged != nullptr ||
                materialized != nullptr) {
              writeIndent();
              output << "bool __gti_mir_call_success_" << instruction.id
                     << " = false;\n";
              if (!instruction.result &&
                  instruction.info.type != SemanticType::Void) {
                writeIndent();
                if (instruction.info.type.kind == SemanticType::Lambda) {
                  output << lifetimeSlotSpelling() << '<'
                         << lambdaSlotTypeSpelling(facts.body,
                                                   instruction.info.type)
                         << "> __gti_mir_discard_" << instruction.id << ";\n";
                } else {
                  output << typeSpelling(instruction.info.type)
                         << " __gti_mir_discard_" << instruction.id << "{};\n";
                }
              }
            }
          }
          if (instruction.kind == MirInstructionKind::Construct &&
              transformedConstructor(instruction) != nullptr) {
            writeIndent();
            output << "bool __gti_mir_construct_success_" << instruction.id
                   << " = false;\n";
          }
          if (instruction.kind == MirInstructionKind::Drop &&
              failureDestructorTarget(program, facts.body, instruction) !=
                  nullptr) {
            writeIndent();
            output << "bool __gti_mir_drop_success_" << instruction.id
                   << " = false;\n";
            if (block.activeFailure != 0) {
              writeIndent();
              output << "::gti_failure_record_v1 "
                        "__gti_mir_cleanup_failure_record_"
                     << instruction.id << "{};\n";
            }
          }
        }
      }
    }
    writeIndent();
    output << "std::size_t __gti_mir_bb = " << facts.body.entry << ";\n";
    writeIndent();
    output << "for (;;) {\n";
    ++indentation;
    writeIndent();
    output << "switch (__gti_mir_bb) {\n";
    ++indentation;
    for (const MirBlock &block : facts.body.blocks) {
      writeIndent();
      output << "case " << block.id << ": {\n";
      ++indentation;
      if (failureForm && block.failureParameter != 0) {
        writeIndent();
        output << "// GTI MIR failure-record " << block.failureParameter
               << " cleanup\n";
      }
      for (const MirInstruction &instruction : block.instructions) {
        emitInstruction(block, instruction, facts);
        emitStagedFieldPublication(instruction, facts);
      }
      emitTerminator(block, facts);
      --indentation;
      writeIndent();
      output << "}\n";
    }
    writeIndent();
    output << "default:\n";
    ++indentation;
    writeIndent();
    output << "std::abort();\n";
    --indentation;
    --indentation;
    writeIndent();
    output << "}\n";
    --indentation;
    writeIndent();
    output << "}\n";
    --indentation;
    writeIndent();
    output << "}\n";
    return output.str();
  }

private:
  [[nodiscard]] const MirClassInstance *
  disarmedConstructorLifecycleOwner(const ScalarBodyFacts &facts) const {
    if (!failureForm || facts.kind != MirBodyKind::Constructor ||
        !facts.owner) {
      return nullptr;
    }
    const MirClassInstance *owner = program.findClassInstance(*facts.owner);
    return owner != nullptr &&
                   failureConstructorDisarmsDeclaredDestructor(program, *owner)
               ? owner
               : nullptr;
  }

  [[nodiscard]] static const std::vector<HirBindingId> &
  emptyParameterBindings() {
    static const std::vector<HirBindingId> empty;
    return empty;
  }

  void writeIndent() {
    for (std::size_t index = 0; index < indentation; ++index) {
      output << "  ";
    }
  }

  [[nodiscard]] static MirPlaceId
  constructDestination(const ScalarBodyFacts &facts,
                       const MirInstruction &construct) {
    return constructDestinationSlot(facts.body, construct);
  }

  // The slot protocol owns any construct whose value a slot-place
  // Initialize consumes; the value route must not bypass slot engagement.
  [[nodiscard]] bool
  slotConsumedConstruct(const ScalarBodyFacts &facts,
                        const MirInstruction &instruction) const {
    if (generatedSpecialMemberConstruction(program, facts.body, instruction)) {
      return true;
    }
    if (!instruction.result) {
      return false;
    }
    for (const MirValueUse &use : facts.body.usesOf(*instruction.result)) {
      const MirInstruction *consumer =
          findInstruction(facts.body, use.instruction);
      if (consumer != nullptr &&
          consumer->kind == MirInstructionKind::Initialize &&
          consumer->destination) {
        const MirPlace *destinationPlace =
            facts.body.findPlace(*consumer->destination);
        if (destinationPlace != nullptr &&
            slotPlace(facts.body, *destinationPlace)) {
          return true;
        }
      }
    }
    return false;
  }

  [[nodiscard]] bool slotPlace(const MirBody &body,
                               const MirPlace &place) const {
    return lifetimeSlotPlace(program, representations, body, place);
  }

  // Duplicate bare binding places (a mutable store view and a read-only
  // view of one local) share the local's single lifetime slot: every
  // sibling spells the lowest-id place so the construct, each read, and
  // the destroy all touch the same slot.
  [[nodiscard]] MirPlaceId canonicalSlotPlaceId(const MirBody &body,
                                                const MirPlace &place) const {
    MirPlaceId canonical = place.id;
    for (const MirPlace &candidate : body.places) {
      if (candidate.root == MirPlaceRootKind::Binding &&
          candidate.binding == place.binding && candidate.projections.empty() &&
          candidate.type.kind == place.type.kind && candidate.id < canonical) {
        canonical = candidate.id;
      }
    }
    return canonical;
  }

  // A trivially-droppable GTI value can still require uninitialized storage
  // because its C++ representation is not default constructible. When no MIR
  // obligation governs that storage, the slot must end the representation
  // lifetime at C++ scope exit rather than require a semantic Drop.
  [[nodiscard]] bool representationOnlySlotPlace(const MirBody &body,
                                                 const MirPlace &place) const {
    if (!slotPlace(body, place) || place.traits.drop != DropKind::Trivial) {
      return false;
    }
    return std::none_of(
        body.dropObligations.begin(), body.dropObligations.end(),
        [&](const MirDropObligation &obligation) {
          const MirPlace *dropPlace = body.findPlace(obligation.place);
          if (dropPlace == nullptr) {
            return true;
          }
          if (place.root == MirPlaceRootKind::Binding &&
              dropPlace->root == MirPlaceRootKind::Binding) {
            return place.binding == dropPlace->binding;
          }
          return canonicalSlotPlaceId(body, place) ==
                 canonicalSlotPlaceId(body, *dropPlace);
        });
  }

  [[nodiscard]] const std::string &lifetimeSlotSpelling() {
    const auto found = std::find_if(
        representations.capabilities().begin(),
        representations.capabilities().end(),
        [](const CppMirEmissionCapabilityRepresentation &row) {
          return row.kind == CppMirEmissionCapabilityKind::LifetimeStorage;
        });
    if (found == representations.capabilities().end() ||
        found->spelling.empty()) {
      throw std::logic_error(
          "general MIR body emission lost the sealed lifetime-slot helper");
    }
    return found->spelling;
  }

  [[nodiscard]] const std::string &storageSpelling(const ScalarBodyFacts &facts,
                                                   SymbolId symbol) {
    const CppMirSymbolRepresentation *row = storageRepresentationForBody(
        program, representations, facts.owner, symbol);
    if (row == nullptr || row->spelling.empty()) {
      throw std::logic_error(
          "general MIR body emission lost an exact storage symbol row");
    }
    return row->spelling;
  }

  [[nodiscard]] const std::string &captureSpelling(std::size_t lambdaOwner,
                                                   SymbolId symbol,
                                                   std::size_t ordinal) {
    const auto found = std::find_if(
        representations.symbols().begin(), representations.symbols().end(),
        [&](const CppMirSymbolRepresentation &row) {
          return row.kind == CppMirSymbolRepresentationKind::Capture &&
                 row.owner == lambdaOwner && row.symbol == symbol &&
                 row.ordinal == ordinal;
        });
    if (found == representations.symbols().end() || found->spelling.empty()) {
      throw std::logic_error(
          "general MIR body emission lost an exact capture name row");
    }
    return found->spelling;
  }

  [[nodiscard]] bool typeRowExists(const SemanticType &type) const {
    return std::any_of(representations.types().begin(),
                       representations.types().end(),
                       [&](const CppMirTypeRepresentation &row) {
                         return row.type == type && !row.spelling.empty();
                       });
  }

  [[nodiscard]] const std::string &typeSpelling(const SemanticType &type) {
    const auto found = std::find_if(
        representations.types().begin(), representations.types().end(),
        [&](const CppMirTypeRepresentation &row) { return row.type == type; });
    if (found == representations.types().end() || found->spelling.empty()) {
      throw std::logic_error(
          "general MIR body emission lost a copied type row");
    }
    return found->spelling;
  }

  [[nodiscard]] std::string lambdaSlotTypeSpelling(const MirBody &body,
                                                   const SemanticType &type) {
    if (typeRowExists(type)) {
      return typeSpelling(type);
    }
    const MaterializedClosure closure =
        materializedClosureForType(program, body, type);
    if (!closure) {
      throw std::logic_error(
          "verified MIR lambda slot lost its materialized closure type");
    }
    return "__gti_mir_closure_type_" + std::to_string(closure.closure->id);
  }

  [[nodiscard]] std::string bodyLocalTypeSpelling(const MirBody &body,
                                                  const SemanticType &type) {
    if (type.kind == SemanticType::Lambda) {
      return lambdaSlotTypeSpelling(body, type);
    }
    if (!semanticTypeContainsLambda(type)) {
      return typeSpelling(type);
    }
    const CppMirTypeRepresentation *row =
        representationTypeRow(representations, type);
    if (type.kind != SemanticType::Class || row == nullptr ||
        row->templateNameSpelling.empty() || type.arguments.empty() ||
        !type.valueArguments.empty()) {
      throw std::logic_error(
          "verified MIR lost a structured lambda-dependent class spelling");
    }
    std::string spelling = row->templateNameSpelling + '<';
    for (std::size_t index = 0; index < type.arguments.size(); ++index) {
      if (index != 0) {
        spelling += ", ";
      }
      spelling += bodyLocalTypeSpelling(body, type.arguments[index]);
    }
    spelling += '>';
    return spelling;
  }

  void emitMaterializedClosureFactory(const MirBody &body,
                                      const MaterializedClosure &closure) {
    const std::string suffix = std::to_string(closure.closure->id);
    const std::string factory = "__gti_mir_closure_factory_" + suffix;
    writeIndent();
    output << "auto " << factory << " = [](";
    for (std::size_t index = 0; index < closure.captures.size(); ++index) {
      if (index != 0) {
        output << ", ";
      }
      const MaterializedClosureCapture &capture = closure.captures[index];
      if (capture.mode == LambdaCaptureMode::Copy) {
        output << "const ";
      }
      output << bodyLocalTypeSpelling(body, capture.source->type)
             << (capture.mode == LambdaCaptureMode::Move ? " &&" : " &")
             << "__gti_mir_capture_arg_" << suffix << '_' << index;
    }
    output << ") {\n";
    ++indentation;
    writeIndent();
    output << "return [";
    for (std::size_t index = 0; index < closure.captures.size(); ++index) {
      if (index != 0) {
        output << ", ";
      }
      const MaterializedClosureCapture &capture = closure.captures[index];
      output << captureSpelling(closure.lambda->id,
                                closure.lambda->captureSymbols[index],
                                index + 1)
             << " = ";
      if (capture.mode == LambdaCaptureMode::Move) {
        output << "std::move(";
      }
      output << "__gti_mir_capture_arg_" << suffix << '_' << index;
      if (capture.mode == LambdaCaptureMode::Move) {
        output << ')';
      }
    }
    output << "](";
    for (std::size_t index = 0; index < closure.lambda->parameterTypes.size();
         ++index) {
      if (index != 0) {
        output << ", ";
      }
      output << typeSpelling(closure.lambda->parameterTypes[index])
             << " __gti_mir_arg_" << index;
    }
    if (failureForm) {
      if (!closure.lambda->parameterTypes.empty()) {
        output << ", ";
      }
      if (closure.lambda->returnType != SemanticType::Void) {
        output << typeSpelling(closure.lambda->returnType)
               << " *__gti_mir_out_result, ";
      }
      output << "::gti_failure_record_v1 *__gti_mir_failure_record) -> bool "
             << ScalarBodyTextEmitter(program, representations, indentation,
                                      true)
                    .emit(*closure.lambda, currentFamilyLabel);
    } else {
      output << ") -> " << typeSpelling(closure.lambda->returnType) << ' '
             << ScalarBodyTextEmitter(program, representations, indentation)
                    .emit(*closure.lambda, currentFamilyLabel);
    }
    output << ";\n";
    --indentation;
    writeIndent();
    output << "};\n";
    writeIndent();
    output << "using __gti_mir_closure_type_" << suffix << " = decltype("
           << factory << '(';
    for (std::size_t index = 0; index < closure.captures.size(); ++index) {
      if (index != 0) {
        output << ", ";
      }
      const MaterializedClosureCapture &capture = closure.captures[index];
      output << "std::declval<";
      if (capture.mode == LambdaCaptureMode::Copy) {
        output << "const ";
      }
      output << bodyLocalTypeSpelling(body, capture.source->type)
             << (capture.mode == LambdaCaptureMode::Move ? " &&" : " &")
             << ">()";
    }
    output << "));\n";
  }

  [[nodiscard]] const std::string &fieldSpelling(const ScalarBodyFacts &facts,
                                                 SymbolId field) {
    if (!facts.owner) {
      throw std::logic_error(
          "general MIR body emission lost the receiver class instance");
    }
    const MirClassInstance *owner = program.findClassInstance(*facts.owner);
    const ResolvedMirField resolved =
        owner == nullptr ? ResolvedMirField{}
                         : resolveMirField(program, owner->type, field);
    if (!resolved) {
      throw std::logic_error(
          "general MIR body emission lost an exact receiver field owner");
    }
    return fieldSpelling(resolved.owner->id, field);
  }

  [[nodiscard]] const std::string &fieldSpelling(HirClassInstanceId owner,
                                                 SymbolId field) {
    const auto found = std::find_if(
        representations.symbols().begin(), representations.symbols().end(),
        [&](const CppMirSymbolRepresentation &row) {
          return row.kind == CppMirSymbolRepresentationKind::Field &&
                 row.owner == owner && row.symbol == field && row.ordinal == 0;
        });
    if (found == representations.symbols().end() || found->spelling.empty()) {
      throw std::logic_error(
          "general MIR body emission lost an exact field symbol row");
    }
    return found->spelling;
  }

  [[nodiscard]] std::string fieldProjectionChainSpelling(
      SemanticType currentType,
      const std::vector<MirPlaceProjection> &projections,
      std::size_t firstProjection) {
    std::string spelling;
    for (std::size_t index = firstProjection; index < projections.size();
         ++index) {
      const MirPlaceProjection &projection = projections[index];
      if (projection.kind != MirProjectionKind::Field) {
        throw std::logic_error(
            "verified field projection chain changed after probing");
      }
      const ResolvedMirField resolved =
          resolveMirField(program, currentType, projection.field);
      if (!resolved) {
        throw std::logic_error(
            "verified field projection lost its concrete class instance");
      }
      spelling += '.';
      spelling += fieldSpelling(resolved.owner->id, projection.field);
      currentType = resolved.field->type;
    }
    return spelling;
  }

  void
  emitFieldProjectionChain(SemanticType currentType,
                           const std::vector<MirPlaceProjection> &projections,
                           std::size_t firstProjection) {
    output << fieldProjectionChainSpelling(std::move(currentType), projections,
                                           firstProjection);
  }

  [[nodiscard]] const std::string &bodySpelling(HirFunctionInstanceId target) {
    const MirBodyAddress address{.kind = MirBodyKind::Function,
                                 .owner = target};
    const auto found = std::find_if(
        representations.bodies().begin(), representations.bodies().end(),
        [&](const CppMirBodyNameRepresentation &row) {
          return row.address == address;
        });
    if (found == representations.bodies().end() || found->spelling.empty()) {
      throw std::logic_error(
          "general MIR body emission lost an exact call-target name row");
    }
    return found->spelling;
  }

  [[nodiscard]] static std::optional<std::size_t>
  parameterIndex(const MirPlace &place, const ScalarBodyFacts &facts) {
    const auto parameter =
        std::find(facts.parameterBindings.begin(),
                  facts.parameterBindings.end(), place.binding);
    if (parameter == facts.parameterBindings.end()) {
      return std::nullopt;
    }
    return static_cast<std::size_t>(
        std::distance(facts.parameterBindings.begin(), parameter));
  }

  [[nodiscard]] static bool
  isSyntheticLogicalConstant(const MirOperand &operand) {
    return operand.kind == MirOperandKind::Constant && operand.value == 0 &&
           operand.place == 0 && operand.loan == 0 && operand.literal &&
           operand.type == SemanticType::Bool &&
           std::holds_alternative<bool>(*operand.literal);
  }

  void emitIntegerLiteral(std::uint64_t value) {
    output << value;
    if (value >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      output << "ULL";
    }
  }

  // The emitted spelling must be value-faithful, not merely well-typed: a
  // magnitude outside the target type would round-trip through C++
  // conversion semantics instead of the verified GTI value, so it is
  // emission drift even when the structural probe admitted the body.
  [[nodiscard]] static bool integerFitsType(std::uint64_t value,
                                            const SemanticType &type) {
    switch (type.kind) {
    case SemanticType::Int8:
      return value <= static_cast<std::uint64_t>(
                          std::numeric_limits<std::int8_t>::max());
    case SemanticType::Int16:
      return value <= static_cast<std::uint64_t>(
                          std::numeric_limits<std::int16_t>::max());
    case SemanticType::Int32:
      return value <= static_cast<std::uint64_t>(
                          std::numeric_limits<std::int32_t>::max());
    case SemanticType::Int64:
      return value <= static_cast<std::uint64_t>(
                          std::numeric_limits<std::int64_t>::max());
    case SemanticType::UInt8:
      return value <= std::numeric_limits<std::uint8_t>::max();
    case SemanticType::UInt16:
      return value <= std::numeric_limits<std::uint16_t>::max();
    case SemanticType::UInt32:
      return value <= std::numeric_limits<std::uint32_t>::max();
    case SemanticType::UInt64:
      return true;
    default:
      return false;
    }
  }

  void emitLiteral(const Literal &literal, const SemanticType &type) {
    if (std::holds_alternative<std::nullptr_t>(literal)) {
      // The null literal spells identically for the null type itself and
      // for a pointer-typed use of it.
      if (type.kind != SemanticType::NullPtr &&
          type.kind != SemanticType::RawPointer &&
          type.kind != SemanticType::CString) {
        throw std::logic_error(
            "verified MIR null literal is not pointer-typed");
      }
      output << "nullptr";
      return;
    }
    if (const auto *integer = std::get_if<std::uint64_t>(&literal)) {
      if (!integerFitsType(*integer, type)) {
        throw std::logic_error(
            "verified MIR scalar literal exceeds its exact result type");
      }
      output << "static_cast<" << typeSpelling(type) << ">(";
      emitIntegerLiteral(*integer);
      output << ')';
      return;
    }
    if (const auto *character = std::get_if<CharacterLiteral>(&literal)) {
      output << "std::uint8_t{" << static_cast<unsigned int>(character->value)
             << '}';
      return;
    }
    if (const auto *boolean = std::get_if<bool>(&literal)) {
      output << (*boolean ? "true" : "false");
      return;
    }
    if (const auto *text = std::get_if<std::string>(&literal)) {
      if (type.kind != SemanticType::StringView) {
        throw std::logic_error(
            "verified MIR string literal is not a string view");
      }
      output << cppMirStringViewLiteralSpelling(*text);
      return;
    }
    if (const auto *value = std::get_if<BinaryFloat>(&literal)) {
      const bool binary64 = value->format == BinaryFloatFormat::Binary64;
      if ((binary64 && type != SemanticType::Double) ||
          (!binary64 && type != SemanticType::Float)) {
        throw std::logic_error(
            "verified MIR floating literal disagrees with its typed value");
      }
      output << cppMirBinaryFloatLiteralSpelling(*value);
      return;
    }
    throw std::logic_error(
        "verified MIR scalar-CFG literal has an unsupported representation");
  }

  void emitOperand(const MirOperand &operand,
                   bool allowSyntheticLogicalConstant = false) {
    if (operand.kind == MirOperandKind::Value) {
      output << "__gti_mir_v_" << operand.value;
      return;
    }
    if (allowSyntheticLogicalConstant && isSyntheticLogicalConstant(operand)) {
      emitLiteral(*operand.literal, operand.type);
      return;
    }
    throw std::logic_error(
        "verified MIR scalar-CFG operand is not a proven value");
  }

  void emitReturnOperand(const ScalarBodyFacts &facts,
                         const MirOperand &operand) {
    if (operand.kind == MirOperandKind::Value) {
      emitOperand(operand);
      return;
    }
    if (operand.kind == MirOperandKind::Constant && operand.literal) {
      emitLiteral(*operand.literal, operand.type);
      return;
    }
    if (operand.kind == MirOperandKind::Copy && operand.place != 0) {
      const MirPlace *place = facts.body.findPlace(operand.place);
      if (place == nullptr || place->type != operand.type) {
        throw std::logic_error(
            "verified MIR copied return lost its exact source place");
      }
      emitStoragePlaceValue(facts, *place);
      return;
    }
    throw std::logic_error(
        "verified MIR return operand is not a proven value or passive copy");
  }

  [[nodiscard]] std::string_view expectedConstructionSpelling() const {
    const auto found = std::find_if(
        representations.capabilities().begin(),
        representations.capabilities().end(),
        [](const CppMirEmissionCapabilityRepresentation &row) {
          return row.kind == CppMirEmissionCapabilityKind::Expected;
        });
    if (found == representations.capabilities().end() ||
        found->spelling.empty()) {
      throw std::logic_error(
          "verified MIR Unexpected lost its Expected capability row");
    }
    return found->spelling;
  }

  // Spells the full inline literal for the fused chain that produced
  // `receiver`: captures from the lambda's Capture rows over the enclosing
  // body's place expressions, positional parameters, and the recursively
  // emitted verified lambda body carrying its own banner marker.
  void emitClosureLiteral(const ScalarBodyFacts &facts, MirValueId receiver,
                          bool transformedInvocation = false) {
    const MirInstruction *closure =
        closureChainDefinition(facts.body, receiver);
    const MirLambdaInstance *lambda =
        closure != nullptr && closure->lambdaTarget
            ? program.findLambda(*closure->lambdaTarget)
            : nullptr;
    if (lambda == nullptr) {
      throw std::logic_error(
          "verified MIR invocation lost its fused closure chain");
    }
    output << '[';
    for (std::size_t index = 0; index < closure->operands.size(); ++index) {
      if (index != 0) {
        output << ", ";
      }
      const MirPlace *captured =
          facts.body.findPlace(closure->operands[index].place);
      if (captured == nullptr) {
        throw std::logic_error("verified MIR closure lost a captured place");
      }
      output << captureSpelling(lambda->id, lambda->captureSymbols[index],
                                index + 1)
             << " = ";
      const bool moves = lambda->captureModes[index] == LambdaCaptureMode::Move;
      if (moves) {
        output << "std::move(";
      }
      emitStoragePlaceValue(facts, *captured);
      if (moves) {
        output << ')';
      }
    }
    output << "](";
    for (std::size_t index = 0; index < lambda->parameterTypes.size();
         ++index) {
      if (index != 0) {
        output << ", ";
      }
      output << typeSpelling(lambda->parameterTypes[index]) << " __gti_mir_arg_"
             << index;
    }
    if (transformedInvocation) {
      if (!lambda->parameterTypes.empty()) {
        output << ", ";
      }
      if (lambda->returnType != SemanticType::Void) {
        output << typeSpelling(lambda->returnType)
               << " *__gti_mir_out_result, ";
      }
      output << "::gti_failure_record_v1 *__gti_mir_failure_record) -> bool "
             << ScalarBodyTextEmitter(program, representations, indentation,
                                      true)
                    .emit(*lambda, currentFamilyLabel);
      return;
    }
    output << ") -> " << typeSpelling(lambda->returnType) << ' '
           << ScalarBodyTextEmitter(program, representations, indentation)
                  .emit(*lambda, currentFamilyLabel);
  }

  void emitMaterializedClosureConstruction(const ScalarBodyFacts &facts,
                                           const MaterializedClosure &closure) {
    output << "__gti_mir_p_"
           << canonicalSlotPlaceId(facts.body, *closure.destination)
           << ".construct_from([&]() { return "
           << "__gti_mir_closure_factory_" << closure.closure->id << '(';
    for (std::size_t index = 0; index < closure.captures.size(); ++index) {
      if (index != 0) {
        output << ", ";
      }
      const MaterializedClosureCapture &capture = closure.captures[index];
      if (capture.mode == LambdaCaptureMode::Move) {
        output << "std::move(";
      }
      emitStoragePlaceValue(facts, *capture.source);
      if (capture.mode == LambdaCaptureMode::Move) {
        output << ')';
      }
    }
    output << "); });\n";
  }

  void emitCompute(const MirInstruction &instruction,
                   const ScalarBodyFacts &facts) {
    if (instruction.operation == MirOperation::Aggregate) {
      const PassiveFixedArrayConstructAggregate passive =
          passiveFixedArrayConstructAggregate(program, facts.body, instruction);
      if (passive) {
        const MirPlaceId slot =
            canonicalSlotPlaceId(facts.body, *passive.destination);
        output << "::new (static_cast<void *>(__gti_mir_p_" << slot
               << ".construction_address())) "
               << typeSpelling(instruction.info.type) << '{';
        for (std::size_t index = 0; index < passive.elements.size(); ++index) {
          if (index != 0) {
            output << ", ";
          }
          const MirInstruction &element = *passive.elements[index];
          output << typeSpelling(element.info.type) << '(';
          for (std::size_t argument = 0; argument < element.operands.size();
               ++argument) {
            if (argument != 0) {
              output << ", ";
            }
            emitOperand(element.operands[argument]);
          }
          output << ')';
        }
        output << "};\n";
        writeIndent();
        output << "__gti_mir_p_" << slot << ".mark_constructed();\n";
        return;
      }
      if (const MirPlace *destination = fixedArrayAggregateDestinationSlot(
              program, facts.body, instruction)) {
        const MirPlaceId slot = canonicalSlotPlaceId(facts.body, *destination);
        output << "::new (static_cast<void *>(__gti_mir_p_" << slot
               << ".construction_address())) "
               << typeSpelling(instruction.info.type) << '{';
        for (std::size_t index = 0; index < instruction.operands.size();
             ++index) {
          if (index != 0) {
            output << ", ";
          }
          const MirOperand &operand = instruction.operands[index];
          if (operand.kind != MirOperandKind::Value ||
              fixedArrayAggregateInputSlot(program, facts.body,
                                           operand.value) == nullptr) {
            throw std::logic_error(
                "verified MIR owning aggregate lost its input slot");
          }
          output << "std::move(__gti_mir_v_" << operand.value << ".get())";
        }
        output << "};\n";
        writeIndent();
        output << "__gti_mir_p_" << slot << ".mark_constructed();\n";
        for (const MirOperand &operand : instruction.operands) {
          writeIndent();
          output << "__gti_mir_v_" << operand.value << ".destroy();\n";
        }
        return;
      }
    }
    if (instruction.operation == MirOperation::Comma) {
      // MIR has already sequenced and emitted both operand definitions. Keep
      // the left use explicit, then publish the comma expression's right-hand
      // value without asking C++ to recover source evaluation order.
      output << "static_cast<void>(";
      emitOperand(instruction.operands.front());
      output << ");\n";
      writeIndent();
      output << "__gti_mir_v_" << *instruction.result << " = ";
      emitOperand(instruction.operands.back());
      output << ";\n";
      return;
    }
    output << "__gti_mir_v_" << *instruction.result << " = ";
    if (instruction.operation == MirOperation::Literal) {
      emitLiteral(*instruction.literal, instruction.info.type);
      output << ";\n";
      return;
    }
    if (instruction.operation == MirOperation::Identity) {
      emitOperand(instruction.operands.front());
      output << ";\n";
      return;
    }
    if (instruction.operation == MirOperation::AddressOf) {
      const MirPlace *place =
          facts.body.findPlace(instruction.operands.front().place);
      if (place == nullptr) {
        throw std::logic_error(
            "verified MIR address formation lost its source place");
      }
      output << "(&";
      if (slotPlace(facts.body, *place)) {
        emitStoragePlaceValue(facts, *place);
      } else {
        emitPlaceExpression(facts, *place);
      }
      output << ");\n";
      return;
    }
    if (instruction.operation == MirOperation::PointerAdd ||
        instruction.operation == MirOperation::PointerSubtract ||
        instruction.operation == MirOperation::PointerDifference) {
      const auto emitPointerOperand = [&](const MirOperand &operand) {
        if (operand.kind == MirOperandKind::Constant && operand.literal) {
          emitLiteral(*operand.literal, operand.type);
          return;
        }
        emitOperand(operand);
      };
      output << '(';
      emitPointerOperand(instruction.operands[0]);
      output << (instruction.operation == MirOperation::PointerAdd ? " + "
                                                                   : " - ");
      emitPointerOperand(instruction.operands[1]);
      output << ");\n";
      return;
    }
    if (instruction.operation == MirOperation::Aggregate) {
      output << typeSpelling(instruction.info.type) << '{';
      for (std::size_t index = 0; index < instruction.operands.size();
           ++index) {
        if (index != 0) {
          output << ", ";
        }
        const MirOperand &operand = instruction.operands[index];
        if (operand.kind == MirOperandKind::Constant && operand.literal) {
          emitLiteral(*operand.literal, operand.type);
        } else {
          emitOperand(operand);
        }
      }
      output << "};\n";
      return;
    }
    if (instruction.operation == MirOperation::PayloadConstruct ||
        instruction.operation == MirOperation::PayloadExtract) {
      const CppMirEnumRepresentation *enumRow = nullptr;
      if (instruction.enumOwner) {
        for (const CppMirEnumRepresentation &row : representations.enums()) {
          if (row.owner == *instruction.enumOwner) {
            enumRow = &row;
          }
        }
      }
      const CppMirPayloadVariantRepresentation *variant = nullptr;
      if (enumRow != nullptr && instruction.enumVariant) {
        for (const CppMirPayloadVariantRepresentation &candidate :
             enumRow->payloadVariants) {
          if (candidate.index == *instruction.enumVariant) {
            variant = &candidate;
          }
        }
      }
      if (variant == nullptr) {
        throw std::logic_error(
            "verified MIR payload operation lost its variant row");
      }
      if (instruction.operation == MirOperation::PayloadConstruct) {
        output << enumRow->spelling << '{' << enumRow->spelling
               << "::" << variant->spelling << '{';
        for (std::size_t index = 0; index < instruction.operands.size();
             ++index) {
          if (index != 0) {
            output << ", ";
          }
          emitOperand(instruction.operands[index]);
        }
        output << "}};\n";
        return;
      }
      output << "std::get<" << variant->index << ">(";
      emitOperand(instruction.operands.front());
      output << ".__gti_value).__gti_field_" << *instruction.payloadIndex
             << ";\n";
      return;
    }
    if (instruction.operation == MirOperation::Convert) {
      if (!instruction.localFailureSites.empty()) {
        // The checked conversion contains its range failure terminally
        // inside numeric_cast, exactly like the compatibility spelling.
        output << "::gti_internal::backend::numeric_cast<"
               << typeSpelling(instruction.info.type) << ">(";
        emitOperand(instruction.operands.front());
        output << ");\n";
        return;
      }
      output << "static_cast<" << typeSpelling(instruction.info.type) << ">(";
      emitOperand(instruction.operands.front());
      output << ");\n";
      return;
    }
    if (instruction.operation == MirOperation::LogicalNot) {
      output << '!';
      emitOperand(instruction.operands.front());
      output << ";\n";
      return;
    }
    if (instruction.operation == MirOperation::ExpectedHasValue) {
      const MirOperand &operand = instruction.operands.front();
      if (operand.kind == MirOperandKind::Value) {
        const MirValue *value = facts.body.findValue(operand.value);
        const MirInstruction *load =
            value == nullptr ? nullptr
                             : findInstruction(facts.body, value->definition);
        if (load != nullptr &&
            expectedObserverLoadConsumer(facts.body, *load) == &instruction) {
          const MirPlace *place =
              facts.body.findPlace(load->operands.front().place);
          if (place == nullptr) {
            throw std::logic_error(
                "verified MIR expected observer lost its fused place");
          }
          emitStoragePlaceValue(facts, *place);
        } else {
          emitOperand(operand);
        }
      } else {
        const MirPlace *place = facts.body.findPlace(operand.place);
        if (place == nullptr) {
          throw std::logic_error(
              "verified MIR expected observer lost its borrowed place");
        }
        emitStoragePlaceValue(facts, *place);
      }
      output << ".has_value();\n";
      return;
    }
    if (instruction.operation == MirOperation::Unexpected) {
      // Spelled inline by the consuming Return; nothing stages here. The
      // leading result assignment this writer already emitted is repaired
      // by the caller, which skips Unexpected before writing it.
      throw std::logic_error(
          "verified MIR Unexpected must spell at its consuming Return");
    }
    if (instruction.operation == MirOperation::Positive ||
        instruction.operation == MirOperation::BitwiseNot) {
      output << "static_cast<" << typeSpelling(instruction.info.type) << ">("
             << (instruction.operation == MirOperation::Positive ? '+' : '~');
      emitOperand(instruction.operands.front());
      output << ");\n";
      return;
    }
    if (instruction.operation == MirOperation::Negate) {
      output << "static_cast<" << typeSpelling(instruction.info.type) << ">(-";
      if (const MirInstruction *literal =
              fusedSignedMinimumLiteral(facts.body, instruction)) {
        const std::uint64_t magnitude =
            std::get<std::uint64_t>(*literal->literal);
        emitIntegerLiteral(magnitude - 1);
        output << " - 1";
      } else {
        emitOperand(instruction.operands.front());
      }
      output << ");\n";
      return;
    }
    if (instruction.operation == MirOperation::Index) {
      // A value-level view element read: the terminal helper reports the
      // defined bound contract and never returns on failure, exactly like
      // the place-projected form.
      output << "::gti_internal::backend::string_view_at(";
      emitOperand(instruction.operands[0]);
      output << ", ";
      emitOperand(instruction.operands[1]);
      output << ");\n";
      return;
    }
    const auto spelling = [&]() -> std::string_view {
      switch (instruction.operation) {
      case MirOperation::BitwiseAnd:
        return "&";
      case MirOperation::BitwiseOr:
        return "|";
      case MirOperation::BitwiseXor:
        return "^";
      case MirOperation::Equal:
        return "==";
      case MirOperation::NotEqual:
        return "!=";
      case MirOperation::Less:
        return "<";
      case MirOperation::LessEqual:
        return "<=";
      case MirOperation::Greater:
        return ">";
      case MirOperation::GreaterEqual:
        return ">=";
      default:
        throw std::logic_error(
            "verified MIR scalar-CFG compute operation is unsupported");
      }
    }();
    const bool castResult = instruction.operation == MirOperation::BitwiseAnd ||
                            instruction.operation == MirOperation::BitwiseOr ||
                            instruction.operation == MirOperation::BitwiseXor;
    if (castResult) {
      output << "static_cast<" << typeSpelling(instruction.info.type) << ">(";
    }
    const auto emitBinaryOperand = [&](const MirOperand &operand) {
      if (operand.kind == MirOperandKind::Constant && operand.literal) {
        emitLiteral(*operand.literal, operand.type);
        return;
      }
      emitOperand(operand);
    };
    emitBinaryOperand(instruction.operands[0]);
    output << ' ' << spelling << ' ';
    emitBinaryOperand(instruction.operands[1]);
    if (castResult) {
      output << ')';
    }
    output << ";\n";
  }

  // A store destination is the declared binding unless the place is
  // Symbol-rooted: a storage place has no local binding and reads and
  // writes its named global through the storage row, exactly like Load.
  [[nodiscard]] std::string destinationSpelling(const ScalarBodyFacts &facts,
                                                MirPlaceId destination) {
    const MirPlace *place = facts.body.findPlace(destination);
    if (place != nullptr && facts.kind == MirBodyKind::Module &&
        place->root == MirPlaceRootKind::Binding &&
        place->projections.empty() && place->symbol != 0) {
      return storageSpelling(facts, place->symbol);
    }
    if (place != nullptr) {
      if (const std::optional<RawMemoryPlaceAccess> access =
              rawMemoryPlaceAccess(facts.body, *place)) {
        return rawMemoryPlaceSpelling(*access) +
               fieldProjectionChainSpelling(access->pointeeType,
                                            place->projections, 1);
      }
    }
    if (place != nullptr && place->root == MirPlaceRootKind::Loan) {
      const MirLoan *loan = facts.body.findLoan(place->loan);
      const std::optional<SemanticType> referent =
          loan == nullptr ? std::nullopt
                          : loanReferentType(program, facts.body, *loan);
      if (!referent) {
        throw std::logic_error(
            "verified loan store lost its concrete referent type");
      }
      return "(*__gti_mir_loan_" + std::to_string(place->loan) + ")" +
             fieldProjectionChainSpelling(*referent, place->projections, 0);
    }
    if (place != nullptr && place->root == MirPlaceRootKind::Symbol) {
      return storageSpelling(facts, place->symbol);
    }
    // A dereference-projected reference place stores through its sibling
    // pointer carrier (ADR 018 §4), exactly as the read path spells it. Field
    // projections after the dereference remain part of that lvalue.
    if (place != nullptr && place->root == MirPlaceRootKind::Binding &&
        !place->projections.empty() &&
        place->projections[0].kind == MirProjectionKind::Dereference) {
      for (const MirPlace &candidate : facts.body.places) {
        if (candidate.id != place->id &&
            candidate.root == MirPlaceRootKind::Binding &&
            candidate.binding == place->binding &&
            candidate.projections.empty()) {
          if (candidate.type.kind != SemanticType::Reference ||
              candidate.type.arguments.size() != 1) {
            throw std::logic_error(
                "dereference store destination lost its reference type");
          }
          return "(*__gti_mir_p_" + std::to_string(candidate.id) + ")" +
                 fieldProjectionChainSpelling(candidate.type.arguments.front(),
                                              place->projections, 1);
        }
      }
      throw std::logic_error(
          "dereference store destination lost its base carrier");
    }
    // A field-projected binding is an lvalue view of its declared root; it
    // has no independent local declaration.
    if (place != nullptr && place->root == MirPlaceRootKind::Binding &&
        !place->projections.empty() &&
        std::all_of(place->projections.begin(), place->projections.end(),
                    [](const MirPlaceProjection &projection) {
                      return projection.kind == MirProjectionKind::Field;
                    })) {
      for (const MirPlace &candidate : facts.body.places) {
        if (candidate.id == place->id ||
            candidate.root != MirPlaceRootKind::Binding ||
            candidate.binding != place->binding ||
            !candidate.projections.empty()) {
          continue;
        }
        std::string base =
            "__gti_mir_p_" +
            std::to_string(slotPlace(facts.body, candidate)
                               ? canonicalSlotPlaceId(facts.body, candidate)
                               : candidate.id);
        if (slotPlace(facts.body, candidate)) {
          base += ".get()";
        }
        return base + fieldProjectionChainSpelling(candidate.type,
                                                   place->projections, 0);
      }
      throw std::logic_error("field store destination lost its binding root");
    }
    return "__gti_mir_p_" + std::to_string(destination);
  }

  void emitPlainInstruction(const MirBlock &block,
                            const MirInstruction &instruction,
                            const ScalarBodyFacts &facts) {
    writeIndent();
    if (instruction.kind == MirInstructionKind::Lifecycle) {
      if (const MirPlace *slot = conditionalClassReturnJoinSlotForTransfer(
              facts.body, instruction)) {
        output << "__gti_mir_p_" << canonicalSlotPlaceId(facts.body, *slot)
               << ".destroy(); // GTI MIR conditional return transfer-out\n";
        return;
      }
      if (instruction.fullExpressionEnd != 0) {
        output << "// GTI MIR full-expression boundary "
               << instruction.fullExpressionEnd << "\n";
      } else {
        output << "// GTI MIR cleanup boundary "
               << instruction.cleanupBoundaryEnd << "\n";
      }
      return;
    }
    if (instruction.kind == MirInstructionKind::Compute) {
      if ((!cppMirTerminalCheckedHelperSpelling(instruction.operation)
                .empty() ||
           instruction.operation == MirOperation::Convert) &&
          (instruction.info.type.kind == SemanticType::Float ||
           instruction.info.type.kind == SemanticType::Double)) {
        // The floating site never fires (IEEE-754 nontrapping); the
        // terminal helper spells identically on both forms.
        writeIndent();
        output << "__gti_mir_v_" << *instruction.result << " = ";
        if (instruction.operation == MirOperation::Convert) {
          output << "static_cast<" << typeSpelling(instruction.info.type)
                 << ">(";
        } else {
          output << cppMirTerminalCheckedHelperSpelling(instruction.operation)
                 << '(';
        }
        for (std::size_t index = 0; index < instruction.operands.size();
             ++index) {
          if (index != 0) {
            output << ", ";
          }
          const MirOperand &operand = instruction.operands[index];
          if (operand.kind == MirOperandKind::Constant && operand.literal) {
            emitLiteral(*operand.literal, operand.type);
          } else {
            emitOperand(operand);
          }
        }
        output << ");\n";
        return;
      }
      if (failureForm && instruction.operation == MirOperation::Index &&
          stringViewIndexFailureSite(program, instruction) &&
          invokePairedInstruction(facts.body, instruction.id)) {
        output << "__gti_mir_failure_status_" << instruction.id
               << " = ::gti_internal::backend::mir_checked_string_view_at_v1(";
        emitOperand(instruction.operands[0]);
        output << ", ";
        emitOperand(instruction.operands[1]);
        output << ", &__gti_mir_v_" << *instruction.result << ");\n";
        return;
      }
      if (failureForm &&
          !cppMirCheckedOperationHelperSpelling(instruction.operation)
               .empty() &&
          !instruction.localFailureSites.empty() &&
          invokePairedInstruction(facts.body, instruction.id)) {
        output << "__gti_mir_failure_status_" << instruction.id << " = "
               << cppMirCheckedOperationHelperSpelling(instruction.operation)
               << '<' << typeSpelling(instruction.info.type) << ">(";
        for (std::size_t index = 0; index < instruction.operands.size();
             ++index) {
          if (index != 0) {
            output << ", ";
          }
          const MirOperand &operand = instruction.operands[index];
          if (operand.kind == MirOperandKind::Constant && operand.literal) {
            emitLiteral(*operand.literal, operand.type);
          } else {
            emitOperand(operand);
          }
        }
        if (!instruction.operands.empty()) {
          output << ", ";
        }
        output << "&__gti_mir_v_" << *instruction.result << ");\n";
        return;
      }
      if (instruction.operation == MirOperation::Unexpected) {
        // The consuming Return spells the construction inline; nothing
        // stages here and the result value is never declared.
        writeIndent();
        output << "// unexpected value " << *instruction.result
               << " spells at its consuming return\n";
        return;
      }
      if (!instruction.localFailureSites.empty() &&
          !cppMirTerminalCheckedHelperSpelling(instruction.operation).empty() &&
          (!failureForm ||
           !invokePairedInstruction(facts.body, instruction.id))) {
        // The compatibility terminal helper both checks and contains, so
        // the result assignment is unconditional. In the failure form
        // this spelling applies exactly when no invoke edge pairs: MIR
        // asserted terminal containment, and the status protocol would
        // silently swallow the failure the helper must report.
        output << "__gti_mir_v_" << *instruction.result << " = "
               << cppMirTerminalCheckedHelperSpelling(instruction.operation)
               << '(';
        for (std::size_t index = 0; index < instruction.operands.size();
             ++index) {
          if (index != 0) {
            output << ", ";
          }
          const MirOperand &operand = instruction.operands[index];
          if (operand.kind == MirOperandKind::Constant && operand.literal) {
            emitLiteral(*operand.literal, operand.type);
          } else {
            emitOperand(operand);
          }
        }
        output << ");\n";
        return;
      }
      emitCompute(instruction, facts);
      return;
    }
    if (instruction.kind == MirInstructionKind::Borrow) {
      const MirLoan *loan = nullptr;
      for (const MirLoan &candidate : facts.body.loans) {
        if (instruction.loan && candidate.id == *instruction.loan) {
          loan = &candidate;
        }
      }
      if (loan != nullptr && loan->kind == MirLoanKind::Stored) {
        writeIndent();
        output << "// GTI MIR stored loan " << loan->id
               << " binds its reference field in the initializer list\n";
        return;
      }
      if (loan != nullptr &&
          ownerBorrowLoanProducer(facts.body, *loan) != nullptr) {
        // The owner borrow's loan has no pointer local; the fused
        // accessor spelling publishes at the consuming return.
        writeIndent();
        output << "// GTI MIR owner-borrow loan " << loan->id
               << " publishes at its consuming return\n";
        return;
      }
      if (loan != nullptr &&
          elementBorrowLoanProducer(facts.body, *loan) != nullptr) {
        // The element borrow's loan has no pointer local; the fused
        // array_at spelling publishes at the consuming return.
        writeIndent();
        output << "// GTI MIR element-borrow loan " << loan->id
               << " publishes at its consuming return\n";
        return;
      }
      const MirPlace *source =
          loan == nullptr ? nullptr : facts.body.findPlace(loan->source);
      if (source == nullptr) {
        throw std::logic_error("verified MIR borrow lost its loan source");
      }
      // A loan produced by a discharged storage read publishes the element
      // address through the trusted reference helper: the bounds proof was
      // discharged by the enclosing container's logical-size check, so the
      // sealed runtime guard is defense in depth, exactly as on the
      // compatibility path.
      if (source->type.kind == SemanticType::Storage ||
          source->type.kind == SemanticType::PrefixStorage) {
        const MirInstruction *read =
            pairedDischargedRead(facts.body, loan->producedBy);
        if (read == nullptr || read->operands.size() != 2) {
          throw std::logic_error(
              "verified MIR storage loan lost its discharged read");
        }
        const MirPlace *storage =
            storageStagedPlace(facts.body, read->operands.front());
        if (storage == nullptr) {
          throw std::logic_error(
              "verified MIR discharged read lost its staged storage place");
        }
        output << "__gti_mir_loan_" << loan->id
               << " = &::gti_internal::backend::"
               << storageReadHelperSpelling(read->intrinsic) << '(';
        emitStoragePlaceValue(facts, *storage);
        output << ", ";
        emitOperand(read->operands.back());
        output << ");\n";
        return;
      }
      output << "__gti_mir_loan_" << loan->id << " = &";
      emitStoragePlaceValue(facts, *source);
      output << ";\n";
      return;
    }
    if (instruction.kind == MirInstructionKind::EndBorrow) {
      output << "// GTI MIR end-borrow loan "
             << (instruction.loan ? *instruction.loan : 0) << "\n";
      return;
    }
    if (instruction.kind == MirInstructionKind::Drop &&
        movedOutOwnerDrop(facts.body, instruction)) {
      // The owner's value moved out; the moved-from local's scope-end
      // destruction is a no-op by representation.
      writeIndent();
      output << "// GTI MIR moved-out owner drop of place "
             << *instruction.destination << "\n";
      return;
    }
    if (instruction.kind == MirInstructionKind::Drop &&
        ownedParameterFieldSourceDrop(program, facts.body, instruction)) {
      // The member-initializer list consumed this by-value parameter. Native
      // parameter destruction performs the MIR drop on its moved-from state.
      writeIndent();
      output << "// GTI MIR owned-field parameter drop of place "
             << *instruction.destination << "\n";
      return;
    }
    if (failureForm && facts.kind == MirBodyKind::Constructor &&
        aggregateConstructorRollbackDrop(facts.body, instruction)) {
      // The caller destroys the complete unpublished C++ object after this
      // constructor reports failure. For the admitted no-base/no-user-drop
      // shape that aggregate destruction is exactly this field rollback (and
      // keeps C++ member lifetime rules intact).
      writeIndent();
      output << "// GTI MIR construction rollback deferred to unpublished "
                "object destruction (drop-obligation "
             << instruction.lifecycle.front().source << ")\n";
      return;
    }
    if (failureForm && instruction.kind == MirInstructionKind::Drop &&
        instruction.destination) {
      if (const MirPlace *place =
              facts.body.findPlace(*instruction.destination);
          place != nullptr && place->root == MirPlaceRootKind::Value &&
          returnMoveDefinition(facts.body, place->value) != nullptr) {
        // The out-parameter's move-assignment consumed this value; the
        // moved-from residual needs no destruction step.
        writeIndent();
        output << "// GTI MIR publication-consumed drop of place "
               << *instruction.destination << "\n";
        return;
      }
    }
    if (instruction.kind == MirInstructionKind::Drop &&
        storeConsumedStorageValueDrop(facts.body, instruction)) {
      // The consuming store moved this value into its destination; the
      // moved-from local's scope-end destruction is a no-op.
      writeIndent();
      output << "// GTI MIR store-consumed storage value drop of place "
             << *instruction.destination << "\n";
      return;
    }
    if (instruction.kind == MirInstructionKind::Drop &&
        trivialMirDrop(
            facts.body, instruction,
            instruction.destination &&
                facts.body.findPlace(*instruction.destination) != nullptr &&
                slotPlace(facts.body,
                          *facts.body.findPlace(*instruction.destination)))) {
      // Scope-end destruction of the declared local is exactly the
      // verified semantics for a trivial obligation.
      writeIndent();
      output << "// GTI MIR trivial drop of place " << *instruction.destination
             << "\n";
      return;
    }
    if (failureForm && instruction.kind == MirInstructionKind::Drop &&
        failureDestructorTarget(program, facts.body, instruction) != nullptr) {
      const MirPlace *slot =
          instruction.destination
              ? facts.body.findPlace(*instruction.destination)
              : nullptr;
      if (slot == nullptr || !slotPlace(facts.body, *slot)) {
        throw std::logic_error(
            "verified MIR failure-capable drop lost its lifetime slot");
      }
      const MirLifecycleEvent &drop = instruction.lifecycle.front();
      output << "__gti_mir_drop_success_" << instruction.id << " = __gti_mir_p_"
             << *instruction.destination
             << (drop.conditional ? ".destroy_with_failure_if_engaged("
                                  : ".destroy_with_failure(");
      if (block.activeFailure != 0) {
        output << "&__gti_mir_cleanup_failure_record_" << instruction.id;
      } else {
        output << "__gti_mir_failure_record";
      }
      output << ");\n";
      return;
    }
    if (failureForm && instruction.kind == MirInstructionKind::Drop &&
        instruction.lifecycle.size() == 1 &&
        instruction.lifecycle.front().failureCleanup) {
      // Failure cleanup destroys the engaged slot exactly like the
      // success path: the propagate edge must never leak an engaged
      // lifetime slot past the early false return.
      const MirPlace *slot =
          instruction.destination
              ? facts.body.findPlace(*instruction.destination)
              : nullptr;
      if (slot == nullptr || !slotPlace(facts.body, *slot)) {
        throw std::logic_error(
            "verified MIR failure cleanup lost its lifetime slot");
      }
      output << "__gti_mir_p_" << *instruction.destination << ".destroy();"
             << " // failure cleanup drop-obligation "
             << instruction.lifecycle.front().source << '\n';
      return;
    }
    if (instruction.kind == MirInstructionKind::Construct) {
      const MirPlaceId destination = constructDestination(facts, instruction);
      const MirPlace *slot =
          destination == 0 ? nullptr : facts.body.findPlace(destination);
      if (slot == nullptr || !slotPlace(facts.body, *slot)) {
        throw std::logic_error(
            "verified MIR class construction lost its reparent slot");
      }
      output << "__gti_mir_p_" << destination << ".construct(";
      for (std::size_t index = 0; index < instruction.operands.size();
           ++index) {
        if (index != 0) {
          output << ", ";
        }
        const bool consumed =
            instruction.operands[index].type.kind == SemanticType::Class ||
            instruction.operands[index].type.kind ==
                SemanticType::UniqueOwner ||
            instruction.operands[index].type.kind == SemanticType::Storage ||
            instruction.operands[index].type.kind ==
                SemanticType::PrefixStorage;
        if (consumed) {
          output << "std::move(";
        }
        emitOperand(instruction.operands[index]);
        if (consumed) {
          output << ')';
        }
      }
      output << ");\n";
      return;
    }
    if (instruction.kind == MirInstructionKind::Drop) {
      const MirPlace *slot =
          instruction.destination
              ? facts.body.findPlace(*instruction.destination)
              : nullptr;
      if (slot != nullptr && slot->root == MirPlaceRootKind::Value &&
          expectedClassResultDestinationSlot(program, facts.body,
                                             slot->value) != nullptr) {
        output << "// GTI MIR expected result shell reparented\n";
        return;
      }
      if (slot == nullptr || !slotPlace(facts.body, *slot)) {
        throw std::logic_error(
            "verified MIR class drop lost its lifetime slot");
      }
      output << "__gti_mir_p_" << *instruction.destination << ".destroy();\n";
      return;
    }
    if (instruction.kind == MirInstructionKind::Load) {
      if (const ClassCopyAssignmentFusion fusion =
              classCopyAssignmentFusion(facts.body, instruction)) {
        output << "// class copy load " << *instruction.result
               << " fuses into assignment " << fusion.assignment->id << "\n";
        return;
      }
      if (expectedObserverLoadConsumer(facts.body, instruction) != nullptr) {
        writeIndent();
        output << "// expected observer load " << *instruction.result
               << " fuses into its single consumer\n";
        return;
      }
      const MirPlace *source =
          facts.body.findPlace(instruction.operands.front().place);
      if (source != nullptr && source->root == MirPlaceRootKind::Symbol) {
        output << "__gti_mir_v_" << *instruction.result << " = ";
        emitPlaceExpression(facts, *source);
        output << ";\n";
        return;
      }
      if (source != nullptr) {
        if (const std::optional<BindingArrayFieldElementAccess> access =
                bindingArrayFieldElementAccess(facts.body, *source)) {
          if (failureForm && !instruction.localFailureSites.empty()) {
            output << "__gti_mir_failure_status_" << instruction.id
                   << " = ::gti_internal::backend::mir_checked_array_read_v1(";
            emitBindingArrayFieldParent(facts, *access);
            output << ", ";
            emitElementIndexValue(access->terminalIndex());
            output << ", &__gti_mir_v_" << *instruction.result << ");\n";
            return;
          }
          if (!instruction.localFailureSites.empty()) {
            output << "__gti_mir_v_" << *instruction.result
                   << " = ::gti_internal::backend::array_at(";
            emitBindingArrayFieldParent(facts, *access);
            output << ", ";
            emitElementIndexValue(access->terminalIndex());
            output << ");\n";
            return;
          }
          output << "__gti_mir_v_" << *instruction.result << " = ";
          emitBindingArrayFieldElement(facts, *access);
          output << ";\n";
          return;
        }
        if (const std::optional<ReceiverArrayElementAccess> access =
                receiverArrayElementAccess(facts.body, *source)) {
          if (failureForm && !instruction.localFailureSites.empty()) {
            output << "__gti_mir_failure_status_" << instruction.id
                   << " = ::gti_internal::backend::mir_checked_array_read_v1(";
            emitReceiverArrayParent(facts, *access);
            output << ", ";
            emitElementIndexValue(access->index);
            output << ", &__gti_mir_v_" << *instruction.result << ");\n";
            return;
          }
          if (!instruction.localFailureSites.empty()) {
            output << "__gti_mir_v_" << *instruction.result
                   << " = ::gti_internal::backend::array_at(";
            emitReceiverArrayParent(facts, *access);
            output << ", ";
            emitElementIndexValue(access->index);
            output << ");\n";
            return;
          }
          output << "__gti_mir_v_" << *instruction.result << " = ";
          emitReceiverArrayElement(facts, *access);
          output << ";\n";
          return;
        }
      }
      if (source != nullptr &&
          (source->root == MirPlaceRootKind::Loan ||
           (source->root == MirPlaceRootKind::This &&
            source->projections.size() == 2) ||
           (source->root == MirPlaceRootKind::Binding &&
            !source->projections.empty() &&
            source->projections[0].kind == MirProjectionKind::Dereference))) {
        output << "__gti_mir_v_" << *instruction.result << " = ";
        emitPlaceExpression(facts, *source);
        output << ";\n";
        return;
      }
      if (source != nullptr) {
        if (const std::optional<ClassSubscriptAccess> access =
                classSubscriptAccess(program, facts.body, *source)) {
          const MirFunctionInstance *member = containedSubscriptMember(
              program, representations, access->owner,
              ReceiverMutability::ReadOnly, access->indexType);
          if (member == nullptr) {
            throw std::logic_error(
                "verified MIR subscript read lost its contained member");
          }
          // The compatibility subscription: a read-only receiver wrapper
          // around the base, the member's emitted name, and the index.
          output << "__gti_mir_v_" << *instruction.result
                 << " = (::gti_internal::backend::read_only_receiver(";
          emitStoragePlaceValue(facts, *facts.body.findPlace(access->base));
          output << "))." << bodySpelling(member->id) << '(';
          emitSubscriptIndex(*access);
          output << ");\n";
          return;
        }
        if (const std::optional<ArrayElementAccess> access =
                viewElementAccess(facts.body, *source)) {
          // The terminal helper reports the defined bound contract and
          // never returns on failure, on both text forms.
          output << "__gti_mir_v_" << *instruction.result
                 << " = ::gti_internal::backend::string_view_at(__gti_mir_p_"
                 << access->array << ", ";
          emitElementIndexValue(*access);
          output << ");\n";
          return;
        }
        if (const std::optional<ArrayElementAccess> access =
                arrayElementAccess(facts.body, *source)) {
          if (failureForm && !instruction.localFailureSites.empty()) {
            output << "__gti_mir_failure_status_" << instruction.id
                   << " = ::gti_internal::backend::mir_checked_array_read_v1(";
            emitArrayParent(facts, *access);
            output << ", ";
            emitElementIndexValue(*access);
            output << ", &__gti_mir_v_" << *instruction.result << ");\n";
            return;
          }
          if (!instruction.localFailureSites.empty()) {
            // The plain shape spells the terminal array_at accessor,
            // which contains the bounds failure inside itself and never
            // returns on it, exactly like the compatibility element read.
            output << "__gti_mir_v_" << *instruction.result
                   << " = ::gti_internal::backend::array_at(";
            emitArrayParent(facts, *access);
            output << ", ";
            emitElementIndexValue(*access);
            output << ");\n";
            return;
          }
          output << "__gti_mir_v_" << *instruction.result << " = ";
          emitArrayElement(facts, *access);
          output << ";\n";
          return;
        }
      }
      output << "__gti_mir_v_" << *instruction.result << " = ";
      // The loaded place binds the live value: a lifetime slot exposes it
      // through its checked accessor.
      emitStoragePlaceValue(
          facts, *facts.body.findPlace(instruction.operands.front().place));
      output << ";\n";
      return;
    }
    if (instruction.kind == MirInstructionKind::Assign) {
      if (instruction.operands.size() == 1 &&
          instruction.operands.front().kind == MirOperandKind::Value) {
        const PassiveCAbiCallAssignmentResult passive =
            passiveCAbiCallAssignmentResult(program, facts.body,
                                            instruction.operands.front().value);
        if (passive.assignment == &instruction &&
            passive.destination != nullptr) {
          const CppMirTypeRepresentation *row =
              representationTypeRow(representations, passive.destination->type);
          if (row == nullptr || !row->copyable) {
            throw std::logic_error(
                "verified MIR passive C ABI assignment lost its copyable "
                "row");
          }
          emitStoragePlaceValue(facts, *passive.destination);
          output << " = __gti_mir_v_" << instruction.operands.front().value
                 << ";\n";
          return;
        }
      }
      if (const ClassCopyAssignmentFusion fusion =
              classCopyAssignmentFusion(facts.body, instruction)) {
        const CppMirTypeRepresentation *row =
            representationTypeRow(representations, fusion.source->type);
        if (row == nullptr || !row->copyable) {
          throw std::logic_error(
              "verified MIR class copy assignment lost its copyable row");
        }
        emitStoragePlaceValue(facts, *fusion.destination);
        output << " = ";
        emitStoragePlaceValue(facts, *fusion.source);
        output << ";\n";
        return;
      }
      if (instruction.operands.size() == 1 &&
          instruction.operands.front().kind == MirOperandKind::Value) {
        const ValueRootedClassCallResultSlot resultSlot =
            valueRootedClassCallResultSlot(program, representations, facts.body,
                                           instruction.operands.front().value);
        if (resultSlot.consumer == &instruction && resultSlot.slot != nullptr) {
          const MirPlace *destination =
              instruction.destination
                  ? facts.body.findPlace(*instruction.destination)
                  : nullptr;
          if (destination == nullptr || !slotPlace(facts.body, *destination)) {
            throw std::logic_error(
                "verified MIR class call assignment lost its destination "
                "lifetime slot");
          }
          emitStoragePlaceValue(facts, *destination);
          output << " = std::move(__gti_mir_p_"
                 << canonicalSlotPlaceId(facts.body, *resultSlot.slot)
                 << ".get());\n";
          return;
        }
      }
      if (const ClassMoveArrayAssignmentFusion fusion =
              classMoveArrayAssignmentFusion(facts.body, instruction)) {
        const std::optional<ArrayElementAccess> access =
            arrayElementAccess(facts.body, *fusion.destination);
        if (!access) {
          throw std::logic_error(
              "verified MIR class move assignment lost its array element");
        }
        if (failureForm && !instruction.localFailureSites.empty()) {
          output << "__gti_mir_failure_status_" << instruction.id
                 << " = ::gti_internal::backend::mir_checked_array_write_v1(";
          emitArrayParent(facts, *access);
          output << ", ";
          emitElementIndexValue(*access);
          output << ", std::move(";
          emitStoragePlaceValue(facts, *fusion.source);
          output << "));\n";
        } else {
          emitArrayElement(facts, *access);
          output << " = std::move(";
          emitStoragePlaceValue(facts, *fusion.source);
          output << ");\n";
        }
        return;
      }
    }
    if (instruction.kind == MirInstructionKind::Initialize &&
        instruction.destination) {
      const MirPlace *slot = facts.body.findPlace(*instruction.destination);
      if (const ConditionalClassBindingJoin join =
              conditionalClassBindingJoinForInitialize(facts.body,
                                                       instruction)) {
        output << "__gti_mir_p_" << canonicalSlotPlaceId(facts.body, *join.slot)
               << ".destroy(); // GTI MIR conditional binding reparent\n";
        return;
      }
      if (slot != nullptr && slot->root == MirPlaceRootKind::Binding &&
          slot->projections.empty() &&
          slot->type.kind == SemanticType::Reference &&
          instruction.operands.size() == 1 &&
          instruction.operands.front().kind == MirOperandKind::Loan) {
        const MirLoan *address =
            referenceBindingAddressLoan(program, facts.body, instruction);
        if (address == nullptr) {
          throw std::logic_error(
              "verified reference binding lost its address loan");
        }
        output << "__gti_mir_p_" << slot->id << " = __gti_mir_loan_"
               << address->id << ";\n";
        return;
      }
      if (slot != nullptr && instruction.operands.size() == 1 &&
          instruction.operands.front().kind == MirOperandKind::Value) {
        const ConstructorFieldResultSlot fieldResult =
            constructorFieldResultSlot(program, facts.body,
                                       instruction.operands.front().value);
        if (fieldResult.initialize == &instruction &&
            fieldResult.field == slot && fieldResult.slot != nullptr) {
          output << destinationSpelling(facts, slot->id)
                 << " = std::move(__gti_mir_p_"
                 << canonicalSlotPlaceId(facts.body, *fieldResult.slot)
                 << ".get());\n";
          writeIndent();
          output << "__gti_mir_p_"
                 << canonicalSlotPlaceId(facts.body, *fieldResult.slot)
                 << ".destroy();\n";
          return;
        }
      }
      if (slot != nullptr && instruction.operands.size() == 1 &&
          instruction.operands.front().kind == MirOperandKind::Value) {
        const MirValue *aggregateValue =
            facts.body.findValue(instruction.operands.front().value);
        const MirInstruction *aggregate =
            aggregateValue == nullptr
                ? nullptr
                : findInstruction(facts.body, aggregateValue->definition);
        const PassiveFixedArrayConstructAggregate passiveAggregate =
            aggregate == nullptr ? PassiveFixedArrayConstructAggregate{}
                                 : passiveFixedArrayConstructAggregate(
                                       program, facts.body, *aggregate);
        const MirPlace *directAggregate =
            aggregate == nullptr ? nullptr
                                 : fixedArrayAggregateDestinationSlot(
                                       program, facts.body, *aggregate);
        if (directAggregate == nullptr && passiveAggregate) {
          directAggregate = passiveAggregate.destination;
        }
        if (directAggregate != nullptr && directAggregate->id == slot->id) {
          output << "// GTI MIR aggregate reparent into p"
                 << canonicalSlotPlaceId(facts.body, *directAggregate) << "\n";
          return;
        }
      }
      if (slot != nullptr && slotPlace(facts.body, *slot)) {
        if (instruction.operands.size() == 1 &&
            instruction.operands.front().kind == MirOperandKind::Value) {
          const MirValue *record =
              facts.body.findValue(instruction.operands.front().value);
          const MirInstruction *definition =
              record == nullptr
                  ? nullptr
                  : findInstruction(facts.body, record->definition);
          const std::optional<GeneratedSpecialMemberConstruction> special =
              definition == nullptr ? std::nullopt
                                    : generatedSpecialMemberConstruction(
                                          program, facts.body, *definition);
          if (special && special->initialize == &instruction &&
              special->destination == slot) {
            output << "__gti_mir_p_" << canonicalSlotPlaceId(facts.body, *slot)
                   << ".construct(";
            if (special->moved) {
              output << "std::move(";
            }
            emitStoragePlaceValue(facts, *special->source);
            if (special->moved) {
              output << ')';
            }
            output << ");\n";
            return;
          }
        }
        if (instruction.operands.size() == 1 &&
            instruction.operands.front().kind == MirOperandKind::Value) {
          const MirPlace *directExpected = expectedClassResultDestinationSlot(
              program, facts.body, instruction.operands.front().value);
          if (directExpected != nullptr && directExpected->id == slot->id) {
            output << "// GTI MIR expected result reparent into p"
                   << canonicalSlotPlaceId(facts.body, *directExpected) << "\n";
            return;
          }
        }
        if (instruction.operands.size() == 1 &&
            instruction.operands.front().kind == MirOperandKind::Value &&
            expectedPayloadInitialize(program, facts.body,
                                      instruction.operands.front().value) ==
                &instruction) {
          const MirValueId payload = instruction.operands.front().value;
          output << "__gti_mir_p_" << canonicalSlotPlaceId(facts.body, *slot)
                 << ".construct(std::move(__gti_mir_v_" << payload
                 << ".get()));\n";
          writeIndent();
          output << "__gti_mir_v_" << payload << ".destroy();\n";
          return;
        }
        if (instruction.operands.size() == 1 &&
            instruction.operands.front().kind == MirOperandKind::Value) {
          const ExpectedDefaultPayloadInitialization defaultPayload =
              expectedDefaultPayloadInitialization(
                  program, facts.body, instruction.operands.front().value);
          if (defaultPayload.initialize == &instruction &&
              defaultPayload.destination == slot) {
            output << "__gti_mir_p_" << canonicalSlotPlaceId(facts.body, *slot)
                   << ".construct("
                   << typeSpelling(instruction.operands.front().type)
                   << "{});\n";
            return;
          }
        }
        if (instruction.operands.size() == 1 &&
            instruction.operands.front().kind == MirOperandKind::Value &&
            (instruction.operands.front().type.kind == SemanticType::Storage ||
             instruction.operands.front().type.kind ==
                 SemanticType::PrefixStorage)) {
          // A storage value constructs its lifetime slot by move.
          output << "__gti_mir_p_" << *instruction.destination
                 << ".construct(std::move(__gti_mir_v_"
                 << instruction.operands.front().value << "));\n";
          return;
        }
        if (instruction.operands.size() == 1 &&
            instruction.operands.front().kind == MirOperandKind::Value &&
            instruction.operands.front().type.kind == SemanticType::Class) {
          const MirPlace *directDestination = classValuePublicationSlot(
              facts.body, instruction.operands.front().value);
          const MirPlace *conditionalDestination =
              conditionalClassReturnJoinSlotForValue(
                  facts.body, instruction.operands.front().value);
          const MirValue *directRecord =
              facts.body.findValue(instruction.operands.front().value);
          const MirInstruction *directDefinition =
              directRecord == nullptr
                  ? nullptr
                  : findInstruction(facts.body, directRecord->definition);
          const bool transformedFailureConstruction =
              failureForm && directDefinition != nullptr &&
              directDefinition->kind == MirInstructionKind::Construct &&
              !directDefinition->destination &&
              transformedConstructor(*directDefinition) != nullptr;
          const MirPlace *publishedDestination = directDestination != nullptr
                                                     ? directDestination
                                                     : conditionalDestination;
          if (publishedDestination != nullptr &&
              publishedDestination->id == instruction.destination &&
              directDefinition != nullptr &&
              (directDefinition->kind == MirInstructionKind::Call ||
               directDefinition->kind == MirInstructionKind::Move ||
               transformedFailureConstruction)) {
            output << "// GTI MIR reparent into p"
                   << canonicalSlotPlaceId(facts.body, *publishedDestination)
                   << "\n";
            return;
          }
          // A class value that lives in a declared local (a transformed
          // callee's published result) engages the slot here by move; the
          // reparent comment is exact only when the paired construct
          // built the value inside this slot already — a destination on
          // the construct itself, not merely a consuming initialize.
          const MirValue *record =
              facts.body.findValue(instruction.operands.front().value);
          const MirInstruction *definition =
              record == nullptr
                  ? nullptr
                  : findInstruction(facts.body, record->definition);
          const bool builtInSlot =
              definition != nullptr &&
              definition->kind == MirInstructionKind::Construct &&
              definition->destination &&
              slotConsumedConstruct(facts, *definition);
          if (definition != nullptr &&
              definition->kind == MirInstructionKind::Construct &&
              !definition->destination && !definition->receiver &&
              slotConsumedConstruct(facts, *definition)) {
            // The undeclarable class value publishes here instead: the
            // slot constructs in place from the construction's own
            // arguments, exactly like the compatibility direct
            // initialization.
            output << "__gti_mir_p_" << *instruction.destination
                   << ".construct(";
            for (std::size_t index = 0; index < definition->operands.size();
                 ++index) {
              if (index != 0) {
                output << ", ";
              }
              // An ordered moved-place chain operand spells std::move
              // over the source place directly.
              if (const MirPlace *chainSource =
                      definition->operands[index].kind == MirOperandKind::Value
                          ? movedPlaceChainSource(
                                facts.body, definition->operands[index].value,
                                *definition)
                          : nullptr) {
                output << "std::move(";
                emitStoragePlaceValue(facts, *chainSource);
                output << ')';
                continue;
              }
              // The construction consumes its arguments: class-typed
              // operands move so deleted copy constructors cannot reject
              // the call.
              const bool consumed = definition->operands[index].type.kind ==
                                        SemanticType::Class ||
                                    definition->operands[index].type.kind ==
                                        SemanticType::UniqueOwner ||
                                    definition->operands[index].type.kind ==
                                        SemanticType::Storage ||
                                    definition->operands[index].type.kind ==
                                        SemanticType::PrefixStorage;
              if (consumed) {
                output << "std::move(";
              }
              emitOperand(definition->operands[index]);
              if (consumed) {
                output << ')';
              }
            }
            output << ");\n";
            return;
          }
          if (!builtInSlot) {
            output << "__gti_mir_p_" << *instruction.destination
                   << ".construct(std::move(__gti_mir_v_"
                   << instruction.operands.front().value << "));\n";
            return;
          }
        }
        output << "// GTI MIR reparent into p" << *instruction.destination
               << "\n";
        return;
      }
    }
    if (const MirPlace *destinationPlace =
            facts.body.findPlace(*instruction.destination)) {
      if (const std::optional<BindingArrayFieldElementAccess> access =
              bindingArrayFieldElementAccess(facts.body, *destinationPlace)) {
        if (failureForm && !instruction.localFailureSites.empty()) {
          output << "__gti_mir_failure_status_" << instruction.id
                 << " = ::gti_internal::backend::mir_checked_array_write_v1(";
          emitBindingArrayFieldParent(facts, *access);
          output << ", ";
          emitElementIndexValue(access->terminalIndex());
          output << ", ";
          emitOperand(instruction.operands.front());
          output << ");\n";
        } else {
          emitBindingArrayFieldElement(facts, *access);
          output << " = ";
          emitOperand(instruction.operands.front());
          output << ";\n";
        }
        if (instruction.kind == MirInstructionKind::Assign &&
            instruction.result &&
            !nonRootRecordUses(facts.body, *instruction.result).empty()) {
          writeIndent();
          output << "__gti_mir_v_" << *instruction.result << " = ";
          emitOperand(instruction.operands.front());
          output << ";\n";
        }
        return;
      }
      if (const std::optional<ReceiverArrayElementAccess> access =
              receiverArrayElementAccess(facts.body, *destinationPlace)) {
        if (failureForm && !instruction.localFailureSites.empty()) {
          output << "__gti_mir_failure_status_" << instruction.id
                 << " = ::gti_internal::backend::mir_checked_array_write_v1(";
          emitReceiverArrayParent(facts, *access);
          output << ", ";
          emitElementIndexValue(access->index);
          output << ", ";
          emitOperand(instruction.operands.front());
          output << ");\n";
        } else {
          emitReceiverArrayElement(facts, *access);
          output << " = ";
          emitOperand(instruction.operands.front());
          output << ";\n";
        }
        if (instruction.kind == MirInstructionKind::Assign &&
            instruction.result &&
            !nonRootRecordUses(facts.body, *instruction.result).empty()) {
          writeIndent();
          output << "__gti_mir_v_" << *instruction.result << " = ";
          emitOperand(instruction.operands.front());
          output << ";\n";
        }
        return;
      }
      if (const std::optional<ClassSubscriptAccess> access =
              classSubscriptAccess(program, facts.body, *destinationPlace)) {
        const MirFunctionInstance *member = containedSubscriptMember(
            program, representations, access->owner,
            ReceiverMutability::Mutable, access->indexType);
        if (member == nullptr) {
          throw std::logic_error(
              "verified MIR subscript store lost its contained member");
        }
        output << '(';
        emitStoragePlaceValue(facts, *facts.body.findPlace(access->base));
        output << ")." << bodySpelling(member->id) << '(';
        emitSubscriptIndex(*access);
        output << ") = ";
        emitOperand(instruction.operands.front());
        output << ";\n";
        if (instruction.kind == MirInstructionKind::Assign &&
            instruction.result &&
            !nonRootRecordUses(facts.body, *instruction.result).empty()) {
          writeIndent();
          output << "__gti_mir_v_" << *instruction.result << " = ";
          emitOperand(instruction.operands.front());
          output << ";\n";
        }
        return;
      }
      if (const std::optional<ArrayElementAccess> access =
              arrayElementAccess(facts.body, *destinationPlace)) {
        if (failureForm && !instruction.localFailureSites.empty()) {
          output << "__gti_mir_failure_status_" << instruction.id
                 << " = ::gti_internal::backend::mir_checked_array_write_v1(";
          emitArrayParent(facts, *access);
          output << ", ";
          emitElementIndexValue(*access);
          output << ", ";
          emitOperand(instruction.operands.front());
          output << ");\n";
        } else {
          emitArrayElement(facts, *access);
          output << " = ";
          emitOperand(instruction.operands.front());
          output << ";\n";
        }
        if (instruction.kind == MirInstructionKind::Assign &&
            instruction.result &&
            !nonRootRecordUses(facts.body, *instruction.result).empty()) {
          writeIndent();
          output << "__gti_mir_v_" << *instruction.result << " = ";
          emitOperand(instruction.operands.front());
          output << ";\n";
        }
        return;
      }
    }
    // The failure form's checked compound assignment detects through the
    // base operation's status helper over the destination place; the
    // paired invoke consumes the status and writes the record. The result
    // value re-reads the committed destination, exactly like the plain
    // assignment's follow-up read (the helper leaves the destination
    // untouched on failure, and the failure edge never reads the result).
    if (failureForm && instruction.kind == MirInstructionKind::Assign &&
        !instruction.localFailureSites.empty() &&
        !cppMirCompoundCheckedHelperSpelling(instruction.operation).empty()) {
      const MirPlace *destinationPlace =
          facts.body.findPlace(*instruction.destination);
      if (destinationPlace == nullptr) {
        throw std::logic_error(
            "verified MIR checked compound assignment lost its place");
      }
      output << "__gti_mir_failure_status_" << instruction.id << " = "
             << cppMirCompoundCheckedHelperSpelling(instruction.operation)
             << '(' << destinationSpelling(facts, *instruction.destination)
             << ", ";
      {
        const MirOperand &operand = instruction.operands.front();
        if (operand.kind == MirOperandKind::Constant && operand.literal) {
          emitLiteral(*operand.literal, operand.type);
        } else {
          emitOperand(operand);
        }
      }
      output << ");\n";
      if (instruction.result &&
          !nonRootRecordUses(facts.body, *instruction.result).empty()) {
        writeIndent();
        output << "__gti_mir_v_" << *instruction.result << " = "
               << destinationSpelling(facts, *instruction.destination) << ";\n";
      }
      return;
    }
    // The closed narrowing compound assignment spells the terminal
    // compatibility helper whole: arithmetic, conversion, and write share
    // one origin, and the helper's returned target re-reads into the
    // result exactly like the plain assignment's follow-up read.
    if (instruction.kind == MirInstructionKind::Assign &&
        !cppMirCompoundAssignHelperSpelling(instruction.operation).empty()) {
      if (instruction.result &&
          !nonRootRecordUses(facts.body, *instruction.result).empty()) {
        output << "__gti_mir_v_" << *instruction.result << " = ";
      }
      output << cppMirCompoundAssignHelperSpelling(instruction.operation) << '('
             << destinationSpelling(facts, *instruction.destination) << ", ";
      emitOperand(instruction.operands.front());
      output << ");\n";
      return;
    }
    // Storage values and unique-owner values are move-only in the C++
    // representation; the store is their consuming use, so it spells as a
    // move.
    const bool movedOperand =
        instruction.operands.front().type.kind == SemanticType::Storage ||
        instruction.operands.front().type.kind == SemanticType::PrefixStorage ||
        instruction.operands.front().type.kind == SemanticType::UniqueOwner ||
        (instruction.operands.front().kind == MirOperandKind::Value &&
         constructorFieldMoveInitialize(
             facts.body, instruction.operands.front().value) == &instruction);
    output << destinationSpelling(facts, *instruction.destination) << " = ";
    if (movedOperand) {
      output << "std::move(";
    }
    emitOperand(instruction.operands.front(),
                instruction.kind == MirInstructionKind::Initialize);
    if (movedOperand) {
      output << ')';
    }
    output << ";\n";
    if (instruction.kind == MirInstructionKind::Assign && !movedOperand &&
        instruction.result &&
        !nonRootRecordUses(facts.body, *instruction.result).empty()) {
      writeIndent();
      output << "__gti_mir_v_" << *instruction.result << " = "
             << destinationSpelling(facts, *instruction.destination) << ";\n";
    }
  }

  // A constructor-prologue instruction whose staged result reaches no
  // other consumer publishes into its initializer's field here: the
  // compatibility route performs the same initialization in the member
  // initializer list, and dropping it leaves the field default-
  // constructed (the allocate-empty family was behaviorally masked only
  // because a zero-capacity allocation equals the default).
  void emitStagedFieldPublication(const MirInstruction &instruction,
                                  const ScalarBodyFacts &facts) {
    if (instruction.result && facts.kind == MirBodyKind::Constructor &&
        ownedParameterFieldBinding(program, facts.body, facts.id,
                                   *instruction.result)) {
      // The shared owned-parameter proof publishes this exact move in the
      // native member-initializer list. Emitting the generic staged-field
      // assignment as well would move the argument twice.
      return;
    }
    const MirConstructorInitializer *initializer =
        stagedConstructorFieldPublication(program, facts.body, facts.id,
                                          instruction);
    if (initializer == nullptr) {
      return;
    }
    writeIndent();
    output << "(*this)." << fieldSpelling(facts, initializer->field)
           << " = std::move(__gti_mir_v_" << *instruction.result << ");\n";
  }

  void emitInstruction(const MirBlock &block, const MirInstruction &instruction,
                       const ScalarBodyFacts &facts) {
    if (facts.kind == MirBodyKind::Module &&
        moduleDataOnlyInitialization(program, instruction) != nullptr) {
      writeIndent();
      output << "// GTI MIR data-only program storage "
             << *instruction.destination << " is initialized statically\n";
      return;
    }
    if (fusedSignedMinimumNegation(facts.body, instruction) != nullptr) {
      writeIndent();
      output << "// signed-minimum magnitude spells at its negation\n";
      return;
    }
    if (storedReferenceFieldPublication(program, facts.body, facts.id,
                                        instruction) != nullptr) {
      writeIndent();
      output << "// stored-reference initializer binds in the member "
                "initializer list\n";
      return;
    }
    if (facts.kind == MirBodyKind::Constructor) {
      if (const std::optional<CppMirCopyParameterFieldBinding> binding =
              copyParameterFieldInstructionBinding(program, facts.body,
                                                   facts.id, instruction.id)) {
        writeIndent();
        output << (binding->loadInstruction == instruction.id
                       ? "// parameter copy publishes in the member "
                         "initializer list\n"
                       : "// field initialization publishes in the member "
                         "initializer list\n");
        return;
      }
    }
    // A frozen closure chain never materializes. An owned closure instead
    // constructs its unique destination slot at the exact Closure step; its
    // factory only supplies a nameable C++ representation type.
    if (instruction.kind == MirInstructionKind::Compute &&
        instruction.operation == MirOperation::Closure) {
      const MaterializedClosure closure =
          materializedClosure(program, facts.body, instruction);
      if (!closureChainAdmits(program, facts.body, instruction) && closure) {
        writeIndent();
        emitMaterializedClosureConstruction(facts, closure);
        return;
      }
      writeIndent();
      output << "// closure value "
             << (instruction.result ? *instruction.result : 0)
             << " spells at its consuming invocation\n";
      return;
    }
    if (instruction.kind == MirInstructionKind::Initialize &&
        instruction.destination) {
      if (const MirPlace *destination =
              facts.body.findPlace(*instruction.destination);
          destination != nullptr &&
          destination->type.kind == SemanticType::Lambda) {
        writeIndent();
        if (materializedClosureForType(program, facts.body,
                                       destination->type)) {
          output << "// closure local " << destination->id
                 << " was constructed at its Closure step\n";
        } else {
          output << "// closure local " << destination->id
                 << " joins the fused chain\n";
        }
        return;
      }
    }
    if (instruction.kind == MirInstructionKind::Load && instruction.result &&
        instruction.operands.size() == 1) {
      if (const MirPlace *source =
              facts.body.findPlace(instruction.operands.front().place);
          source != nullptr && source->type.kind == SemanticType::Lambda) {
        const MirInstruction *closure =
            closureChainDefinition(facts.body, *instruction.result);
        if (closure != nullptr &&
            closureChainAdmits(program, facts.body, *closure)) {
          writeIndent();
          output << "// load " << *instruction.result
                 << " rejoins the fused closure chain\n";
          return;
        }
        if (const MirPlace *destination =
                lambdaValueDestinationSlot(facts.body, *instruction.result)) {
          writeIndent();
          output << "__gti_mir_p_"
                 << canonicalSlotPlaceId(facts.body, *destination)
                 << ".construct(";
          emitStoragePlaceValue(facts, *source);
          output << ");\n";
          return;
        }
        writeIndent();
        if (materializedClosureForType(program, facts.body, source->type)) {
          output << "// load " << *instruction.result
                 << " aliases the materialized closure slot\n";
        } else {
          output << "// load " << *instruction.result
                 << " rejoins the fused closure chain\n";
        }
        return;
      }
    }
    if (instruction.kind == MirInstructionKind::Load &&
        instruction.operands.size() == 1 &&
        instruction.operands.front().place != 0) {
      if (const MirPlace *place =
              facts.body.findPlace(instruction.operands.front().place);
          place != nullptr &&
          (place->type.kind == SemanticType::Storage ||
           place->type.kind == SemanticType::PrefixStorage)) {
        // The staged storage never materializes; the storage-intrinsic
        // call spells the place lvalue directly.
        writeIndent();
        output << "// load " << (instruction.result ? *instruction.result : 0)
               << " stages a storage place\n";
        return;
      }
    }
    if (facts.kind == MirBodyKind::Constructor &&
        instruction.kind == MirInstructionKind::Move && instruction.result &&
        ownedParameterFieldBinding(program, facts.body, facts.id,
                                   *instruction.result)) {
      writeIndent();
      output << "// move " << *instruction.result
             << " publishes in the member initializer list\n";
      return;
    }
    if (facts.kind == MirBodyKind::Constructor &&
        instruction.kind == MirInstructionKind::Initialize &&
        instruction.operands.size() == 1 &&
        instruction.operands.front().kind == MirOperandKind::Value &&
        ownedParameterFieldInitializer(program, facts.body, facts.id,
                                       instruction.operands.front().value) !=
            nullptr &&
        constructorFieldMoveInitialize(
            facts.body, instruction.operands.front().value) == &instruction) {
      writeIndent();
      output << "// field initialization publishes in the member initializer "
                "list\n";
      return;
    }
    if (instruction.kind == MirInstructionKind::Move && instruction.result &&
        (constructorFieldMoveInitialize(facts.body, *instruction.result) !=
             nullptr ||
         stagedConstructorFieldPublication(program, facts.body, facts.id,
                                           instruction) != nullptr)) {
      // Constructor initializer metadata owns this publication rather than an
      // ordinary MIR use. Preserve the Move's sequencing with an inferred
      // transient, then publish it into the field immediately below.
      writeIndent();
      output << "auto __gti_mir_v_" << *instruction.result << " = std::move(";
      emitMoveSource(facts, instruction);
      output << ");\n";
      return;
    }
    if (instruction.kind == MirInstructionKind::Move && instruction.result &&
        sequencedMovedArgument(facts.body, *instruction.result) ==
            &instruction) {
      // The sequenced moved argument declares here, keeping MIR's
      // move-before-call order; auto needs no representation row.
      writeIndent();
      output << "auto __gti_mir_v_" << *instruction.result << " = std::move(";
      emitMoveSource(facts, instruction);
      output << ");\n";
      return;
    }
    if (instruction.kind == MirInstructionKind::Move && instruction.result) {
      if (const ClassMoveArrayAssignmentFusion fusion =
              classMoveArrayAssignmentFusion(facts.body, instruction)) {
        writeIndent();
        output << "// move " << *instruction.result
               << " stages into fixed-array assignment "
               << fusion.assignment->id << "\n";
        return;
      }
    }
    if (instruction.kind == MirInstructionKind::Move && instruction.result &&
        instruction.operands.size() == 1) {
      if (const MirPlace *source =
              facts.body.findPlace(instruction.operands.front().place);
          source != nullptr && source->type.kind == SemanticType::Lambda) {
        const bool returnPublication =
            failureForm && returnMoveDefinition(
                               facts.body, *instruction.result) == &instruction;
        if (!returnPublication) {
          if (const MirPlace *destination =
                  lambdaValueDestinationSlot(facts.body, *instruction.result)) {
            writeIndent();
            output << "__gti_mir_p_"
                   << canonicalSlotPlaceId(facts.body, *destination)
                   << ".construct(std::move(";
            emitMoveSource(facts, instruction);
            output << "));\n";
            return;
          }
          // The moved callable stages its place; the invocation spells
          // std::move over the place expression directly.
          writeIndent();
          output << "// move " << *instruction.result
                 << " stages a callable place\n";
          return;
        }
      }
    }
    if (failureForm && instruction.kind == MirInstructionKind::Move &&
        instruction.result &&
        returnMoveDefinition(facts.body, *instruction.result) == &instruction) {
      // The publication happens here, where MIR consumes the source —
      // before any later drop of the moved-from local — and the paired
      // Return then only reports success.
      writeIndent();
      const bool placementPublication =
          facts.body.returnType.kind == SemanticType::Class ||
          facts.body.returnType.kind == SemanticType::Lambda ||
          expectedClassPlacementResultType(program, representations,
                                           facts.body.returnType);
      if (placementPublication) {
        output << "std::construct_at(__gti_mir_out_result, std::move(";
      } else {
        output << "*__gti_mir_out_result = std::move(";
      }
      emitMoveSource(facts, instruction);
      output << (placementPublication ? "));\n" : ");\n");
      return;
    }
    if (failureForm && instruction.kind == MirInstructionKind::Load &&
        instruction.result &&
        returnCopyLoadDefinition(facts.body, *instruction.result) ==
            &instruction) {
      const MirPlace *source =
          facts.body.findPlace(instruction.operands.front().place);
      if (source == nullptr) {
        throw std::logic_error(
            "verified MIR copy return lost its source place");
      }
      writeIndent();
      output << "std::construct_at(__gti_mir_out_result, ";
      emitStoragePlaceValue(facts, *source);
      output << ");\n";
      return;
    }
    if (instruction.kind == MirInstructionKind::Move && instruction.result &&
        instruction.operands.size() == 1) {
      if (const MirPlace *destination = expectedMoveDestinationSlot(
              program, facts.body, *instruction.result)) {
        writeIndent();
        output << "__gti_mir_p_"
               << canonicalSlotPlaceId(facts.body, *destination)
               << ".construct(std::move(";
        emitMoveSource(facts, instruction);
        output << "));\n";
        return;
      }
      if (const MirPlace *destination = conditionalClassReturnJoinSlotForValue(
              facts.body, *instruction.result)) {
        writeIndent();
        output << "__gti_mir_p_"
               << canonicalSlotPlaceId(facts.body, *destination)
               << ".construct(std::move(";
        emitMoveSource(facts, instruction);
        output << "));\n";
        return;
      }
      if (const MirPlace *destination =
              classValuePublicationSlot(facts.body, *instruction.result)) {
        writeIndent();
        output << "__gti_mir_p_"
               << canonicalSlotPlaceId(facts.body, *destination)
               << ".construct(std::move(";
        emitMoveSource(facts, instruction);
        output << "));\n";
        return;
      }
    }
    if (instruction.kind == MirInstructionKind::Load && instruction.result &&
        nonRootRecordUses(facts.body, *instruction.result).size() == 1) {
      const MirInstruction *user = findInstruction(
          facts.body, nonRootRecordUses(facts.body, *instruction.result)
                          .front()
                          .instruction);
      if (user != nullptr && user->kind == MirInstructionKind::Call &&
          (user->intrinsic == IntrinsicKind::UniqueOwnerBorrow ||
           user->intrinsic == IntrinsicKind::UniqueOwnerBorrowMut ||
           user->intrinsic == IntrinsicKind::UniqueOwnerIsNull)) {
        // The owner field never copies; the intrinsic spells the field
        // lvalue directly.
        writeIndent();
        output << "// load " << *instruction.result
               << " stages the owner field\n";
        return;
      }
    }
    if (instruction.kind == MirInstructionKind::Move && instruction.result &&
        !nonRootRecordUses(facts.body, *instruction.result).empty()) {
      const MirInstruction *user = findInstruction(
          facts.body, nonRootRecordUses(facts.body, *instruction.result)
                          .front()
                          .instruction);
      if (user != nullptr && user->kind == MirInstructionKind::CallInput &&
          user->result &&
          copyStagedCallInput(facts.body, *user->result) == user) {
        // The moved source feeds a value-staged call input; the consuming
        // call spells std::move over the place expression directly.
        writeIndent();
        output << "// move " << *instruction.result
               << " stages into its call input\n";
        return;
      }
      {
        const MovedChainTerminal terminal =
            movedChainTerminal(facts.body, *instruction.result);
        if (terminal.consumer != nullptr &&
            terminal.consumer->kind == MirInstructionKind::Construct &&
            movedPlaceChainSource(facts.body, terminal.top,
                                  *terminal.consumer) != nullptr) {
          // The moved source reaches its construction in order; the
          // construction spells std::move over the place directly.
          writeIndent();
          output << "// move " << *instruction.result
                 << " stages into its construction\n";
          return;
        }
        if (terminal.consumer != nullptr &&
            terminal.consumer->kind == MirInstructionKind::Compute &&
            terminal.consumer->operation == MirOperation::Closure &&
            materializedClosure(program, facts.body, *terminal.consumer) &&
            movedPlaceChainSource(facts.body, terminal.top,
                                  *terminal.consumer) != nullptr) {
          writeIndent();
          output << "// move " << *instruction.result
                 << " stages into its materialized closure\n";
          return;
        }
      }
    }
    if (instruction.kind == MirInstructionKind::Move) {
      // By-value element staging: the moved place feeds exactly the
      // staged element value.
      writeIndent();
      output << "__gti_mir_v_" << *instruction.result << " = std::move(";
      emitMoveSource(facts, instruction);
      output << ");\n";
      return;
    }
    if (instruction.kind == MirInstructionKind::Construct &&
        instruction.result) {
      const InlineFailureConstructorArgument nested =
          inlineFailureConstructorArgument(program, facts.body,
                                           *instruction.result);
      if (nested.producer == &instruction) {
        writeIndent();
        output << "// construct " << *instruction.result
               << " spells at its consuming failure constructor\n";
        return;
      }
      const StagedClassResult staged =
          stagedClassResultForSource(facts.body, *instruction.result);
      if (staged.producer == &instruction && staged.slot != nullptr) {
        writeIndent();
        output << "__gti_mir_p_"
               << canonicalSlotPlaceId(facts.body, *staged.slot)
               << ".construct(";
        for (std::size_t index = 0; index < instruction.operands.size();
             ++index) {
          if (index != 0) {
            output << ", ";
          }
          if (const MirPlace *chainSource =
                  instruction.operands[index].kind == MirOperandKind::Value
                      ? movedPlaceChainSource(facts.body,
                                              instruction.operands[index].value,
                                              instruction)
                      : nullptr) {
            output << "std::move(";
            emitStoragePlaceValue(facts, *chainSource);
            output << ')';
            continue;
          }
          const bool consumed =
              instruction.operands[index].type.kind == SemanticType::Class ||
              instruction.operands[index].type.kind ==
                  SemanticType::UniqueOwner ||
              instruction.operands[index].type.kind == SemanticType::Storage ||
              instruction.operands[index].type.kind ==
                  SemanticType::PrefixStorage;
          if (consumed) {
            output << "std::move(";
          }
          emitOperand(instruction.operands[index]);
          if (consumed) {
            output << ')';
          }
        }
        output << ");\n";
        return;
      }
    }
    if (failureForm && instruction.kind == MirInstructionKind::Construct) {
      if (const MirConstructorInstance *target =
              transformedConstructor(instruction)) {
        if (!instruction.result) {
          throw std::logic_error(
              "verified MIR failure construction lost its result value");
        }
        const bool publishesReturn =
            returnConstructDefinition(facts.body, *instruction.result) ==
                &instruction &&
            facts.body.returnType == instruction.info.type;
        const ExpectedPayloadReturnSlot expectedPayload =
            expectedPayloadReturnSlot(program, representations, facts.body,
                                      *instruction.result);
        const MirPlace *slot =
            publishesReturn || expectedPayload
                ? nullptr
                : classValuePublicationSlot(facts.body, *instruction.result);
        const ConstructorFieldResultSlot fieldResult =
            publishesReturn || expectedPayload
                ? ConstructorFieldResultSlot{}
                : constructorFieldResultSlot(program, facts.body,
                                             *instruction.result);
        if (slot == nullptr) {
          slot = fieldResult.slot;
        }
        if (!publishesReturn && !expectedPayload && slot == nullptr) {
          throw std::logic_error(
              "verified MIR failure construction lost its exact destination");
        }
        const auto emitDestination = [&] {
          if (publishesReturn) {
            output << "__gti_mir_out_result";
          } else {
            output << "__gti_mir_p_" << canonicalSlotPlaceId(facts.body, *slot)
                   << ".construction_address()";
          }
        };
        writeIndent();
        if (expectedPayload) {
          output << "__gti_mir_v_" << *instruction.result
                 << ".construct_from([&]() { return "
                 << typeSpelling(instruction.info.type) << '(';
        } else {
          output << "std::construct_at(";
          emitDestination();
          output << ", ";
        }
        output << cppMirFailureConstructorTagSpelling(target->id) << "{}";
        for (std::size_t index = 0; index < instruction.operands.size();
             ++index) {
          output << ", ";
          if (const MirPlace *chainSource =
                  instruction.operands[index].kind == MirOperandKind::Value
                      ? movedPlaceChainSource(facts.body,
                                              instruction.operands[index].value,
                                              instruction)
                      : nullptr) {
            output << "std::move(";
            emitStoragePlaceValue(facts, *chainSource);
            output << ')';
            continue;
          }
          if (instruction.operands[index].kind == MirOperandKind::Value) {
            const InlineFailureConstructorArgument nested =
                inlineFailureConstructorArgument(
                    program, facts.body, instruction.operands[index].value);
            if (nested.consumer == &instruction) {
              output << typeSpelling(nested.producer->info.type) << '(';
              for (std::size_t nestedIndex = 0;
                   nestedIndex < nested.producer->operands.size();
                   ++nestedIndex) {
                if (nestedIndex != 0) {
                  output << ", ";
                }
                emitOperand(nested.producer->operands[nestedIndex]);
              }
              output << ')';
              continue;
            }
          }
          if (const MirInstruction *staged = borrowStagedCallInput(
                  facts.body, instruction.operands[index])) {
            const MirPlace *place =
                facts.body.findPlace(staged->operands.front().place);
            if (place == nullptr) {
              throw std::logic_error(
                  "verified MIR failure constructor lost a staged borrowed "
                  "argument place");
            }
            emitStoragePlaceValue(facts, *place);
            continue;
          }
          if (const MirInstruction *staged =
                  instruction.operands[index].kind == MirOperandKind::Value
                      ? loanStagedCallInput(facts.body,
                                            instruction.operands[index].value)
                      : nullptr) {
            output << "(*__gti_mir_loan_" << staged->operands.front().loan
                   << ')';
            continue;
          }
          const bool consumed =
              instruction.operands[index].type.kind == SemanticType::Class ||
              instruction.operands[index].type.kind ==
                  SemanticType::UniqueOwner ||
              instruction.operands[index].type.kind == SemanticType::Storage ||
              instruction.operands[index].type.kind ==
                  SemanticType::PrefixStorage;
          if (consumed) {
            output << "std::move(";
          }
          emitOperand(instruction.operands[index]);
          if (consumed) {
            output << ')';
          }
        }
        output << ", &__gti_mir_construct_success_" << instruction.id
               << ", __gti_mir_failure_record";
        output << (expectedPayload ? "); });\n" : ");\n");
        if (expectedPayload) {
          writeIndent();
          output << "if (!__gti_mir_construct_success_" << instruction.id
                 << ") {\n";
          ++indentation;
          writeIndent();
          output << "__gti_mir_v_" << *instruction.result << ".destroy();\n";
          --indentation;
          writeIndent();
          output << "}\n";
          return;
        }
        writeIndent();
        output << "if (__gti_mir_construct_success_" << instruction.id
               << ") {\n";
        ++indentation;
        if (!publishesReturn) {
          writeIndent();
          output << "__gti_mir_p_" << canonicalSlotPlaceId(facts.body, *slot)
                 << ".mark_constructed();\n";
        }
        --indentation;
        writeIndent();
        output << "} else {\n";
        ++indentation;
        writeIndent();
        output << "std::destroy_at(";
        emitDestination();
        output << ");\n";
        --indentation;
        writeIndent();
        output << "}\n";
        return;
      }
    }
    if (instruction.kind == MirInstructionKind::Construct &&
        instruction.result &&
        passiveFixedArrayConstructInput(program, facts.body,
                                        *instruction.result) != nullptr) {
      writeIndent();
      output << "// construct " << *instruction.result
             << " publishes in its fixed-array aggregate\n";
      return;
    }
    if (instruction.kind == MirInstructionKind::Construct &&
        generatedSpecialMemberConstruction(program, facts.body, instruction)) {
      writeIndent();
      output << "// generated special-member construct "
             << (instruction.result ? *instruction.result : 0)
             << " publishes at its consuming initialize\n";
      return;
    }
    if (instruction.kind == MirInstructionKind::Construct &&
        instruction.result &&
        returnConstructDefinition(facts.body, *instruction.result) ==
            &instruction) {
      writeIndent();
      output << "// construct " << *instruction.result
             << " publishes at its consuming return\n";
      return;
    }
    if (instruction.kind == MirInstructionKind::Construct &&
        instruction.result && !instruction.destination &&
        !instruction.receiver &&
        instruction.info.type.kind == SemanticType::Class &&
        slotConsumedConstruct(facts, instruction)) {
      // The undeclarable class value publishes at its consuming
      // initialize, which constructs the destination slot in place from
      // these arguments.
      writeIndent();
      output << "// construct " << *instruction.result
             << " publishes at its consuming initialize\n";
      return;
    }
    if (instruction.kind == MirInstructionKind::Construct &&
        instruction.result && !instruction.destination &&
        !instruction.receiver &&
        instruction.info.type.kind == SemanticType::Class &&
        !slotConsumedConstruct(facts, instruction)) {
      // A value-producing construction assigns the constructor call into
      // its declared class local; the row's boundary proof guaranteed the
      // declaration above. Slot-consumed constructs keep the slot
      // protocol's own spelling.
      writeIndent();
      output << "__gti_mir_v_" << *instruction.result << " = "
             << typeSpelling(instruction.info.type) << '(';
      for (std::size_t index = 0; index < instruction.operands.size();
           ++index) {
        if (index != 0) {
          output << ", ";
        }
        // An ordered moved-place chain operand spells std::move over
        // the source place directly.
        if (const MirPlace *chainSource =
                instruction.operands[index].kind == MirOperandKind::Value
                    ? movedPlaceChainSource(facts.body,
                                            instruction.operands[index].value,
                                            instruction)
                    : nullptr) {
          output << "std::move(";
          emitStoragePlaceValue(facts, *chainSource);
          output << ')';
          continue;
        }
        // The construction consumes its arguments: class-typed operands
        // move so deleted copy constructors cannot reject the call.
        const bool consumed =
            instruction.operands[index].type.kind == SemanticType::Class ||
            instruction.operands[index].type.kind ==
                SemanticType::UniqueOwner ||
            instruction.operands[index].type.kind == SemanticType::Storage ||
            instruction.operands[index].type.kind ==
                SemanticType::PrefixStorage;
        if (consumed) {
          output << "std::move(";
        }
        emitOperand(instruction.operands[index]);
        if (consumed) {
          output << ')';
        }
      }
      output << ");\n";
      return;
    }
    if (instruction.kind != MirInstructionKind::CallInput &&
        instruction.kind != MirInstructionKind::Call) {
      emitPlainInstruction(block, instruction, facts);
      return;
    }
    writeIndent();
    if (instruction.kind == MirInstructionKind::CallInput) {
      if (instruction.operands.front().kind == MirOperandKind::BorrowRead ||
          instruction.operands.front().kind == MirOperandKind::BorrowWrite) {
        // The staged borrow never materializes; the call spells the place.
        output << "// call input " << *instruction.result
               << " stages a borrowed place\n";
        return;
      }
      if (instruction.result &&
          loanStagedCallInput(facts.body, *instruction.result) ==
              &instruction) {
        // The staged loan never materializes; the call spells the
        // dereferenced pointer carrier (ADR 018 §4).
        output << "// call input " << *instruction.result
               << " stages a loaned argument\n";
        return;
      }
      if (instruction.result &&
          copyStagedCallInput(facts.body, *instruction.result) ==
              &instruction) {
        // The staged copy never materializes; the call spells the source
        // place and C++ copies at the call boundary.
        output << "// call input " << *instruction.result
               << " stages a by-value argument copy\n";
        return;
      }
      if (instruction.result) {
        const StagedClassResult staged =
            stagedClassResultForResult(facts.body, *instruction.result);
        if (staged.stage == &instruction) {
          output << "// call input " << *instruction.result
                 << " stages a placement class result\n";
          return;
        }
      }
      if (instruction.result) {
        const InlineFailureConstructorArgument nested =
            inlineFailureConstructorArgument(program, facts.body,
                                             *instruction.result);
        if (nested.stage == &instruction) {
          output << "// call input " << *instruction.result
                 << " stages an inline constructor argument\n";
          return;
        }
      }
      if (instruction.result) {
        const MovedChainTerminal terminal =
            movedChainTerminal(facts.body, *instruction.result);
        if (terminal.consumer != nullptr &&
            terminal.consumer->kind == MirInstructionKind::Construct &&
            movedPlaceChainSource(facts.body, terminal.top,
                                  *terminal.consumer) != nullptr) {
          // The staged link of an ordered moved-place chain never
          // materializes; the construction spells std::move over the
          // place.
          output << "// call input " << *instruction.result
                 << " links a moved-place chain\n";
          return;
        }
      }
      output << "__gti_mir_v_" << *instruction.result << " = ";
      emitOperand(instruction.operands.front());
      if (instruction.info.type == SemanticType::CString &&
          instruction.operands.front().type == SemanticType::StringView) {
        output << ".data()";
      }
      output << ";\n";
      return;
    }
    const auto emitCallArgument = [&](const MirOperand &operand,
                                      bool marshalled,
                                      bool transformedCallable) {
      if (operand.kind == MirOperandKind::Value &&
          sequencedMovedArgument(facts.body, operand.value) != nullptr) {
        output << "std::move(__gti_mir_v_" << operand.value << ')';
        return;
      }
      if (operand.kind == MirOperandKind::Value &&
          operand.type.kind == SemanticType::Lambda) {
        const MirPlace *moved =
            movedPlaceChainSource(facts.body, operand.value, instruction);
        if (moved != nullptr && moved->type == operand.type) {
          output << "std::move(";
          emitStoragePlaceValue(facts, *moved);
          output << ')';
          return;
        }
      }
      if (operand.kind == MirOperandKind::Value &&
          operand.type.kind == SemanticType::Lambda &&
          closureChainDefinition(facts.body, operand.value) != nullptr) {
        emitClosureLiteral(facts, operand.value, transformedCallable);
        return;
      }
      if (operand.kind == MirOperandKind::Value) {
        const ClassSsaLifetimeSlot valueSlot = classSsaLifetimeSlot(
            program, representations, facts.body, operand.value);
        if (valueSlot.consumer == &instruction &&
            valueSlot.consumerKind == ClassSsaSlotConsumerKind::CallArgument) {
          output << "std::move(__gti_mir_v_" << operand.value << ".get())";
          return;
        }
        const StagedClassResult staged =
            stagedClassResultForResult(facts.body, operand.value);
        if (staged.slot != nullptr) {
          output << "std::move(__gti_mir_p_"
                 << canonicalSlotPlaceId(facts.body, *staged.slot) << ".get())";
          return;
        }
        if (const MirInstruction *producer = inlineNestedCallResult(
                program, representations, facts.body, operand.value)) {
          const MirInstruction *producerStage =
              borrowStagedCallInput(facts.body, *producer->receiver);
          const MirOperand &producerBorrow =
              producerStage != nullptr ? producerStage->operands.front()
                                       : *producer->receiver;
          const MirPlace *producerPlace =
              (producerBorrow.kind == MirOperandKind::BorrowRead ||
               producerBorrow.kind == MirOperandKind::BorrowWrite) &&
                      producerBorrow.place != 0
                  ? facts.body.findPlace(producerBorrow.place)
                  : nullptr;
          if (producerPlace == nullptr) {
            throw std::logic_error(
                "verified MIR inline nested call lost its receiver place");
          }
          emitStoragePlaceValue(facts, *producerPlace);
          output << '.' << bodySpelling(*producer->functionTarget) << "()";
          return;
        }
        if (sequencedMovedArgument(facts.body, operand.value) != nullptr) {
          output << "std::move(__gti_mir_v_" << operand.value << ')';
          return;
        }
        if (const MirInstruction *stage =
                callableArgumentStage(facts.body, operand.value)) {
          const MirPlace *place =
              facts.body.findPlace(stage->operands.front().place);
          if (place == nullptr) {
            throw std::logic_error(
                "verified MIR call lost its staged callable argument place");
          }
          emitStoragePlaceValue(facts, *place);
          return;
        }
        if (const MirInstruction *stage =
                loanStagedCallInput(facts.body, operand.value)) {
          output << "(*__gti_mir_loan_" << stage->operands.front().loan << ')';
          return;
        }
        if (const MirInstruction *stage =
                copyStagedCallInput(facts.body, operand.value)) {
          const StagedTemporarySource source =
              stagedTemporarySourceFor(facts.body, *stage);
          if (source.place == nullptr) {
            throw std::logic_error(
                "verified MIR call lost its value-staged source place");
          }
          if (source.moved) {
            output << "std::move(";
          }
          emitStoragePlaceValue(facts, *source.place);
          if (source.moved) {
            output << ')';
          }
          return;
        }
      }
      if (const MirInstruction *staged =
              borrowStagedCallInput(facts.body, operand)) {
        const MirPlace *place =
            facts.body.findPlace(staged->operands.front().place);
        if (place == nullptr) {
          throw std::logic_error(
              "verified MIR call lost a staged borrowed argument place");
        }
        emitStoragePlaceValue(facts, *place);
        return;
      }
      if ((operand.kind == MirOperandKind::BorrowRead ||
           operand.kind == MirOperandKind::BorrowWrite) &&
          operand.place != 0) {
        const MirPlace *place = facts.body.findPlace(operand.place);
        if (place == nullptr) {
          throw std::logic_error(
              "verified MIR call lost a direct borrowed argument place");
        }
        emitStoragePlaceValue(facts, *place);
        return;
      }
      if (marshalled) {
        output << "::gti_internal::backend::to_c_string_view(";
      }
      emitOperand(operand);
      if (marshalled) {
        output << ')';
      }
    };
    const auto emitCallOperands = [&](const MirFunctionInstance *target,
                                      bool cBoundary, bool transformed) {
      bool emitted = false;
      for (std::size_t index = 0; index < instruction.operands.size();
           ++index) {
        if (emitted) {
          output << ", ";
        }
        const MirOperand &operand = instruction.operands[index];
        const bool transformedCallable =
            transformed && target != nullptr &&
            std::any_of(target->callableParameters.begin(),
                        target->callableParameters.end(),
                        [&](const MirCallableParameter &parameter) {
                          return parameter.parameterIndex == index;
                        });
        emitCallArgument(
            operand, cBoundary && operand.type.kind == SemanticType::StringView,
            transformedCallable);
        emitted = true;
      }
      return emitted;
    };
    if (storageReferenceReadCall(facts.body, instruction)) {
      // A call-result loan binds the element address here; otherwise the
      // element is published by the loan-producing Borrow and the call
      // site itself stages nothing.
      if (const MirLoan *loan =
              producedCallResultLoan(facts.body, instruction)) {
        const MirPlace *storage =
            storageStagedPlace(facts.body, instruction.operands.front());
        if (storage == nullptr) {
          throw std::logic_error(
              "verified MIR discharged read lost its staged storage place");
        }
        output << "__gti_mir_loan_" << loan->id
               << " = &::gti_internal::backend::"
               << storageReadHelperSpelling(instruction.intrinsic) << '(';
        emitStoragePlaceValue(facts, *storage);
        output << ", ";
        emitOperand(instruction.operands.back());
        output << ");\n";
        return;
      }
      output << "// discharged storage read " << instruction.id
             << " publishes through its loan\n";
      return;
    }
    if (callableValueInvocation(instruction)) {
      if (failureForm) {
        const MirInstruction *closure =
            closureChainDefinition(facts.body, instruction.receiver->value);
        const MirLambdaInstance *lambda =
            closure != nullptr && closure->lambdaTarget
                ? program.findLambda(*closure->lambdaTarget)
                : nullptr;
        const MirInstruction *stage =
            callableReceiverStage(facts.body, instruction.receiver->value);
        const MirPlace *stagedPlace =
            stage != nullptr && stage->operands.size() == 1
                ? facts.body.findPlace(stage->operands.front().place)
                : nullptr;
        const MirPlace *materializedPlace = materializedCallableReceiverPlace(
            program, facts.body, instruction.receiver->value);
        if (lambda == nullptr && stagedPlace == nullptr &&
            materializedPlace == nullptr) {
          throw std::logic_error(
              "verified MIR transformed invocation lost its callable");
        }
        output << "__gti_mir_call_success_" << instruction.id << " = ";
        if (materializedPlace != nullptr) {
          emitStoragePlaceValue(facts, *materializedPlace);
        } else if (lambda != nullptr) {
          emitClosureLiteral(facts, instruction.receiver->value, true);
        } else {
          emitStoragePlaceValue(facts, *stagedPlace);
        }
        output << '(';
        bool needsComma = false;
        for (const MirOperand &operand : instruction.operands) {
          if (needsComma) {
            output << ", ";
          }
          if (operand.kind == MirOperandKind::Loan) {
            output << "(*__gti_mir_loan_" << operand.loan << ')';
          } else {
            emitOperand(operand);
          }
          needsComma = true;
        }
        if (instruction.info.type != SemanticType::Void) {
          if (needsComma) {
            output << ", ";
          }
          output << '&';
          if (instruction.result) {
            output << "__gti_mir_v_" << *instruction.result;
          } else {
            output << "__gti_mir_discard_" << instruction.id;
          }
          needsComma = true;
        }
        if (needsComma) {
          output << ", ";
        }
        output << "__gti_mir_failure_record);\n";
        return;
      }
      // The success form spells the fused closure literal followed by its
      // ordinary argument list; every checked operation is terminally
      // contained only on this compatibility-shaped path.
      if (instruction.result) {
        output << "__gti_mir_v_" << *instruction.result << " = ";
      }
      if (const MirPlace *materialized = materializedCallableReceiverPlace(
              program, facts.body, instruction.receiver->value)) {
        emitStoragePlaceValue(facts, *materialized);
      } else if (const MirInstruction *stage = callableReceiverStage(
                     facts.body, instruction.receiver->value)) {
        const MirPlace *place =
            facts.body.findPlace(stage->operands.front().place);
        if (place == nullptr) {
          throw std::logic_error(
              "verified MIR invocation lost its staged callable place");
        }
        const bool moves = stage->kind == MirInstructionKind::Move;
        if (moves) {
          output << "std::move(";
        }
        emitStoragePlaceValue(facts, *place);
        if (moves) {
          output << ')';
        }
      } else {
        emitClosureLiteral(facts, instruction.receiver->value);
      }
      output << '(';
      for (std::size_t index = 0; index < instruction.operands.size();
           ++index) {
        if (index != 0) {
          output << ", ";
        }
        // A loaned argument dereferences its pointer carrier (ADR 018
        // §4); every other argument is a staged value.
        if (instruction.operands[index].kind == MirOperandKind::Loan) {
          output << "(*__gti_mir_loan_" << instruction.operands[index].loan
                 << ')';
        } else {
          emitOperand(instruction.operands[index]);
        }
      }
      output << ");\n";
      return;
    }
    if (instruction.kind == MirInstructionKind::Call &&
        instruction.intrinsic == IntrinsicKind::PrefixStorageLength) {
      const MirPlace *storage =
          storageStagedPlace(facts.body, instruction.operands.front());
      if (storage == nullptr) {
        throw std::logic_error(
            "verified MIR length read lost its staged storage place");
      }
      output << "__gti_mir_v_" << *instruction.result
             << " = ::gti_internal::backend::prefix_storage_length(";
      emitStoragePlaceValue(facts, *storage);
      output << ");\n";
      return;
    }
    if (instruction.kind == MirInstructionKind::Call &&
        instruction.intrinsic == IntrinsicKind::AllocateStorage) {
      if (!failureForm || !storageAllocationFailureCall(program, instruction)) {
        throw std::logic_error(
            "verified MIR raw-storage allocation lost its checked shape");
      }
      output << "__gti_mir_failure_status_" << instruction.id
             << " = ::gti_internal::backend::mir_allocate_storage_v1<"
             << typeSpelling(instruction.info.type.arguments.front()) << ">(";
      emitOperand(instruction.operands.front());
      output << ", &__gti_mir_v_" << *instruction.result << ");\n";
      return;
    }
    if (instruction.kind == MirInstructionKind::Call &&
        sparseStorageIntrinsic(instruction.intrinsic)) {
      const MirPlace *storage =
          instruction.operands.empty()
              ? nullptr
              : storageStagedPlace(facts.body, instruction.operands.front());
      if (storage == nullptr || instruction.localFailureSites.empty()) {
        throw std::logic_error(
            "verified MIR sparse-storage intrinsic lost its staged storage "
            "place");
      }
      const bool checked =
          failureForm && invokePairedInstruction(facts.body, instruction.id);
      if (checked) {
        output << "__gti_mir_failure_status_" << instruction.id << " = "
               << sparseStorageHelperSpelling(instruction.intrinsic) << '(';
      } else {
        output << sparseStoragePlainHelperSpelling(instruction.intrinsic)
               << '(';
      }
      emitStoragePlaceValue(facts, *storage);
      for (std::size_t index = 1; index < instruction.operands.size();
           ++index) {
        output << ", ";
        if (const MirPlace *second =
                storageStagedPlace(facts.body, instruction.operands[index])) {
          emitStoragePlaceValue(facts, *second);
          continue;
        }
        emitCallArgument(instruction.operands[index], false, false);
      }
      output << ");\n";
      return;
    }
    if (instruction.kind == MirInstructionKind::Call &&
        prefixStorageIntrinsic(instruction.intrinsic)) {
      if (instruction.intrinsic == IntrinsicKind::PrefixStorageAppend &&
          !invokePairedInstruction(facts.body, instruction.id)) {
        // The unpaired append contains its exhaustion terminally inside
        // the compatibility helper, in either form.
        const MirPlace *storage =
            storageStagedPlace(facts.body, instruction.operands.front());
        if (storage == nullptr) {
          throw std::logic_error(
              "verified MIR terminal append lost its staged storage place");
        }
        output << "::gti_internal::backend::prefix_storage_append(";
        emitStoragePlaceValue(facts, *storage);
        for (std::size_t index = 1; index < instruction.operands.size();
             ++index) {
          output << ", ";
          emitCallArgument(instruction.operands[index], false, false);
        }
        output << ");\n";
        return;
      }
      if (!failureForm &&
          instruction.intrinsic == IntrinsicKind::AllocatePrefixStorage) {
        // The plain allocation contains exhaustion terminally inside the
        // compatibility helper, exactly like the initializer-list call
        // site the compatibility constructor spells.
        output << "__gti_mir_v_" << *instruction.result
               << " = ::gti_internal::backend::allocate_prefix_storage<"
               << typeSpelling(instruction.info.type.arguments.front()) << ">(";
        emitOperand(instruction.operands.front());
        output << ");\n";
        return;
      }
      if (instruction.intrinsic == IntrinsicKind::AllocatePrefixStorage) {
        output << "__gti_mir_failure_status_" << instruction.id << " = "
               << prefixStorageHelperSpelling(instruction.intrinsic) << '<'
               << typeSpelling(instruction.info.type.arguments.front()) << ">(";
        emitOperand(instruction.operands.front());
        output << ", &__gti_mir_v_" << *instruction.result << ");\n";
        return;
      }
      const MirPlace *storage =
          storageStagedPlace(facts.body, instruction.operands.front());
      if (storage == nullptr || instruction.localFailureSites.empty()) {
        throw std::logic_error(
            "verified MIR storage intrinsic lost its staged storage place");
      }
      output << "__gti_mir_failure_status_" << instruction.id << " = "
             << prefixStorageHelperSpelling(instruction.intrinsic) << '(';
      emitStoragePlaceValue(facts, *storage);
      for (std::size_t index = 1; index < instruction.operands.size();
           ++index) {
        output << ", ";
        if (const MirPlace *second =
                storageStagedPlace(facts.body, instruction.operands[index])) {
          emitStoragePlaceValue(facts, *second);
          continue;
        }
        emitCallArgument(instruction.operands[index], false, false);
      }
      output << ");\n";
      return;
    }
    if (instruction.kind == MirInstructionKind::Call &&
        instruction.intrinsic == IntrinsicKind::ExpectedValueOr) {
      if (!instruction.receiver || !instruction.result ||
          instruction.operands.size() != 1 ||
          instruction.receiver->type.kind != SemanticType::Expected ||
          instruction.receiver->type.arguments.size() != 2 ||
          instruction.info.type != instruction.receiver->type.arguments[0] ||
          instruction.operands.front().type != instruction.info.type) {
        throw std::logic_error(
            "verified MIR expected value_or lost its exact shape");
      }
      writeIndent();
      output << "__gti_mir_v_" << *instruction.result << " = ";
      if (instruction.receiver->kind == MirOperandKind::Value) {
        emitOperand(*instruction.receiver);
      } else if (instruction.receiver->kind == MirOperandKind::BorrowRead &&
                 instruction.receiver->place != 0) {
        const MirPlace *receiver =
            facts.body.findPlace(instruction.receiver->place);
        if (receiver == nullptr ||
            receiver->type != instruction.receiver->type) {
          throw std::logic_error(
              "verified MIR expected value_or lost its borrowed receiver");
        }
        emitStoragePlaceValue(facts, *receiver);
      } else {
        throw std::logic_error(
            "verified MIR expected value_or has an unsupported receiver");
      }
      output << ".value_or(";
      emitOperand(instruction.operands.front());
      output << ");\n";
      return;
    }
    if (instruction.kind == MirInstructionKind::Call &&
        (instruction.intrinsic == IntrinsicKind::IntegerCheckedAdd ||
         instruction.intrinsic == IntrinsicKind::IntegerCheckedSubtract ||
         instruction.intrinsic == IntrinsicKind::IntegerCheckedMultiply)) {
      if (!instruction.result || instruction.operands.size() != 2 ||
          instruction.info.type.arguments.size() != 2) {
        throw std::logic_error(
            "verified MIR checked-result intrinsic lost its exact shape");
      }
      output << "__gti_mir_v_" << *instruction.result << " = "
             << cppIntegerArithmeticIntrinsicSpelling(instruction.intrinsic)
             << '<' << typeSpelling(instruction.info.type.arguments[1]) << ">(";
      emitOperand(instruction.operands[0]);
      output << ", ";
      emitOperand(instruction.operands[1]);
      output << ");\n";
      return;
    }
    if (failureForm && storageBoundsCheckCall(instruction)) {
      const std::optional<std::string_view> detail =
          storageBoundsDetailSpelling(program, instruction);
      if (!detail) {
        throw std::logic_error(
            "verified MIR logical bounds check lost its exact failure detail");
      }
      output << "__gti_mir_failure_status_" << instruction.id
             << " = ::gti_internal::backend::mir_checked_index_bounds_v1(";
      emitOperand(instruction.operands[0]);
      output << ", ";
      emitOperand(instruction.operands[1]);
      output << ", " << *detail << ");\n";
      return;
    }
    if (storageBoundsCheckCall(instruction)) {
      // The plain logical-size check keeps the terminal compatibility helper.
      output << "::gti_internal::backend::index_bounds_check(";
      emitOperand(instruction.operands[0]);
      output << ", ";
      emitOperand(instruction.operands[1]);
      output << ");\n";
      return;
    }
    if (failureForm && checkedConversionIntrinsicCall(instruction)) {
      output << "__gti_mir_failure_status_" << instruction.id
             << " = ::gti_internal::backend::mir_checked_convert_v1<"
             << typeSpelling(instruction.info.type) << ">(";
      emitOperand(instruction.operands.front());
      output << ", &__gti_mir_v_" << *instruction.result << ");\n";
      return;
    }
    const bool voidExpectedValue =
        instruction.intrinsic == IntrinsicKind::ExpectedValue &&
        instruction.receiver &&
        instruction.receiver->type.kind == SemanticType::Expected &&
        instruction.receiver->type.arguments.size() == 2 &&
        instruction.receiver->type.arguments.front() == SemanticType::Void &&
        instruction.info.type == SemanticType::Void && !instruction.result;
    if (failureForm && instruction.kind == MirInstructionKind::Call &&
        instruction.receiver && (instruction.result || voidExpectedValue) &&
        (instruction.intrinsic == IntrinsicKind::ExpectedValue ||
         instruction.intrinsic == IntrinsicKind::ExpectedError) &&
        invokePairedInstruction(facts.body, instruction.id)) {
      const MirInstruction *staged =
          borrowStagedCallInput(facts.body, *instruction.receiver);
      const MirOperand &receiverBorrow =
          staged != nullptr ? staged->operands.front() : *instruction.receiver;
      const MirPlace *expectedPlace =
          receiverBorrow.kind == MirOperandKind::BorrowRead &&
                  receiverBorrow.place != 0
              ? facts.body.findPlace(receiverBorrow.place)
              : nullptr;
      if (expectedPlace == nullptr ||
          instruction.localFailureSites.size() != 1) {
        throw std::logic_error(
            "verified MIR expected extraction lost its staged place or site");
      }
      if (voidExpectedValue) {
        output << "__gti_mir_failure_status_" << instruction.id
               << " = ::gti_internal::backend::mir_expected_void_value_v1(";
        emitStoragePlaceValue(facts, *expectedPlace);
        output << ");\n";
        return;
      }
      const MirPlace *classExtraction =
          expectedClassExtractionPlace(facts.body, *instruction.result);
      output << "__gti_mir_failure_status_" << instruction.id << " = "
             << (instruction.intrinsic == IntrinsicKind::ExpectedValue
                     ? (classExtraction != nullptr
                            ? "::gti_internal::backend::"
                              "mir_expected_value_ref_v1"
                            : "::gti_internal::backend::mir_expected_value_v1")
                     : (classExtraction != nullptr
                            ? "::gti_internal::backend::"
                              "mir_expected_error_ref_v1"
                            : "::gti_internal::backend::mir_expected_error_v1"))
             << '(';
      emitStoragePlaceValue(facts, *expectedPlace);
      output << ", &__gti_mir_"
             << (classExtraction != nullptr ? "ref_v_" : "v_")
             << *instruction.result << ");\n";
      return;
    }
    if (instruction.kind == MirInstructionKind::Call &&
        (instruction.intrinsic ==
             IntrinsicKind::NumericTypeParameterConversion ||
         instruction.intrinsic == IntrinsicKind::NumericAliasConversion)) {
      if (!instruction.result || instruction.operands.size() != 1) {
        throw std::logic_error(
            "verified MIR numeric-conversion intrinsic lost its operand");
      }
      output << "__gti_mir_v_" << *instruction.result
             << " = ::gti_internal::backend::numeric_cast<"
             << typeSpelling(instruction.info.type) << ">(";
      emitOperand(instruction.operands.front());
      output << ");\n";
      return;
    }
    if (instruction.kind == MirInstructionKind::Call && instruction.receiver &&
        (instruction.intrinsic == IntrinsicKind::StringViewSize ||
         instruction.intrinsic == IntrinsicKind::StringViewEmpty ||
         instruction.intrinsic == IntrinsicKind::ArraySize ||
         instruction.intrinsic == IntrinsicKind::ExpectedValue ||
         instruction.intrinsic == IntrinsicKind::ExpectedError)) {
      // A builtin member read spells the staged place's member directly,
      // exactly like the compatibility route's generic member spelling;
      // the expected extractions contain their wrong-state failure inside
      // the spelled member itself.
      const MirInstruction *staged =
          borrowStagedCallInput(facts.body, *instruction.receiver);
      const MirOperand &receiverBorrow =
          staged != nullptr ? staged->operands.front() : *instruction.receiver;
      const MirPlace *viewPlace =
          receiverBorrow.kind == MirOperandKind::BorrowRead &&
                  receiverBorrow.place != 0
              ? facts.body.findPlace(receiverBorrow.place)
              : nullptr;
      const bool terminalVoidExpectedValue =
          instruction.intrinsic == IntrinsicKind::ExpectedValue &&
          instruction.receiver->type.kind == SemanticType::Expected &&
          instruction.receiver->type.arguments.size() == 2 &&
          instruction.receiver->type.arguments.front() == SemanticType::Void &&
          instruction.info.type == SemanticType::Void && !instruction.result &&
          !invokePairedInstruction(facts.body, instruction.id);
      if (viewPlace == nullptr ||
          (!instruction.result && !terminalVoidExpectedValue)) {
        throw std::logic_error(
            "verified MIR builtin member read lost its staged place");
      }
      writeIndent();
      if (terminalVoidExpectedValue) {
        emitStoragePlaceValue(facts, *viewPlace);
        output << ".value();\n";
        return;
      }
      const MirPlace *classExtraction =
          expectedClassExtractionPlace(facts.body, *instruction.result);
      output << "__gti_mir_" << (classExtraction != nullptr ? "ref_v_" : "v_")
             << *instruction.result << " = ";
      if (classExtraction != nullptr) {
        output << "std::addressof(";
      }
      emitStoragePlaceValue(facts, *viewPlace);
      switch (instruction.intrinsic) {
      case IntrinsicKind::StringViewSize:
      case IntrinsicKind::ArraySize:
        output << ".size()";
        break;
      case IntrinsicKind::StringViewEmpty:
        output << ".empty()";
        break;
      case IntrinsicKind::ExpectedValue:
        output << ".value()";
        break;
      default:
        output << ".error()";
        break;
      }
      if (classExtraction != nullptr) {
        output << ')';
      }
      output << ";\n";
      return;
    }
    if (instruction.kind == MirInstructionKind::Call &&
        (instruction.intrinsic == IntrinsicKind::UniqueOwnerBorrow ||
         instruction.intrinsic == IntrinsicKind::UniqueOwnerBorrowMut)) {
      // The borrow publishes at its consuming return, which spells the
      // backend accessor over the owner field directly.
      writeIndent();
      output << "// owner borrow " << instruction.id
             << " publishes at its consuming return\n";
      return;
    }
    if (instruction.kind == MirInstructionKind::Call &&
        instruction.intrinsic == IntrinsicKind::UniqueOwnerIsNull) {
      const MirInstruction *staged =
          instruction.operands.size() == 1
              ? definitionFor(facts.body, instruction.operands.front())
              : nullptr;
      const MirPlace *ownerPlace =
          staged != nullptr && staged->kind == MirInstructionKind::Load &&
                  staged->operands.size() == 1
              ? facts.body.findPlace(staged->operands.front().place)
              : nullptr;
      if (ownerPlace == nullptr || !instruction.result) {
        throw std::logic_error(
            "verified MIR owner observer lost its staged place");
      }
      writeIndent();
      output << "__gti_mir_v_" << *instruction.result
             << " = ::gti_internal::backend::unique_owner_is_null(";
      emitStoragePlaceValue(facts, *ownerPlace);
      output << ");\n";
      return;
    }
    if (instruction.kind == MirInstructionKind::Call &&
        instruction.intrinsic == IntrinsicKind::UniqueOwnerUpcast) {
      if (instruction.receiver || instruction.functionTarget ||
          !instruction.result || instruction.operands.size() != 1 ||
          instruction.info.type.kind != SemanticType::UniqueOwner ||
          instruction.info.type.arguments.size() != 1 ||
          instruction.operands.front().type.kind != SemanticType::UniqueOwner ||
          instruction.operands.front().type.arguments.size() != 1) {
        throw std::logic_error(
            "verified MIR unique-owner upcast lost its exact shape");
      }
      writeIndent();
      output << "__gti_mir_v_" << *instruction.result
             << " = ::gti_internal::backend::unique_owner_upcast<"
             << typeSpelling(instruction.info.type.arguments.front()) << ">(";
      output << "std::move(";
      emitOperand(instruction.operands.front());
      output << "));\n";
      return;
    }
    if (instruction.kind == MirInstructionKind::Call &&
        instruction.intrinsic == IntrinsicKind::AllocateUniqueOwner) {
      // MIR has already flattened the source pack and staged each element
      // with its exact copy/move policy. The sealed helper only performs the
      // allocation and construction represented by this ordinary input list.
      if (!instruction.result || instruction.receiver ||
          instruction.functionTarget ||
          instruction.info.type.kind != SemanticType::UniqueOwner ||
          instruction.info.type.arguments.size() != 1) {
        throw std::logic_error(
            "verified MIR unique-owner allocation lost its instance shape");
      }
      output << "__gti_mir_v_" << *instruction.result
             << " = ::gti_internal::backend::make_unique<"
             << typeSpelling(instruction.info.type.arguments.front()) << ">(";
      (void)emitCallOperands(nullptr, false, false);
      output << ");\n";
      return;
    }
    if (instruction.kind == MirInstructionKind::Call &&
        instruction.intrinsic ==
            IntrinsicKind::DefaultTypeParameterConstruction &&
        !instruction.functionTarget && !instruction.receiver &&
        instruction.operands.empty() && instruction.localFailureSites.empty() &&
        instruction.result) {
      if (returnDefaultConstructionDefinition(
              facts.body, *instruction.result) == &instruction) {
        // The fused value publishes at its consuming return.
        output << "// default construction " << *instruction.result
               << " publishes at its consuming return\n";
        return;
      }
      if (const StagedClassResult staged =
              stagedClassResultForSource(facts.body, *instruction.result);
          staged.producer == &instruction && staged.slot != nullptr) {
        output << "__gti_mir_p_"
               << canonicalSlotPlaceId(facts.body, *staged.slot)
               << ".construct();\n";
        return;
      }
      if (const ClassSsaLifetimeSlot valueSlot = classSsaLifetimeSlot(
              program, representations, facts.body, *instruction.result);
          valueSlot.producer == &instruction) {
        output << "__gti_mir_v_" << *instruction.result << ".construct();\n";
        return;
      }
      if (const MirPlace *destination =
              classValuePublicationSlot(facts.body, *instruction.result)) {
        output << "__gti_mir_p_"
               << canonicalSlotPlaceId(facts.body, *destination)
               << ".construct();\n";
        return;
      }
      // The substituted type parameter's default construction spells the
      // concrete value initialization.
      output << "__gti_mir_v_" << *instruction.result << " = "
             << typeSpelling(instruction.info.type) << "{};\n";
      return;
    }
    if (instruction.kind == MirInstructionKind::Call &&
        instruction.intrinsic != IntrinsicKind::None) {
      const std::string_view helper =
          cppIntegerArithmeticIntrinsicSpelling(instruction.intrinsic);
      if (!scalarSpellableArithmeticIntrinsic(instruction.intrinsic) ||
          helper.empty() || instruction.receiver || !instruction.result ||
          instruction.operands.size() != 2) {
        throw std::logic_error(
            "verified MIR intrinsic call is outside the spellable "
            "arithmetic helper family");
      }
      output << "__gti_mir_v_" << *instruction.result << " = " << helper << '(';
      emitOperand(instruction.operands[0]);
      output << ", ";
      emitOperand(instruction.operands[1]);
      output << ");\n";
      return;
    }
    if (!instruction.functionTarget && !instruction.constructorTarget &&
        !instruction.lambdaTarget && !instruction.bodyTarget &&
        !instruction.callableInvocation && !instruction.receiver &&
        (instruction.intrinsic == IntrinsicKind::None ||
         instruction.intrinsic ==
             IntrinsicKind::DefaultTypeParameterConstruction) &&
        instruction.operands.empty() && instruction.callableArguments.empty() &&
        instruction.localFailureSites.empty() && instruction.result) {
      if (returnDefaultConstructionDefinition(
              facts.body, *instruction.result) == &instruction) {
        output << "// default construction " << *instruction.result
               << " publishes at its consuming return\n";
        return;
      }
      if (const ExpectedDefaultPayloadInitialization defaultPayload =
              expectedDefaultPayloadInitialization(program, facts.body,
                                                   *instruction.result);
          defaultPayload.producer == &instruction) {
        output << "// default construction " << *instruction.result
               << " publishes at its Expected initializer\n";
        return;
      }
      if (const StagedClassResult staged =
              stagedClassResultForSource(facts.body, *instruction.result);
          staged.producer == &instruction && staged.slot != nullptr) {
        output << "__gti_mir_p_"
               << canonicalSlotPlaceId(facts.body, *staged.slot)
               << ".construct();\n";
        return;
      }
      if (const ClassSsaLifetimeSlot valueSlot = classSsaLifetimeSlot(
              program, representations, facts.body, *instruction.result);
          valueSlot.producer == &instruction) {
        output << "__gti_mir_v_" << *instruction.result << ".construct();\n";
        return;
      }
      if (const MirPlace *destination =
              classValuePublicationSlot(facts.body, *instruction.result)) {
        output << "__gti_mir_p_"
               << canonicalSlotPlaceId(facts.body, *destination)
               << ".construct();\n";
        return;
      }
      // The explicit default construction of a class with no declared
      // constructor: the value-initialized temporary assigns into the
      // declared local, exactly like the compatibility spelling.
      output << "__gti_mir_v_" << *instruction.result << " = "
             << typeSpelling(instruction.info.type) << "{};\n";
      return;
    }
    if (!instruction.functionTarget) {
      throw std::logic_error(
          "verified MIR direct call lost its exact target declaration");
    }
    if (instruction.result &&
        inlineNestedCallResult(program, representations, facts.body,
                               *instruction.result) == &instruction) {
      // The nested contained-member call spells inline at its consuming
      // argument; nothing publishes here.
      output << "// call " << instruction.id
             << " spells inline at its consuming argument\n";
      return;
    }
    // A receiver-carrying call spells its staged borrowed place followed by
    // the qualified member name: the explicit qualification states the
    // static dispatch MIR proved.
    const MirPlace *receiverPlace = nullptr;
    bool receiverMoved = false;
    const MirFunctionInstance *target =
        instruction.functionTarget
            ? program.findFunctionInstance(*instruction.functionTarget)
            : nullptr;
    if (instruction.receiver) {
      // Staged through a CallInput, or borrowed directly on the receiver
      // operand (a self-member call); both name the spellable place.
      const MirInstruction *staged =
          borrowStagedCallInput(facts.body, *instruction.receiver);
      const MirOperand &receiverBorrow =
          staged != nullptr ? staged->operands.front() : *instruction.receiver;
      const MirInstruction *movedStage =
          staged == nullptr &&
                  instruction.receiver->kind == MirOperandKind::Value
              ? copyStagedCallInput(facts.body, instruction.receiver->value)
              : nullptr;
      if (movedStage != nullptr) {
        const StagedTemporarySource source =
            stagedTemporarySourceFor(facts.body, *movedStage);
        receiverPlace = source.moved ? source.place : nullptr;
        receiverMoved = receiverPlace != nullptr;
      }
      if (receiverPlace == nullptr) {
        receiverPlace = (receiverBorrow.kind == MirOperandKind::BorrowRead ||
                         receiverBorrow.kind == MirOperandKind::BorrowWrite) &&
                                receiverBorrow.place != 0
                            ? facts.body.findPlace(receiverBorrow.place)
                            : nullptr;
      }
      if (receiverPlace == nullptr) {
        receiverPlace = directTemporaryReceiver(facts.body, instruction).slot;
      }
      if (receiverPlace == nullptr) {
        throw std::logic_error(
            "verified MIR receiver call lost its staged borrowed place");
      }
      // `std::move(receiver)` requests consuming overload resolution. When
      // semantics selected the mutable or read-only fallback, the compatibility
      // dispatcher invokes that exact overload on the original lvalue.
      receiverMoved =
          receiverMoved && target != nullptr &&
          target->receiverMutability == ReceiverMutability::Consuming;
    }
    const auto emitReceiverExpression = [&] {
      const bool exactVirtualDispatch =
          receiverPlace != nullptr && target != nullptr &&
          instruction.dispatch == CallDispatch::Virtual &&
          instruction.dispatchOwner.kind == SemanticType::Class;
      if (exactVirtualDispatch) {
        output << "static_cast<";
        if (target->receiverMutability == ReceiverMutability::ReadOnly) {
          output << "const ";
        }
        output << typeSpelling(instruction.dispatchOwner)
               << (target->receiverMutability == ReceiverMutability::Consuming
                       ? " &&>("
                       : " &>(");
        emitStoragePlaceValue(facts, *receiverPlace);
        output << ')';
        return;
      }
      if (receiverMoved) {
        output << "std::move(";
      }
      emitStoragePlaceValue(facts, *receiverPlace);
      if (receiverMoved) {
        output << ')';
      }
    };
    const auto retireConsumedClassResultStages = [&] {
      for (const MirOperand &operand : instruction.operands) {
        if (operand.kind != MirOperandKind::Value) {
          continue;
        }
        const ClassSsaLifetimeSlot valueSlot = classSsaLifetimeSlot(
            program, representations, facts.body, operand.value);
        if (valueSlot.consumer == &instruction &&
            valueSlot.consumerKind == ClassSsaSlotConsumerKind::CallArgument) {
          writeIndent();
          output << "__gti_mir_v_" << operand.value << ".destroy();\n";
          continue;
        }
        const StagedClassResult staged =
            stagedClassResultForResult(facts.body, operand.value);
        if (staged.consumer != &instruction || staged.slot == nullptr) {
          continue;
        }
        writeIndent();
        output << "__gti_mir_p_"
               << canonicalSlotPlaceId(facts.body, *staged.slot)
               << ".destroy();\n";
      }
    };
    const bool boundaryContainedTransformedCall =
        !failureForm &&
        wrapperContainedCallSite(program, representations, facts.body, block,
                                 instruction);
    if ((failureForm || boundaryContainedTransformedCall) &&
        transformedCallee(instruction) != nullptr) {
      // The callee's transformed body carries the derived name and writes
      // the caller's record on failure; the paired Invoke branches on the
      // success bool.
      const MirFunctionInstance &callee = *transformedCallee(instruction);
      if (boundaryContainedTransformedCall) {
        output << "::gti_failure_record_v1 __gti_mir_boundary_failure_"
               << instruction.id << "{};\n";
        writeIndent();
        output << "bool __gti_mir_call_success_" << instruction.id
               << " = false;\n";
        writeIndent();
      }
      const MirPlace *classDestination =
          instruction.result && callee.returnType.kind == SemanticType::Class
              ? classValuePublicationSlot(facts.body, *instruction.result)
              : nullptr;
      const ConstructorFieldResultSlot constructorFieldDestination =
          instruction.result && callee.returnType.kind == SemanticType::Class
              ? constructorFieldResultSlot(program, facts.body,
                                           *instruction.result)
              : ConstructorFieldResultSlot{};
      const ValueRootedClassCallResultSlot valueRootedClassDestination =
          instruction.result && callee.returnType.kind == SemanticType::Class
              ? valueRootedClassCallResultSlot(program, representations,
                                               facts.body, *instruction.result)
              : ValueRootedClassCallResultSlot{};
      const MirPlace *lambdaDestination =
          instruction.result && callee.returnType.kind == SemanticType::Lambda
              ? lambdaValueDestinationSlot(facts.body, *instruction.result)
              : nullptr;
      const bool expectedClassPlacement = expectedClassPlacementResultType(
          program, representations, callee.returnType);
      const MirPlace *expectedClassDestination =
          instruction.result && expectedClassPlacement
              ? expectedClassResultDestinationSlot(program, facts.body,
                                                   *instruction.result)
              : nullptr;
      const bool placementDirectReturn =
          instruction.result &&
          (callee.returnType.kind == SemanticType::Class ||
           expectedClassPlacement) &&
          placementDirectReturnCall(program, representations, facts.body,
                                    *instruction.result) == &instruction;
      const bool aggregateInputSlot =
          instruction.result && callee.returnType.kind == SemanticType::Class &&
          fixedArrayAggregateInputSlot(program, facts.body,
                                       *instruction.result) != nullptr;
      const bool expectedInputSlot =
          instruction.result && callee.returnType.kind == SemanticType::Class &&
          expectedPayloadInitialize(program, facts.body, *instruction.result) !=
              nullptr;
      const bool expectedPayloadReturn =
          instruction.result && callee.returnType.kind == SemanticType::Class &&
          expectedPayloadReturnSlot(program, representations, facts.body,
                                    *instruction.result)
                  .producer == &instruction;
      const ClassSsaLifetimeSlot classValueSlot =
          instruction.result
              ? classSsaLifetimeSlot(program, representations, facts.body,
                                     *instruction.result)
              : ClassSsaLifetimeSlot{};
      output << "__gti_mir_call_success_" << instruction.id << " = ";
      if (receiverPlace != nullptr) {
        emitReceiverExpression();
        output << '.';
      }
      const std::string &calleeSpelling =
          bodySpelling(*instruction.functionTarget);
      const std::string sibling = cppMirFailureSiblingSpelling(
          instruction.dispatch == CallDispatch::Virtual
              ? unqualifiedBodySpelling(calleeSpelling)
              : std::string_view(calleeSpelling));
      if (sibling.empty()) {
        throw std::logic_error(
            "verified MIR failure call lost its transformed sibling name");
      }
      output << sibling << '(';
      if (emitCallOperands(&callee, false, true)) {
        output << ", ";
      }
      if (callee.returnType == SemanticType::Void) {
        output << (boundaryContainedTransformedCall
                       ? "&__gti_mir_boundary_failure_"
                       : "__gti_mir_failure_record");
        if (boundaryContainedTransformedCall) {
          output << instruction.id;
        }
        output << ");\n";
      } else if (callee.returnType.kind == SemanticType::Reference) {
        const MirLoan *paired = producedCallResultLoan(facts.body, instruction);
        if (paired != nullptr) {
          output << "&__gti_mir_loan_" << paired->id;
        } else {
          output << "&__gti_mir_discard_" << instruction.id;
        }
        output << ", "
               << (boundaryContainedTransformedCall
                       ? "&__gti_mir_boundary_failure_"
                       : "__gti_mir_failure_record");
        if (boundaryContainedTransformedCall) {
          output << instruction.id;
        }
        output << ");\n";
      } else if (callee.returnType.kind == SemanticType::Class ||
                 callee.returnType.kind == SemanticType::Lambda ||
                 expectedClassPlacement) {
        if (instruction.result) {
          if (classDestination == nullptr && lambdaDestination == nullptr &&
              expectedClassDestination == nullptr && !placementDirectReturn &&
              !aggregateInputSlot && !expectedInputSlot &&
              !expectedPayloadReturn && !classValueSlot &&
              !constructorFieldDestination && !valueRootedClassDestination) {
            throw std::logic_error(
                "verified MIR class call lost its exact destination slot");
          }
          if (classDestination != nullptr) {
            output << "__gti_mir_p_"
                   << canonicalSlotPlaceId(facts.body, *classDestination)
                   << ".construction_address()";
          } else if (constructorFieldDestination.slot != nullptr) {
            output << "__gti_mir_p_"
                   << canonicalSlotPlaceId(facts.body,
                                           *constructorFieldDestination.slot)
                   << ".construction_address()";
          } else if (valueRootedClassDestination.slot != nullptr) {
            output << "__gti_mir_p_"
                   << canonicalSlotPlaceId(facts.body,
                                           *valueRootedClassDestination.slot)
                   << ".construction_address()";
          } else if (lambdaDestination != nullptr) {
            output << "__gti_mir_p_"
                   << canonicalSlotPlaceId(facts.body, *lambdaDestination)
                   << ".construction_address()";
          } else if (expectedClassDestination != nullptr) {
            output << "__gti_mir_p_"
                   << canonicalSlotPlaceId(facts.body,
                                           *expectedClassDestination)
                   << ".construction_address()";
          } else if (placementDirectReturn) {
            output << "__gti_mir_out_result";
          } else {
            output << "__gti_mir_v_" << *instruction.result
                   << ".construction_address()";
          }
        } else {
          output << "__gti_mir_discard_" << instruction.id
                 << ".construction_address()";
        }
        output << ", "
               << (boundaryContainedTransformedCall
                       ? "&__gti_mir_boundary_failure_"
                       : "__gti_mir_failure_record");
        if (boundaryContainedTransformedCall) {
          output << instruction.id;
        }
        output << ");\n";
      } else {
        if (instruction.result) {
          output << "&__gti_mir_v_" << *instruction.result;
        } else {
          output << "&__gti_mir_discard_" << instruction.id;
        }
        output << ", "
               << (boundaryContainedTransformedCall
                       ? "&__gti_mir_boundary_failure_"
                       : "__gti_mir_failure_record");
        if (boundaryContainedTransformedCall) {
          output << instruction.id;
        }
        output << ");\n";
      }
      if (callee.returnType.kind == SemanticType::Class ||
          callee.returnType.kind == SemanticType::Lambda ||
          expectedClassPlacement) {
        writeIndent();
        output << "if (__gti_mir_call_success_" << instruction.id << ") {\n";
        ++indentation;
        writeIndent();
        if (classDestination != nullptr) {
          output << "__gti_mir_p_"
                 << canonicalSlotPlaceId(facts.body, *classDestination)
                 << ".mark_constructed();\n";
        } else if (constructorFieldDestination.slot != nullptr) {
          output << "__gti_mir_p_"
                 << canonicalSlotPlaceId(facts.body,
                                         *constructorFieldDestination.slot)
                 << ".mark_constructed();\n";
        } else if (valueRootedClassDestination.slot != nullptr) {
          output << "__gti_mir_p_"
                 << canonicalSlotPlaceId(facts.body,
                                         *valueRootedClassDestination.slot)
                 << ".mark_constructed();\n";
        } else if (lambdaDestination != nullptr) {
          output << "__gti_mir_p_"
                 << canonicalSlotPlaceId(facts.body, *lambdaDestination)
                 << ".mark_constructed();\n";
        } else if (expectedClassDestination != nullptr) {
          output << "__gti_mir_p_"
                 << canonicalSlotPlaceId(facts.body, *expectedClassDestination)
                 << ".mark_constructed();\n";
        } else if (placementDirectReturn) {
          output << "// caller owns the directly published placement result\n";
        } else if (aggregateInputSlot || expectedInputSlot ||
                   expectedPayloadReturn || classValueSlot) {
          output << "__gti_mir_v_" << *instruction.result
                 << ".mark_constructed();\n";
        } else {
          output << "__gti_mir_discard_" << instruction.id
                 << ".mark_constructed();\n";
          writeIndent();
          output << "__gti_mir_discard_" << instruction.id << ".destroy();\n";
        }
        --indentation;
        writeIndent();
        output << "}\n";
      }
      if (boundaryContainedTransformedCall) {
        writeIndent();
        output << "if (!__gti_mir_call_success_" << instruction.id << ") {\n";
        ++indentation;
        writeIndent();
        output << "::gti_rt_failure_terminate_v1(\n";
        ++indentation;
        writeIndent();
        output << "&__gti_mir_boundary_failure_" << instruction.id << ",\n";
        writeIndent();
        output << "&::gti_internal::backend::"
                  "__gti_failure_artifact_descriptor_v1,\n";
        writeIndent();
        output << "nullptr, nullptr);\n";
        --indentation;
        --indentation;
        writeIndent();
        output << "}\n";
      }
      retireConsumedClassResultStages();
      if (!invokePairedInstruction(facts.body, instruction.id)) {
        if (!failureStatusCannotFail(callee.body)) {
          throw std::logic_error(
              "verified transformed MIR call lost its invoke edge");
        }
      }
      return;
    }
    const MirLoan *pairedResultLoan =
        !failureForm ? producedCallResultLoan(facts.body, instruction)
                     : nullptr;
    const MirPlace *classDestination =
        instruction.result
            ? classValuePublicationSlot(facts.body, *instruction.result)
            : nullptr;
    const ConstructorFieldResultSlot constructorFieldDestination =
        instruction.result && instruction.info.type.kind == SemanticType::Class
            ? constructorFieldResultSlot(program, facts.body,
                                         *instruction.result)
            : ConstructorFieldResultSlot{};
    const MirPlace *classResultDestination =
        classDestination != nullptr ? classDestination
                                    : constructorFieldDestination.slot;
    if (pairedResultLoan != nullptr) {
      // A reference-returning contained callee publishes its result
      // through the paired loan pointer: the referent binds by address,
      // exactly like the compatibility reference binding. Spelling the
      // returned reference into a value local instead would leave the
      // loan pointer null at its first read.
      output << "__gti_mir_loan_" << pairedResultLoan->id << " = &(";
    } else if (classResultDestination != nullptr) {
      // The result object's identity is either the consuming binding slot or
      // the exact constructor-field staging slot. The factory form preserves
      // C++17 guaranteed copy elision for a plain class-returning call without
      // requiring a move constructor.
      output << "__gti_mir_p_"
             << canonicalSlotPlaceId(facts.body, *classResultDestination)
             << ".construct_from([&]() { return ";
    } else if (instruction.result) {
      if (!failureForm &&
          returnCallDefinition(facts.body, *instruction.result) ==
              &instruction) {
        // The class result publishes at its consuming return: the call
        // spells the return expression and the value never declares.
        output << "return ";
      } else {
        output << "__gti_mir_v_" << *instruction.result << " = ";
      }
    }
    if (receiverPlace != nullptr) {
      emitReceiverExpression();
      output << '.';
    }
    const std::string &calleeSpelling =
        bodySpelling(*instruction.functionTarget);
    output << (instruction.dispatch == CallDispatch::Virtual
                   ? unqualifiedBodySpelling(calleeSpelling)
                   : std::string_view(calleeSpelling));
    output << '(';
    const bool cBoundary =
        target != nullptr && target->linkage == LanguageLinkage::C;
    // The C prototype takes ::gti_c_string_view; the shared operand writer
    // marshals views and expands an exact trailing concrete GTI pack.
    (void)emitCallOperands(target, cBoundary, false);
    output << ')';
    if (pairedResultLoan != nullptr) {
      output << ')';
    }
    output << (classResultDestination != nullptr ? "; });\n" : ";\n");
    retireConsumedClassResultStages();
  }

  void emitSwitchInteger(const EnumConstant &value, const SemanticType &type) {
    if (!value.negative) {
      output << value.magnitude;
      if (type == SemanticType::UInt64 &&
          value.magnitude > static_cast<std::uint64_t>(
                                std::numeric_limits<std::int64_t>::max())) {
        output << "ULL";
      }
      return;
    }

    std::uint64_t signedLimit = 0;
    switch (type.kind) {
    case SemanticType::Int8:
      signedLimit = std::uint64_t{1} << 7U;
      break;
    case SemanticType::Int16:
      signedLimit = std::uint64_t{1} << 15U;
      break;
    case SemanticType::Int32:
      signedLimit = std::uint64_t{1} << 31U;
      break;
    case SemanticType::Int64:
      signedLimit = std::uint64_t{1} << 63U;
      break;
    default:
      break;
    }
    if (signedLimit != 0 && value.magnitude == signedLimit) {
      output << "(-" << signedLimit - 1 << "LL - 1LL)";
      return;
    }
    output << '-' << value.magnitude;
  }

  void emitTerminator(const MirBlock &block, const ScalarBodyFacts &facts) {
    const MirTerminator &terminator = block.terminator;
    switch (terminator.kind) {
    case MirTerminatorKind::Invoke: {
      const MirInstruction *producer =
          findInstruction(facts.body, terminator.invokeInstruction);
      if (failureForm && producer != nullptr &&
          callableValueInvocation(*producer)) {
        writeIndent();
        output << "__gti_mir_bb = __gti_mir_call_success_" << producer->id
               << " ? " << terminator.target << " : " << terminator.elseTarget
               << ";\n";
        writeIndent();
        output << "continue;\n";
        return;
      }
      if (producer != nullptr &&
          ((!failureForm && (callableValueInvocation(*producer) ||
                             deducedCallableCallee(program, *producer))) ||
           terminallyContainedPlainCallee(program, representations,
                                          *producer) ||
           terminallyContainedPlainConstructor(program, representations,
                                               *producer) ||
           // The unique-owner allocation contains its failure terminally
           // inside the backend helper, exactly like the compatibility
           // call site.
           producer->intrinsic == IntrinsicKind::AllocateUniqueOwner ||
           // Plain prefix allocation uses the compatibility helper, which
           // terminates on exhaustion before publishing its result.
           (!failureForm &&
            producer->intrinsic == IntrinsicKind::AllocatePrefixStorage) ||
           (!failureForm && storageBoundsCheckCall(*producer)))) {
        // The fused literal or template callee contains its failure
        // terminally; the edge is a plain goto and the else block never
        // runs.
        writeIndent();
        output << "__gti_mir_bb = " << terminator.target << ";\n";
        writeIndent();
        output << "continue;\n";
        return;
      }
      if (!failureForm) {
        // Plain shape: checked arithmetic spells its terminal helper, a
        // template body's may-raise call reaches a terminally-contained
        // convention, and a propagating construction terminates at its own
        // site; none ever returns on failure, so the edge is a plain goto.
        // The probe admits exactly these producers.
        if ((producer != nullptr &&
             producer->kind == MirInstructionKind::Compute &&
             (!cppMirTerminalCheckedHelperSpelling(producer->operation)
                   .empty() ||
              // The view element read and the checked conversion spell
              // their terminal helpers (string_view_at, numeric_cast).
              producer->operation == MirOperation::Index ||
              producer->operation == MirOperation::Convert) &&
             !producer->localFailureSites.empty()) ||
            // The closed compound assignment spells its terminal helper;
            // it never returns on failure, so the edge is a plain goto.
            (producer != nullptr &&
             producer->kind == MirInstructionKind::Assign &&
             !cppMirCompoundAssignHelperSpelling(producer->operation).empty() &&
             !producer->localFailureSites.empty()) ||
            (producer != nullptr &&
             producer->kind == MirInstructionKind::Call) ||
            (producer != nullptr &&
             producer->kind == MirInstructionKind::Construct &&
             producer->localFailureSites.empty() &&
             producer->definedFailure.propagation ==
                 FailurePropagationKind::Constructor)) {
          writeIndent();
          output << "__gti_mir_bb = " << terminator.target << ";\n";
          writeIndent();
          output << "continue;\n";
          return;
        }
        throw std::logic_error(
            "verified MIR invoke is outside the failure vocabulary");
      }
      if (producer != nullptr && producer->kind == MirInstructionKind::Drop &&
          failureDestructorTarget(program, facts.body, *producer) != nullptr) {
        writeIndent();
        output << "__gti_mir_bb = __gti_mir_drop_success_" << producer->id
               << " ? " << terminator.target << " : " << terminator.elseTarget
               << ";\n";
        writeIndent();
        output << "continue;\n";
        return;
      }
      if (producer != nullptr && transformedConstructor(*producer) != nullptr) {
        writeIndent();
        output << "__gti_mir_bb = __gti_mir_construct_success_" << producer->id
               << " ? " << terminator.target << " : " << terminator.elseTarget
               << ";\n";
        writeIndent();
        output << "continue;\n";
        return;
      }
      // The vacuous-else producers the probe admits: a discharged
      // storage read (flow proved the bound) and a propagating
      // construction (constructor failure terminates at its own site);
      // their edges are plain gotos.
      if (producer != nullptr &&
          ((producer->kind == MirInstructionKind::Compute &&
            (!cppMirTerminalCheckedHelperSpelling(producer->operation)
                  .empty() ||
             producer->operation == MirOperation::Convert) &&
            (producer->info.type.kind == SemanticType::Float ||
             producer->info.type.kind == SemanticType::Double)) ||
           (producer->kind == MirInstructionKind::Call &&
            prefixStorageIntrinsic(producer->intrinsic) &&
            producer->localFailureSites.empty() &&
            producer->definedFailure.propagation ==
                FailurePropagationKind::None) ||
           (producer->kind == MirInstructionKind::Construct &&
            producer->localFailureSites.empty() &&
            producer->definedFailure.propagation ==
                FailurePropagationKind::Constructor))) {
        writeIndent();
        output << "__gti_mir_bb = " << terminator.target << ";\n";
        writeIndent();
        output << "continue;\n";
        return;
      }
      if (producer != nullptr && producer->kind == MirInstructionKind::Call &&
          transformedCallee(*producer) != nullptr) {
        writeIndent();
        output << "__gti_mir_bb = __gti_mir_call_success_" << producer->id
               << " ? " << terminator.target << " : " << terminator.elseTarget
               << ";\n";
        writeIndent();
        output << "continue;\n";
        return;
      }
      const bool elementDetector =
          producer != nullptr &&
          (producer->kind == MirInstructionKind::Load ||
           producer->kind == MirInstructionKind::Assign) &&
          producer->localFailureSites.size() == 1;
      const bool storageDetector =
          producer != nullptr && producer->kind == MirInstructionKind::Call &&
          (storageAllocationFailureCall(program, *producer) ||
           (sparseStorageIntrinsic(producer->intrinsic) &&
            !producer->localFailureSites.empty()) ||
           (prefixStorageIntrinsic(producer->intrinsic) &&
            !producer->localFailureSites.empty()) ||
           storageBoundsCheckCall(*producer) ||
           checkedConversionIntrinsicCall(*producer));
      const bool expectedDetector =
          producer != nullptr && producer->kind == MirInstructionKind::Call &&
          (producer->intrinsic == IntrinsicKind::ExpectedValue ||
           producer->intrinsic == IntrinsicKind::ExpectedError) &&
          producer->localFailureSites.size() == 1;
      const bool viewIndexDetector =
          producer != nullptr && stringViewIndexFailureSite(program, *producer);
      if (producer == nullptr ||
          (!elementDetector && !storageDetector && !expectedDetector &&
           !viewIndexDetector &&
           (producer->kind != MirInstructionKind::Compute ||
            cppMirCheckedOperationHelperSpelling(producer->operation)
                .empty()))) {
        throw std::logic_error(
            "verified MIR invoke is outside the failure vocabulary");
      }
      writeIndent();
      output << "if (__gti_mir_failure_status_" << producer->id
             << ".code == GTI_FAILURE_CODE_NONE_V1) {\n";
      ++indentation;
      writeIndent();
      output << "__gti_mir_bb = " << terminator.target << ";\n";
      --indentation;
      writeIndent();
      output << "} else {\n";
      ++indentation;
      emitFailureRecordWrite(*producer);
      writeIndent();
      output << "__gti_mir_bb = " << terminator.elseTarget << ";\n";
      --indentation;
      writeIndent();
      output << "}\n";
      writeIndent();
      output << "continue;\n";
      return;
    }
    case MirTerminatorKind::PropagateFailure:
      if (!failureForm) {
        // Only an inline literal's plain shape admits this terminator:
        // every failure source is contained terminally inside its helper,
        // so the block is unreachable.
        writeIndent();
        output << "std::abort();\n";
        return;
      }
      writeIndent();
      output << "// GTI MIR propagate failure-record "
             << terminator.failureRecord << " after cleanup\n";
      writeIndent();
      output << (facts.kind == MirBodyKind::Constructor ? "return;\n"
                                                        : "return false;\n");
      return;
    case MirTerminatorKind::TerminateCleanupFailure: {
      const MirFailureRecord *secondary =
          facts.body.findFailureRecord(terminator.failureRecord);
      const MirBlock *producerBlock =
          secondary == nullptr ? nullptr
                               : facts.body.findBlock(secondary->producerBlock);
      const MirInstruction *producer =
          secondary == nullptr
              ? nullptr
              : findInstruction(facts.body, secondary->producerInstruction);
      if (!failureForm || block.activeFailure == 0 ||
          block.failureParameter != terminator.failureRecord ||
          block.activeFailure == terminator.failureRecord ||
          secondary == nullptr || producerBlock == nullptr ||
          producerBlock->activeFailure != block.activeFailure ||
          producer == nullptr || producer->kind != MirInstructionKind::Drop ||
          failureDestructorTarget(program, facts.body, *producer) == nullptr) {
        throw std::logic_error(
            "verified MIR cleanup-failure terminator lost its primary and "
            "secondary records");
      }
      writeIndent();
      output << "::gti_failure_emergency_v1 __gti_mir_failure_emergency_"
             << block.id << "{\n";
      ++indentation;
      writeIndent();
      output << "GTI_FAILURE_ABI_VERSION_V1, UINT32_C(0),\n";
      writeIndent();
      output << "*__gti_mir_failure_record,\n";
      writeIndent();
      output << "__gti_mir_cleanup_failure_record_" << producer->id << "\n";
      --indentation;
      writeIndent();
      output << "};\n";
      writeIndent();
      output << "::gti_rt_failure_terminate_cleanup_v1(\n";
      ++indentation;
      writeIndent();
      output << "&__gti_mir_failure_emergency_" << block.id << ",\n";
      writeIndent();
      output << "&::gti_internal::backend::"
                "__gti_failure_artifact_descriptor_v1,\n";
      writeIndent();
      output << "&::gti_internal::backend::"
                "__gti_failure_artifact_descriptor_v1);\n";
      --indentation;
      return;
    }
    default:
      break;
    }
    switch (terminator.kind) {
    case MirTerminatorKind::Goto:
      writeIndent();
      output << "__gti_mir_bb = " << terminator.target << ";\n";
      writeIndent();
      output << "continue;\n";
      return;
    case MirTerminatorKind::Branch:
      writeIndent();
      output << "__gti_mir_bb = ";
      emitOperand(*terminator.value);
      output << " ? " << terminator.target << " : " << terminator.elseTarget
             << ";\n";
      writeIndent();
      output << "continue;\n";
      return;
    case MirTerminatorKind::Switch: {
      // A payload-enum discriminant switches over the variant record's
      // alternative index; its cases are the copied variant indices,
      // exactly like the compatibility payload switch.
      const CppMirEnumRepresentation *payloadRow = nullptr;
      if (terminator.value->type.kind == SemanticType::Enum) {
        for (const CppMirEnumRepresentation &row : representations.enums()) {
          if (row.owner == terminator.value->type.enumId &&
              !row.payloadVariants.empty()) {
            payloadRow = &row;
          }
        }
      }
      writeIndent();
      output << "switch (";
      emitOperand(*terminator.value);
      if (payloadRow != nullptr) {
        output << ".__gti_value.index()";
      }
      output << ") {\n";
      ++indentation;
      for (const MirSwitchTarget &target : terminator.switchTargets) {
        writeIndent();
        if (payloadRow != nullptr) {
          output << "case " << target.value->value.magnitude << ":\n";
        } else {
          output << "case static_cast<" << typeSpelling(target.value->type)
                 << ">(";
          emitSwitchInteger(target.value->value, target.value->type);
          output << "):\n";
        }
        ++indentation;
        writeIndent();
        output << "__gti_mir_bb = " << target.target << ";\n";
        writeIndent();
        output << "break;\n";
        --indentation;
      }
      writeIndent();
      output << "default:\n";
      ++indentation;
      writeIndent();
      output << "__gti_mir_bb = " << terminator.target << ";\n";
      writeIndent();
      output << "break;\n";
      --indentation;
      --indentation;
      writeIndent();
      output << "}\n";
      writeIndent();
      output << "continue;\n";
      return;
    }
    case MirTerminatorKind::Return: {
      const ClassSsaLifetimeSlot returnedClassSlot =
          terminator.value && terminator.value->kind == MirOperandKind::Value
              ? classSsaLifetimeSlot(program, representations, facts.body,
                                     terminator.value->value)
              : ClassSsaLifetimeSlot{};
      if (failureForm && facts.kind == MirBodyKind::Constructor) {
        if (const MirClassInstance *owner =
                disarmedConstructorLifecycleOwner(facts)) {
          writeIndent();
          output << "(*this).__gti_lifecycle_active_" << owner->declaration
                 << " = true;\n";
        }
        writeIndent();
        output << "*__gti_mir_constructor_success = true;\n";
        writeIndent();
        output << "return;\n";
        return;
      }
      if (terminator.returnLoan && *terminator.returnLoan != 0 &&
          !(terminator.value &&
            terminator.value->kind == MirOperandKind::Value &&
            (returnConstructDefinition(facts.body, terminator.value->value) !=
                 nullptr ||
             returnMoveDefinition(facts.body, terminator.value->value) !=
                 nullptr)) &&
          !(returnedClassSlot.returnBlock == &block &&
            returnedClassSlot.consumerKind ==
                ClassSsaSlotConsumerKind::Return)) {
        // A return loan produced by an owner borrow spells the backend
        // accessor over the owner field directly — no loan pointer ever
        // binds, exactly like the compatibility body.
        const auto ownerBorrowField = [&]() -> const MirPlace * {
          const MirLoan *loan = facts.body.findLoan(*terminator.returnLoan);
          const MirInstruction *producer =
              loan == nullptr ? nullptr
                              : ownerBorrowLoanProducer(facts.body, *loan);
          const MirValue *operandValue =
              producer != nullptr && producer->operands.size() == 1
                  ? facts.body.findValue(producer->operands.front().value)
                  : nullptr;
          const MirInstruction *fieldLoad =
              operandValue == nullptr
                  ? nullptr
                  : findInstruction(facts.body, operandValue->definition);
          return fieldLoad != nullptr &&
                         fieldLoad->kind == MirInstructionKind::Load &&
                         fieldLoad->operands.size() == 1 &&
                         fieldLoad->operands.front().place != 0
                     ? facts.body.findPlace(fieldLoad->operands.front().place)
                     : nullptr;
        };
        if (const MirPlace *field = ownerBorrowField()) {
          writeIndent();
          if (failureForm) {
            output << "*__gti_mir_out_result = &::gti_internal::backend::"
                      "owner_access((*this)."
                   << fieldSpelling(facts, field->projections.front().field)
                   << ");\n";
            writeIndent();
            output << "return true;\n";
          } else {
            output << "return ::gti_internal::backend::owner_access((*this)."
                   << fieldSpelling(facts, field->projections.front().field)
                   << ");\n";
          }
          return;
        }
        if (const MirLoan *returnLoan =
                facts.body.findLoan(*terminator.returnLoan)) {
          if (const MirInstruction *borrow =
                  elementBorrowLoanProducer(facts.body, *returnLoan)) {
            // The bounds-checked element borrow spells the terminal
            // array_at accessor over the field and staged index, exactly
            // like the compatibility subscript body.
            const MirPlace *place =
                facts.body.findPlace(borrow->operands.front().place);
            writeIndent();
            if (failureForm) {
              output << "*__gti_mir_out_result = &::gti_internal::backend::"
                        "array_at((*this)."
                     << fieldSpelling(facts, place->projections[0].field)
                     << ", __gti_mir_v_" << place->projections[1].index
                     << ");\n";
              writeIndent();
              output << "return true;\n";
            } else {
              output << "return ::gti_internal::backend::array_at((*this)."
                     << fieldSpelling(facts, place->projections[0].field)
                     << ", __gti_mir_v_" << place->projections[1].index
                     << ");\n";
            }
            return;
          }
        }
        if (failureForm) {
          // The loan pointer publishes through the `T **` out-parameter
          // (ADR 018 §5); the wrapper dereferences on the boundary.
          writeIndent();
          output << "*__gti_mir_out_result = __gti_mir_loan_"
                 << *terminator.returnLoan << ";\n";
          writeIndent();
          output << "return true;\n";
          return;
        }
        writeIndent();
        output << "return *__gti_mir_loan_" << *terminator.returnLoan << ";\n";
        return;
      }
      if (failureForm) {
        writeIndent();
        output << "// GTI MIR return publication\n";
        if (terminator.value) {
          const MirInstruction *construct =
              terminator.value->kind == MirOperandKind::Value
                  ? returnConstructDefinition(facts.body,
                                              terminator.value->value)
                  : nullptr;
          const MirInstruction *defaulted =
              construct == nullptr &&
                      terminator.value->kind == MirOperandKind::Value
                  ? returnDefaultConstructionDefinition(facts.body,
                                                        terminator.value->value)
                  : nullptr;
          const MirInstruction *moved =
              construct == nullptr &&
                      terminator.value->kind == MirOperandKind::Value
                  ? returnMoveDefinition(facts.body, terminator.value->value)
                  : nullptr;
          const MirInstruction *copied =
              construct == nullptr && moved == nullptr &&
                      terminator.value->kind == MirOperandKind::Value
                  ? returnCopyLoadDefinition(facts.body,
                                             terminator.value->value)
                  : nullptr;
          const MirInstruction *directPlacementCall =
              terminator.value->kind == MirOperandKind::Value
                  ? placementDirectReturnCall(program, representations,
                                              facts.body,
                                              terminator.value->value)
                  : nullptr;
          const ExpectedPayloadReturnSlot expectedPayload =
              terminator.value->kind == MirOperandKind::Value
                  ? expectedPayloadReturnSlot(program, representations,
                                              facts.body,
                                              terminator.value->value)
                  : ExpectedPayloadReturnSlot{};
          if (expectedPayload) {
            writeIndent();
            output << "std::construct_at(__gti_mir_out_result, std::move("
                   << "__gti_mir_v_" << terminator.value->value
                   << ".get()));\n";
            writeIndent();
            output << "__gti_mir_v_" << terminator.value->value
                   << ".destroy();\n";
          } else if (construct != nullptr &&
                     transformedConstructor(*construct) != nullptr) {
            // The transformed constructor placed the complete result into
            // the function's uninitialized out-parameter before its Invoke.
            writeIndent();
            output << "// GTI MIR constructed value published at its "
                      "constructor call\n";
          } else if (moved != nullptr) {
            // Already published at its Move; nothing to assign here.
            writeIndent();
            output << "// GTI MIR moved value published at its move\n";
          } else if (copied != nullptr) {
            // Already published at its copy Load; nothing to assign here.
            writeIndent();
            output << "// GTI MIR copied value published at its load\n";
          } else if (directPlacementCall != nullptr) {
            // The transformed callee received this function's own placement
            // result, and the success edge transferred that identity out.
            writeIndent();
            output << "// GTI MIR placement value published at its call\n";
          } else {
            writeIndent();
            const bool classPublication =
                facts.body.returnType.kind == SemanticType::Class;
            const bool expectedClassPublication =
                expectedClassPlacementResultType(program, representations,
                                                 facts.body.returnType);
            if (classPublication) {
              if (returnedClassSlot.returnBlock == &block &&
                  returnedClassSlot.consumerKind ==
                      ClassSsaSlotConsumerKind::Return) {
                output << "std::construct_at(__gti_mir_out_result, std::move("
                       << "__gti_mir_v_" << terminator.value->value
                       << ".get()))";
              } else {
                if (defaulted == nullptr && construct == nullptr) {
                  throw std::logic_error(
                      "verified MIR class return lost its construction "
                      "definition");
                }
                output << "std::construct_at(__gti_mir_out_result";
                if (construct != nullptr) {
                  for (std::size_t index = 0;
                       index < construct->operands.size(); ++index) {
                    output << ", ";
                    if (const MirInstruction *staged = borrowStagedCallInput(
                            facts.body, construct->operands[index])) {
                      const MirPlace *place =
                          facts.body.findPlace(staged->operands.front().place);
                      if (place == nullptr) {
                        throw std::logic_error(
                            "verified MIR publication construct lost a staged "
                            "borrowed argument place");
                      }
                      emitPlaceExpression(facts, *place);
                    } else {
                      const MirOperand &operand = construct->operands[index];
                      if (operand.kind == MirOperandKind::Value) {
                        if (const MirPlace *chainSource = movedPlaceChainSource(
                                facts.body, operand.value, *construct)) {
                          output << "std::move(";
                          emitStoragePlaceValue(facts, *chainSource);
                          output << ')';
                          continue;
                        }
                      }
                      const bool consumed =
                          operand.type.kind == SemanticType::Class ||
                          operand.type.kind == SemanticType::UniqueOwner ||
                          operand.type.kind == SemanticType::Storage ||
                          operand.type.kind == SemanticType::PrefixStorage;
                      if (consumed) {
                        output << "std::move(";
                      }
                      emitOperand(operand);
                      if (consumed) {
                        output << ')';
                      }
                    }
                  }
                }
                output << ')';
              }
            } else if (expectedClassPublication) {
              output << "std::construct_at(__gti_mir_out_result, ";
              if (defaulted != nullptr) {
                output << typeSpelling(defaulted->info.type) << "{}";
              } else if (construct != nullptr) {
                output << typeSpelling(construct->info.type) << '(';
                for (std::size_t index = 0; index < construct->operands.size();
                     ++index) {
                  if (index != 0) {
                    output << ", ";
                  }
                  if (const MirInstruction *staged = borrowStagedCallInput(
                          facts.body, construct->operands[index])) {
                    const MirPlace *place =
                        facts.body.findPlace(staged->operands.front().place);
                    if (place == nullptr) {
                      throw std::logic_error(
                          "verified MIR expected publication construct lost "
                          "a staged borrowed argument place");
                    }
                    emitPlaceExpression(facts, *place);
                  } else {
                    const MirOperand &operand = construct->operands[index];
                    if (operand.kind == MirOperandKind::Value) {
                      if (const MirPlace *chainSource = movedPlaceChainSource(
                              facts.body, operand.value, *construct)) {
                        output << "std::move(";
                        emitStoragePlaceValue(facts, *chainSource);
                        output << ')';
                        continue;
                      }
                    }
                    const bool consumed =
                        operand.type.kind == SemanticType::Class ||
                        operand.type.kind == SemanticType::UniqueOwner ||
                        operand.type.kind == SemanticType::Storage ||
                        operand.type.kind == SemanticType::PrefixStorage;
                    if (consumed) {
                      output << "std::move(";
                    }
                    emitOperand(operand);
                    if (consumed) {
                      output << ')';
                    }
                  }
                }
                output << ')';
              } else if (const MirInstruction *unexpectedValue =
                             terminator.value->kind == MirOperandKind::Value
                                 ? unexpectedDefinition(facts.body,
                                                        terminator.value->value)
                                 : nullptr;
                         unexpectedValue != nullptr) {
                output << expectedConstructionSpelling() << '(';
                emitOperand(unexpectedValue->operands.front());
                output << ')';
              } else {
                const bool consumed =
                    terminator.value->type.kind == SemanticType::Class ||
                    terminator.value->type.kind == SemanticType::Expected ||
                    terminator.value->type.kind == SemanticType::Unexpected;
                if (consumed) {
                  output << "std::move(";
                }
                emitReturnOperand(facts, *terminator.value);
                if (consumed) {
                  output << ')';
                }
              }
              output << ')';
            } else if (defaulted != nullptr) {
              // Passive default construction publishes by assignment.
              output << "*__gti_mir_out_result = "
                     << typeSpelling(defaulted->info.type) << "{}";
            } else if (construct != nullptr) {
              output << "*__gti_mir_out_result = "
                     << typeSpelling(construct->info.type) << '(';
              for (std::size_t index = 0; index < construct->operands.size();
                   ++index) {
                if (index != 0) {
                  output << ", ";
                }
                // A borrow-staged argument never materialized a local;
                // the constructor call spells its place directly, exactly
                // like a call argument.
                if (const MirInstruction *staged = borrowStagedCallInput(
                        facts.body, construct->operands[index])) {
                  const MirPlace *place =
                      facts.body.findPlace(staged->operands.front().place);
                  if (place == nullptr) {
                    throw std::logic_error(
                        "verified MIR publication construct lost a staged "
                        "borrowed argument place");
                  }
                  emitPlaceExpression(facts, *place);
                } else {
                  emitOperand(construct->operands[index]);
                }
              }
              output << ')';
            } else if (const MirInstruction *unexpectedValue =
                           terminator.value->kind == MirOperandKind::Value
                               ? unexpectedDefinition(facts.body,
                                                      terminator.value->value)
                               : nullptr;
                       unexpectedValue != nullptr) {
              // The unexpected wrapper converts into the expected-typed
              // result exactly like the plain return spelling; the
              // wrapped value never declares a local.
              output << "*__gti_mir_out_result = "
                     << expectedConstructionSpelling() << '(';
              emitOperand(unexpectedValue->operands.front());
              output << ')';
            } else {
              output << "*__gti_mir_out_result = ";
              emitReturnOperand(facts, *terminator.value);
            }
            output << ";\n";
            if (returnedClassSlot.returnBlock == &block &&
                returnedClassSlot.consumerKind ==
                    ClassSsaSlotConsumerKind::Return) {
              writeIndent();
              output << "__gti_mir_v_" << terminator.value->value
                     << ".destroy();\n";
            }
          }
        }
        writeIndent();
        output << "return true;\n";
        return;
      }
      if (terminator.value && terminator.value->kind == MirOperandKind::Value &&
          returnCallDefinition(facts.body, terminator.value->value) !=
              nullptr) {
        // The class result already published at its defining call.
        writeIndent();
        output << "// GTI MIR class result returned at its call\n";
        return;
      }
      if (terminator.value && terminator.value->kind == MirOperandKind::Value) {
        if (returnedClassSlot.returnBlock == &block &&
            returnedClassSlot.consumerKind ==
                ClassSsaSlotConsumerKind::Return) {
          writeIndent();
          output << "return std::move(__gti_mir_v_" << terminator.value->value
                 << ".get());\n";
          return;
        }
        if (const MirInstruction *defaulted =
                returnDefaultConstructionDefinition(facts.body,
                                                    terminator.value->value)) {
          // The substituted default construction publishes the concrete
          // value initialization inline at the return.
          writeIndent();
          output << "return " << typeSpelling(defaulted->info.type) << "{};\n";
          return;
        }
        if (const MirInstruction *construct = returnConstructDefinition(
                facts.body, terminator.value->value)) {
          // The class value publishes its constructor call inline at the
          // return, exactly like the compatibility return expression.
          writeIndent();
          output << "return " << typeSpelling(construct->info.type) << '(';
          for (std::size_t index = 0; index < construct->operands.size();
               ++index) {
            if (index != 0) {
              output << ", ";
            }
            // A borrow-staged argument never materialized a local; the
            // constructor call spells its place directly.
            if (const MirInstruction *staged = borrowStagedCallInput(
                    facts.body, construct->operands[index])) {
              const MirPlace *place =
                  facts.body.findPlace(staged->operands.front().place);
              if (place == nullptr) {
                throw std::logic_error(
                    "verified MIR publication construct lost a staged "
                    "borrowed argument place");
              }
              emitPlaceExpression(facts, *place);
            } else {
              emitOperand(construct->operands[index]);
            }
          }
          output << ");\n";
          return;
        }
      }
      writeIndent();
      output << "return";
      if (!terminator.value &&
          facts.body.returnType.kind == SemanticType::Expected) {
        // The valueless success return of an Expected<void, E> body
        // spells the engaged empty value, exactly like the compatibility
        // `return {};`.
        output << " {}";
      }
      if (terminator.value) {
        output << ' ';
        const MirInstruction *unexpectedValue =
            terminator.value->kind == MirOperandKind::Value
                ? unexpectedDefinition(facts.body, terminator.value->value)
                : nullptr;
        if (unexpectedValue != nullptr) {
          // The unexpected wrapper converts into the expected-typed
          // result here; the construction call is copied from the
          // Expected capability row for the emitted standard.
          output << expectedConstructionSpelling() << '(';
          emitOperand(unexpectedValue->operands.front());
          output << ')';
        } else {
          emitReturnOperand(facts, *terminator.value);
        }
      }
      output << ";\n";
      return;
    }
    case MirTerminatorKind::Exit:
      if (facts.kind != MirBodyKind::Module) {
        throw std::logic_error(
            "verified MIR Exit escaped the Module/0 runtime body");
      }
      writeIndent();
      output << (failureForm ? "return true;\n" : "return;\n");
      return;
    case MirTerminatorKind::Unreachable:
      writeIndent();
      output << "std::abort();\n";
      return;
    default:
      break;
    }
    throw std::logic_error("verified MIR scalar-CFG terminator is unsupported");
  }

  // Spells an lvalue expression for a place the loan machinery touches:
  // ordinary bindings, storage globals, receiver fields, receiver field
  // elements, sibling-array elements, and loan carriers (ADR 018).
  // Spells the VALUE held at a place: a lifetime slot exposes it through
  // its checked accessor, every other place is its own lvalue.
  void emitStoragePlaceValue(const ScalarBodyFacts &facts,
                             const MirPlace &place) {
    emitPlaceExpression(facts, place);
    if (slotPlace(facts.body, place)) {
      output << ".get()";
    }
  }

  void emitPlaceExpression(const ScalarBodyFacts &facts,
                           const MirPlace &place) {
    if (const std::optional<std::size_t> parameter = packElementParameterIndex(
            program, facts.body, {.kind = facts.kind, .owner = facts.id},
            place)) {
      output << "__gti_mir_arg_" << *parameter;
      return;
    }
    if (place.root == MirPlaceRootKind::Loan) {
      output << "(*__gti_mir_loan_" << place.loan << ')';
      const MirLoan *loan = facts.body.findLoan(place.loan);
      const std::optional<SemanticType> referent =
          loan == nullptr ? std::nullopt
                          : loanReferentType(program, facts.body, *loan);
      if (!referent) {
        throw std::logic_error(
            "verified loan projection lost its concrete referent type");
      }
      emitFieldProjectionChain(*referent, place.projections, 0);
      return;
    }
    if (place.root == MirPlaceRootKind::Symbol) {
      if (place.capture != 0 &&
          facts.instanceLabel == std::string_view("lambda-instance")) {
        output << captureSpelling(facts.id, place.symbol, place.capture);
      } else {
        if (place.projections.size() == 1 &&
            place.projections.front().kind == MirProjectionKind::Dereference) {
          output << "(*" << storageSpelling(facts, place.symbol) << ')';
        } else {
          output << storageSpelling(facts, place.symbol);
        }
      }
      return;
    }
    if (facts.kind == MirBodyKind::Module &&
        place.root == MirPlaceRootKind::Binding && place.projections.empty() &&
        place.symbol != 0) {
      output << storageSpelling(facts, place.symbol);
      return;
    }
    if (place.root == MirPlaceRootKind::This) {
      if (place.projections.empty()) {
        output << "(*this)";
        return;
      }
      if (place.projections.size() == 1 &&
          place.projections[0].kind == MirProjectionKind::Field) {
        output << "__gti_mir_p_" << place.id;
        return;
      }
      if (place.projections.size() == 2 &&
          place.projections[0].kind == MirProjectionKind::Field &&
          place.projections[1].kind == MirProjectionKind::Dereference) {
        // A reference member dereferences implicitly; the bound reference
        // local already names the referent.
        output << "__gti_mir_p_" << place.id;
        return;
      }
      if (place.projections.size() == 2 &&
          place.projections[0].kind == MirProjectionKind::Field &&
          place.projections[1].kind == MirProjectionKind::Index) {
        output << "(*this)." << fieldSpelling(facts, place.projections[0].field)
               << '[';
        if (place.projections[1].constantIndex) {
          output << "static_cast<std::size_t>("
                 << *place.projections[1].constantIndex << ")";
        } else {
          output << "static_cast<std::size_t>(__gti_mir_v_"
                 << place.projections[1].index << ')';
        }
        output << ']';
        return;
      }
    }
    if (place.root == MirPlaceRootKind::Value &&
        expectedClassExtractionPlace(facts.body, place.value) == &place) {
      output << "(*__gti_mir_ref_v_" << place.value << ')';
      return;
    }
    if (const std::optional<ArrayElementAccess> access =
            arrayElementAccess(facts.body, place)) {
      emitArrayElement(facts, *access);
      return;
    }
    if (const std::optional<BindingArrayFieldElementAccess> access =
            bindingArrayFieldElementAccess(facts.body, place)) {
      emitBindingArrayFieldElement(facts, *access);
      return;
    }
    if (const std::optional<ConstantArrayElementFieldAccess> access =
            constantArrayElementFieldAccess(facts.body, place)) {
      emitArrayElement(facts, access->element);
      emitFieldProjectionChain(access->elementType, access->fields, 0);
      return;
    }
    if (const std::optional<RawMemoryPlaceAccess> access =
            rawMemoryPlaceAccess(facts.body, place)) {
      output << rawMemoryPlaceSpelling(*access);
      emitFieldProjectionChain(access->pointeeType, place.projections, 1);
      return;
    }
    if (place.root == MirPlaceRootKind::Binding && !place.projections.empty() &&
        place.projections[0].kind == MirProjectionKind::Dereference) {
      const MirPlace *base = nullptr;
      for (const MirPlace &candidate : facts.body.places) {
        if (candidate.id != place.id &&
            candidate.root == MirPlaceRootKind::Binding &&
            candidate.binding == place.binding &&
            candidate.projections.empty()) {
          base = &candidate;
        }
      }
      if (base == nullptr) {
        throw std::logic_error("dereference projection lost its base carrier");
      }
      output << "(*__gti_mir_p_" << base->id << ')';
      if (base->type.kind != SemanticType::Reference ||
          base->type.arguments.size() != 1) {
        throw std::logic_error(
            "verified dereference projection lost its referent type");
      }
      emitFieldProjectionChain(base->type.arguments.front(), place.projections,
                               1);
      return;
    }
    if (place.root == MirPlaceRootKind::Binding && !place.projections.empty() &&
        std::all_of(place.projections.begin(), place.projections.end(),
                    [](const MirPlaceProjection &projection) {
                      return projection.kind == MirProjectionKind::Field;
                    })) {
      const MirPlace *base = nullptr;
      for (const MirPlace &candidate : facts.body.places) {
        if (candidate.id != place.id &&
            candidate.root == MirPlaceRootKind::Binding &&
            candidate.binding == place.binding &&
            candidate.projections.empty()) {
          base = &candidate;
          break;
        }
      }
      if (base == nullptr) {
        throw std::logic_error("field projection lost its binding root");
      }
      output << "__gti_mir_p_"
             << (slotPlace(facts.body, *base)
                     ? canonicalSlotPlaceId(facts.body, *base)
                     : base->id);
      if (slotPlace(facts.body, *base)) {
        output << ".get()";
      }
      emitFieldProjectionChain(base->type, place.projections, 0);
      return;
    }
    if (place.projections.empty()) {
      output << "__gti_mir_p_"
             << (slotPlace(facts.body, place)
                     ? canonicalSlotPlaceId(facts.body, place)
                     : place.id);
      return;
    }
    throw std::logic_error("place expression is outside the loan vocabulary");
  }

  void emitElementIndex(const MirPlaceProjection &projection) {
    if (projection.constantIndex) {
      output << "static_cast<std::size_t>(" << *projection.constantIndex << ")";
      return;
    }
    output << "static_cast<std::size_t>(__gti_mir_v_" << projection.index
           << ")";
  }

  void emitArrayParent(const ScalarBodyFacts &facts,
                       const ArrayElementAccess &access) {
    const MirPlace *array = facts.body.findPlace(access.array);
    output << "__gti_mir_p_" << access.array;
    if (array != nullptr && slotPlace(facts.body, *array)) {
      output << ".get()";
    }
    for (std::size_t index = 0; index + 1 < access.indices.size(); ++index) {
      output << '[';
      emitElementIndex(access.indices[index]);
      output << ']';
    }
  }

  void emitArrayElement(const ScalarBodyFacts &facts,
                        const ArrayElementAccess &access) {
    emitArrayParent(facts, access);
    output << '[';
    emitElementIndex(access.terminalIndex());
    output << ']';
  }

  void
  emitBindingArrayFieldParent(const ScalarBodyFacts &facts,
                              const BindingArrayFieldElementAccess &access) {
    const MirPlace *array = facts.body.findPlace(access.array);
    if (array == nullptr) {
      throw std::logic_error(
          "verified native array field access lost its sibling place");
    }
    emitPlaceExpression(facts, *array);
    for (std::size_t index = 0; index + 1 < access.indices.size(); ++index) {
      output << '[';
      emitElementIndex(access.indices[index]);
      output << ']';
    }
  }

  void
  emitBindingArrayFieldElement(const ScalarBodyFacts &facts,
                               const BindingArrayFieldElementAccess &access) {
    emitBindingArrayFieldParent(facts, access);
    output << '[';
    emitElementIndex(access.terminalIndex());
    output << ']';
  }

  void emitReceiverArrayParent(const ScalarBodyFacts &facts,
                               const ReceiverArrayElementAccess &access) {
    output << "(*this)." << fieldSpelling(facts, access.field);
  }

  void emitReceiverArrayElement(const ScalarBodyFacts &facts,
                                const ReceiverArrayElementAccess &access) {
    emitReceiverArrayParent(facts, access);
    output << '[';
    emitElementIndex(access.index);
    output << ']';
  }

  void emitMoveSource(const ScalarBodyFacts &facts,
                      const MirInstruction &instruction) {
    if (instruction.kind != MirInstructionKind::Move ||
        instruction.operands.size() != 1 ||
        instruction.operands.front().kind != MirOperandKind::Move ||
        instruction.operands.front().place == 0) {
      throw std::logic_error("verified MIR move lost its exact source place");
    }
    const MirPlace *source =
        facts.body.findPlace(instruction.operands.front().place);
    if (source == nullptr) {
      throw std::logic_error("verified MIR move source place is unavailable");
    }
    if (instruction.localFailureSites.empty()) {
      emitStoragePlaceValue(facts, *source);
      return;
    }
    if (const std::optional<ArrayElementAccess> access =
            arrayElementAccess(facts.body, *source)) {
      output << "::gti_internal::backend::array_at(";
      emitArrayParent(facts, *access);
      output << ", ";
      emitElementIndexValue(*access);
      output << ')';
      return;
    }
    if (const std::optional<BindingArrayFieldElementAccess> access =
            bindingArrayFieldElementAccess(facts.body, *source)) {
      output << "::gti_internal::backend::array_at(";
      emitBindingArrayFieldParent(facts, *access);
      output << ", ";
      emitElementIndexValue(access->terminalIndex());
      output << ')';
      return;
    }
    if (const std::optional<ReceiverArrayElementAccess> access =
            receiverArrayElementAccess(facts.body, *source)) {
      output << "::gti_internal::backend::array_at(";
      emitReceiverArrayParent(facts, *access);
      output << ", ";
      emitElementIndexValue(access->index);
      output << ')';
      return;
    }
    throw std::logic_error(
        "verified MIR checked move lost its array element place");
  }

  void emitSubscriptIndex(const ClassSubscriptAccess &access) {
    if (access.index != 0) {
      output << "__gti_mir_v_" << access.index;
      return;
    }
    output << *access.constantIndex;
  }

  void emitElementIndexValue(const ArrayElementAccess &access) {
    emitElementIndexValue(access.terminalIndex());
  }

  void emitElementIndexValue(const MirPlaceProjection &projection) {
    if (projection.constantIndex) {
      output << "UINT64_C(" << *projection.constantIndex << ")";
      return;
    }
    output << "__gti_mir_v_" << projection.index;
  }

  [[nodiscard]] const MirFunctionInstance *
  transformedCallee(const MirInstruction &instruction) const {
    return transformedFailureCallee(program, representations, instruction);
  }

  [[nodiscard]] const MirConstructorInstance *
  transformedConstructor(const MirInstruction &instruction) const {
    if (instruction.kind != MirInstructionKind::Construct ||
        !instruction.constructorTarget ||
        instruction.constructorKind != ConstructorKind::Ordinary ||
        !instruction.localFailureSites.empty() ||
        instruction.definedFailure.propagation !=
            FailurePropagationKind::Constructor) {
      return nullptr;
    }
    const MirConstructorInstance *target =
        program.findConstructorInstance(*instruction.constructorTarget);
    const MirClassInstance *owner =
        target == nullptr ? nullptr : program.findClassInstance(target->owner);
    return target != nullptr && owner != nullptr &&
                   owner->type == instruction.info.type &&
                   failureConstructorBoundaryEligible(program, *target)
               ? target
               : nullptr;
  }

  [[nodiscard]] static const MirInstruction *
  findInstruction(const MirBody &body, MirInstructionId id) {
    const MirInstruction *found = nullptr;
    for (const MirBlock &block : body.blocks) {
      for (const MirInstruction &instruction : block.instructions) {
        if (instruction.id != id) {
          continue;
        }
        if (found != nullptr) {
          throw std::logic_error("verified MIR duplicated an instruction id");
        }
        found = &instruction;
      }
    }
    return found;
  }

  // The record's contents are wholly MIR-owned: the detector's exact site
  // plus the program's artifact identity (ADR 017).
  void emitFailureRecordWrite(const MirInstruction &instruction) {
    if (instruction.localFailureSites.size() != 1 ||
        instruction.definedFailure.localOrigins.size() != 1 ||
        program.failureMetadata().findSite(
            instruction.localFailureSites.front()) == nullptr) {
      throw std::logic_error(
          "failure-form MIR detector lost its canonical site");
    }
    writeIndent();
    output << "*__gti_mir_failure_record = ::gti_failure_record_v1{\n";
    ++indentation;
    writeIndent();
    output << "GTI_FAILURE_ABI_VERSION_V1, __gti_mir_failure_status_"
           << instruction.id << ".code, __gti_mir_failure_status_"
           << instruction.id << ".detail, UINT32_C("
           << instruction.localFailureSites.front() << "), UINT32_C(0), {";
    const auto &bytes = program.failureMetadata().artifactIdentity().bytes;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
      if (index != 0) {
        output << ", ";
      }
      output << static_cast<unsigned int>(bytes[index]);
    }
    output << "}};\n";
    --indentation;
  }

  const MirProgram &program;
  const CppMirBodyEmissionMap &representations;
  std::size_t indentation;
  bool failureForm = false;
  std::string_view currentFamilyLabel;
  std::ostringstream output;
};

} // namespace

std::optional<CppMirTypeRepresentationKind>
cppMirExpectedTypeRepresentation(const SemanticType &type) {
  return expectedTypeRepresentation(type);
}

std::string cppMirBinaryFloatLiteralSpelling(BinaryFloat value) {
  if (!validBinaryFloat(value)) {
    throw std::logic_error(
        "floating literal contains bits outside its declared format");
  }
  std::ostringstream text;
  const bool binary64 = value.format == BinaryFloatFormat::Binary64;
  text << (binary64 ? "std::bit_cast<double>(std::uint64_t{0x"
                    : "std::bit_cast<float>(std::uint32_t{0x")
       << std::hex << std::setw(binary64 ? 16 : 8) << std::setfill('0')
       << value.bits << (binary64 ? "ULL})" : "U})");
  return text.str();
}

std::string cppMirStringViewLiteralSpelling(std::string_view value) {
  std::string result = "std::string_view{\"";
  for (const char character : value) {
    switch (character) {
    case '\\':
      result += "\\\\";
      break;
    case '"':
      result += "\\\"";
      break;
    case '\n':
      result += "\\n";
      break;
    case '\r':
      result += "\\r";
      break;
    case '\t':
      result += "\\t";
      break;
    case '\0':
      result += "\\000";
      break;
    default: {
      const auto byte = static_cast<unsigned char>(character);
      if (byte < 32 || byte >= 127) {
        result += '\\';
        result += static_cast<char>('0' + ((byte >> 6U) & 0x07U));
        result += static_cast<char>('0' + ((byte >> 3U) & 0x07U));
        result += static_cast<char>('0' + (byte & 0x07U));
      } else {
        result += character;
      }
      break;
    }
    }
  }
  result += "\", ";
  result += std::to_string(value.size());
  result += '}';
  return result;
}

std::string_view cppMirCheckedOperationHelperSpelling(MirOperation operation) {
  switch (operation) {
  case MirOperation::Add:
    return "::gti_internal::backend::mir_checked_add_v1";
  case MirOperation::Subtract:
    return "::gti_internal::backend::mir_checked_subtract_v1";
  case MirOperation::Multiply:
    return "::gti_internal::backend::mir_checked_multiply_v1";
  case MirOperation::Divide:
    return "::gti_internal::backend::mir_checked_divide_v1";
  case MirOperation::Remainder:
    return "::gti_internal::backend::mir_checked_remainder_v1";
  case MirOperation::ShiftLeft:
    return "::gti_internal::backend::mir_checked_shift_left_v1";
  case MirOperation::ShiftRight:
    return "::gti_internal::backend::mir_checked_shift_right_v1";
  case MirOperation::Negate:
    return "::gti_internal::backend::mir_checked_negate_v1";
  case MirOperation::Convert:
    return "::gti_internal::backend::mir_checked_convert_v1";
  default:
    return {};
  }
}

// A checked compound assignment detects through the status-returning
// compound helper family, which composes exactly the terminal
// add_assign contract: the base operation checks in the C++ common type
// and the narrowing conversion checks the commit.
[[nodiscard]] std::string_view
cppMirCompoundCheckedHelperSpelling(MirOperation operation) {
  switch (operation) {
  case MirOperation::AddAssign:
    return "::gti_internal::backend::mir_checked_compound_add_v1";
  case MirOperation::SubtractAssign:
    return "::gti_internal::backend::mir_checked_compound_subtract_v1";
  case MirOperation::MultiplyAssign:
    return "::gti_internal::backend::mir_checked_compound_multiply_v1";
  case MirOperation::DivideAssign:
    return "::gti_internal::backend::mir_checked_compound_divide_v1";
  case MirOperation::RemainderAssign:
    return "::gti_internal::backend::mir_checked_compound_remainder_v1";
  case MirOperation::ShiftLeftAssign:
    return "::gti_internal::backend::mir_checked_compound_shift_left_v1";
  case MirOperation::ShiftRightAssign:
    return "::gti_internal::backend::mir_checked_compound_shift_right_v1";
  default:
    return {};
  }
}

std::string_view
cppIntegerArithmeticIntrinsicSpelling(IntrinsicKind intrinsic) {
  switch (intrinsic) {
  case IntrinsicKind::IntegerWrappingAdd:
    return "::gti_internal::backend::wrapping_add";
  case IntrinsicKind::IntegerWrappingSubtract:
    return "::gti_internal::backend::wrapping_sub";
  case IntrinsicKind::IntegerWrappingMultiply:
    return "::gti_internal::backend::wrapping_mul";
  case IntrinsicKind::IntegerSaturatingAdd:
    return "::gti_internal::backend::saturating_add";
  case IntrinsicKind::IntegerSaturatingSubtract:
    return "::gti_internal::backend::saturating_sub";
  case IntrinsicKind::IntegerSaturatingMultiply:
    return "::gti_internal::backend::saturating_mul";
  case IntrinsicKind::IntegerCheckedAdd:
    return "::gti_internal::backend::checked_add";
  case IntrinsicKind::IntegerCheckedSubtract:
    return "::gti_internal::backend::checked_sub";
  case IntrinsicKind::IntegerCheckedMultiply:
    return "::gti_internal::backend::checked_mul";
  default:
    return {};
  }
}

bool CppMirBodyEmitter::supportsBodyText(MirBodyAddress address) const {
  return supportsBodyTextImpl(address, false);
}

bool CppMirBodyEmitter::supportsFailureBodyText(MirBodyAddress address) const {
  return supportsBodyTextImpl(address, true);
}

bool cppMirConstructorStatusCannotFail(const MirProgram &program,
                                       HirConstructorInstanceId constructorId) {
  std::unordered_set<HirConstructorInstanceId> visiting;
  std::unordered_set<HirConstructorInstanceId> proven;
  const auto prove = [&](const auto &self,
                         HirConstructorInstanceId id) -> bool {
    if (proven.contains(id)) {
      return true;
    }
    if (!visiting.insert(id).second) {
      return false;
    }
    const MirConstructorInstance *constructor =
        program.findConstructorInstance(id);
    bool valid = constructor != nullptr &&
                 constructor->definitionKind == MirDefinitionKind::Source &&
                 failureStatusCannotFail(constructor->body);
    if (valid) {
      for (const MirConstructorInitializer &initializer :
           constructor->initializers) {
        if (initializer.constructorTarget) {
          const MirConstructorInstance *target =
              program.findConstructorInstance(*initializer.constructorTarget);
          const MirClassInstance *targetOwner =
              target == nullptr ? nullptr
                                : program.findClassInstance(target->owner);
          if (target == nullptr || targetOwner == nullptr ||
              targetOwner->type != initializer.targetType ||
              (initializer.base && target->owner != *initializer.base) ||
              !self(self, target->id)) {
            valid = false;
            break;
          }
        } else if (initializer.kind == ConstructorInitializerTargetKind::Base &&
                   !initializer.generatedDefault) {
          // A selected, non-default base constructor must retain its target;
          // otherwise its ability to report failure is unknowable here.
          valid = false;
          break;
        }
      }
    }
    visiting.erase(id);
    if (valid) {
      proven.insert(id);
    }
    return valid;
  };
  return prove(prove, constructorId);
}

std::optional<std::vector<CppMirStoredReferenceBinding>>
cppMirStoredReferenceBindings(const MirConstructorInstance &constructor) {
  std::vector<CppMirStoredReferenceBinding> bindings;
  std::vector<bool> loanUsed(constructor.body.loans.size(), false);
  for (std::size_t index = 0; index < constructor.initializers.size();
       ++index) {
    const MirConstructorInitializer &initializer =
        constructor.initializers[index];
    if (!initializer.storesReference) {
      continue;
    }
    if (initializer.kind != ConstructorInitializerTargetKind::Field ||
        initializer.field == 0) {
      return std::nullopt;
    }
    const MirLoan *stored = nullptr;
    std::size_t storedIndex = 0;
    for (std::size_t loanIndex = 0; loanIndex < constructor.body.loans.size();
         ++loanIndex) {
      const MirLoan &loan = constructor.body.loans[loanIndex];
      if (loan.kind != MirLoanKind::Stored ||
          loan.storedField != initializer.field) {
        continue;
      }
      if (stored != nullptr) {
        return std::nullopt;
      }
      stored = &loan;
      storedIndex = loanIndex;
    }
    if (stored == nullptr || loanUsed[storedIndex]) {
      return std::nullopt;
    }
    loanUsed[storedIndex] = true;
    // The loan source must be the dereference carrier of one reference
    // parameter: the initializer list then binds the field straight to the
    // C++ reference parameter.
    const MirPlace *source = constructor.body.findPlace(stored->source);
    if (source == nullptr || source->root != MirPlaceRootKind::Binding ||
        source->projections.size() != 1 ||
        source->projections.front().kind != MirProjectionKind::Dereference) {
      return std::nullopt;
    }
    const auto parameter =
        std::find(constructor.parameterBindings.begin(),
                  constructor.parameterBindings.end(), source->binding);
    if (parameter == constructor.parameterBindings.end()) {
      return std::nullopt;
    }
    bindings.push_back(
        {.initializer = index,
         .field = initializer.field,
         .parameter = static_cast<std::size_t>(
             parameter - constructor.parameterBindings.begin())});
  }
  // Every Stored loan must be claimed by exactly one initializer; a stray
  // stored loan would silently drop its binding.
  for (std::size_t loanIndex = 0; loanIndex < constructor.body.loans.size();
       ++loanIndex) {
    if (constructor.body.loans[loanIndex].kind == MirLoanKind::Stored &&
        !loanUsed[loanIndex]) {
      return std::nullopt;
    }
  }
  return bindings;
}

std::optional<std::vector<CppMirBaseParameterInitializerBinding>>
cppMirBaseParameterInitializerBindings(
    const MirProgram &program, const MirConstructorInstance &constructor) {
  const auto scalarArgumentType = [](const SemanticType &type) {
    switch (type.kind) {
    case SemanticType::Int8:
    case SemanticType::Int16:
    case SemanticType::Int32:
    case SemanticType::Int64:
    case SemanticType::UInt8:
    case SemanticType::UInt16:
    case SemanticType::UInt32:
    case SemanticType::UInt64:
    case SemanticType::Float:
    case SemanticType::Double:
    case SemanticType::Bool:
    case SemanticType::Char:
    case SemanticType::Enum:
    case SemanticType::NullPtr:
      return true;
    default:
      return false;
    }
  };

  std::vector<CppMirBaseParameterInitializerBinding> bindings;
  for (std::size_t initializerIndex = 0;
       initializerIndex < constructor.initializers.size(); ++initializerIndex) {
    const MirConstructorInitializer &initializer =
        constructor.initializers[initializerIndex];
    if (initializer.kind != ConstructorInitializerTargetKind::Base) {
      continue;
    }
    if (initializer.generatedDefault) {
      if (initializer.constructorTarget || !initializer.arguments.empty() ||
          !initializer.base || initializer.storesReference ||
          initializer.ownedParameter) {
        return std::nullopt;
      }
      continue;
    }
    if (!initializer.base || !initializer.constructorTarget ||
        initializer.storesReference || initializer.ownedParameter) {
      return std::nullopt;
    }
    const MirClassInstance *base = program.findClassInstance(*initializer.base);
    const MirConstructorInstance *target =
        program.findConstructorInstance(*initializer.constructorTarget);
    if (base == nullptr || target == nullptr || target->owner != base->id ||
        base->type != initializer.targetType ||
        target->parameterTypes.size() != initializer.arguments.size()) {
      return std::nullopt;
    }

    CppMirBaseParameterInitializerBinding binding{
        .initializer = initializerIndex,
        .baseType = initializer.targetType,
        .base = base->id,
        .constructor = target->id};
    binding.parameters.reserve(initializer.arguments.size());
    for (std::size_t argumentIndex = 0;
         argumentIndex < initializer.arguments.size(); ++argumentIndex) {
      const HirValueId argument = initializer.arguments[argumentIndex];
      const MirValue *value = nullptr;
      const MirInstruction *load = nullptr;
      for (const MirValue &candidate : constructor.body.values) {
        if (candidate.sourceValue != argument) {
          continue;
        }
        const MirInstruction *definition =
            findInstruction(constructor.body, candidate.definition);
        if (definition == nullptr ||
            definition->kind != MirInstructionKind::Load ||
            !definition->result || *definition->result != candidate.id ||
            definition->hirValue != argument || definition->destination ||
            definition->receiver || definition->operands.size() != 1 ||
            definition->operands.front().kind != MirOperandKind::Copy ||
            definition->operands.front().place == 0 ||
            !definition->localFailureSites.empty() ||
            !definition->definedFailure.empty() ||
            !definition->lifecycle.empty() ||
            definition->info.type != candidate.info.type ||
            !scalarArgumentType(candidate.info.type) ||
            !onlyRootRecordUses(constructor.body, candidate.id)) {
          continue;
        }
        if (value != nullptr) {
          return std::nullopt;
        }
        value = &candidate;
        load = definition;
      }
      const MirPlace *source =
          load == nullptr
              ? nullptr
              : constructor.body.findPlace(load->operands.front().place);
      if (value == nullptr || load == nullptr || source == nullptr ||
          source->root != MirPlaceRootKind::Binding ||
          !source->projections.empty() || !source->initiallyAvailable ||
          source->type != value->info.type ||
          load->operands.front().type != source->type ||
          target->parameterTypes[argumentIndex] != source->type) {
        return std::nullopt;
      }
      const auto parameter =
          std::find(constructor.parameterBindings.begin(),
                    constructor.parameterBindings.end(), source->binding);
      if (parameter == constructor.parameterBindings.end()) {
        return std::nullopt;
      }
      const std::size_t parameterIndex = static_cast<std::size_t>(
          parameter - constructor.parameterBindings.begin());
      if (parameterIndex >= constructor.parameterTypes.size() ||
          constructor.parameterTypes[parameterIndex] != source->type) {
        return std::nullopt;
      }
      binding.parameters.push_back(parameterIndex);
    }

    const MirFullExpression *fullExpression = nullptr;
    for (const MirFullExpression &candidate :
         constructor.body.fullExpressions) {
      if (candidate.constructorInitializer != initializerIndex + 1) {
        continue;
      }
      if (fullExpression != nullptr ||
          candidate.roots != initializer.arguments) {
        return std::nullopt;
      }
      fullExpression = &candidate;
    }
    if (initializer.arguments.empty()) {
      // An explicit `Base()` initializer has no expression roots, so HIR and
      // MIR deliberately publish no full-expression row. Prove that the body
      // likewise contains no hidden initializer work before spelling the
      // exact zero-argument native base call.
      if (fullExpression != nullptr ||
          std::any_of(
              constructor.body.blocks.begin(), constructor.body.blocks.end(),
              [&](const MirBlock &block) {
                return std::any_of(
                    block.instructions.begin(), block.instructions.end(),
                    [&](const MirInstruction &instruction) {
                      return instruction.constructorInitializer ==
                             initializerIndex + 1;
                    });
              })) {
        return std::nullopt;
      }
      bindings.push_back(std::move(binding));
      continue;
    }
    if (fullExpression == nullptr) {
      return std::nullopt;
    }
    const std::size_t ends = std::accumulate(
        constructor.body.blocks.begin(), constructor.body.blocks.end(),
        std::size_t{0}, [&](std::size_t count, const MirBlock &block) {
          return count +
                 static_cast<std::size_t>(std::count_if(
                     block.instructions.begin(), block.instructions.end(),
                     [&](const MirInstruction &instruction) {
                       return instruction.fullExpressionEnd ==
                              fullExpression->id;
                     }));
        });
    if (ends != 1) {
      return std::nullopt;
    }
    bindings.push_back(std::move(binding));
  }
  return bindings;
}

std::optional<std::vector<CppMirCopyParameterFieldBinding>>
cppMirCopyParameterFieldBindings(const MirProgram &program,
                                 const MirConstructorInstance &constructor) {
  const auto scalarParameterType = [](const SemanticType &type) {
    switch (type.kind) {
    case SemanticType::Int8:
    case SemanticType::Int16:
    case SemanticType::Int32:
    case SemanticType::Int64:
    case SemanticType::UInt8:
    case SemanticType::UInt16:
    case SemanticType::UInt32:
    case SemanticType::UInt64:
    case SemanticType::Float:
    case SemanticType::Double:
    case SemanticType::Bool:
    case SemanticType::Char:
    case SemanticType::Enum:
    case SemanticType::NullPtr:
      return true;
    default:
      return false;
    }
  };

  const MirClassInstance *owner = program.findClassInstance(constructor.owner);
  if (owner == nullptr) {
    return std::nullopt;
  }

  std::vector<CppMirCopyParameterFieldBinding> bindings;
  for (std::size_t initializerIndex = 0;
       initializerIndex < constructor.initializers.size(); ++initializerIndex) {
    const MirConstructorInitializer &initializer =
        constructor.initializers[initializerIndex];
    if (initializer.kind != ConstructorInitializerTargetKind::Field ||
        initializer.storesReference || initializer.ownedParameter ||
        initializer.generatedDefault || initializer.field == 0 ||
        initializer.arguments.size() != 1 ||
        initializer.arguments.front() == 0 ||
        !scalarParameterType(initializer.targetType)) {
      continue;
    }

    const auto field =
        std::find_if(owner->declaredFields.begin(), owner->declaredFields.end(),
                     [&](const MirClassFieldInfo &candidate) {
                       return candidate.symbol == initializer.field &&
                              candidate.type == initializer.targetType;
                     });
    if (field == owner->declaredFields.end() ||
        std::count_if(owner->declaredFields.begin(),
                      owner->declaredFields.end(),
                      [&](const MirClassFieldInfo &candidate) {
                        return candidate.symbol == initializer.field;
                      }) != 1) {
      continue;
    }

    const MirValue *loadedValue = nullptr;
    const MirInstruction *load = nullptr;
    std::size_t sourceValueCount = 0;
    for (const MirValue &value : constructor.body.values) {
      if (value.sourceValue != initializer.arguments.front()) {
        continue;
      }
      ++sourceValueCount;
      const MirInstruction *definition =
          findInstruction(constructor.body, value.definition);
      if (definition == nullptr ||
          definition->kind != MirInstructionKind::Load || !definition->result ||
          *definition->result != value.id ||
          definition->hirValue != initializer.arguments.front() ||
          definition->constructorInitializer != 0 || definition->destination ||
          definition->receiver || definition->operands.size() != 1 ||
          definition->operands.front().kind != MirOperandKind::Copy ||
          definition->operands.front().type != initializer.targetType ||
          !definition->localFailureSites.empty() ||
          !definition->definedFailure.empty() ||
          !definition->lifecycle.empty() ||
          definition->fullExpressionEnd != 0 ||
          definition->cleanupBoundaryEnd != 0 ||
          definition->info.type != initializer.targetType ||
          value.info.type != initializer.targetType || !definition->ownership) {
        continue;
      }
      load = definition;
      loadedValue = &value;
    }
    if (sourceValueCount != 1 || load == nullptr || loadedValue == nullptr) {
      continue;
    }

    const MirPlace *source =
        constructor.body.findPlace(load->operands.front().place);
    if (source == nullptr || source->root != MirPlaceRootKind::Binding ||
        !source->projections.empty() ||
        source->type != initializer.targetType || !source->initiallyAvailable ||
        !source->key || load->ownership->kind != OwnershipEventKind::Read ||
        load->ownership->place != *source->key ||
        load->ownership->before != OwnershipStateSet::Available ||
        load->ownership->after != OwnershipStateSet::Available ||
        !load->ownership->reachable) {
      continue;
    }

    std::size_t parameter = 0;
    std::size_t parameterMatches = 0;
    for (std::size_t index = 0; index < constructor.parameterBindings.size();
         ++index) {
      if (index < constructor.parameterTypes.size() &&
          constructor.parameterTypes[index] == initializer.targetType &&
          constructor.parameterBindings[index] == source->binding) {
        parameter = index;
        ++parameterMatches;
      }
    }
    if (parameterMatches != 1) {
      continue;
    }

    const MirInstruction *initialize = nullptr;
    std::size_t initializeMatches = 0;
    bool malformedInitialize = false;
    for (const MirBlock &block : constructor.body.blocks) {
      for (const MirInstruction &instruction : block.instructions) {
        if (instruction.constructorInitializer != initializerIndex + 1 ||
            instruction.kind != MirInstructionKind::Initialize) {
          continue;
        }
        const MirPlace *destination =
            instruction.destination
                ? constructor.body.findPlace(*instruction.destination)
                : nullptr;
        if (instruction.hirValue != initializer.arguments.front() ||
            destination == nullptr || instruction.receiver ||
            instruction.operands.size() != 1 ||
            instruction.operands.front().kind != MirOperandKind::Value ||
            instruction.operands.front().value != loadedValue->id ||
            instruction.operands.front().type != initializer.targetType ||
            !instruction.localFailureSites.empty() ||
            !instruction.definedFailure.empty() ||
            !instruction.lifecycle.empty() ||
            instruction.fullExpressionEnd != 0 ||
            instruction.cleanupBoundaryEnd != 0 ||
            instruction.info.type != initializer.targetType ||
            destination->root != MirPlaceRootKind::This ||
            destination->projections.size() != 1 ||
            destination->projections.front().kind != MirProjectionKind::Field ||
            destination->projections.front().field != initializer.field ||
            destination->type != initializer.targetType) {
          malformedInitialize = true;
          continue;
        }
        initialize = &instruction;
        ++initializeMatches;
      }
    }
    const std::vector<MirValueUse> uses =
        constructor.body.usesOf(loadedValue->id);
    if (malformedInitialize || initializeMatches != 1 ||
        initialize == nullptr || uses.size() != 1 ||
        uses.front().kind != MirValueUseKind::InstructionOperand ||
        uses.front().instruction != initialize->id ||
        uses.front().operandIndex != 0) {
      continue;
    }

    const MirFullExpression *fullExpression = nullptr;
    std::size_t fullExpressionMatches = 0;
    bool malformedFullExpression = false;
    for (const MirFullExpression &candidate :
         constructor.body.fullExpressions) {
      if (candidate.constructorInitializer != initializerIndex + 1) {
        continue;
      }
      if (candidate.roots != initializer.arguments) {
        malformedFullExpression = true;
        continue;
      }
      fullExpression = &candidate;
      ++fullExpressionMatches;
    }
    if (malformedFullExpression || fullExpressionMatches != 1 ||
        fullExpression == nullptr) {
      continue;
    }
    const MirInstruction *boundary = nullptr;
    std::size_t boundaryMatches = 0;
    bool malformedBoundary = false;
    for (const MirBlock &block : constructor.body.blocks) {
      for (const MirInstruction &instruction : block.instructions) {
        if (instruction.fullExpressionEnd != fullExpression->id) {
          continue;
        }
        if (instruction.kind != MirInstructionKind::Lifecycle ||
            instruction.result || instruction.destination ||
            instruction.receiver || !instruction.operands.empty() ||
            !instruction.localFailureSites.empty() ||
            !instruction.definedFailure.empty() ||
            !instruction.lifecycle.empty() ||
            instruction.cleanupBoundaryEnd != 0) {
          malformedBoundary = true;
          continue;
        }
        boundary = &instruction;
        ++boundaryMatches;
      }
    }
    if (malformedBoundary || boundaryMatches != 1 || boundary == nullptr ||
        std::any_of(bindings.begin(), bindings.end(),
                    [&](const CppMirCopyParameterFieldBinding &binding) {
                      return binding.field == initializer.field ||
                             binding.parameter == parameter ||
                             binding.sourcePlace == source->id ||
                             binding.loadedValue == loadedValue->id ||
                             binding.loadInstruction == load->id ||
                             binding.initializeInstruction == initialize->id ||
                             binding.boundaryInstruction == boundary->id;
                    })) {
      continue;
    }
    bindings.push_back({.initializer = initializerIndex,
                        .field = initializer.field,
                        .parameter = parameter,
                        .sourcePlace = source->id,
                        .loadedValue = loadedValue->id,
                        .loadInstruction = load->id,
                        .initializeInstruction = initialize->id,
                        .boundaryInstruction = boundary->id});
  }
  return bindings;
}

std::optional<std::vector<CppMirOwnedParameterFieldBinding>>
cppMirOwnedParameterFieldBindings(const MirProgram &program,
                                  const MirConstructorInstance &constructor) {
  const MirClassInstance *owner = program.findClassInstance(constructor.owner);
  if (owner == nullptr) {
    return std::nullopt;
  }

  std::vector<CppMirOwnedParameterFieldBinding> bindings;
  for (std::size_t initializerIndex = 0;
       initializerIndex < constructor.initializers.size(); ++initializerIndex) {
    const MirConstructorInitializer &initializer =
        constructor.initializers[initializerIndex];
    if (!initializer.ownedParameter) {
      continue;
    }
    const std::size_t parameter = *initializer.ownedParameter;
    if (initializer.kind != ConstructorInitializerTargetKind::Field ||
        initializer.field == 0 || initializer.storesReference ||
        initializer.generatedDefault || initializer.arguments.size() != 1 ||
        initializer.arguments.front() == 0 ||
        parameter >= constructor.parameterBindings.size() ||
        parameter >= constructor.parameterTypes.size() ||
        initializer.targetType != constructor.parameterTypes[parameter]) {
      return std::nullopt;
    }

    const auto field =
        std::find_if(owner->declaredFields.begin(), owner->declaredFields.end(),
                     [&](const MirClassFieldInfo &candidate) {
                       return candidate.symbol == initializer.field &&
                              candidate.type == initializer.targetType;
                     });
    if (field == owner->declaredFields.end() ||
        std::count_if(owner->declaredFields.begin(),
                      owner->declaredFields.end(),
                      [&](const MirClassFieldInfo &candidate) {
                        return candidate.symbol == initializer.field;
                      }) != 1) {
      return std::nullopt;
    }

    const MirPlace *source = nullptr;
    for (const MirPlace &place : constructor.body.places) {
      if (place.root != MirPlaceRootKind::Binding ||
          !place.projections.empty() ||
          place.binding != constructor.parameterBindings[parameter] ||
          place.type != initializer.targetType || !place.initiallyAvailable) {
        continue;
      }
      if (source != nullptr) {
        return std::nullopt;
      }
      source = &place;
    }
    if (source == nullptr || !source->key) {
      return std::nullopt;
    }

    const MirInstruction *move = nullptr;
    const MirValue *movedValue = nullptr;
    for (const MirValue &value : constructor.body.values) {
      const MirInstruction *definition =
          findInstruction(constructor.body, value.definition);
      if (value.sourceValue != initializer.arguments.front() ||
          definition == nullptr ||
          definition->kind != MirInstructionKind::Move || !definition->result ||
          *definition->result != value.id ||
          definition->hirValue != initializer.arguments.front() ||
          definition->operands.size() != 1 ||
          definition->operands.front().kind != MirOperandKind::Move ||
          definition->operands.front().place != source->id ||
          definition->operands.front().type != initializer.targetType ||
          definition->destination || definition->receiver ||
          definition->intrinsic != IntrinsicKind::Move ||
          !definition->localFailureSites.empty() ||
          !definition->definedFailure.empty() ||
          definition->info.type != initializer.targetType ||
          value.info.type != initializer.targetType || !definition->ownership ||
          definition->ownership->kind != OwnershipEventKind::Move ||
          definition->ownership->place != *source->key ||
          definition->ownership->before != OwnershipStateSet::Available ||
          definition->ownership->after != OwnershipStateSet::Moved ||
          !definition->ownership->reachable) {
        continue;
      }
      if (move != nullptr) {
        return std::nullopt;
      }
      move = definition;
      movedValue = &value;
    }
    if (move == nullptr || movedValue == nullptr) {
      return std::nullopt;
    }

    const MirDropObligation *parameterDrop = nullptr;
    for (const MirDropObligation &drop : constructor.body.dropObligations) {
      if (drop.kind != MirDropObligationKind::Binding ||
          drop.place != source->id || drop.binding != source->binding ||
          drop.dropType.type != source->type || !drop.initiallyActive) {
        continue;
      }
      if (parameterDrop != nullptr) {
        return std::nullopt;
      }
      parameterDrop = &drop;
    }
    if (parameterDrop == nullptr) {
      return std::nullopt;
    }

    const MirInstruction *sourceDrop = nullptr;
    for (const MirBlock &block : constructor.body.blocks) {
      for (const MirInstruction &instruction : block.instructions) {
        if (instruction.kind != MirInstructionKind::Drop ||
            instruction.destination != source->id ||
            instruction.lifecycle.size() != 1) {
          continue;
        }
        const MirLifecycleEvent &event = instruction.lifecycle.front();
        if (event.kind != MirLifecycleEventKind::Drop ||
            event.source != parameterDrop->id || event.target != 0 ||
            event.conditional || event.failureCleanup ||
            instruction.info.type != source->type) {
          continue;
        }
        if (sourceDrop != nullptr) {
          return std::nullopt;
        }
        sourceDrop = &instruction;
      }
    }
    if (sourceDrop == nullptr) {
      return std::nullopt;
    }
    const bool scheduledDrop = std::any_of(
        constructor.body.cleanupBoundaries.begin(),
        constructor.body.cleanupBoundaries.end(),
        [&](const MirCleanupBoundary &boundary) {
          return boundary.kind == MirCleanupBoundaryKind::Normal &&
                 std::count(boundary.obligations.begin(),
                            boundary.obligations.end(), parameterDrop->id) == 1;
        });
    if (!scheduledDrop) {
      return std::nullopt;
    }

    const MirFullExpression *fullExpression = nullptr;
    for (const MirFullExpression &candidate :
         constructor.body.fullExpressions) {
      if (candidate.constructorInitializer != initializerIndex + 1) {
        continue;
      }
      if (fullExpression != nullptr ||
          candidate.roots != initializer.arguments) {
        return std::nullopt;
      }
      fullExpression = &candidate;
    }
    if (fullExpression == nullptr) {
      return std::nullopt;
    }
    std::size_t fullExpressionEnds = 0;
    for (const MirBlock &block : constructor.body.blocks) {
      for (const MirInstruction &instruction : block.instructions) {
        if (instruction.fullExpressionEnd == fullExpression->id) {
          ++fullExpressionEnds;
        }
      }
    }
    if (fullExpressionEnds != 1) {
      return std::nullopt;
    }

    MirInstructionId initializeInstruction = 0;
    if (const MirInstruction *initialize =
            constructorFieldMoveInitialize(constructor.body, movedValue->id)) {
      if (ownedParameterFieldInitializer(program, constructor.body,
                                         constructor.id,
                                         movedValue->id) != &initializer ||
          initialize->constructorInitializer != initializerIndex + 1) {
        return std::nullopt;
      }
      initializeInstruction = initialize->id;
    } else {
      // Passive lexical fields have no distinct MIR field carrier: the
      // initializer metadata is the publication authority and the Move's
      // value is deliberately unconsumed. Active-cleanup fields must retain
      // the explicit Initialize/rollback schedule above.
      if (field->requiresActiveCleanup || !move->lifecycle.empty() ||
          !onlyRootRecordUses(constructor.body, movedValue->id) ||
          !constructor.body.failureRecords.empty()) {
        return std::nullopt;
      }
    }

    const bool duplicate =
        std::any_of(bindings.begin(), bindings.end(),
                    [&](const CppMirOwnedParameterFieldBinding &binding) {
                      return binding.field == initializer.field ||
                             binding.parameter == parameter ||
                             binding.sourcePlace == source->id ||
                             binding.movedValue == movedValue->id ||
                             binding.moveInstruction == move->id ||
                             binding.dropInstruction == sourceDrop->id ||
                             binding.parameterDrop == parameterDrop->id;
                    });
    if (duplicate) {
      return std::nullopt;
    }
    bindings.push_back({.initializer = initializerIndex,
                        .field = initializer.field,
                        .parameter = parameter,
                        .sourcePlace = source->id,
                        .movedValue = movedValue->id,
                        .moveInstruction = move->id,
                        .initializeInstruction = initializeInstruction,
                        .dropInstruction = sourceDrop->id,
                        .parameterDrop = parameterDrop->id});
  }
  return bindings;
}

namespace {

[[nodiscard]] std::optional<std::size_t>
hostedModuleOperationCount(const MirHostedStartupPlan &plan,
                           std::size_t baseSize, std::size_t insertion) {
  using Kind = MirHostedStartupOperationKind;
  using Behavior = MirHostedStartupFailureBehavior;
  if (insertion > baseSize) {
    return std::nullopt;
  }
  if (plan.operations.size() == baseSize) {
    return 0;
  }
  if (plan.operations.size() == baseSize + 1) {
    const MirHostedStartupOperation &call = plan.operations[insertion];
    if (call.kind == Kind::CallProgramInitialization &&
        call.failureBehavior == Behavior::None && !call.terminator &&
        call.failureRecord == 0) {
      return 1;
    }
    return std::nullopt;
  }
  if (plan.operations.size() == baseSize + 3) {
    const MirHostedStartupOperation &call = plan.operations[insertion];
    const MirHostedStartupOperation &route = plan.operations[insertion + 1];
    const MirHostedStartupOperation &contain = plan.operations[insertion + 2];
    if (call.kind == Kind::CallProgramInitialization &&
        call.failureBehavior == Behavior::Propagate && !call.terminator &&
        route.kind == Kind::RouteOperationFailure &&
        route.failureBehavior == Behavior::None && route.terminator &&
        route.failureRecord != 0 && contain.kind == Kind::ContainFailure &&
        contain.failureBehavior == Behavior::None && contain.terminator) {
      return 3;
    }
  }
  return std::nullopt;
}

} // namespace

bool cppMirHostedStartupNoArgumentsSchedule(const MirProgram &program) {
  const std::optional<MirHostedStartupPlan> &plan = program.hostedStartupPlan();
  if (!plan || plan->kind != ProgramEntryKind::NoArguments ||
      plan->entry == 0 ||
      plan->exitPolicy != MirHostedStartupExitPolicy::ImmediateExit70) {
    return false;
  }
  // Module initialization is either already represented by passive C++
  // definitions (the one-operation failure-free prefix) or called through
  // the runtime Module/0 body, including its propagated failure route.
  const std::optional<std::size_t> moduleOperations =
      hostedModuleOperationCount(*plan, 4, 0);
  if (!moduleOperations) {
    return false;
  }
  return plan->operations[*moduleOperations].kind ==
             MirHostedStartupOperationKind::CallEntry &&
         plan->operations[*moduleOperations + 1].kind ==
             MirHostedStartupOperationKind::RouteOperationFailure &&
         plan->operations[*moduleOperations + 2].kind ==
             MirHostedStartupOperationKind::ContainFailure &&
         plan->operations[*moduleOperations + 3].kind ==
             MirHostedStartupOperationKind::ReturnEntry;
}

// The failure-free no-argument variant: an entry that cannot raise emits
// the bare call/return adapter, so the schedule is exactly CallEntry with
// no failure behavior followed by ReturnEntry under the same exit policy.
bool cppMirHostedStartupFailureFreeSchedule(const MirProgram &program) {
  const std::optional<MirHostedStartupPlan> &plan = program.hostedStartupPlan();
  if (!plan || plan->kind != ProgramEntryKind::NoArguments ||
      plan->entry == 0 ||
      plan->exitPolicy != MirHostedStartupExitPolicy::ImmediateExit70) {
    return false;
  }
  const std::optional<std::size_t> moduleOperations =
      hostedModuleOperationCount(*plan, 2, 0);
  if (!moduleOperations) {
    return false;
  }
  return plan->operations[*moduleOperations].kind ==
             MirHostedStartupOperationKind::CallEntry &&
         plan->operations[*moduleOperations].failureBehavior ==
             MirHostedStartupFailureBehavior::None &&
         plan->operations[*moduleOperations + 1].kind ==
             MirHostedStartupOperationKind::ReturnEntry &&
         plan->operations[*moduleOperations + 1].failureBehavior ==
             MirHostedStartupFailureBehavior::None;
}

bool cppMirHostedStartupOwnedArgumentsSchedule(const MirProgram &program) {
  const std::optional<MirHostedStartupPlan> &plan = program.hostedStartupPlan();
  if (!plan || plan->kind != ProgramEntryKind::OwnedArguments ||
      plan->entry == 0 || plan->appendFunction == 0 ||
      plan->vectorConstructor == 0 || plan->stringConstructor == 0 ||
      plan->exitPolicy != MirHostedStartupExitPolicy::ImmediateExit70) {
    return false;
  }
  using Kind = MirHostedStartupOperationKind;
  using Behavior = MirHostedStartupFailureBehavior;
  struct ExpectedOperation {
    Kind kind;
    Behavior behavior;
    bool terminator;
    bool record;
    bool drop;
  };
  // The exact marshaling schedule the emitted argc/argv main performs:
  // detect-validated count and conversion, propagating vector
  // construction, then the per-argument loop — view read, string
  // construction and append under the drop/end failure-cleanup envelope —
  // and the entry call, each failure routed and terminally contained.
  static constexpr ExpectedOperation expected[] = {
      {Kind::ValidateArgumentCount, Behavior::Detect, false, false, false},
      {Kind::RouteOperationFailure, Behavior::None, true, true, false},
      {Kind::ContainFailure, Behavior::None, true, false, false},
      {Kind::ConvertArgumentCount, Behavior::Detect, false, false, false},
      {Kind::RouteOperationFailure, Behavior::None, true, true, false},
      {Kind::ContainFailure, Behavior::None, true, false, false},
      {Kind::ConstructArgumentVector, Behavior::Propagate, false, false, true},
      {Kind::RouteOperationFailure, Behavior::None, true, true, false},
      {Kind::ContainFailure, Behavior::None, true, false, false},
      {Kind::InitializeArgumentIndex, Behavior::None, false, false, false},
      {Kind::EnterArgumentLoop, Behavior::None, true, false, false},
      {Kind::LoadArgumentIndex, Behavior::None, false, false, false},
      {Kind::TestArgumentIndex, Behavior::None, false, false, false},
      {Kind::BranchArgumentLoop, Behavior::None, true, false, false},
      {Kind::ReadArgumentView, Behavior::None, false, false, false},
      {Kind::PrepareStringConstructorArgument, Behavior::None, false, false,
       false},
      {Kind::ConstructArgumentString, Behavior::Propagate, false, false, true},
      {Kind::RouteOperationFailure, Behavior::None, true, true, false},
      {Kind::DropFailureCleanup, Behavior::None, false, false, false},
      {Kind::EndFailureCleanup, Behavior::None, false, false, false},
      {Kind::ContainFailure, Behavior::None, true, false, false},
      {Kind::PrepareAppendReceiver, Behavior::None, false, false, false},
      {Kind::PrepareAppendArgumentMove, Behavior::None, false, false, true},
      {Kind::CallAppend, Behavior::Propagate, false, false, false},
      {Kind::RouteOperationFailure, Behavior::None, true, true, false},
      {Kind::DropFailureCleanup, Behavior::None, false, false, false},
      {Kind::EndFailureCleanup, Behavior::None, false, false, false},
      {Kind::ContainFailure, Behavior::None, true, false, false},
      {Kind::AdvanceArgumentIndex, Behavior::None, false, false, false},
      {Kind::ContinueArgumentLoop, Behavior::None, true, false, false},
      {Kind::PrepareEntryCount, Behavior::None, false, false, false},
      {Kind::PrepareEntryArgumentsMove, Behavior::None, false, false, true},
      {Kind::CallEntry, Behavior::Propagate, false, false, false},
      {Kind::RouteOperationFailure, Behavior::None, true, true, false},
      {Kind::ContainFailure, Behavior::None, true, false, false},
      {Kind::ReturnEntry, Behavior::None, true, false, false},
  };
  constexpr std::size_t moduleInsertion = 6;
  const std::optional<std::size_t> moduleOperations =
      hostedModuleOperationCount(*plan, std::size(expected), moduleInsertion);
  if (!moduleOperations) {
    return false;
  }
  std::vector<MirFailureRecordId> routedRecords;
  for (std::size_t index = 0; index < std::size(expected); ++index) {
    const std::size_t operationIndex =
        index + (index >= moduleInsertion ? *moduleOperations : 0);
    const MirHostedStartupOperation &operation =
        plan->operations[operationIndex];
    const ExpectedOperation &shape = expected[index];
    if (operation.kind != shape.kind ||
        operation.failureBehavior != shape.behavior ||
        operation.terminator != shape.terminator ||
        (operation.failureRecord != 0) != shape.record ||
        (operation.dropObligation != 0) != shape.drop) {
      return false;
    }
    if (shape.record) {
      if (std::find(routedRecords.begin(), routedRecords.end(),
                    operation.failureRecord) != routedRecords.end()) {
        return false;
      }
      routedRecords.push_back(operation.failureRecord);
    }
  }
  return true;
}

const MirFunctionInstance *
cppMirVirtualFailureContractRoot(const MirProgram &program,
                                 const MirFunctionInstance &function) {
  const auto contractRoot = [&](const MirFunctionInstance &candidate) {
    if (!candidate.owner || !candidate.virtualMethod ||
        candidate.overrideMethod || candidate.staticMember ||
        !candidate.mayRaiseDefinedFailure ||
        candidate.linkage != LanguageLinkage::Gti ||
        (candidate.pureVirtual
             ? candidate.definitionKind != MirDefinitionKind::Declaration
             : candidate.definitionKind != MirDefinitionKind::Source) ||
        !candidate.callableParameters.empty() ||
        std::find(candidate.virtualRoots.begin(), candidate.virtualRoots.end(),
                  candidate.declaration) == candidate.virtualRoots.end()) {
      return false;
    }
    const MirClassInstance *owner = program.findClassInstance(*candidate.owner);
    return owner != nullptr && owner->polymorphic &&
           (!candidate.pureVirtual || owner->abstract);
  };
  if (contractRoot(function)) {
    return &function;
  }
  if (!function.owner || !function.virtualMethod || function.pureVirtual ||
      !function.overrideMethod || function.staticMember ||
      function.linkage != LanguageLinkage::Gti ||
      function.definitionKind != MirDefinitionKind::Source ||
      function.virtualRoots.empty() || !function.callableParameters.empty()) {
    return nullptr;
  }

  const auto derivesFrom =
      [&](const auto &self, HirClassInstanceId derived, HirClassInstanceId base,
          std::unordered_set<HirClassInstanceId> &seen) -> bool {
    if (derived == base) {
      return true;
    }
    if (!seen.insert(derived).second) {
      return false;
    }
    const MirClassInstance *instance = program.findClassInstance(derived);
    if (instance == nullptr) {
      return false;
    }
    return std::any_of(instance->structuralBases.begin(),
                       instance->structuralBases.end(),
                       [&](const HirBaseInstance &candidate) {
                         return candidate.instance == base ||
                                self(self, candidate.instance, base, seen);
                       });
  };
  const auto sameSignature = [&](const MirFunctionInstance &candidate) {
    return candidate.returnType == function.returnType &&
           candidate.parameterTypes == function.parameterTypes &&
           candidate.receiverMutability == function.receiverMutability &&
           candidate.overloadedOperator == function.overloadedOperator &&
           candidate.returnBorrowOrigin == function.returnBorrowOrigin &&
           candidate.returnBorrowParameter == function.returnBorrowParameter &&
           candidate.returnBorrowAccess == function.returnBorrowAccess;
  };

  const MirFunctionInstance *found = nullptr;
  for (const MirFunctionInstance &candidate : program.functionInstances()) {
    if (!contractRoot(candidate) || !sameSignature(candidate) ||
        std::find(function.virtualRoots.begin(), function.virtualRoots.end(),
                  candidate.declaration) == function.virtualRoots.end()) {
      continue;
    }
    std::unordered_set<HirClassInstanceId> seen;
    if (!derivesFrom(derivesFrom, *function.owner, *candidate.owner, seen)) {
      continue;
    }
    if (found != nullptr) {
      return nullptr;
    }
    found = &candidate;
  }
  return found;
}

std::string cppMirFailureSiblingSpelling(std::string_view memberSpelling) {
  constexpr std::string_view keyword = "operator";
  const std::size_t at = memberSpelling.find(keyword);
  const std::size_t after =
      at == std::string_view::npos ? 0 : at + keyword.size();
  const bool bridge =
      at != std::string_view::npos &&
      (after >= memberSpelling.size() ||
       (!std::isalnum(static_cast<unsigned char>(memberSpelling[after])) &&
        memberSpelling[after] != '_'));
  if (!bridge) {
    std::size_t scope = std::string_view::npos;
    std::size_t depth = 0;
    for (std::size_t index = 0; index < memberSpelling.size(); ++index) {
      if (memberSpelling[index] == '<') {
        ++depth;
      } else if (memberSpelling[index] == '>' && depth != 0) {
        --depth;
      } else if (depth == 0 && memberSpelling[index] == ':' &&
                 index + 1 < memberSpelling.size() &&
                 memberSpelling[index + 1] == ':') {
        scope = index;
        ++index;
      }
    }
    const std::size_t templateArguments = memberSpelling.find(
        '<', scope == std::string_view::npos ? 0 : scope + 2);
    if (templateArguments == std::string_view::npos) {
      return std::string(memberSpelling) + "__gti_mir_failure";
    }
    return std::string(memberSpelling.substr(0, templateArguments)) +
           "__gti_mir_failure";
  }
  const std::string_view symbol = memberSpelling.substr(after);
  static constexpr std::pair<std::string_view, std::string_view> tokens[] = {
      {"==", "eq"},     {"!=", "ne"},    {"<=", "le"},   {">=", "ge"},
      {"<<", "shl"},    {">>", "shr"},   {"<", "lt"},    {">", "gt"},
      {"+", "plus"},    {"-", "minus"},  {"*", "star"},  {"/", "slash"},
      {"%", "percent"}, {"[]", "index"}, {"()", "call"}, {"!", "not"},
      {"~", "tilde"},   {"&", "amp"},    {"|", "pipe"},  {"^", "caret"},
      {"=", "assign"},
  };
  for (const auto &[spelling, token] : tokens) {
    if (symbol == spelling) {
      return std::string(memberSpelling.substr(0, at)) + "__gti_mir_op_" +
             std::string(token) + "__gti_mir_failure";
    }
  }
  return {};
}

std::string
cppMirFailureConstructorTagSpelling(HirConstructorInstanceId constructor) {
  return "::gti_internal::backend::mir_failure_constructor_tag_v1<" +
         std::to_string(constructor) + ">";
}

bool CppMirBodyEmitter::boundaryDeclarationBody(MirBodyAddress address) const {
  if (address.kind != MirBodyKind::Function) {
    return false;
  }
  const MirFunctionInstance *function =
      program_.findFunctionInstance(address.owner);
  if (function == nullptr ||
      function->definitionKind == MirFunctionInstance::DefinitionKind::Source) {
    return false;
  }
  const MirBody &body = function->body;
  if (body.blocks.size() != 1 || !body.loans.empty() ||
      !body.dropObligations.empty() || !body.cleanupBoundaries.empty() ||
      !body.failureRecords.empty()) {
    return false;
  }
  const MirBlock &block = body.blocks.front();
  return block.reachable && block.instructions.empty() &&
         (block.terminator.kind == MirTerminatorKind::Return ||
          block.terminator.kind == MirTerminatorKind::Unreachable);
}

bool CppMirBodyEmitter::supportsBodyTextImpl(MirBodyAddress address,
                                             bool failureForm) const {
  // The vocabulary is shared between function and destructor bodies; the
  // probe needs only the body, the owning class instance for field rows,
  // and the receiver mutability for the store direction. The failure form
  // (ADR 017) restricts to leaf function bodies under the transformed
  // private ABI and additionally admits checked detectors and failure
  // control flow.
  const MirBody *bodyPointer = nullptr;
  std::optional<HirClassInstanceId> owner;
  ReceiverMutability receiverMutability = ReceiverMutability::ReadOnly;
  const std::vector<HirBindingId> *parameterBindings = nullptr;
  // A deduced-callable template body (plain shape, Function kind, callable
  // parameters): its concrete callable types are spellable only under a
  // template emission's overlay rows, and every failure convention it can
  // reach is terminally contained, exactly like the compatibility path.
  bool callableTemplateBody = false;
  // Set when the Constructor case proved the stored-reference pairing, so
  // the generic loan rule can admit the paired Stored loans.
  bool storedReferenceBindings = false;
  switch (address.kind) {
  case MirBodyKind::Module: {
    if (address.owner != 0 ||
        failureForm != moduleMayRaiseDefinedFailure(program_)) {
      return false;
    }
    bodyPointer = &program_.module();
    break;
  }
  case MirBodyKind::Function: {
    const MirFunctionInstance *function =
        program_.findFunctionInstance(address.owner);
    if (function == nullptr) {
      {
        return false;
      }
    }
    // A plain body that may raise is not complete MIR execution: every
    // checked helper on that path contains failure in compatibility code and
    // makes MIR's failure successor dead in the generated text. Such a body
    // may emit only through the explicit failure form, where the record and
    // cleanup edges remain visible.
    if (!failureForm && function->mayRaiseDefinedFailure) {
      return false;
    }
    if (failureForm) {
      // The transformed sibling's name comes from the shared naming
      // authority: plain and mangled member names carry the suffix
      // directly, a structural operator bridge derives its mangled token
      // sibling, and an operator outside the token map keeps the
      // compatibility route.
      if (function->overloadedOperator) {
        const MirBodyAddress self{.kind = MirBodyKind::Function,
                                  .owner = address.owner};
        const auto row = std::find_if(
            representations_.bodies().begin(), representations_.bodies().end(),
            [&](const CppMirBodyNameRepresentation &candidate) {
              return candidate.address == self;
            });
        if (row == representations_.bodies().end() ||
            cppMirFailureSiblingSpelling(row->spelling).empty()) {
          {
            return false;
          }
        }
      }
      // The transformed ABI publishes passive values through an ordinary
      // out-parameter and owning class values by placement construction in
      // caller-provided uninitialized storage. A body that cannot raise
      // keeps its plain form instead.
      const std::optional<CppMirTypeRepresentationKind> returnKind =
          cppMirExpectedTypeRepresentation(function->returnType);
      const bool exactClassResult =
          returnKind && *returnKind == CppMirTypeRepresentationKind::Class &&
          std::any_of(representations_.types().begin(),
                      representations_.types().end(),
                      [&](const CppMirTypeRepresentation &row) {
                        return row.type == function->returnType &&
                               !row.spelling.empty();
                      });
      const bool exactLambdaResult =
          returnKind && *returnKind == CppMirTypeRepresentationKind::Lambda &&
          std::any_of(representations_.types().begin(),
                      representations_.types().end(),
                      [&](const CppMirTypeRepresentation &row) {
                        return row.type == function->returnType &&
                               !row.spelling.empty();
                      });
      const bool exactExpectedClassResult = expectedClassPlacementResultType(
          program_, representations_, function->returnType);
      const bool failureFreeVirtualOverride =
          !function->mayRaiseDefinedFailure && function->virtualMethod &&
          function->overrideMethod &&
          cppMirVirtualFailureContractRoot(program_, *function) != nullptr;
      if ((!function->mayRaiseDefinedFailure && !failureFreeVirtualOverride) ||
          !returnKind ||
          (*returnKind != CppMirTypeRepresentationKind::Scalar &&
           // The passive string view publishes by value through the
           // ordinary out-parameter exactly like a scalar.
           *returnKind != CppMirTypeRepresentationKind::StringView &&
           // A passive fixed array is also default-constructible and
           // assignable through the ordinary typed out-parameter.
           *returnKind != CppMirTypeRepresentationKind::FixedArray &&
           *returnKind != CppMirTypeRepresentationKind::Void &&
           // A loan-returning body publishes through a `T **`
           // out-parameter (ADR 018 §5); its Return-with-loan rule and
           // the caller's paired loan own the rest of the proof.
           *returnKind != CppMirTypeRepresentationKind::Reference &&
           !exactClassResult && !exactLambdaResult &&
           !exactExpectedClassResult &&
           // An expected-typed result publishes by value through the
           // ordinary out-parameter; the scalar-payload demand keeps the
           // boundary default-constructible on every shipped standard.
           !(*returnKind == CppMirTypeRepresentationKind::Enum &&
             cppMirEnumBoundaryRow(representations_, function->returnType)) &&
           !(*returnKind == CppMirTypeRepresentationKind::Expected &&
             function->returnType.arguments.size() == 2 &&
             cppMirExpectedTypeRepresentation(
                 function->returnType.arguments.front()) &&
             (*cppMirExpectedTypeRepresentation(
                  function->returnType.arguments.front()) ==
                  CppMirTypeRepresentationKind::Scalar ||
              *cppMirExpectedTypeRepresentation(
                  function->returnType.arguments.front()) ==
                  CppMirTypeRepresentationKind::Void)))) {
        {
          return false;
        }
      }
    }
    bodyPointer = &function->body;
    owner = function->owner;
    receiverMutability = function->receiverMutability;
    parameterBindings = &function->parameterBindings;
    // Both forms ride the overlay route: the plain template since 0.209,
    // and the transformed sibling template whose invocations keep the
    // same terminally-contained callable conventions.
    callableTemplateBody = !function->callableParameters.empty();
    break;
  }
  case MirBodyKind::Destructor: {
    const MirDestructorInstance *destructor =
        program_.findDestructorInstance(address.owner);
    if (destructor == nullptr || destructor->owner == 0) {
      {
        return false;
      }
    }
    if ((failureForm && !destructor->mayRaiseDefinedFailure) ||
        (!failureForm && destructor->mayRaiseDefinedFailure)) {
      return false;
    }
    bodyPointer = &destructor->body;
    owner = destructor->owner;
    receiverMutability = ReceiverMutability::Mutable;
    break;
  }
  case MirBodyKind::Constructor: {
    const MirConstructorInstance *constructor =
        program_.findConstructorInstance(address.owner);
    const bool exactFailureBoundary =
        constructor != nullptr &&
        failureConstructorBoundaryEligible(program_, *constructor);
    const std::optional<std::vector<CppMirBaseParameterInitializerBinding>>
        baseBindings =
            constructor == nullptr ? std::nullopt
                                   : cppMirBaseParameterInitializerBindings(
                                         program_, *constructor);
    if (constructor == nullptr || constructor->owner == 0 ||
        (failureForm ? !exactFailureBoundary : !baseBindings)) {
      {
        return false;
      }
    }
    // Stores-reference initializers bind their fields in the C++ member
    // initializer list from the paired Stored loans; the single pairing
    // authority declines anything outside that exact shape. A nonzero
    // constructor borrow origin is the caller-side lifetime fact of
    // exactly that schedule, so it is admissible only alongside it.
    {
      const std::optional<std::vector<CppMirStoredReferenceBinding>> bindings =
          cppMirStoredReferenceBindings(*constructor);
      if (!bindings || (constructor->borrowOrigin != BorrowOriginKind::None &&
                        bindings->empty())) {
        {
          return false;
        }
      }
      storedReferenceBindings = !bindings->empty();
    }
    // Owned-parameter fields are likewise emitted in the native
    // member-initializer list. Admission and spelling must share the exact
    // move/drop proof so the body never consumes the parameter twice.
    if (!cppMirOwnedParameterFieldBindings(program_, *constructor)) {
      return false;
    }
    if (!cppMirCopyParameterFieldBindings(program_, *constructor)) {
      return false;
    }
    bodyPointer = &constructor->body;
    owner = constructor->owner;
    receiverMutability = ReceiverMutability::Mutable;
    parameterBindings = &constructor->parameterBindings;
    break;
  }
  case MirBodyKind::Lambda: {
    const MirLambdaInstance *lambda = program_.findLambda(address.owner);
    if (lambda == nullptr) {
      {
        return false;
      }
    }
    if (failureForm) {
      // A fused literal invoked directly from a transformed body is private
      // generated C++: its signature can carry the same explicit result and
      // failure-record channel as the enclosing body. Keep the first slice to
      // passive value results; references, owning class values, and callable
      // parameters need their own publication contracts.
      const std::optional<CppMirTypeRepresentationKind> returnKind =
          cppMirExpectedTypeRepresentation(lambda->returnType);
      const bool passiveResult =
          returnKind &&
          (*returnKind == CppMirTypeRepresentationKind::Void ||
           *returnKind == CppMirTypeRepresentationKind::Scalar ||
           *returnKind == CppMirTypeRepresentationKind::StringView ||
           (*returnKind == CppMirTypeRepresentationKind::Enum &&
            cppMirEnumBoundaryRow(representations_, lambda->returnType)) ||
           (*returnKind == CppMirTypeRepresentationKind::Expected &&
            lambda->returnType.arguments.size() == 2 &&
            cppMirExpectedTypeRepresentation(
                lambda->returnType.arguments.front()) &&
            (*cppMirExpectedTypeRepresentation(
                 lambda->returnType.arguments.front()) ==
                 CppMirTypeRepresentationKind::Scalar ||
             *cppMirExpectedTypeRepresentation(
                 lambda->returnType.arguments.front()) ==
                 CppMirTypeRepresentationKind::Void)));
      if (!passiveResult) {
        return false;
      }
    }
    bodyPointer = &lambda->body;
    receiverMutability = ReceiverMutability::ReadOnly;
    parameterBindings = &lambda->parameterBindings;
    break;
  }
  default: {
    return false;
  }
  }
  const MirBody &body = *bodyPointer;
  if (body.blocks.empty() || body.entry == 0 ||
      body.entry > body.blocks.size()) {
    {
      return false;
    }
  }
  const auto loanById = [&](MirLoanId id) -> const MirLoan * {
    for (const MirLoan &loan : body.loans) {
      if (loan.id == id) {
        return &loan;
      }
    }
    return nullptr;
  };
  // Loan erasure (ADR 018): every loan needs a resolvable source place
  // whose type row spells the pointer local. A call-result loan binds the
  // exact element address its producing discharged storage read returns;
  // stored and parameter loans wait for their own slices.
  for (const MirLoan &loan : body.loans) {
    const MirPlace *loanSource = body.findPlace(loan.source);
    if (loanSource == nullptr) {
      {
        return false;
      }
    }
    if (loan.kind == MirLoanKind::Local || loan.kind == MirLoanKind::Return) {
      continue;
    }
    if (loan.kind == MirLoanKind::Stored) {
      // A field-carrying stored loan is admitted only when the
      // Constructor case proved the bijective stores-reference pairing;
      // the binding spells in the member initializer list and no pointer
      // local exists. A field-less stored loan rides a borrow-carrying
      // object value: the object local holds the borrow internally, no
      // pointer ever binds, and its call-result children bind their own
      // pointers — admissible exactly when nothing roots a place at it.
      if (loan.storedField == 0 && !storedReferenceBindings) {
        bool rootedPlace = false;
        for (const MirPlace &place : body.places) {
          if (place.root == MirPlaceRootKind::Loan && place.loan == loan.id) {
            rootedPlace = true;
          }
        }
        if (rootedPlace) {
          return false;
        }
        continue;
      }
      if (!storedReferenceBindings) {
        {
          return false;
        }
      }
      continue;
    }
    if (loan.kind == MirLoanKind::Parameter) {
      // The entry loan aliases the reference parameter's pointer carrier
      // (ADR 018 §4): bound once in the prelude, dereferenced at use.
      // Admission requires exactly the carrier shape the reference-local
      // place vocabulary already spells.
      if (!loan.entry || loan.carriers.empty() ||
          loanSource->root != MirPlaceRootKind::Binding ||
          !loanSource->projections.empty() ||
          loanSource->type.kind != SemanticType::Reference ||
          parameterBindings == nullptr ||
          std::find(parameterBindings->begin(), parameterBindings->end(),
                    loanSource->binding) == parameterBindings->end()) {
        {
          return false;
        }
      }
      continue;
    }
    if (loan.kind != MirLoanKind::CallResult) {
      {
        return false;
      }
    }
    // An owner-borrow's call-result loan never binds a pointer local:
    // the paired return loan publishes the backend accessor expression
    // directly at the consuming return.
    {
      bool ownerBorrowLoan = false;
      for (const MirBlock &block : body.blocks) {
        for (const MirInstruction &member : block.instructions) {
          if (member.kind == MirInstructionKind::Call &&
              member.hirValue == loan.producedBy &&
              (member.intrinsic == IntrinsicKind::UniqueOwnerBorrow ||
               member.intrinsic == IntrinsicKind::UniqueOwnerBorrowMut)) {
            ownerBorrowLoan = true;
          }
        }
      }
      if (ownerBorrowLoan) {
        continue;
      }
    }
    const MirInstruction *discharged =
        pairedDischargedRead(body, loan.producedBy);
    const MirInstruction *referenceCall =
        discharged == nullptr ? loanProducingReferenceCall(program_, body, loan)
                              : nullptr;
    if (discharged != nullptr) {
      if (producedCallResultLoan(body, *discharged) == nullptr ||
          (loanSource->type.kind != SemanticType::Storage &&
           loanSource->type.kind != SemanticType::PrefixStorage) ||
          loanSource->type.arguments.empty()) {
        {
          return false;
        }
      }
    } else if (referenceCall != nullptr) {
      // The pointer local declares from the callee's return element row;
      // the receiver staging still spells the source place separately.
      const MirFunctionInstance *referenceTarget =
          referenceCall->functionTarget
              ? program_.findFunctionInstance(*referenceCall->functionTarget)
              : nullptr;
      const bool elementRow =
          referenceTarget != nullptr &&
          !referenceTarget->returnType.arguments.empty() &&
          std::any_of(
              representations_.types().begin(), representations_.types().end(),
              [&](const CppMirTypeRepresentation &row) {
                return row.type ==
                           referenceTarget->returnType.arguments.front() &&
                       !row.spelling.empty();
              });
      if (producedCallResultLoan(body, *referenceCall) == nullptr ||
          !elementRow) {
        {
          return false;
        }
      }
    } else {
      {
        return false;
      }
    }
    // The pointer local declares the element type; the producing call's
    // own branch validates its staged storage place and index.
    bool borrowed = false;
    for (const MirBlock &block : body.blocks) {
      for (const MirInstruction &instruction : block.instructions) {
        borrowed =
            borrowed || (instruction.kind == MirInstructionKind::Borrow &&
                         instruction.loan && *instruction.loan == loan.id);
      }
    }
    if (borrowed) {
      // A Borrow would assign the pointer a second time.
      {
        return false;
      }
    }
  }

  const auto typeRow = [&](const SemanticType &type) {
    const auto found = std::find_if(
        representations_.types().begin(), representations_.types().end(),
        [&](const CppMirTypeRepresentation &row) { return row.type == type; });
    return found != representations_.types().end() && !found->spelling.empty();
  };
  // A class value declares in the prelude only when its row carries the
  // 0.215 boundary proof (usable default constructor and move assignment).
  const auto constructibleClassRow = [&](const SemanticType &type) {
    const auto found = std::find_if(
        representations_.types().begin(), representations_.types().end(),
        [&](const CppMirTypeRepresentation &row) { return row.type == type; });
    return found != representations_.types().end() &&
           !found->spelling.empty() && found->boundaryConstructible;
  };
  const auto fieldRow = [&](SymbolId field) {
    if (!owner) {
      {
        return false;
      }
    }
    const MirClassInstance *receiverOwner = program_.findClassInstance(*owner);
    const ResolvedMirField resolved =
        receiverOwner == nullptr
            ? ResolvedMirField{}
            : resolveMirField(program_, receiverOwner->type, field);
    if (!resolved) {
      return false;
    }
    const auto found = std::find_if(
        representations_.symbols().begin(), representations_.symbols().end(),
        [&](const CppMirSymbolRepresentation &row) {
          return row.kind == CppMirSymbolRepresentationKind::Field &&
                 row.owner == resolved.owner->id && row.symbol == field &&
                 row.ordinal == 0 && row.type == resolved.field->type;
        });
    return found != representations_.symbols().end() &&
           !found->spelling.empty();
  };
  const auto bodyRow = [&](HirFunctionInstanceId target) {
    const MirBodyAddress callee{.kind = MirBodyKind::Function, .owner = target};
    const auto found = std::find_if(
        representations_.bodies().begin(), representations_.bodies().end(),
        [&](const CppMirBodyNameRepresentation &row) {
          return row.address == callee;
        });
    return found != representations_.bodies().end() && !found->spelling.empty();
  };
  const auto valueOperand = [](const MirOperand &operand) {
    return operand.kind == MirOperandKind::Value;
  };
  const auto slotPlace = [&](const MirPlace &place) {
    return lifetimeSlotPlace(program_, representations_, body, place);
  };
  const auto lifetimeSlotRow = [&]() {
    return std::any_of(
        representations_.capabilities().begin(),
        representations_.capabilities().end(),
        [](const CppMirEmissionCapabilityRepresentation &row) {
          return row.kind == CppMirEmissionCapabilityKind::LifetimeStorage &&
                 !row.spelling.empty();
        });
  };
  const auto captureRow = [&](std::size_t lambdaOwner, SymbolId symbol,
                              std::size_t ordinal) {
    return std::any_of(
        representations_.symbols().begin(), representations_.symbols().end(),
        [&](const CppMirSymbolRepresentation &row) {
          return row.kind == CppMirSymbolRepresentationKind::Capture &&
                 row.owner == lambdaOwner && row.symbol == symbol &&
                 row.ordinal == ordinal && !row.spelling.empty();
        });
  };
  const auto capabilityRow = [&](CppMirEmissionCapabilityKind kind) {
    return std::any_of(representations_.capabilities().begin(),
                       representations_.capabilities().end(),
                       [&](const CppMirEmissionCapabilityRepresentation &row) {
                         return row.kind == kind && !row.spelling.empty();
                       });
  };
  const auto lambdaBodyRow = [&](HirLambdaId target) {
    const MirBodyAddress nested{.kind = MirBodyKind::Lambda, .owner = target};
    const auto found = std::find_if(
        representations_.bodies().begin(), representations_.bodies().end(),
        [&](const CppMirBodyNameRepresentation &row) {
          return row.address == nested;
        });
    return found != representations_.bodies().end() && !found->spelling.empty();
  };
  const auto constructSlot = [&](const MirInstruction &construct) {
    return constructDestinationSlot(body, construct);
  };
  const auto syntheticBool = [](const MirOperand &operand) {
    return operand.kind == MirOperandKind::Constant && operand.value == 0 &&
           operand.place == 0 && operand.loan == 0 && operand.literal &&
           operand.type == SemanticType::Bool &&
           std::holds_alternative<bool>(*operand.literal);
  };
  const auto literalSupported = [&](const std::optional<Literal> &literal,
                                    const SemanticType &type) {
    if (!literal) {
      {
        return false;
      }
    }
    if (std::holds_alternative<std::nullptr_t>(*literal)) {
      return ScalarBodyTextEmitter::spellableLiteral(*literal, type) &&
             typeRow(type);
    }
    if (std::holds_alternative<std::uint64_t>(*literal)) {
      return typeRow(type);
    }
    if (std::holds_alternative<std::string>(*literal)) {
      return type.kind == SemanticType::StringView && typeRow(type);
    }
    if (const auto *value = std::get_if<BinaryFloat>(&*literal)) {
      return validBinaryFloat(*value) &&
             (value->format == BinaryFloatFormat::Binary64
                  ? type == SemanticType::Double
                  : type == SemanticType::Float) &&
             typeRow(type);
    }
    return std::holds_alternative<CharacterLiteral>(*literal) ||
           std::holds_alternative<bool>(*literal);
  };

  for (const MirPlace &place : body.places) {
    if (address.kind == MirBodyKind::Module &&
        place.root == MirPlaceRootKind::Binding && place.projections.empty() &&
        place.symbol != 0) {
      const CppMirSymbolRepresentation *storage = storageRepresentationForBody(
          program_, representations_, std::nullopt, place.symbol);
      if (storage == nullptr || storage->spelling.empty() ||
          storage->type != place.type ||
          place.type.kind == SemanticType::Reference) {
        return false;
      }
      continue;
    }
    if (place.root == MirPlaceRootKind::This) {
      if (place.projections.empty()) {
        continue;
      }
      // One projected field, or a reference field followed by its
      // dereference: C++ reference members dereference implicitly, so
      // both bind through the same member spelling.
      const bool referenceFieldChain =
          place.projections.size() == 2 &&
          place.projections[0].kind == MirProjectionKind::Field &&
          place.projections[1].kind == MirProjectionKind::Dereference;
      // A field element place carries the staged index value; it spells
      // only inside the fused array_at accessor, never as an alias.
      const bool fieldElementChain =
          place.projections.size() == 2 &&
          place.projections[0].kind == MirProjectionKind::Field &&
          place.projections[1].kind == MirProjectionKind::Index &&
          place.projections[1].index != 0;
      if ((place.projections.size() != 1 && !referenceFieldChain &&
           !fieldElementChain) ||
          place.projections.front().kind != MirProjectionKind::Field ||
          !fieldRow(place.projections.front().field)) {
        {
          return false;
        }
      }
      // A storage-typed receiver field is only the staging carrier for
      // storage-intrinsic calls; it needs a field row but no value row.
      continue;
    }
    if (address.kind == MirBodyKind::Constructor &&
        ownedParameterFieldSourcePlace(program_, body, place)) {
      // The native member-initializer list consumes this parameter directly;
      // a body-local slot would move from the argument a second time.
      continue;
    }
    if (slotPlace(place)) {
      const bool materializedLambda =
          place.type.kind == SemanticType::Lambda &&
          static_cast<bool>(
              materializedClosureForType(program_, body, place.type));
      const bool representedType =
          semanticTypeContainsLambda(place.type)
              ? lambdaDependentTypeRepresentable(program_, representations_,
                                                 body, place.type)
              : typeRow(place.type);
      if (!lifetimeSlotRow() || (!representedType && !materializedLambda)) {
        {
          return false;
        }
      }
      // A function or transformed constructor's slot-allocated parameter
      // constructs its slot from the argument in the shared prelude. Lambda
      // slot parameters still need a separate closure-boundary contract.
      if (parameterBindings != nullptr &&
          std::find(parameterBindings->begin(), parameterBindings->end(),
                    place.binding) != parameterBindings->end() &&
          address.kind != MirBodyKind::Function &&
          !(failureForm && address.kind == MirBodyKind::Constructor)) {
        {
          return false;
        }
      }
      continue;
    }
    if (place.root == MirPlaceRootKind::Symbol) {
      if (place.capture != 0) {
        // A capture place spells its Capture row name inside the literal.
        if (address.kind != MirBodyKind::Lambda || !place.projections.empty() ||
            !captureRow(address.owner, place.symbol, place.capture)) {
          {
            return false;
          }
        }
        continue;
      }
      const CppMirSymbolRepresentation *storage = storageRepresentationForBody(
          program_, representations_, owner, place.symbol);
      const bool directStorage = place.projections.empty();
      const bool dereferencedPointer =
          place.projections.size() == 1 &&
          place.projections.front().kind == MirProjectionKind::Dereference &&
          storage != nullptr &&
          storage->type.kind == SemanticType::RawPointer &&
          storage->type.arguments.size() == 1 &&
          storage->type.arguments.front() == place.type;
      if ((!directStorage && !dereferencedPointer) || storage == nullptr ||
          storage->spelling.empty()) {
        {
          return false;
        }
      }
      continue;
    }
    if (const std::optional<RawMemoryPlaceAccess> access =
            rawMemoryPlaceAccess(body, place)) {
      if (!capabilityRow(CppMirEmissionCapabilityKind::RawMemory) ||
          !typeRow(access->pointerType) || !typeRow(place.type) ||
          !exactFieldProjectionRows(program_, representations_,
                                    access->pointeeType, place.projections, 1,
                                    place.type)) {
        return false;
      }
      continue;
    }
    if (const std::optional<ArrayElementAccess> access =
            arrayElementAccess(body, place)) {
      const MirPlace *array = body.findPlace(access->array);
      if (array == nullptr || !typeRow(array->type) || !typeRow(place.type)) {
        {
          return false;
        }
      }
      continue;
    }
    if (const std::optional<BindingArrayFieldElementAccess> access =
            bindingArrayFieldElementAccess(body, place)) {
      const MirPlace *array = body.findPlace(access->array);
      if (array == nullptr || !typeRow(array->type) || !typeRow(place.type)) {
        return false;
      }
      continue;
    }
    if (const std::optional<ConstantArrayElementFieldAccess> access =
            constantArrayElementFieldAccess(body, place)) {
      const MirPlace *array = body.findPlace(access->element.array);
      if (array == nullptr || !typeRow(array->type) || !typeRow(place.type) ||
          !exactFieldProjectionRows(program_, representations_,
                                    access->elementType, access->fields, 0,
                                    place.type)) {
        return false;
      }
      continue;
    }
    if (const std::optional<ArrayElementAccess> access =
            viewElementAccess(body, place)) {
      const MirPlace *view = body.findPlace(access->array);
      if (view == nullptr || !typeRow(view->type) || !typeRow(place.type)) {
        {
          return false;
        }
      }
      continue;
    }
    if (classSubscriptAccess(program_, body, place)) {
      // Reads and stores spell the class's subscript member; each
      // direction proves its contained member at its instruction rule.
      if (!typeRow(place.type)) {
        return false;
      }
      continue;
    }
    if (place.root == MirPlaceRootKind::Loan) {
      const MirLoan *loan = loanById(place.loan);
      const std::optional<SemanticType> referent =
          loan == nullptr ? std::nullopt
                          : loanReferentType(program_, body, *loan);
      if (!referent || !typeRow(place.type) ||
          !exactFieldProjectionRows(program_, representations_, *referent,
                                    place.projections, 0, place.type)) {
        {
          return false;
        }
      }
      continue;
    }
    if (place.root == MirPlaceRootKind::This && place.projections.size() == 2 &&
        place.projections[0].kind == MirProjectionKind::Field &&
        place.projections[1].kind == MirProjectionKind::Index) {
      if (!fieldRow(place.projections[0].field) || !typeRow(place.type)) {
        {
          return false;
        }
      }
      continue;
    }
    if (place.root == MirPlaceRootKind::Binding && place.projections.empty() &&
        place.type.kind == SemanticType::Reference) {
      // Parameters bind from their signature argument. Locals bind from the
      // one retained loan that names this binding as a carrier; the emitter
      // declares a pointer and Initialize copies that exact loan pointer.
      const bool parameter =
          parameterBindings != nullptr &&
          std::find(parameterBindings->begin(), parameterBindings->end(),
                    place.binding) != parameterBindings->end();
      const MirLoan *carrier = loanCarriedByBinding(body, place.binding);
      if (!typeRow(place.type) || place.type.arguments.size() != 1 ||
          (!parameter && carrier == nullptr)) {
        {
          return false;
        }
      }
      continue;
    }
    if (place.root == MirPlaceRootKind::Binding && !place.projections.empty() &&
        place.projections[0].kind == MirProjectionKind::Dereference) {
      const MirPlace *base = nullptr;
      for (const MirPlace &candidate : body.places) {
        if (candidate.id != place.id &&
            candidate.root == MirPlaceRootKind::Binding &&
            candidate.binding == place.binding &&
            candidate.projections.empty()) {
          base = &candidate;
        }
      }
      const SemanticType *referent =
          base != nullptr && base->type.kind == SemanticType::Reference &&
                  base->type.arguments.size() == 1
              ? &base->type.arguments.front()
              : nullptr;
      if (referent == nullptr || !typeRow(place.type) ||
          !exactFieldProjectionRows(program_, representations_, *referent,
                                    place.projections, 1, place.type)) {
        {
          return false;
        }
      }
      continue;
    }
    if (place.root == MirPlaceRootKind::Binding && !place.projections.empty() &&
        std::all_of(place.projections.begin(), place.projections.end(),
                    [](const MirPlaceProjection &projection) {
                      return projection.kind == MirProjectionKind::Field;
                    })) {
      const auto base =
          std::find_if(body.places.begin(), body.places.end(),
                       [&](const MirPlace &candidate) {
                         return candidate.id != place.id &&
                                candidate.root == MirPlaceRootKind::Binding &&
                                candidate.binding == place.binding &&
                                candidate.projections.empty();
                       });
      if (base == body.places.end() || base->type.kind != SemanticType::Class ||
          !typeRow(base->type) || !typeRow(place.type) ||
          !exactFieldProjectionRows(program_, representations_, base->type,
                                    place.projections, 0, place.type)) {
        return false;
      }
      continue;
    }
    if (place.root == MirPlaceRootKind::Binding && place.projections.empty() &&
        place.type.kind == SemanticType::Lambda) {
      // A lexical callable binding needs a real lifetime slot. A template
      // overlay names parameter/result callables as T; a locally-created
      // owned closure derives its otherwise-unnameable type from its verified
      // factory. Trivial frozen closures remain representation-free.
      const bool materialized = static_cast<bool>(
          materializedClosureForType(program_, body, place.type));
      if ((slotPlace(place) && !typeRow(place.type) && !materialized) ||
          (callableTemplateBody && !typeRow(place.type) && !materialized)) {
        {
          return false;
        }
      }
      continue;
    }
    if (copyStageForTemporary(body, place) != nullptr) {
      // The by-value argument staging temporary never materializes: the
      // consuming call spells the source place and C++ performs the copy
      // at the call boundary.
      continue;
    }
    if (place.root == MirPlaceRootKind::Value &&
        expectedClassResultDestinationSlot(program_, body, place.value) !=
            nullptr) {
      // The transformed callee constructs directly in the consuming binding;
      // this value-root place carries the temporary identity only.
      continue;
    }
    if (place.root == MirPlaceRootKind::Value &&
        placementDirectReturnCall(program_, representations_, body,
                                  place.value) != nullptr) {
      // The transformed callee constructs directly in this body's caller;
      // the value-root place carries only the returned drop identity.
      continue;
    }
    if (place.root == MirPlaceRootKind::Value &&
        expectedPayloadReturnSlot(program_, representations_, body,
                                  place.value)) {
      // The value-root place is represented by the dedicated payload slot.
      continue;
    }
    if (place.root == MirPlaceRootKind::Value &&
        expectedClassExtractionPlace(body, place.value) == &place) {
      // The failure helper publishes the existing payload's address into the
      // value carrier; this place is the lvalue view over that pointer.
      continue;
    }
    if (unreferencedValueRootedPlace(body, place)) {
      // A pure root record: the rooted value flows through its own uses
      // and the place spells nothing.
      continue;
    }
    if (place.root == MirPlaceRootKind::Binding && place.projections.empty() &&
        place.type.kind == SemanticType::TypePack &&
        parameterBindings != nullptr &&
        std::find(parameterBindings->begin(), parameterBindings->end(),
                  place.binding) != parameterBindings->end()) {
      // The trailing parameter pack's flattened parameters spell at the
      // one forwarding call; the pack place itself never declares.
      continue;
    }
    if (packElementParameterIndex(program_, body, address, place)) {
      if (!typeRow(place.type)) {
        return false;
      }
      continue;
    }
    if (!place.projections.empty() || !typeRow(place.type) ||
        // A class-typed local declares value-initialized, so its row must
        // carry the boundary proof (a deleted default constructor cannot
        // spell the declaration).
        (place.type.kind == SemanticType::Class &&
         !constructibleClassRow(place.type))) {
      {
        return false;
      }
    }
  }
  for (const MirValue &value : body.values) {
    const MirInstruction *owningDefinition =
        findInstruction(body, value.definition);
    if (owningDefinition != nullptr &&
        fixedArrayAggregateDestinationSlot(program_, body, *owningDefinition) !=
            nullptr) {
      continue;
    }
    if (value.info.type.kind == SemanticType::TypePack) {
      // Concrete pack elements must be projected into ordinary typed MIR
      // places and staged individually. A TypePack SSA value would hide
      // copy/move policy from the backend.
      return false;
    }
    if (expectedClassResultDestinationSlot(program_, body, value.id) !=
        nullptr) {
      continue;
    }
    if (expectedMoveDestinationSlot(program_, body, value.id) != nullptr) {
      continue;
    }
    if (placementDirectReturnCall(program_, representations_, body, value.id) !=
        nullptr) {
      continue;
    }
    if (expectedPayloadReturnSlot(program_, representations_, body, value.id)) {
      continue;
    }
    if (value.info.type.kind == SemanticType::Class) {
      if (fixedArrayAggregateInputSlot(program_, body, value.id) != nullptr ||
          expectedPayloadInitialize(program_, body, value.id) != nullptr ||
          expectedDefaultPayloadInitialization(program_, body, value.id) ||
          expectedClassExtractionPlace(body, value.id) != nullptr) {
        continue;
      }
      if (passiveCAbiCallAssignmentResult(program_, body, value.id)) {
        const CppMirTypeRepresentation *row =
            representationTypeRow(representations_, value.info.type);
        if (row == nullptr || row->spelling.empty() ||
            !row->boundaryConstructible || !row->copyable) {
          return false;
        }
        continue;
      }
      // A class value is admissible only when the declaration loop will
      // declare it or a no-local vocabulary spells it at its consumer;
      // anything else would assign into an undeclared local (measured on
      // the caller bodies the pack admission flipped).
      const MirInstruction *definition =
          findInstruction(body, value.definition);
      const auto slotConsumed = [&](const MirInstruction &construct) {
        if (!construct.result) {
          return false;
        }
        for (const MirValueUse &use : body.usesOf(*construct.result)) {
          const MirInstruction *consumer =
              findInstruction(body, use.instruction);
          if (consumer != nullptr &&
              consumer->kind == MirInstructionKind::Initialize &&
              consumer->destination) {
            const MirPlace *destinationPlace =
                body.findPlace(*consumer->destination);
            if (destinationPlace != nullptr && slotPlace(*destinationPlace)) {
              return true;
            }
          }
        }
        return false;
      };
      const bool constructDefined =
          definition != nullptr &&
          definition->kind == MirInstructionKind::Construct &&
          definition->result && *definition->result == value.id &&
          !definition->destination && !definition->receiver;
      // A copy load assigns the place into the declared local when the
      // copied row proves both copy members usable.
      const auto copyLoadResult = [&]() {
        if (definition == nullptr ||
            definition->kind != MirInstructionKind::Load ||
            definition->operands.size() != 1 ||
            definition->operands.front().kind != MirOperandKind::Copy ||
            definition->operands.front().place == 0 ||
            body.findPlace(definition->operands.front().place) == nullptr ||
            !definition->localFailureSites.empty()) {
          return false;
        }
        const auto row = std::find_if(
            representations_.types().begin(), representations_.types().end(),
            [&](const CppMirTypeRepresentation &candidate) {
              return candidate.type == value.info.type;
            });
        return row != representations_.types().end() && row->copyable;
      };
      const auto movedIntoValueStage = [&]() {
        const std::vector<MirValueUse> stageUses =
            nonRootRecordUses(body, value.id);
        if (definition == nullptr ||
            definition->kind != MirInstructionKind::Move ||
            stageUses.size() != 1) {
          return false;
        }
        const MirInstruction *user =
            findInstruction(body, stageUses.front().instruction);
        return user != nullptr && user->kind == MirInstructionKind::CallInput &&
               user->result && copyStagedCallInput(body, *user->result) == user;
      };
      const bool noLocal =
          (definition != nullptr &&
           definition->kind == MirInstructionKind::CallInput) ||
          isBorrowStagedResult(body, value) ||
          isStorageStagedResult(body, value) ||
          discardedAssignmentResult(body, value.id) ||
          directTemporaryReceiverForValue(body, value.id) ||
          unexpectedDefinition(body, value.id) != nullptr ||
          (definition != nullptr &&
           (returnConstructDefinition(body, value.id) == definition ||
            returnDefaultConstructionDefinition(body, value.id) == definition ||
            (failureForm &&
             (returnMoveDefinition(body, value.id) == definition ||
              (returnCopyLoadDefinition(body, value.id) == definition &&
               copyLoadResult()))) ||
            returnCallDefinition(body, value.id) == definition)) ||
          (definition != nullptr && copyLoadResult() &&
           classCopyAssignmentFusion(body, *definition)) ||
          (definition != nullptr &&
           classMoveArrayAssignmentFusion(body, *definition)) ||
          (constructDefined && slotConsumed(*definition)) ||
          passiveFixedArrayConstructInput(program_, body, value.id) !=
              nullptr ||
          classValuePublicationSlot(body, value.id) != nullptr ||
          constructorFieldResultSlot(program_, body, value.id) ||
          classSsaLifetimeSlot(program_, representations_, body, value.id) ||
          valueRootedClassCallResultSlot(program_, representations_, body,
                                         value.id) ||
          (definition != nullptr &&
           stagedClassResultForSource(body, value.id).producer == definition) ||
          conditionalClassReturnJoinSlotForValue(body, value.id) != nullptr ||
          conditionalClassBindingJoinSlotForValue(body, value.id) != nullptr ||
          movedIntoValueStage() ||
          constructorFieldMoveInitialize(body, value.id) != nullptr ||
          (address.kind == MirBodyKind::Constructor &&
           ownedParameterFieldBinding(program_, body, address.owner,
                                      value.id)) ||
          (address.kind == MirBodyKind::Constructor &&
           copyParameterFieldBinding(program_, body, address.owner,
                                     value.id)) ||
          (definition != nullptr &&
           stagedConstructorFieldPublication(program_, body, address.owner,
                                             *definition) != nullptr) ||
          (definition != nullptr &&
           storedReferenceFieldPublication(program_, body, address.owner,
                                           *definition) != nullptr) ||
          // A sequenced moved argument declares at its move with auto and
          // needs no representation row.
          sequencedMovedArgument(body, value.id) != nullptr ||
          // An owner-borrow element value publishes through its return
          // loan's fused accessor spelling; it never declares a local.
          (definition != nullptr &&
           definition->kind == MirInstructionKind::Call &&
           (definition->intrinsic == IntrinsicKind::UniqueOwnerBorrow ||
            definition->intrinsic == IntrinsicKind::UniqueOwnerBorrowMut) &&
           nonRootRecordUses(body, value.id).empty()) ||
          // A transformed reference-returning call publishes its referent
          // through the paired call-result loan pointer. The MIR value only
          // identifies that result and must not materialize a class local.
          callResultLoanIdentity(body, value) ||
          // A nested contained-member call result spells inline at its
          // consuming argument and never declares a local.
          inlineNestedCallResult(program_, representations_, body, value.id) !=
              nullptr ||
          inlineFailureConstructorArgument(program_, body, value.id) ||
          [&]() {
            const MovedChainTerminal terminal =
                movedChainTerminal(body, value.id);
            return terminal.consumer != nullptr &&
                   terminal.consumer->kind == MirInstructionKind::Construct &&
                   movedPlaceChainSource(body, terminal.top,
                                         *terminal.consumer) != nullptr;
          }() ||
          [&]() {
            const MovedChainTerminal terminal =
                movedChainTerminal(body, value.id);
            return terminal.consumer != nullptr &&
                   terminal.consumer->kind == MirInstructionKind::Compute &&
                   terminal.consumer->operation == MirOperation::Closure &&
                   materializedClosure(program_, body, *terminal.consumer) &&
                   movedPlaceChainSource(body, terminal.top,
                                         *terminal.consumer) != nullptr;
          }() ||
          (definition != nullptr &&
           storageReferenceReadCall(body, *definition));
      if (!noLocal) {
        {
          return false;
        }
      }
      continue;
    }
    // A borrow-staged call input never materializes as a local, so its
    // staged value needs no representation row.
    if (isBorrowStagedResult(body, value)) {
      continue;
    }
    if (isStorageStagedResult(body, value)) {
      continue;
    }
    if (value.info.type.kind == SemanticType::Lambda) {
      // Lambda SSA records never declare. They either remain in the frozen
      // fused chain, publish into an exact lifetime slot, read a materialized
      // receiver, or carry one no-intervening-use move to a call/construct or
      // transformed return publication.
      const MirInstruction *definition =
          findInstruction(body, value.definition);
      const MovedChainTerminal terminal = movedChainTerminal(body, value.id);
      const bool movedToConsumer =
          terminal.consumer != nullptr &&
          (terminal.consumer->kind == MirInstructionKind::Call ||
           terminal.consumer->kind == MirInstructionKind::Construct) &&
          movedPlaceChainSource(body, terminal.top, *terminal.consumer) !=
              nullptr;
      const bool returned =
          failureForm && returnMoveDefinition(body, value.id) == definition;
      const bool ownedConstructorField =
          address.kind == MirBodyKind::Constructor &&
          ownedParameterFieldBinding(program_, body, address.owner, value.id);
      if (closureChainDefinition(body, value.id) == nullptr &&
          lambdaValueDestinationSlot(body, value.id) == nullptr &&
          materializedCallableReceiverPlace(program_, body, value.id) ==
              nullptr &&
          !movedToConsumer && !returned && !ownedConstructorField &&
          !(callableTemplateBody &&
            (callableReceiverStage(body, value.id) != nullptr ||
             callableArgumentStage(body, value.id) != nullptr))) {
        {
          return false;
        }
      }
      continue;
    }
    if (value.info.type.kind == SemanticType::Reference) {
      // A reference-typed value never declares a local: a paired
      // call-result loan carries it, and every consumer reads through
      // the loan's place instead.
      const MirValue *referenceValue = body.findValue(value.id);
      const MirInstruction *definition =
          referenceValue == nullptr
              ? nullptr
              : findInstruction(body, referenceValue->definition);
      if (loanStagedCallInput(body, value.id) != nullptr) {
        // The loan-staged reference argument spells its dereferenced
        // pointer carrier at the consuming call and never declares.
        continue;
      }
      if (definition == nullptr ||
          producedCallResultLoan(body, *definition) == nullptr ||
          !body.usesOf(value.id).empty()) {
        {
          return false;
        }
      }
      continue;
    }
    if (!typeRow(value.info.type)) {
      {
        return false;
      }
    }
  }

  for (const MirBlock &block : body.blocks) {
    // Borrow-staged call inputs whose consuming call has not yet appeared.
    // Between staging and the call, the spelled place expression must stay
    // valid: only value-producing reads may intervene, none may observe a
    // staged value, and the block must not end with staging pending.
    std::vector<MirValueId> pendingStaged;
    // A borrowed receiver is evaluated before its arguments. Nested checked
    // argument evaluation can therefore route through several Invoke success
    // blocks before the consuming call. Admit only the exact linear form:
    // every hop is an Invoke success edge and every successor has one CFG
    // predecessor, so the original stage dominates the eventual consumer and
    // no join can enter with the stage absent.
    const auto stagedAcrossInvokeSuccessChain = [&](MirValueId id,
                                                    MirBlockId targetBlock) {
      const MirValue *record = body.findValue(id);
      const MirInstruction *stage =
          record == nullptr ? nullptr
                            : findInstruction(body, record->definition);
      if (stage == nullptr || stage->kind != MirInstructionKind::CallInput ||
          record->definitionBlock == 0 ||
          record->definitionBlock == targetBlock) {
        return false;
      }
      std::unordered_set<MirBlockId> visited;
      MirBlockId current = record->definitionBlock;
      while (current != targetBlock && visited.insert(current).second) {
        const MirBlock *source = body.findBlock(current);
        if (source == nullptr ||
            source->terminator.kind != MirTerminatorKind::Invoke ||
            source->terminator.target == 0) {
          return false;
        }
        const MirBlockId next = source->terminator.target;
        std::size_t predecessors = 0;
        for (const MirBlock &candidate : body.blocks) {
          const MirTerminator &edge = candidate.terminator;
          bool reaches = false;
          switch (edge.kind) {
          case MirTerminatorKind::Goto:
            reaches = edge.target == next;
            break;
          case MirTerminatorKind::Branch:
          case MirTerminatorKind::Invoke:
            reaches = edge.target == next || edge.elseTarget == next;
            break;
          case MirTerminatorKind::Switch:
            reaches = edge.target == next ||
                      std::any_of(edge.switchTargets.begin(),
                                  edge.switchTargets.end(),
                                  [&](const MirSwitchTarget &target) {
                                    return target.target == next;
                                  });
            break;
          default:
            break;
          }
          predecessors += reaches ? 1 : 0;
        }
        if (predecessors != 1) {
          return false;
        }
        current = next;
      }
      return current == targetBlock;
    };
    const auto referencesPendingStaged = [&](const MirInstruction &between) {
      for (const MirOperand &operand : between.operands) {
        if (operand.kind == MirOperandKind::Value &&
            std::find(pendingStaged.begin(), pendingStaged.end(),
                      operand.value) != pendingStaged.end()) {
          return true;
        }
      }
      return between.receiver &&
             between.receiver->kind == MirOperandKind::Value &&
             std::find(pendingStaged.begin(), pendingStaged.end(),
                       between.receiver->value) != pendingStaged.end();
    };
    for (const MirInstruction &instruction : block.instructions) {
      if (!pendingStaged.empty()) {
        const bool consumingFailureConstructor =
            failureForm && instruction.kind == MirInstructionKind::Construct &&
            instruction.constructorTarget &&
            instruction.constructorKind == ConstructorKind::Ordinary &&
            instruction.localFailureSites.empty() &&
            instruction.definedFailure.propagation ==
                FailurePropagationKind::Constructor;
        const bool consumingCall =
            (instruction.kind == MirInstructionKind::Call &&
             (instruction.intrinsic == IntrinsicKind::None ||
              instruction.intrinsic == IntrinsicKind::PrefixStorageAppend ||
              sparseStorageIntrinsic(instruction.intrinsic))) ||
            consumingFailureConstructor ||
            // The class-valued publication construct consumes its staged
            // arguments exactly like a call: the constructor call spells
            // the staged places directly.
            (instruction.kind == MirInstructionKind::Construct &&
             instruction.result &&
             returnConstructDefinition(body, *instruction.result) ==
                 &instruction);
        // A Move between the staging and its consuming call is inert
        // argument staging: it neither raises nor observes, so the
        // staged callable's deferred spelling stays exact. Lifecycle and
        // EndBorrow spell as comments and touch no state, so they are
        // equally inert inside the window.
        if (!consumingCall &&
            (instruction.kind != MirInstructionKind::CallInput &&
             instruction.kind != MirInstructionKind::Load &&
             instruction.kind != MirInstructionKind::Compute &&
             instruction.kind != MirInstructionKind::Move &&
             instruction.kind != MirInstructionKind::Lifecycle &&
             instruction.kind != MirInstructionKind::EndBorrow &&
             instruction.kind != MirInstructionKind::Call)) {
          {
            return false;
          }
        }
        if (!consumingCall && referencesPendingStaged(instruction)) {
          {
            return false;
          }
        }
      }
      switch (instruction.kind) {
      case MirInstructionKind::Lifecycle:
        continue;
      case MirInstructionKind::Construct: {
        if (generatedSpecialMemberConstruction(program_, body, instruction)) {
          continue;
        }
        if (instruction.result) {
          const StagedClassResult staged =
              stagedClassResultForSource(body, *instruction.result);
          if (staged.producer == &instruction) {
            // The exact prepared-parameter schedule owns this construction.
            // A Constructor propagation with no paired Invoke is terminally
            // contained by the compatibility constructor it spells.
            continue;
          }
        }
        if (instruction.result) {
          const InlineFailureConstructorArgument nested =
              inlineFailureConstructorArgument(program_, body,
                                               *instruction.result);
          if (nested.producer == &instruction) {
            continue;
          }
        }
        if (instruction.result &&
            passiveFixedArrayConstructInput(program_, body,
                                            *instruction.result) != nullptr) {
          // The shared aggregate proof owns this failure-free constructor
          // result and publishes it directly in operand order.
          continue;
        }
        if (failureForm && instruction.constructorTarget &&
            instruction.constructorKind == ConstructorKind::Ordinary &&
            instruction.localFailureSites.empty() &&
            instruction.definedFailure.propagation ==
                FailurePropagationKind::Constructor) {
          const MirConstructorInstance *target =
              program_.findConstructorInstance(*instruction.constructorTarget);
          const MirClassInstance *targetOwner =
              target == nullptr ? nullptr
                                : program_.findClassInstance(target->owner);
          const bool publishesReturn =
              instruction.result &&
              returnConstructDefinition(body, *instruction.result) ==
                  &instruction;
          const MirPlace *slot =
              !publishesReturn && instruction.result
                  ? classValuePublicationSlot(body, *instruction.result)
                  : nullptr;
          const ConstructorFieldResultSlot fieldResult =
              !publishesReturn && instruction.result
                  ? constructorFieldResultSlot(program_, body,
                                               *instruction.result)
                  : ConstructorFieldResultSlot{};
          if (slot == nullptr) {
            slot = fieldResult.slot;
          }
          const bool ownerTypeMatches =
              targetOwner != nullptr &&
              targetOwner->type == instruction.info.type;
          const bool boundaryEligible =
              target != nullptr &&
              failureConstructorBoundaryEligible(program_, *target);
          const bool containedPlain = terminallyContainedPlainConstructor(
              program_, representations_, instruction);
          const bool operandsReady =
              std::all_of(instruction.operands.begin(),
                          instruction.operands.end(), valueOperand);
          if (target == nullptr || targetOwner == nullptr ||
              !ownerTypeMatches || (!boundaryEligible && !containedPlain) ||
              !instruction.result || (!publishesReturn && slot == nullptr) ||
              !operandsReady) {
            return false;
          }
          // A failure-capable publication constructor may consume an exact
          // borrow stage just like the plain publication form below. The
          // stage must still be pending in this block or handed across the
          // unique successful invoke edge that reaches this block.
          bool stagedConsumable = true;
          for (const MirOperand &operand : instruction.operands) {
            const MirInstruction *stage = borrowStagedCallInput(body, operand);
            if (stage == nullptr || !stage->result) {
              continue;
            }
            const auto pending = std::find(pendingStaged.begin(),
                                           pendingStaged.end(), *stage->result);
            if (pending != pendingStaged.end()) {
              pendingStaged.erase(pending);
            } else if (!stagedAcrossInvokeSuccessChain(*stage->result,
                                                       block.id)) {
              stagedConsumable = false;
            }
          }
          if (!stagedConsumable) {
            return false;
          }
          continue;
        }
        // The slot vocabulary spells argument-less generated-default
        // construction only; a declared constructor's arguments would be
        // silently dropped by `.construct()`.
        if (instruction.result &&
            returnConstructDefinition(body, *instruction.result) ==
                &instruction) {
          // The class-valued publication construct: every operand is a
          // staged value and the constructor's own row spells the call.
          // Both forms fuse — the plain return spells the constructor
          // call inline exactly like the compatibility return.
          if (!typeRow(instruction.info.type) ||
              !std::all_of(instruction.operands.begin(),
                           instruction.operands.end(), valueOperand) ||
              !instruction.localFailureSites.empty()) {
            {
              return false;
            }
          }
          // A borrow-staged argument consumes its stage here: pending in
          // this block, or handed off from the unique invoke-predecessor.
          bool stagedConsumable = true;
          for (const MirOperand &operand : instruction.operands) {
            const MirValue *record = body.findValue(operand.value);
            const MirInstruction *stage =
                record == nullptr ? nullptr
                                  : findInstruction(body, record->definition);
            if (stage == nullptr ||
                stage->kind != MirInstructionKind::CallInput ||
                stage->operands.empty() ||
                (stage->operands.front().kind != MirOperandKind::BorrowRead &&
                 stage->operands.front().kind != MirOperandKind::BorrowWrite)) {
              continue;
            }
            const auto pending = std::find(pendingStaged.begin(),
                                           pendingStaged.end(), operand.value);
            if (pending != pendingStaged.end()) {
              pendingStaged.erase(pending);
            } else if (!stagedAcrossInvokeSuccessChain(operand.value,
                                                       block.id)) {
              stagedConsumable = false;
            }
          }
          if (!stagedConsumable) {
            {
              return false;
            }
          }
          continue;
        }
        if (instruction.result && !instruction.destination &&
            !instruction.receiver &&
            instruction.constructorKind == ConstructorKind::Ordinary &&
            instruction.localFailureSites.empty() &&
            std::all_of(instruction.operands.begin(),
                        instruction.operands.end(), valueOperand) &&
            constructibleClassRow(instruction.info.type)) {
          // A value-producing construction assigns the constructor call
          // into its declared class local; the row's boundary proof
          // guarantees the declaration and the assignment both compile,
          // and a propagating construction's invoke edge stays vacuous
          // under the producer rules. The slot protocol keeps ownership
          // of any construct whose value a slot-place Initialize
          // consumes: the value route would bypass slot engagement and
          // later slot reads would find the slot empty.
          bool slotConsumer = false;
          for (const MirValueUse &use : body.usesOf(*instruction.result)) {
            const MirInstruction *consumer =
                findInstruction(body, use.instruction);
            if (consumer != nullptr &&
                consumer->kind == MirInstructionKind::Initialize &&
                consumer->destination) {
              const MirPlace *destinationPlace =
                  body.findPlace(*consumer->destination);
              if (destinationPlace != nullptr && slotPlace(*destinationPlace)) {
                slotConsumer = true;
              }
            }
          }
          if (!slotConsumer) {
            continue;
          }
        }
        const MirPlaceId destination = constructSlot(instruction);
        const StagedClassResult staged =
            instruction.result
                ? stagedClassResultForSource(body, *instruction.result)
                : StagedClassResult{};
        const MirPlace *slot =
            staged.producer == &instruction
                ? staged.slot
                : (destination == 0 ? nullptr : body.findPlace(destination));
        if (slot == nullptr || !slotPlace(*slot) || instruction.receiver ||
            !std::all_of(instruction.operands.begin(),
                         instruction.operands.end(), valueOperand) ||
            instruction.constructorKind != ConstructorKind::Ordinary) {
          {
            return false;
          }
        }
        continue;
      }
      case MirInstructionKind::Borrow: {
        const MirLoan *loan =
            instruction.loan ? loanById(*instruction.loan) : nullptr;
        // The fused element borrow carries its bounds failure site; the
        // terminal array_at accessor contains it.
        if (loan == nullptr ||
            (!instruction.localFailureSites.empty() &&
             elementBorrowLoanProducer(body, *loan) != &instruction)) {
          {
            return false;
          }
        }
        if (loan->kind == MirLoanKind::Stored) {
          // The reference field binds in the member initializer list;
          // the Borrow spells as a comment only.
          continue;
        }
        const MirPlace *source = body.findPlace(loan->source);
        if (source == nullptr || !typeRow(source->type)) {
          {
            return false;
          }
        }
        if (source->type.kind == SemanticType::Storage ||
            source->type.kind == SemanticType::PrefixStorage) {
          // A storage-sourced loan publishes a discharged read's element;
          // the pairing, the staged storage place, and the element type
          // row must all resolve or the body declines.
          const MirInstruction *read =
              pairedDischargedRead(body, loan->producedBy);
          if (read == nullptr || read->operands.size() != 2 ||
              storageStagedPlace(body, read->operands.front()) == nullptr ||
              source->type.arguments.empty() ||
              !typeRow(source->type.arguments.front())) {
            {
              return false;
            }
          }
        }
        continue;
      }
      case MirInstructionKind::EndBorrow:
        if (!instruction.loan || loanById(*instruction.loan) == nullptr) {
          {
            return false;
          }
        }
        continue;
      case MirInstructionKind::Drop: {
        const MirPlace *droppedPlace =
            instruction.destination ? body.findPlace(*instruction.destination)
                                    : nullptr;
        if (trivialMirDrop(body, instruction,
                           droppedPlace != nullptr &&
                               slotPlace(*droppedPlace)) ||
            movedOutOwnerDrop(body, instruction) ||
            storeConsumedStorageValueDrop(body, instruction) ||
            ownedParameterFieldSourceDrop(program_, body, instruction)) {
          continue;
        }
        if (instruction.destination) {
          const MirPlace *resultShell =
              body.findPlace(*instruction.destination);
          if (resultShell != nullptr &&
              resultShell->root == MirPlaceRootKind::Value &&
              expectedClassResultDestinationSlot(
                  program_, body, resultShell->value) != nullptr) {
            continue;
          }
        }
        if (failureForm && address.kind == MirBodyKind::Constructor &&
            aggregateConstructorRollbackDrop(body, instruction)) {
          continue;
        }
        if (failureForm && instruction.destination) {
          if (const MirPlace *dropped =
                  body.findPlace(*instruction.destination);
              dropped != nullptr && dropped->root == MirPlaceRootKind::Value &&
              returnMoveDefinition(body, dropped->value) != nullptr) {
            continue;
          }
        }
        if (failureDestructorTarget(program_, body, instruction) != nullptr) {
          const MirPlace *dropSlot =
              instruction.destination ? body.findPlace(*instruction.destination)
                                      : nullptr;
          const MirFailureRecord *record =
              body.findFailureRecord(block.terminator.failureRecord);
          const MirBlock *parameter =
              record == nullptr ? nullptr
                                : body.findBlock(record->parameterBlock);
          const bool cleanupFailure = block.activeFailure != 0;
          if (!failureForm || dropSlot == nullptr || !slotPlace(*dropSlot) ||
              instruction.lifecycle.front().failureCleanup != cleanupFailure ||
              block.terminator.kind != MirTerminatorKind::Invoke ||
              block.terminator.invokeInstruction != instruction.id ||
              record == nullptr || record->producerBlock != block.id ||
              record->producerInstruction != instruction.id ||
              parameter == nullptr ||
              parameter->id != block.terminator.elseTarget ||
              parameter->failureParameter != record->id ||
              (cleanupFailure &&
               (parameter->activeFailure != block.activeFailure ||
                parameter->terminator.kind !=
                    MirTerminatorKind::TerminateCleanupFailure ||
                parameter->terminator.failureRecord != record->id))) {
            return false;
          }
          continue;
        }
        if (failureForm && instruction.lifecycle.size() == 1 &&
            instruction.lifecycle.front().failureCleanup &&
            instruction.destination) {
          // The failure-cleanup text destroys the slot, so the probe
          // demands the same slot place the success path demands.
          const MirPlace *cleanupSlot =
              body.findPlace(*instruction.destination);
          if (cleanupSlot == nullptr || !slotPlace(*cleanupSlot)) {
            {
              return false;
            }
          }
          continue;
        }
        const MirPlace *slot = instruction.destination
                                   ? body.findPlace(*instruction.destination)
                                   : nullptr;
        if (slot == nullptr || !slotPlace(*slot)) {
          {
            return false;
          }
        }
        continue;
      }
      case MirInstructionKind::Compute: {
        if (!instruction.result) {
          {
            return false;
          }
        }
        if (!cppMirTerminalCheckedHelperSpelling(instruction.operation)
                 .empty() &&
            !instruction.localFailureSites.empty() &&
            // A PAIRED site carries the record protocol: its report names
            // the exact source site, which the terminal helper cannot
            // spell (the cli arithmetic fixtures pin file:line and exit
            // 70). Only unpaired sites — where MIR lowered no edge — are
            // the compatibility terminal-helper shape.
            (!failureForm ? address.kind == MirBodyKind::Lambda ||
                                !instructionHasInvoke(block, instruction)
                          : !instructionHasInvoke(block, instruction))) {
          // Plain-shape checked arithmetic spells the compatibility
          // terminal helper: it contains the failure itself and never
          // returns on it, so the paired invoke edge cannot branch. The
          // operand types must equal the result type so the helper's
          // deduced result assigns without conversion.
          const auto terminalOperand = [&](const MirOperand &operand) {
            return valueOperand(operand) ||
                   (operand.kind == MirOperandKind::Constant &&
                    operand.literal.has_value() && typeRow(operand.type));
          };
          if (instruction.localFailureSites.size() != 1 ||
              instruction.definedFailure.localOrigins.size() != 1 ||
              program_.failureMetadata().findSite(
                  instruction.localFailureSites.front()) == nullptr ||
              instruction.operands.empty() || instruction.operands.size() > 2 ||
              !std::all_of(instruction.operands.begin(),
                           instruction.operands.end(), terminalOperand) ||
              !typeRow(instruction.info.type) ||
              !std::all_of(instruction.operands.begin(),
                           instruction.operands.end(),
                           [&](const MirOperand &operand) {
                             return operand.type == instruction.info.type;
                           })) {
            {
              return false;
            }
          }
          continue;
        }
        if ((!cppMirTerminalCheckedHelperSpelling(instruction.operation)
                  .empty() ||
             instruction.operation == MirOperation::Convert) &&
            (instruction.info.type.kind == SemanticType::Float ||
             instruction.info.type.kind == SemanticType::Double)) {
          // IEEE-754 nontrapping results are not defined runtime failures
          // (docs/language/execution.md): a floating site never fires,
          // the compatibility helper performs no check, and any paired
          // failure edge is dead — the terminal helper spells on both
          // forms.
          const auto floatingOperand = [&](const MirOperand &operand) {
            const bool numeric =
                operand.type.kind == SemanticType::Float ||
                operand.type.kind == SemanticType::Double ||
                constantIntegerDomain(operand.type).has_value();
            return numeric &&
                   (valueOperand(operand) ||
                    (operand.kind == MirOperandKind::Constant &&
                     operand.literal.has_value() && typeRow(operand.type)));
          };
          if (instruction.operands.empty() || instruction.operands.size() > 2 ||
              !std::all_of(instruction.operands.begin(),
                           instruction.operands.end(), floatingOperand) ||
              !typeRow(instruction.info.type)) {
            {
              return false;
            }
          }
          continue;
        }
        if (failureForm &&
            !cppMirCheckedOperationHelperSpelling(instruction.operation)
                 .empty() &&
            !instruction.localFailureSites.empty() &&
            instructionHasInvoke(block, instruction)) {
          // A checked detector spells as its status helper and its failure
          // edge writes one exact record: one site, one origin, both known
          // to the program's failure metadata.
          // Arithmetic helpers are integral-only except Convert, whose exact
          // status helper also accepts a floating source for an integral
          // target. Floating targets were discharged as IEEE-754 conversions
          // above.
          const auto integralKind = [](const SemanticType &type) {
            switch (type.kind) {
            case SemanticType::Int8:
            case SemanticType::Int16:
            case SemanticType::Int32:
            case SemanticType::Int64:
            case SemanticType::UInt8:
            case SemanticType::UInt16:
            case SemanticType::UInt32:
            case SemanticType::UInt64:
            case SemanticType::Char:
              return true;
            default: {
              return false;
            }
            }
          };
          const auto detectorOperand = [&](const MirOperand &operand) {
            const bool floatingSource =
                instruction.operation == MirOperation::Convert &&
                (operand.type.kind == SemanticType::Float ||
                 operand.type.kind == SemanticType::Double);
            return (integralKind(operand.type) || floatingSource) &&
                   (valueOperand(operand) ||
                    (operand.kind == MirOperandKind::Constant &&
                     operand.literal.has_value() && typeRow(operand.type)));
          };
          if (!integralKind(instruction.info.type)) {
            {
              return false;
            }
          }
          if (instruction.localFailureSites.size() != 1 ||
              instruction.definedFailure.localOrigins.size() != 1 ||
              program_.failureMetadata().findSite(
                  instruction.localFailureSites.front()) == nullptr ||
              instruction.operands.empty() || instruction.operands.size() > 2 ||
              !std::all_of(instruction.operands.begin(),
                           instruction.operands.end(), detectorOperand) ||
              !typeRow(instruction.info.type)) {
            {
              return false;
            }
          }
          continue;
        }
        switch (instruction.operation) {
        case MirOperation::Literal:
          if (!literalSupported(instruction.literal, instruction.info.type)) {
            {
              return false;
            }
          }
          continue;
        case MirOperation::Identity:
        case MirOperation::LogicalNot:
          if (instruction.operands.size() != 1 ||
              !valueOperand(instruction.operands.front())) {
            {
              return false;
            }
          }
          continue;
        case MirOperation::Comma:
          // The operand definitions carry evaluation order. This instruction
          // only consumes the left value and publishes the right value, so
          // keep the initial slice to passive results until MIR grows an
          // explicit ownership-transfer form for class-valued comma results.
          if (instruction.operands.size() != 2 ||
              !valueOperand(instruction.operands.front()) ||
              !valueOperand(instruction.operands.back()) ||
              instruction.operands.back().type != instruction.info.type ||
              instruction.info.traits.drop != DropKind::Trivial ||
              instruction.info.traits.containsBorrowedState ||
              !instruction.localFailureSites.empty() ||
              !instruction.definedFailure.empty() ||
              !typeRow(instruction.info.type)) {
            return false;
          }
          continue;
        case MirOperation::ExpectedHasValue:
          if (instruction.operands.size() != 1 ||
              instruction.info.type != SemanticType::Bool ||
              instruction.operands.front().type.kind !=
                  SemanticType::Expected ||
              instruction.operands.front().type.arguments.size() != 2 ||
              !(valueOperand(instruction.operands.front()) ||
                (instruction.operands.front().kind ==
                     MirOperandKind::BorrowRead &&
                 instruction.operands.front().place != 0 &&
                 body.findPlace(instruction.operands.front().place) !=
                     nullptr &&
                 body.findPlace(instruction.operands.front().place)->type ==
                     instruction.operands.front().type)) ||
              !instruction.localFailureSites.empty()) {
            {
              return false;
            }
          }
          continue;
        case MirOperation::Unexpected: {
          // Spells through the Expected capability row's construction
          // call; the result is the expected-typed value itself.
          const auto expectedRow = std::find_if(
              representations_.capabilities().begin(),
              representations_.capabilities().end(),
              [](const CppMirEmissionCapabilityRepresentation &row) {
                return row.kind == CppMirEmissionCapabilityKind::Expected;
              });
          if (instruction.operands.size() != 1 ||
              !valueOperand(instruction.operands.front()) ||
              !instruction.localFailureSites.empty() ||
              expectedRow == representations_.capabilities().end() ||
              expectedRow->spelling.empty()) {
            {
              return false;
            }
          }
          // The wrapper value never materializes (std::unexpected has no
          // default construction): its only consumer must be the Return
          // that converts it into the expected-typed result, where the
          // construction spells inline.
          {
            const std::vector<MirValueUse> &uses =
                body.usesOf(*instruction.result);
            bool returnConsumed = false;
            if (uses.size() == 1 &&
                uses.front().kind == MirValueUseKind::Terminator) {
              const MirBlock *userBlock = nullptr;
              for (const MirBlock &candidate : body.blocks) {
                if (candidate.id == uses.front().block) {
                  userBlock = &candidate;
                }
              }
              returnConsumed =
                  userBlock != nullptr &&
                  userBlock->terminator.kind == MirTerminatorKind::Return &&
                  userBlock->terminator.value &&
                  userBlock->terminator.value->kind == MirOperandKind::Value &&
                  userBlock->terminator.value->value == *instruction.result;
            }
            if (!returnConsumed) {
              {
                return false;
              }
            }
          }
          continue;
        }
        case MirOperation::Closure: {
          const MirLambdaInstance *lambda =
              instruction.lambdaTarget
                  ? program_.findLambda(*instruction.lambdaTarget)
                  : nullptr;
          const bool fused = lambda != nullptr &&
                             closureChainAdmits(program_, body, instruction);
          const MaterializedClosure materialized =
              !fused ? materializedClosure(program_, body, instruction)
                     : MaterializedClosure{};
          if (lambda == nullptr ||
              !capabilityRow(CppMirEmissionCapabilityKind::Closure) ||
              !lambdaBodyRow(lambda->id) || (!fused && !materialized) ||
              !typeRow(lambda->returnType)) {
            {
              return false;
            }
          }
          bool rows = true;
          for (const SemanticType &type : lambda->parameterTypes) {
            rows = rows && typeRow(type);
          }
          for (std::size_t index = 0; index < lambda->captureSymbols.size();
               ++index) {
            rows = rows &&
                   captureRow(lambda->id, lambda->captureSymbols[index],
                              index + 1) &&
                   lambdaDependentTypeRepresentable(
                       program_, representations_, body,
                       lambda->captureTypes[index]);
          }
          // A fused literal carries its ordinary body. A materialized closure
          // adopts this enclosing body's calling convention because later
          // invocations dispatch through the stored callable object.
          if (!rows || !supportsBodyTextImpl(
                           {.kind = MirBodyKind::Lambda, .owner = lambda->id},
                           materialized ? failureForm : false)) {
            {
              return false;
            }
          }
          continue;
        }
        case MirOperation::Positive:
        case MirOperation::BitwiseNot:
          if (instruction.operands.size() != 1 ||
              !valueOperand(instruction.operands.front()) ||
              !typeRow(instruction.info.type)) {
            {
              return false;
            }
          }
          continue;
        case MirOperation::Negate:
          // Checked negation is handled by the detector/terminal-helper
          // rules above. A failure-free negation, including a proven
          // program-constant substitution, is an ordinary unary compute.
          if (instruction.operands.size() != 1 ||
              !valueOperand(instruction.operands.front()) ||
              !instruction.localFailureSites.empty() ||
              !instruction.definedFailure.empty() ||
              (instruction.operands.front().type != instruction.info.type &&
               fusedSignedMinimumLiteral(body, instruction) == nullptr) ||
              !typeRow(instruction.info.type)) {
            return false;
          }
          continue;
        case MirOperation::Aggregate:
          // The empty aggregate spells as the row type's value
          // initialization; a fixed-array aggregate spells its staged
          // elements in operand order inside the braces, exactly like
          // the compatibility aggregate initialization.
          if ((!instruction.operands.empty() &&
               (instruction.info.type.kind != SemanticType::Array ||
                !std::all_of(
                    instruction.operands.begin(), instruction.operands.end(),
                    [&](const MirOperand &operand) {
                      return valueOperand(operand) ||
                             (operand.kind == MirOperandKind::Constant &&
                              operand.literal.has_value());
                    }))) ||
              !instruction.localFailureSites.empty() ||
              !typeRow(instruction.info.type)) {
            {
              return false;
            }
          }
          continue;
        case MirOperation::PayloadConstruct:
        case MirOperation::PayloadExtract: {
          // Both spell through the copied payload variant row: a
          // construction wraps the variant record, an extraction reads
          // one field of the proven alternative. Both are pure computes
          // over staged values.
          const CppMirEnumRepresentation *enumRow = nullptr;
          if (instruction.enumOwner) {
            for (const CppMirEnumRepresentation &row :
                 representations_.enums()) {
              if (row.owner == *instruction.enumOwner) {
                enumRow = &row;
              }
            }
          }
          const CppMirPayloadVariantRepresentation *variant = nullptr;
          if (enumRow != nullptr && instruction.enumVariant) {
            for (const CppMirPayloadVariantRepresentation &candidate :
                 enumRow->payloadVariants) {
              if (candidate.index == *instruction.enumVariant) {
                variant = &candidate;
              }
            }
          }
          const bool stagedOperands =
              std::all_of(instruction.operands.begin(),
                          instruction.operands.end(), valueOperand);
          if (variant == nullptr || variant->spelling.empty() ||
              !stagedOperands || !instruction.localFailureSites.empty() ||
              !capabilityRow(CppMirEmissionCapabilityKind::Payload) ||
              !typeRow(instruction.info.type) ||
              (instruction.operation == MirOperation::PayloadConstruct
                   ? variant->fieldTypes.size() != instruction.operands.size()
                   : instruction.operands.size() != 1 ||
                         !instruction.payloadIndex ||
                         *instruction.payloadIndex >=
                             variant->fieldTypes.size())) {
            {
              return false;
            }
          }
          continue;
        }
        case MirOperation::Convert:
          // An unchecked conversion is proven in-range by MIR. A checked
          // one in the failure form belongs to the detector rules above;
          // in the plain form its single site spells the terminal
          // numeric_cast, which contains the range failure at the site
          // exactly like the compatibility spelling.
          if (instruction.localFailureSites.size() > 1 ||
              (instruction.localFailureSites.size() == 1 &&
               program_.failureMetadata().findSite(
                   instruction.localFailureSites.front()) == nullptr) ||
              instruction.operands.size() != 1 ||
              !valueOperand(instruction.operands.front()) ||
              !typeRow(instruction.info.type)) {
            {
              return false;
            }
          }
          continue;
        case MirOperation::BitwiseAnd:
        case MirOperation::BitwiseOr:
        case MirOperation::BitwiseXor:
          if (instruction.operands.size() != 2 ||
              !valueOperand(instruction.operands[0]) ||
              !valueOperand(instruction.operands[1]) ||
              !typeRow(instruction.info.type)) {
            {
              return false;
            }
          }
          continue;
        case MirOperation::Equal:
        case MirOperation::NotEqual:
        case MirOperation::Less:
        case MirOperation::LessEqual:
        case MirOperation::Greater:
        case MirOperation::GreaterEqual: {
          // A literal comparison operand spells inline, exactly like the
          // checked detectors' constant operands.
          const auto comparisonOperand = [&](const MirOperand &operand) {
            return valueOperand(operand) ||
                   (operand.kind == MirOperandKind::Constant &&
                    operand.literal.has_value() && typeRow(operand.type));
          };
          if (instruction.operands.size() != 2 ||
              !comparisonOperand(instruction.operands[0]) ||
              !comparisonOperand(instruction.operands[1])) {
            {
              return false;
            }
          }
          continue;
        }
        case MirOperation::AddressOf: {
          const MirOperand *source = instruction.operands.size() == 1
                                         ? &instruction.operands.front()
                                         : nullptr;
          const MirPlace *place =
              source != nullptr && source->kind == MirOperandKind::Address
                  ? body.findPlace(source->place)
                  : nullptr;
          const bool exactPointer =
              instruction.info.type.kind == SemanticType::RawPointer &&
              instruction.info.type.arguments.size() == 1 &&
              source != nullptr && place != nullptr &&
              instruction.info.type.arguments.front() == source->type &&
              source->type == place->type &&
              instruction.info.type.pointerAccess == place->access;
          if (!exactPointer || !instruction.localFailureSites.empty() ||
              !instruction.definedFailure.empty() ||
              !capabilityRow(CppMirEmissionCapabilityKind::RawMemory) ||
              !typeRow(instruction.info.type)) {
            return false;
          }
          continue;
        }
        case MirOperation::PointerAdd:
        case MirOperation::PointerSubtract:
        case MirOperation::PointerDifference: {
          const auto rawPointerType = [](const SemanticType &type) {
            return type.kind == SemanticType::RawPointer &&
                   type.arguments.size() == 1 &&
                   type.arguments.front() != SemanticType::Void;
          };
          const auto pointerOperand = [&](const MirOperand &operand,
                                          const SemanticType &type) {
            return valueOperand(operand) && operand.type == type &&
                   rawPointerType(operand.type);
          };
          const auto offsetOperand = [&](const MirOperand &operand) {
            const bool exactConstant =
                operand.kind == MirOperandKind::Constant &&
                operand.value == 0 && operand.place == 0 && operand.loan == 0 &&
                literalSupported(operand.literal, operand.type);
            return rawPointerOffsetType(operand.type) &&
                   (valueOperand(operand) || exactConstant);
          };
          bool exactOperation = instruction.operands.size() == 2;
          if (exactOperation &&
              instruction.operation == MirOperation::PointerAdd) {
            exactOperation = rawPointerType(instruction.info.type) &&
                             ((pointerOperand(instruction.operands[0],
                                              instruction.info.type) &&
                               offsetOperand(instruction.operands[1])) ||
                              (offsetOperand(instruction.operands[0]) &&
                               pointerOperand(instruction.operands[1],
                                              instruction.info.type)));
          } else if (exactOperation &&
                     instruction.operation == MirOperation::PointerSubtract) {
            exactOperation = rawPointerType(instruction.info.type) &&
                             pointerOperand(instruction.operands[0],
                                            instruction.info.type) &&
                             offsetOperand(instruction.operands[1]);
          } else if (exactOperation) {
            exactOperation =
                instruction.info.type == SemanticType::Int64 &&
                pointerOperand(instruction.operands[0],
                               instruction.operands[0].type) &&
                instruction.operands[0].type == instruction.operands[1].type &&
                valueOperand(instruction.operands[1]);
          }
          if (!exactOperation || !instruction.localFailureSites.empty() ||
              !instruction.definedFailure.empty() ||
              !capabilityRow(CppMirEmissionCapabilityKind::RawMemory) ||
              !typeRow(instruction.info.type)) {
            return false;
          }
          continue;
        }
        case MirOperation::Index: {
          // The plain form uses the terminal string_view_at helper; the
          // failure form writes a status and follows the exact Invoke edge.
          if (instruction.operands.size() != 2 ||
              !valueOperand(instruction.operands[0]) ||
              !valueOperand(instruction.operands[1]) ||
              instruction.operands[0].type.kind != SemanticType::StringView ||
              !instruction.result || !typeRow(instruction.info.type) ||
              (failureForm &&
               (!stringViewIndexFailureSite(program_, instruction) ||
                !invokePairedInstruction(body, instruction.id)))) {
            {
              return false;
            }
          }
          continue;
        }
        default: {
          return false;
        }
        }
      }
      case MirInstructionKind::Load: {
        if (!instruction.result || instruction.operands.size() != 1 ||
            instruction.operands.front().place == 0) {
          {
            return false;
          }
        }
        if (const MirPlace *source =
                body.findPlace(instruction.operands.front().place);
            source != nullptr) {
          if (slotPlace(*source) &&
              source->type.kind == SemanticType::Expected &&
              expectedObserverLoadConsumer(body, instruction) == nullptr) {
            return false;
          }
          if (const std::optional<ClassSubscriptAccess> subscript =
                  classSubscriptAccess(program_, body, *source)) {
            // The subscript member call contains failure terminally in
            // its own emitted body, so the plain call is exact on both
            // text forms and any paired invoke edge is a plain goto.
            if (containedSubscriptMember(program_, representations_,
                                         subscript->owner,
                                         ReceiverMutability::ReadOnly,
                                         subscript->indexType) == nullptr ||
                !typeRow(instruction.info.type)) {
              return false;
            }
            continue;
          }
        }
        if (!instruction.localFailureSites.empty()) {
          // A bounds-checked element load is a failure detector: exactly
          // one site and origin, spelled through the checked-read helper.
          const MirPlace *source =
              body.findPlace(instruction.operands.front().place);
          // The failure form detects through the checked-read helper; the
          // plain shape spells the terminal array_at accessor, which
          // contains the bounds failure inside itself.
          if (source == nullptr ||
              (!arrayElementAccess(body, *source) &&
               !bindingArrayFieldElementAccess(body, *source) &&
               !receiverArrayElementAccess(body, *source)) ||
              instruction.localFailureSites.size() != 1 ||
              instruction.definedFailure.localOrigins.size() != 1 ||
              program_.failureMetadata().findSite(
                  instruction.localFailureSites.front()) == nullptr ||
              !typeRow(instruction.info.type)) {
            {
              return false;
            }
          }
        }
        continue;
      }
      case MirInstructionKind::Initialize: {
        if (address.kind == MirBodyKind::Module &&
            moduleDataOnlyInitialization(program_, instruction) != nullptr) {
          continue;
        }
        if (!instruction.destination || instruction.operands.size() != 1) {
          {
            return false;
          }
        }
        const MirPlace *destination = body.findPlace(*instruction.destination);
        // A store into a receiver field is only expressible through the
        // mutable-receiver binding; under a read-only receiver the text step
        // would bind the field const and the emitted C++ would not compile.
        if (destination != nullptr &&
            destination->root == MirPlaceRootKind::This &&
            receiverMutability != ReceiverMutability::Mutable) {
          {
            return false;
          }
        }
        if (destination != nullptr &&
            classSubscriptAccess(program_, body, *destination)) {
          // An initializing subscript store uses the same mutable member
          // call as an assignment.
          if (const std::optional<ClassSubscriptAccess> subscript =
                  classSubscriptAccess(program_, body, *destination);
              containedSubscriptMember(program_, representations_,
                                       subscript->owner,
                                       ReceiverMutability::Mutable,
                                       subscript->indexType) == nullptr ||
              !valueOperand(instruction.operands.front())) {
            return false;
          } else {
            continue;
          }
        }
        if (destination != nullptr && slotPlace(*destination)) {
          // The reparenting Initialize is the slot construct's paired
          // destination and emits as a comment only.
          if (!valueOperand(instruction.operands.front())) {
            {
              return false;
            }
          }
          continue;
        }
        if (destination != nullptr &&
            destination->root == MirPlaceRootKind::Binding &&
            destination->projections.empty() &&
            destination->type.kind == SemanticType::Reference) {
          if (referenceBindingAddressLoan(program_, body, instruction) ==
              nullptr) {
            return false;
          }
          continue;
        }
        if (!valueOperand(instruction.operands.front()) &&
            !syntheticBool(instruction.operands.front())) {
          {
            return false;
          }
        }
        continue;
      }
      case MirInstructionKind::Assign: {
        if (!instruction.destination || !instruction.result ||
            instruction.operands.size() != 1 ||
            !valueOperand(instruction.operands.front())) {
          {
            return false;
          }
        }
        const MirPlace *destination = body.findPlace(*instruction.destination);
        if (destination != nullptr &&
            destination->root == MirPlaceRootKind::This &&
            receiverMutability != ReceiverMutability::Mutable) {
          {
            return false;
          }
        }
        if (destination != nullptr) {
          if (const std::optional<ClassSubscriptAccess> subscript =
                  classSubscriptAccess(program_, body, *destination)) {
            // The mutable subscript member call contains failure
            // terminally in its own emitted body; the plain call is
            // exact on both text forms.
            if (containedSubscriptMember(program_, representations_,
                                         subscript->owner,
                                         ReceiverMutability::Mutable,
                                         subscript->indexType) == nullptr) {
              return false;
            }
            continue;
          }
        }
        if (!instruction.localFailureSites.empty()) {
          // A checked compound assignment into a bare scalar local is the
          // same detector shape as the value-level checked compute: the
          // base operation's status helper reads the place, checks, and
          // commits, and the paired invoke consumes the status.
          const bool checkedCompound =
              failureForm && instruction.kind == MirInstructionKind::Assign &&
              !cppMirCompoundCheckedHelperSpelling(instruction.operation)
                   .empty() &&
              destination != nullptr &&
              destination->root == MirPlaceRootKind::Binding &&
              // Either a bare local, or the dereference-projected
              // reference place whose sibling pointer carrier the store
              // path already spells (ADR 018 Â§4).
              (destination->projections.empty() ||
               (destination->projections.size() == 1 &&
                destination->projections[0].kind ==
                    MirProjectionKind::Dereference &&
                std::any_of(body.places.begin(), body.places.end(),
                            [&](const MirPlace &candidate) {
                              return candidate.id != destination->id &&
                                     candidate.root ==
                                         MirPlaceRootKind::Binding &&
                                     candidate.binding ==
                                         destination->binding &&
                                     candidate.projections.empty();
                            }))) &&
              expectedTypeRepresentation(destination->type) ==
                  CppMirTypeRepresentationKind::Scalar &&
              typeRow(destination->type) && instruction.operands.size() == 1 &&
              (valueOperand(instruction.operands.front()) ||
               (instruction.operands.front().kind == MirOperandKind::Constant &&
                instruction.operands.front().literal.has_value())) &&
              instruction.localFailureSites.size() == 1 &&
              instruction.definedFailure.localOrigins.size() == 1 &&
              program_.failureMetadata().findSite(
                  instruction.localFailureSites.front()) != nullptr &&
              invokePairedInstruction(body, instruction.id);
          // A bounds-checked element store is a failure detector spelled
          // through the checked-write helper.
          if (!checkedCompound &&
              (!failureForm || destination == nullptr ||
               (!arrayElementAccess(body, *destination) &&
                !bindingArrayFieldElementAccess(body, *destination) &&
                !receiverArrayElementAccess(body, *destination)) ||
               instruction.localFailureSites.size() != 1 ||
               instruction.definedFailure.localOrigins.size() != 1 ||
               program_.failureMetadata().findSite(
                   instruction.localFailureSites.front()) == nullptr)) {
            {
              return false;
            }
          }
        }
        continue;
      }
      case MirInstructionKind::Move: {
        // By-value staging moves one exact place. A checked fixed-array
        // element move without an Invoke edge is terminally contained by the
        // same array_at guard as a checked load.
        const MirPlace *movedSource =
            instruction.operands.size() == 1
                ? body.findPlace(instruction.operands.front().place)
                : nullptr;
        const bool terminalElementMove =
            movedSource != nullptr &&
            (arrayElementAccess(body, *movedSource) ||
             bindingArrayFieldElementAccess(body, *movedSource) ||
             receiverArrayElementAccess(body, *movedSource)) &&
            instruction.localFailureSites.size() == 1 &&
            instruction.definedFailure.localOrigins.size() == 1 &&
            program_.failureMetadata().findSite(
                instruction.localFailureSites.front()) != nullptr &&
            !invokePairedInstruction(body, instruction.id);
        if (!instruction.result || instruction.operands.size() != 1 ||
            instruction.operands.front().kind != MirOperandKind::Move ||
            instruction.operands.front().place == 0 || movedSource == nullptr ||
            (!instruction.localFailureSites.empty() && !terminalElementMove)) {
          {
            return false;
          }
        }
        continue;
      }
      case MirInstructionKind::CallInput: {
        if (!instruction.result || instruction.receiver ||
            instruction.operands.size() != 1) {
          {
            return false;
          }
        }
        const MirOperand &staged = instruction.operands.front();
        if (staged.kind == MirOperandKind::BorrowRead ||
            staged.kind == MirOperandKind::BorrowWrite) {
          // A borrowed call input stages a place the call spells directly;
          // the write form carries the mutable receiver.
          if (staged.place == 0 || body.findPlace(staged.place) == nullptr) {
            {
              return false;
            }
          }
          pendingStaged.push_back(*instruction.result);
          continue;
        }
        if (loanStagedCallInput(body, *instruction.result) == &instruction) {
          // The loan-staged input spells its dereferenced pointer carrier
          // at the consuming call; nothing stages here.
          continue;
        }
        if (copyStagedCallInput(body, *instruction.result) == &instruction) {
          // The by-value copy stage spells its source place at the
          // consuming call; the temporary never materializes.
          continue;
        }
        if (!valueOperand(staged)) {
          {
            return false;
          }
        }
        continue;
      }
      case MirInstructionKind::Call: {
        if (storageReferenceReadCall(body, instruction)) {
          // The element publishes through the loan-producing Borrow: the
          // call needs its staged storage place, a value index, and a
          // result no other instruction consumes.
          if (instruction.functionTarget || instruction.operands.size() != 2 ||
              storageStagedPlace(body, instruction.operands.front()) ==
                  nullptr ||
              !valueOperand(instruction.operands.back()) ||
              !instruction.result || !typeRow(instruction.info.type)) {
            {
              return false;
            }
          }
          if (instruction.receiver &&
              ((instruction.receiver->kind != MirOperandKind::BorrowRead &&
                instruction.receiver->kind != MirOperandKind::BorrowWrite) ||
               instruction.receiver->place == 0 ||
               body.findPlace(instruction.receiver->place) == nullptr)) {
            {
              return false;
            }
          }
          continue;
        }
        if (instruction.intrinsic == IntrinsicKind::PrefixStorageLength) {
          // The logical-length read carries no failure and spells the
          // shipped helper over the staged storage place on both forms.
          if (instruction.functionTarget || !instruction.result ||
              !instruction.localFailureSites.empty() ||
              instruction.operands.size() != 1 ||
              storageStagedPlace(body, instruction.operands.front()) ==
                  nullptr ||
              !typeRow(instruction.info.type)) {
            {
              return false;
            }
          }
          continue;
        }
        if (instruction.intrinsic == IntrinsicKind::AllocateStorage) {
          if (!failureForm ||
              !storageAllocationFailureCall(program_, instruction) ||
              !typeRow(instruction.info.type) ||
              !typeRow(instruction.info.type.arguments.front()) ||
              !typeRow(instruction.operands.front().type) ||
              !invokePairedInstruction(body, instruction.id)) {
            return false;
          }
          continue;
        }
        if (sparseStorageIntrinsic(instruction.intrinsic)) {
          std::size_t expectedOperands = 0;
          switch (instruction.intrinsic) {
          case IntrinsicKind::StorageConstruct:
            expectedOperands = 2;
            break;
          case IntrinsicKind::StorageDestroy:
            expectedOperands = 2;
            break;
          case IntrinsicKind::StorageRelocate:
          case IntrinsicKind::StorageShiftRight:
          case IntrinsicKind::StorageShiftLeft:
            expectedOperands = 3;
            break;
          default:
            break;
          }
          const bool variadicConstruct =
              instruction.intrinsic == IntrinsicKind::StorageConstruct;
          const bool operandCountReady =
              variadicConstruct
                  ? instruction.operands.size() >= expectedOperands
                  : instruction.operands.size() == expectedOperands;
          const MirPlace *firstStorage =
              instruction.operands.empty()
                  ? nullptr
                  : storageStagedPlace(body, instruction.operands.front());
          const bool receiverReady =
              !instruction.receiver ||
              ((instruction.receiver->kind == MirOperandKind::BorrowRead ||
                instruction.receiver->kind == MirOperandKind::BorrowWrite) &&
               instruction.receiver->place != 0 &&
               body.findPlace(instruction.receiver->place) != nullptr);
          if (!failureForm || !invokePairedInstruction(body, instruction.id) ||
              instruction.functionTarget || instruction.lambdaTarget ||
              instruction.bodyTarget || instruction.callableInvocation ||
              instruction.result || !operandCountReady ||
              firstStorage == nullptr ||
              firstStorage->type.kind != SemanticType::Storage ||
              firstStorage->type.arguments.size() != 1 ||
              instruction.localFailureSites.size() != 1 ||
              instruction.definedFailure.localOrigins.size() != 1 ||
              program_.failureMetadata().findSite(
                  instruction.localFailureSites.front()) == nullptr ||
              !receiverReady || !typeRow(firstStorage->type) ||
              !typeRow(firstStorage->type.arguments.front()) ||
              (instruction.constructorTarget && !variadicConstruct)) {
            return false;
          }
          for (std::size_t index = 1; index < instruction.operands.size();
               ++index) {
            if (storageStagedPlace(body, instruction.operands[index]) !=
                nullptr) {
              if (instruction.intrinsic != IntrinsicKind::StorageRelocate ||
                  index != 1) {
                return false;
              }
              continue;
            }
            if (!valueOperand(instruction.operands[index]) ||
                !typeRow(instruction.operands[index].type)) {
              return false;
            }
          }
          for (const MirOperand &operand : instruction.operands) {
            const MirInstruction *stage = definitionFor(body, operand);
            if (stage == nullptr ||
                stage->kind != MirInstructionKind::CallInput ||
                !stage->result ||
                storageStagedPlace(body, operand) == nullptr) {
              continue;
            }
            pendingStaged.erase(std::remove(pendingStaged.begin(),
                                            pendingStaged.end(),
                                            *stage->result),
                                pendingStaged.end());
          }
          continue;
        }
        if (prefixStorageIntrinsic(instruction.intrinsic)) {
          // The storage failure form spells the shipped mir_prefix_*_v1
          // checked helper over the staged storage place lvalue; every
          // operation carries checkable sites, so the success form
          // declines — except the plain allocation, which contains
          // exhaustion terminally inside the compatibility helper and
          // publishes into its storage-typed result. The modeling
          // receiver is a raw borrow of a spellable place and never
          // spells.
          const bool plainTerminalAllocation =
              !failureForm &&
              instruction.intrinsic == IntrinsicKind::AllocatePrefixStorage &&
              !instruction.functionTarget &&
              instruction.localFailureSites.size() == 1 &&
              program_.failureMetadata().findSite(
                  instruction.localFailureSites.front()) != nullptr;
          // An append with no failure edge contains its exhaustion
          // terminally inside the compatibility helper in either form.
          const bool terminalAppend =
              instruction.intrinsic == IntrinsicKind::PrefixStorageAppend &&
              !instruction.functionTarget &&
              instruction.localFailureSites.size() == 1 &&
              program_.failureMetadata().findSite(
                  instruction.localFailureSites.front()) != nullptr &&
              !instructionHasInvoke(block, instruction) &&
              instruction.operands.size() == 2;
          if (!plainTerminalAllocation && !terminalAppend &&
              (!failureForm || instruction.functionTarget ||
               instruction.localFailureSites.empty() ||
               instruction.definedFailure.localOrigins.empty() ||
               program_.failureMetadata().findSite(
                   instruction.localFailureSites.front()) == nullptr)) {
            {
              return false;
            }
          }
          if (instruction.intrinsic == IntrinsicKind::AllocatePrefixStorage) {
            // Allocation publishes into its storage-typed result value;
            // interior exhaustion keeps the sealed legacy path.
            if (!instruction.result || instruction.operands.size() != 1 ||
                !valueOperand(instruction.operands.front()) ||
                !typeRow(instruction.info.type) ||
                instruction.info.type.arguments.size() != 1 ||
                !typeRow(instruction.info.type.arguments.front())) {
              {
                return false;
              }
            }
            // A growth-path allocation borrows the container it will
            // replace as its modeling receiver; the receiver is a raw
            // borrow of a spellable place and never spells, exactly like
            // the other storage intrinsics below.
            if (instruction.receiver &&
                ((instruction.receiver->kind != MirOperandKind::BorrowRead &&
                  instruction.receiver->kind != MirOperandKind::BorrowWrite) ||
                 instruction.receiver->place == 0 ||
                 body.findPlace(instruction.receiver->place) == nullptr)) {
              {
                return false;
              }
            }
            continue;
          }
          if (instruction.result || instruction.operands.empty()) {
            {
              return false;
            }
          }
          if (instruction.receiver &&
              ((instruction.receiver->kind != MirOperandKind::BorrowRead &&
                instruction.receiver->kind != MirOperandKind::BorrowWrite) ||
               instruction.receiver->place == 0 ||
               body.findPlace(instruction.receiver->place) == nullptr)) {
            {
              return false;
            }
          }
          if (storageStagedPlace(body, instruction.operands.front()) ==
              nullptr) {
            {
              return false;
            }
          }
          for (std::size_t index = 1; index < instruction.operands.size();
               ++index) {
            if (storageStagedPlace(body, instruction.operands[index]) !=
                nullptr) {
              // Relocation's destination stages a second storage place.
              continue;
            }
            if (!valueOperand(instruction.operands[index])) {
              {
                return false;
              }
            }
          }
          for (const MirOperand &operand : instruction.operands) {
            const MirInstruction *stage = definitionFor(body, operand);
            if (stage == nullptr ||
                stage->kind != MirInstructionKind::CallInput ||
                !stage->result ||
                storageStagedPlace(body, operand) == nullptr) {
              continue;
            }
            pendingStaged.erase(std::remove(pendingStaged.begin(),
                                            pendingStaged.end(),
                                            *stage->result),
                                pendingStaged.end());
          }
          continue;
        }
        if (callableValueInvocation(instruction)) {
          // A plain body may invoke a fused literal or a staged callable
          // parameter through its ordinary signature. A transformed body is
          // stricter: only an exact fused literal whose own MIR body supports
          // the transformed convention can forward the current record and
          // preserve this frame's cleanup edge.
          const MirInstruction *closure =
              closureChainDefinition(body, instruction.receiver->value);
          const MirLambdaInstance *lambda =
              closure != nullptr && closure->lambdaTarget
                  ? program_.findLambda(*closure->lambdaTarget)
                  : nullptr;
          const bool fusedLiteral =
              closure != nullptr &&
              closureChainAdmits(program_, body, *closure);
          const MirPlace *materializedPlace = materializedCallableReceiverPlace(
              program_, body, instruction.receiver->value);
          const bool stagedPlace =
              callableTemplateBody &&
              callableReceiverStage(body, instruction.receiver->value) !=
                  nullptr;
          const bool transformedLiteral =
              !failureForm ||
              (lambda != nullptr &&
               supportsBodyTextImpl(
                   {.kind = MirBodyKind::Lambda, .owner = lambda->id}, true));
          const MirLambdaInstance *stagedLambda =
              instruction.lambdaTarget
                  ? program_.findLambda(*instruction.lambdaTarget)
                  : nullptr;
          const bool transformedMaterialized =
              !failureForm || materializedPlace == nullptr ||
              (stagedLambda != nullptr &&
               supportsBodyTextImpl(
                   {.kind = MirBodyKind::Lambda, .owner = stagedLambda->id},
                   true));
          const bool transformedStaged =
              !failureForm || !stagedPlace ||
              (stagedLambda != nullptr &&
               supportsBodyTextImpl(
                   {.kind = MirBodyKind::Lambda, .owner = stagedLambda->id},
                   true));
          // An invocation argument is a staged value or a loan of an
          // admitted entry parameter (spelled as the dereferenced
          // pointer carrier; the loan loop above vetted every loan).
          const auto invocationOperand = [&](const MirOperand &operand) {
            return valueOperand(operand) ||
                   (operand.kind == MirOperandKind::Loan && operand.loan != 0 &&
                    body.findLoan(operand.loan) != nullptr);
          };
          if (!pendingStaged.empty() ||
              !capabilityRow(CppMirEmissionCapabilityKind::Closure) ||
              !capabilityRow(CppMirEmissionCapabilityKind::CallableDispatch) ||
              (!fusedLiteral && !stagedPlace && materializedPlace == nullptr) ||
              (failureForm && fusedLiteral && !transformedLiteral) ||
              !transformedMaterialized ||
              (failureForm && stagedPlace && !transformedStaged) ||
              (failureForm && stagedPlace &&
               (instruction.info.type.kind == SemanticType::Reference ||
                instruction.info.type.kind == SemanticType::Class ||
                instruction.info.type.kind == SemanticType::Lambda)) ||
              !std::all_of(instruction.operands.begin(),
                           instruction.operands.end(), invocationOperand) ||
              (instruction.result && !typeRow(instruction.info.type))) {
            {
              return false;
            }
          }
          continue;
        }
        // A receiver-carrying call spells its staged borrowed place and the
        // qualified member name — the explicit qualification states the
        // static dispatch MIR proved. Admission requires a read-only,
        // source-defined GTI member target; write-staged receivers wait
        // for measured demand.
        std::vector<MirValueId> consumedStaged;
        if (instruction.receiver &&
            (instruction.intrinsic == IntrinsicKind::StringViewSize ||
             instruction.intrinsic == IntrinsicKind::StringViewEmpty ||
             instruction.intrinsic == IntrinsicKind::ArraySize ||
             instruction.intrinsic == IntrinsicKind::ExpectedValue ||
             instruction.intrinsic == IntrinsicKind::ExpectedError)) {
          // A builtin member read spells the staged place's member
          // directly: the view size/empty pair is failure-free, and the
          // expected extractions contain their wrong-state failure
          // terminally inside the spelled member itself.
          const bool expectedExtraction =
              instruction.intrinsic == IntrinsicKind::ExpectedValue ||
              instruction.intrinsic == IntrinsicKind::ExpectedError;
          const bool voidExpectedValue =
              instruction.intrinsic == IntrinsicKind::ExpectedValue &&
              instruction.receiver->type.kind == SemanticType::Expected &&
              instruction.receiver->type.arguments.size() == 2 &&
              instruction.receiver->type.arguments.front() ==
                  SemanticType::Void &&
              instruction.info.type == SemanticType::Void &&
              !instruction.result;
          const MirInstruction *staged =
              borrowStagedCallInput(body, *instruction.receiver);
          const MirOperand &receiverBorrow = staged != nullptr
                                                 ? staged->operands.front()
                                                 : *instruction.receiver;
          const MirPlace *viewPlace =
              receiverBorrow.kind == MirOperandKind::BorrowRead &&
                      receiverBorrow.place != 0
                  ? body.findPlace(receiverBorrow.place)
                  : nullptr;
          if (viewPlace == nullptr ||
              (!instruction.result && !voidExpectedValue) ||
              !instruction.operands.empty() ||
              instruction.localFailureSites.size() >
                  (expectedExtraction ? 1u : 0u) ||
              instruction.dispatch != CallDispatch::Static ||
              !typeRow(instruction.info.type)) {
            {
              return false;
            }
          }
          if (staged != nullptr) {
            consumedStaged.push_back(*staged->result);
          }
          if (!std::all_of(
                  consumedStaged.begin(), consumedStaged.end(),
                  [&](MirValueId id) {
                    return std::find(pendingStaged.begin(), pendingStaged.end(),
                                     id) != pendingStaged.end() ||
                           stagedAcrossInvokeSuccessChain(id, block.id);
                  })) {
            {
              return false;
            }
          }
          for (const MirValueId id : consumedStaged) {
            pendingStaged.erase(
                std::remove(pendingStaged.begin(), pendingStaged.end(), id),
                pendingStaged.end());
          }
          continue;
        }
        if (instruction.receiver &&
            (instruction.intrinsic == IntrinsicKind::UniqueOwnerBorrow ||
             instruction.intrinsic == IntrinsicKind::UniqueOwnerBorrowMut)) {
          // The owner borrow validates in its dedicated intrinsic arm; its
          // receiver borrows the owner field directly with no staging step.
          continue;
        }
        if (instruction.receiver &&
            instruction.intrinsic == IntrinsicKind::ExpectedValueOr) {
          // value_or reads either a declared Expected SSA value or one exact
          // read-borrowed place. The latter spells the live place directly;
          // neither route uses a CallInput staging schedule.
          const MirPlace *receiverPlace =
              instruction.receiver->kind == MirOperandKind::BorrowRead &&
                      instruction.receiver->place != 0
                  ? body.findPlace(instruction.receiver->place)
                  : nullptr;
          const bool valueReceiver =
              instruction.receiver->kind == MirOperandKind::Value &&
              instruction.receiver->value != 0 &&
              body.findValue(instruction.receiver->value) != nullptr;
          if (!instruction.result || instruction.operands.size() != 1 ||
              !valueOperand(instruction.operands.front()) ||
              !typeRow(instruction.info.type) ||
              (!valueReceiver &&
               (receiverPlace == nullptr ||
                receiverPlace->type != instruction.receiver->type))) {
            return false;
          }
          continue;
        }
        if (instruction.receiver) {
          if (instruction.intrinsic != IntrinsicKind::None ||
              // A virtual call spells the ordinary member call through
              // the base-typed receiver — C++ carries the dispatch — so
              // it only demands the named callee's body-name row like
              // the static member call.
              (instruction.dispatch != CallDispatch::Static &&
               !(instruction.dispatch == CallDispatch::Virtual &&
                 instruction.functionTarget &&
                 bodyRow(*instruction.functionTarget)))) {
            {
              return false;
            }
          }
          // The receiver borrow arrives staged through a CallInput or
          // directly on the receiver operand (a self-member call borrows
          // its own receiver with no staging step); both name a spellable
          // place. A MoveValue stage is receiver transport rather than an
          // overload qualifier: semantic resolution may intentionally fall
          // back from an explicit move to a mutable or read-only operator().
          const MirInstruction *staged =
              borrowStagedCallInput(body, *instruction.receiver);
          const MirOperand &receiverBorrow = staged != nullptr
                                                 ? staged->operands.front()
                                                 : *instruction.receiver;
          const MirInstruction *movedStage =
              staged == nullptr &&
                      instruction.receiver->kind == MirOperandKind::Value
                  ? copyStagedCallInput(body, instruction.receiver->value)
                  : nullptr;
          const StagedTemporarySource movedSource =
              movedStage != nullptr
                  ? stagedTemporarySourceFor(body, *movedStage)
                  : StagedTemporarySource{};
          const DirectTemporaryReceiver temporaryReceiver =
              directTemporaryReceiver(body, instruction);
          const MirPlace *receiverPlace =
              movedSource.place != nullptr && movedSource.moved
                  ? movedSource.place
                  : ((receiverBorrow.kind == MirOperandKind::BorrowRead ||
                      receiverBorrow.kind == MirOperandKind::BorrowWrite) &&
                             receiverBorrow.place != 0
                         ? body.findPlace(receiverBorrow.place)
                         : temporaryReceiver.slot);
          if (receiverPlace == nullptr) {
            {
              return false;
            }
          }
          const MirFunctionInstance *target =
              instruction.functionTarget
                  ? program_.findFunctionInstance(*instruction.functionTarget)
                  : nullptr;
          const ReceiverMutability stagedMutability =
              movedSource.place != nullptr && movedSource.moved
                  ? ReceiverMutability::Consuming
                  : (temporaryReceiver ? ReceiverMutability::ReadOnly
                     : receiverBorrow.kind == MirOperandKind::BorrowWrite
                         ? ReceiverMutability::Mutable
                         : ReceiverMutability::ReadOnly);
          const bool virtualFailureTarget =
              target != nullptr &&
              virtualFailureCallee(program_, instruction) == target;
          const bool ordinaryStaticTarget =
              target != nullptr &&
              instruction.dispatch == CallDispatch::Static &&
              !target->virtualMethod && !target->pureVirtual &&
              !target->overrideMethod &&
              target->definitionKind ==
                  MirFunctionInstance::DefinitionKind::Source;
          const bool receiverMutabilityMatches =
              target != nullptr &&
              (target->receiverMutability == stagedMutability ||
               (stagedMutability == ReceiverMutability::Consuming &&
                instruction.callableInvocation &&
                *instruction.callableInvocation ==
                    callableInvocationCapability(target->receiverMutability)));
          if (target == nullptr || !target->owner || target->staticMember ||
              (!ordinaryStaticTarget && !virtualFailureTarget) ||
              !receiverMutabilityMatches ||
              target->linkage != LanguageLinkage::Gti) {
            {
              return false;
            }
          }
          // The staged place must BE the member's owner object: a place
          // may also be a concrete derived object whose structural base graph
          // contains that owner. C++ member lookup then performs the exact
          // base adjustment; an unrelated wrapper or implicit owner
          // dereference still fails closed.
          const MirClassInstance *ownerInstance =
              program_.findClassInstance(*target->owner);
          if (ownerInstance == nullptr ||
              (instruction.dispatch == CallDispatch::Virtual &&
               !typeRow(instruction.dispatchOwner)) ||
              !classTypeIsOrDerivesFrom(program_, receiverPlace->type,
                                        ownerInstance->id)) {
            {
              return false;
            }
          }
          if (staged != nullptr) {
            consumedStaged.push_back(*staged->result);
          }
        }
        if (!failureForm && instruction.functionTarget) {
          // The success form spells plain calls; a failure-capable GTI
          // callee needs the transformed convention and stays with the
          // failure form fail-closed. Inside a deduced-callable template
          // body every reachable callee convention is terminally
          // contained (the compatibility wrapper or another template), so
          // the plain call is exact there.
          const MirFunctionInstance *target =
              program_.findFunctionInstance(*instruction.functionTarget);
          if (target == nullptr ||
              (!callableTemplateBody &&
               !deducedCallableCallee(program_, instruction) &&
               !terminallyContainedPlainCallee(program_, representations_,
                                               instruction) &&
               !terminallyContainedMemberCallee(program_, representations_,
                                                instruction) &&
               !wrapperContainedCallSite(program_, representations_, body,
                                         block, instruction) &&
               target->mayRaiseDefinedFailure &&
               target->linkage == LanguageLinkage::Gti &&
               target->definitionKind ==
                   MirFunctionInstance::DefinitionKind::Source)) {
            {
              return false;
            }
          }
        }
        // A paired call-result loan owns the publication: the plain call
        // spells the loan capture, so a direct consumer of the result
        // value would read the never-assigned local instead of the
        // loan's referent.
        if (!failureForm && instruction.result &&
            producedCallResultLoan(body, instruction) != nullptr &&
            !nonRootRecordUses(body, *instruction.result).empty()) {
          {
            return false;
          }
        }
        if (!failureForm && instruction.result &&
            instruction.info.type.kind == SemanticType::Class &&
            returnCallDefinition(body, *instruction.result) == nullptr &&
            returnDefaultConstructionDefinition(body, *instruction.result) ==
                nullptr &&
            classValuePublicationSlot(body, *instruction.result) == nullptr &&
            !constructorFieldResultSlot(program_, body, *instruction.result) &&
            !passiveCAbiCallAssignmentResult(program_, body,
                                             *instruction.result) &&
            // A nested contained-member result spells inline at its
            // consuming argument and needs no local either.
            inlineNestedCallResult(program_, representations_, body,
                                   *instruction.result) == nullptr) {
          // A plain class result publishes only at its consuming return;
          // any other consumer would need an undeclarable class local.
          {
            return false;
          }
        }
        if (failureForm) {
          const MirFunctionInstance *target =
              instruction.functionTarget
                  ? program_.findFunctionInstance(*instruction.functionTarget)
                  : nullptr;
          if (instruction.functionTarget && target == nullptr) {
            {
              return false;
            }
          }
          if (target != nullptr && target->mayRaiseDefinedFailure &&
              terminallyContainedPlainCallee(program_, representations_,
                                             instruction) &&
              target->callableParameters.empty()) {
            // The callee's own plain body contains its failure terminally,
            // so this call spells the plain name and its invoke edge is a
            // plain goto; only the result row is demanded.
            if (instruction.result && !typeRow(instruction.info.type)) {
              {
                return false;
              }
            }
          } else if (target != nullptr && target->mayRaiseDefinedFailure &&
                     target->linkage == LanguageLinkage::Gti &&
                     (target->definitionKind == MirDefinitionKind::Source ||
                      virtualFailureCallee(program_, instruction) == target)) {
            // A failure-capable callee is reached through the transformed
            // convention: passive values publish into a typed result,
            // while a class value placement-constructs the exact consuming
            // lifetime slot (or an empty discard slot), and the caller
            // forwards its failure-record pointer.
            // Whether the callee's transformed body actually exists is the
            // admission fixpoint's question, answered before any body from
            // this program is selected.
            if (!invokePairedInstruction(body, instruction.id) &&
                !failureStatusCannotFail(target->body)) {
              return false;
            }
            const std::optional<CppMirTypeRepresentationKind> returnKind =
                cppMirExpectedTypeRepresentation(target->returnType);
            bool callableArgumentsReady = true;
            for (const MirCallableParameter &parameter :
                 target->callableParameters) {
              if (parameter.parameterIndex >= instruction.operands.size()) {
                callableArgumentsReady = false;
                break;
              }
              const MirOperand &operand =
                  instruction.operands[parameter.parameterIndex];
              const MirInstruction *closure =
                  operand.kind == MirOperandKind::Value
                      ? closureChainDefinition(body, operand.value)
                      : nullptr;
              const MirLambdaInstance *lambda =
                  closure != nullptr && closure->lambdaTarget
                      ? program_.findLambda(*closure->lambdaTarget)
                      : nullptr;
              const bool stagedTemplateCallable =
                  operand.type.kind == SemanticType::Lambda &&
                  callableArgumentStage(body, operand.value) != nullptr;
              const MirPlace *movedCallable =
                  operand.type.kind == SemanticType::Lambda
                      ? movedPlaceChainSource(body, operand.value, instruction)
                      : nullptr;
              const MaterializedClosure materializedCallable =
                  movedCallable != nullptr
                      ? materializedClosureForType(program_, body, operand.type)
                      : MaterializedClosure{};
              const bool movedMaterializedCallable =
                  movedCallable != nullptr && materializedCallable &&
                  supportsBodyTextImpl(
                      {.kind = MirBodyKind::Lambda,
                       .owner = materializedCallable.lambda->id},
                      true);
              const bool lambdaReady =
                  operand.type.kind == SemanticType::Lambda &&
                  (stagedTemplateCallable || movedMaterializedCallable ||
                   (lambda != nullptr &&
                    supportsBodyTextImpl(
                        {.kind = MirBodyKind::Lambda, .owner = lambda->id},
                        true)));
              // A concrete class callable is nameable at this call site and
              // emitCallArgument already spells its staged value. Its own
              // invocation convention is verified in the callee body.
              const bool classReady =
                  operand.type.kind == SemanticType::Class &&
                  valueOperand(operand) && typeRow(operand.type);
              if (!lambdaReady && !classReady) {
                callableArgumentsReady = false;
                break;
              }
            }
            bool returnReady = false;
            if (returnKind) {
              switch (*returnKind) {
              case CppMirTypeRepresentationKind::Void:
                returnReady = true;
                break;
              case CppMirTypeRepresentationKind::Scalar:
              case CppMirTypeRepresentationKind::StringView:
              case CppMirTypeRepresentationKind::FixedArray:
                returnReady = typeRow(target->returnType);
                break;
              case CppMirTypeRepresentationKind::Expected:
                if (expectedClassPlacementResultType(program_, representations_,
                                                     target->returnType)) {
                  returnReady =
                      !instruction.result ||
                      expectedClassResultDestinationSlot(
                          program_, body, *instruction.result) != nullptr ||
                      placementDirectReturnCall(program_, representations_,
                                                body, *instruction.result) ==
                          &instruction;
                } else {
                  returnReady = typeRow(target->returnType);
                }
                break;
              case CppMirTypeRepresentationKind::Class:
                returnReady =
                    typeRow(target->returnType) &&
                    (!instruction.result ||
                     placementDirectReturnCall(program_, representations_, body,
                                               *instruction.result) ==
                         &instruction ||
                     classValuePublicationSlot(body, *instruction.result) !=
                         nullptr ||
                     constructorFieldResultSlot(program_, body,
                                                *instruction.result) ||
                     valueRootedClassCallResultSlot(program_, representations_,
                                                    body,
                                                    *instruction.result) ||
                     classSsaLifetimeSlot(program_, representations_, body,
                                          *instruction.result) ||
                     fixedArrayAggregateInputSlot(
                         program_, body, *instruction.result) != nullptr ||
                     expectedPayloadInitialize(
                         program_, body, *instruction.result) != nullptr ||
                     expectedPayloadReturnSlot(program_, representations_, body,
                                               *instruction.result)
                             .producer == &instruction);
                break;
              case CppMirTypeRepresentationKind::Lambda:
                returnReady =
                    (typeRow(target->returnType) ||
                     materializedClosureForType(program_, body,
                                                target->returnType)) &&
                    (!instruction.result ||
                     lambdaValueDestinationSlot(body, *instruction.result) !=
                         nullptr);
                break;
              case CppMirTypeRepresentationKind::Reference:
                returnReady =
                    producedCallResultLoan(body, instruction) != nullptr;
                break;
              case CppMirTypeRepresentationKind::Enum:
                returnReady =
                    cppMirEnumBoundaryRow(representations_, target->returnType);
                break;
              default:
                break;
              }
            }
            if (!callableArgumentsReady ||
                target->linkage != LanguageLinkage::Gti ||
                (target->definitionKind !=
                     MirFunctionInstance::DefinitionKind::Source &&
                 virtualFailureCallee(program_, instruction) != target) ||
                !returnReady) {
              {
                return false;
              }
            }
          }
        }
        if (instruction.intrinsic != IntrinsicKind::None) {
          // A numeric-conversion intrinsic spells as the shipped
          // numeric_cast helper over one staged operand.
          if ((instruction.intrinsic ==
                   IntrinsicKind::NumericTypeParameterConversion ||
               instruction.intrinsic ==
                   IntrinsicKind::NumericAliasConversion) &&
              !instruction.functionTarget && instruction.result &&
              instruction.localFailureSites.empty() &&
              instruction.operands.size() == 1 &&
              valueOperand(instruction.operands.front()) &&
              typeRow(instruction.info.type)) {
            continue;
          }
          // A checked-result intrinsic produces its failure inside the
          // Expected value — no edges — and spells as the shipped helper
          // with the error type as its template argument.
          if ((instruction.intrinsic == IntrinsicKind::IntegerCheckedAdd ||
               instruction.intrinsic == IntrinsicKind::IntegerCheckedSubtract ||
               instruction.intrinsic ==
                   IntrinsicKind::IntegerCheckedMultiply) &&
              !instruction.functionTarget && instruction.result &&
              instruction.localFailureSites.empty() &&
              instruction.operands.size() == 2 &&
              std::all_of(instruction.operands.begin(),
                          instruction.operands.end(), valueOperand) &&
              instruction.info.type.kind == SemanticType::Expected &&
              instruction.info.type.arguments.size() == 2 &&
              typeRow(instruction.info.type) &&
              typeRow(instruction.info.type.arguments[1])) {
            continue;
          }
          if (storageBoundsCheckCall(instruction)) {
            // The plain form spells the terminal compatibility helper. The
            // failure form emits a status detector and follows its exact
            // Invoke/record/cleanup edge.
            if (instruction.result || instruction.operands.size() != 2 ||
                !std::all_of(instruction.operands.begin(),
                             instruction.operands.end(), valueOperand) ||
                !storageBoundsDetailSpelling(program_, instruction) ||
                (failureForm &&
                 !invokePairedInstruction(body, instruction.id))) {
              {
                return false;
              }
            }
            continue;
          }
          if (checkedConversionIntrinsicCall(instruction)) {
            // The checked-conversion detector: one site, one origin, one
            // staged operand. The failure form spells the status helper
            // writing the converted result; the plain shape spells the
            // terminal numeric_cast, which contains its failure by
            // terminating at the site.
            if (!instruction.result ||
                instruction.definedFailure.localOrigins.size() != 1 ||
                program_.failureMetadata().findSite(
                    instruction.localFailureSites.front()) == nullptr ||
                instruction.operands.size() != 1 ||
                !valueOperand(instruction.operands.front()) ||
                !typeRow(instruction.info.type)) {
              {
                return false;
              }
            }
            continue;
          }
          if (instruction.intrinsic == IntrinsicKind::UniqueOwnerBorrow ||
              instruction.intrinsic == IntrinsicKind::UniqueOwnerBorrowMut) {
            // The owner borrow spells the backend accessor over the
            // receiver's owner field and publishes through its paired
            // return loan at the consuming return; the null-state failure
            // contains terminally inside the accessor.
            const MirValue *operandValue =
                instruction.operands.size() == 1 &&
                        valueOperand(instruction.operands.front())
                    ? body.findValue(instruction.operands.front().value)
                    : nullptr;
            const MirInstruction *fieldLoad =
                operandValue == nullptr
                    ? nullptr
                    : findInstruction(body, operandValue->definition);
            const MirPlace *fieldPlace =
                fieldLoad != nullptr &&
                        fieldLoad->kind == MirInstructionKind::Load &&
                        fieldLoad->operands.size() == 1 &&
                        fieldLoad->operands.front().place != 0
                    ? body.findPlace(fieldLoad->operands.front().place)
                    : nullptr;
            if (fieldPlace == nullptr ||
                fieldPlace->root != MirPlaceRootKind::This ||
                fieldPlace->projections.size() != 1 ||
                fieldPlace->projections.front().kind !=
                    MirProjectionKind::Field ||
                !fieldRow(fieldPlace->projections.front().field) ||
                !instruction.result ||
                !body.usesOf(*instruction.result).empty()) {
              {
                return false;
              }
            }
            continue;
          }
          if (instruction.intrinsic == IntrinsicKind::UniqueOwnerIsNull) {
            const MirValue *operandValue =
                instruction.operands.size() == 1 &&
                        valueOperand(instruction.operands.front())
                    ? body.findValue(instruction.operands.front().value)
                    : nullptr;
            const MirInstruction *staged =
                operandValue == nullptr
                    ? nullptr
                    : findInstruction(body, operandValue->definition);
            const MirPlace *ownerPlace =
                staged != nullptr && staged->kind == MirInstructionKind::Load &&
                        staged->operands.size() == 1
                    ? body.findPlace(staged->operands.front().place)
                    : nullptr;
            if (ownerPlace == nullptr ||
                ownerPlace->type.kind != SemanticType::UniqueOwner ||
                ownerPlace->type.arguments.size() != 1 ||
                operandValue->info.type != ownerPlace->type ||
                instruction.receiver || instruction.functionTarget ||
                !instruction.result ||
                instruction.info.type != SemanticType::Bool ||
                !instruction.localFailureSites.empty() ||
                nonRootRecordUses(body, operandValue->id).size() != 1 ||
                !typeRow(ownerPlace->type) ||
                !typeRow(ownerPlace->type.arguments.front())) {
              return false;
            }
            continue;
          }
          if (instruction.intrinsic == IntrinsicKind::UniqueOwnerUpcast) {
            const SemanticType &sourceType =
                instruction.operands.empty()
                    ? SemanticType{}
                    : instruction.operands.front().type;
            if (instruction.receiver || instruction.functionTarget ||
                !instruction.result || instruction.operands.size() != 1 ||
                !valueOperand(instruction.operands.front()) ||
                instruction.info.type.kind != SemanticType::UniqueOwner ||
                instruction.info.type.arguments.size() != 1 ||
                sourceType.kind != SemanticType::UniqueOwner ||
                sourceType.arguments.size() != 1 ||
                !instruction.localFailureSites.empty() ||
                instruction.definedFailure.propagation !=
                    FailurePropagationKind::None ||
                !typeRow(instruction.info.type) || !typeRow(sourceType) ||
                !typeRow(instruction.info.type.arguments.front()) ||
                !typeRow(sourceType.arguments.front())) {
              return false;
            }
            continue;
          }
          if (instruction.intrinsic == IntrinsicKind::AllocateUniqueOwner) {
            // Allocation consumes the ordinary explicit input schedule. Each
            // pack element has already become a typed CallInput carrying its
            // exact copy/move policy.
            if (!instruction.result || instruction.receiver ||
                instruction.functionTarget ||
                instruction.info.type.kind != SemanticType::UniqueOwner ||
                instruction.info.type.arguments.size() != 1 ||
                !typeRow(instruction.info.type) ||
                !typeRow(instruction.info.type.arguments.front()) ||
                !std::all_of(
                    instruction.operands.begin(), instruction.operands.end(),
                    [&](const MirOperand &operand) {
                      return valueOperand(operand) && typeRow(operand.type);
                    })) {
              {
                return false;
              }
            }
            continue;
          }
          // The substituted type parameter's default construction names
          // no body and takes the empty-call shape below.
          if (instruction.intrinsic ==
                  IntrinsicKind::DefaultTypeParameterConstruction &&
              !instruction.functionTarget && !instruction.receiver &&
              instruction.operands.empty() &&
              instruction.localFailureSites.empty() && instruction.result &&
              typeRow(instruction.info.type)) {
            continue;
          }
          // An arithmetic intrinsic call names no body: it spells directly
          // as the shipped helper over its two staged scalar operands.
          if (instruction.functionTarget ||
              !scalarSpellableArithmeticIntrinsic(instruction.intrinsic) ||
              !instruction.result || instruction.operands.size() != 2 ||
              !std::all_of(instruction.operands.begin(),
                           instruction.operands.end(), valueOperand)) {
            {
              return false;
            }
          }
          continue;
        }
        // The explicit default construction of a class with no declared
        // constructor names no body: the value-initialized temporary
        // assigns into the declared local, so only the type row is in
        // question.
        if (!instruction.functionTarget && !instruction.constructorTarget &&
            !instruction.lambdaTarget && !instruction.bodyTarget &&
            !instruction.callableInvocation && !instruction.receiver &&
            // The substituted type parameter's default construction is
            // the same shape: the concrete instance spells T{}.
            (instruction.intrinsic == IntrinsicKind::None ||
             instruction.intrinsic ==
                 IntrinsicKind::DefaultTypeParameterConstruction) &&
            instruction.operands.empty() &&
            instruction.callableArguments.empty() &&
            instruction.localFailureSites.empty() && instruction.result &&
            typeRow(instruction.info.type)) {
          continue;
        }
        if (!instruction.functionTarget ||
            !bodyRow(*instruction.functionTarget)) {
          {
            return false;
          }
        }
        for (const MirOperand &operand : instruction.operands) {
          if (const MirInstruction *staged =
                  borrowStagedCallInput(body, operand)) {
            // A borrow-staged argument passes the place as a C++ lvalue;
            // the callee's reference parameter binds it directly.
            consumedStaged.push_back(*staged->result);
            continue;
          }
          // A direct borrow argument binds the live place lvalue with no
          // staging step, exactly like the direct-borrow receiver.
          if ((operand.kind == MirOperandKind::BorrowRead ||
               operand.kind == MirOperandKind::BorrowWrite) &&
              operand.place != 0 && body.findPlace(operand.place) != nullptr) {
            continue;
          }
          if (!valueOperand(operand)) {
            {
              return false;
            }
          }
        }
        if (const MirFunctionInstance *target =
                program_.findFunctionInstance(*instruction.functionTarget);
            target != nullptr && target->linkage == LanguageLinkage::C) {
          // The C boundary takes ::gti_c_string_view: a view argument is
          // marshalled through the shipped converter, but no reverse
          // converter is modelled, so a view result stays outside the
          // vocabulary.
          if (instruction.info.type.kind == SemanticType::StringView) {
            {
              return false;
            }
          }
        }
        // Every staged borrow in flight must feed exactly this call: a
        // borrowed place must not outlive its block or bypass a call that
        // could observe or invalidate it.
        // Each call consumes exactly its own staged inputs; borrows
        // staged for a later call in the same block stay pending — the
        // window rule polices what may sit between, and the block-end
        // check still demands the set drains to empty. A consumed borrow
        // staged in the unique invoke-predecessor block is equally
        // exact: the predecessor's block-end rule proved the hand-off.
        if (!std::all_of(consumedStaged.begin(), consumedStaged.end(),
                         [&](MirValueId id) {
                           return std::find(pendingStaged.begin(),
                                            pendingStaged.end(),
                                            id) != pendingStaged.end() ||
                                  stagedAcrossInvokeSuccessChain(id, block.id);
                         })) {
          {
            return false;
          }
        }
        for (const MirValueId id : consumedStaged) {
          pendingStaged.erase(
              std::remove(pendingStaged.begin(), pendingStaged.end(), id),
              pendingStaged.end());
        }
        continue;
      }
      default: {
        return false;
      }
      }
    }
    if (!pendingStaged.empty() &&
        block.terminator.kind == MirTerminatorKind::Invoke) {
      // A staged borrow may hand off across the invoke edge when every
      // leftover's consuming call sits in the success target and the
      // else path never references the staged places.
      bool handsOff = true;
      for (const MirValueId id : pendingStaged) {
        bool consumedInTarget = false;
        for (const MirValueUse &use : body.usesOf(id)) {
          // A staged borrow hands off whether the success target consumes
          // it as a call receiver or as a staged argument value (the
          // publication construct's operands arrive this way).
          if ((use.kind == MirValueUseKind::InstructionReceiver ||
               use.kind == MirValueUseKind::InstructionOperand) &&
              stagedAcrossInvokeSuccessChain(id, use.block)) {
            consumedInTarget = true;
          }
        }
        if (!consumedInTarget) {
          handsOff = false;
        }
        const MirValue *record = body.findValue(id);
        const MirInstruction *stage =
            record == nullptr ? nullptr
                              : findInstruction(body, record->definition);
        const MirPlace *stagedPlace =
            stage != nullptr && !stage->operands.empty()
                ? body.findPlace(stage->operands.front().place)
                : nullptr;
        if (stagedPlace == nullptr) {
          handsOff = false;
          continue;
        }
        for (const MirBlock &candidate : body.blocks) {
          if (candidate.id != block.terminator.elseTarget) {
            continue;
          }
          for (const MirInstruction &member : candidate.instructions) {
            if (member.kind == MirInstructionKind::Drop) {
              continue;
            }
            if ((member.destination &&
                 *member.destination == stagedPlace->id) ||
                (member.receiver &&
                 member.receiver->place == stagedPlace->id)) {
              handsOff = false;
            }
            for (const MirOperand &operand : member.operands) {
              if (operand.place == stagedPlace->id) {
                handsOff = false;
              }
            }
          }
        }
      }
      if (handsOff) {
        pendingStaged.clear();
      }
    }
    if (!pendingStaged.empty()) {
      // A staged borrow must be consumed by a call in its own block.
      {
        return false;
      }
    }
    switch (block.terminator.kind) {
    case MirTerminatorKind::Invoke: {
      if (block.terminator.target == 0 || block.terminator.elseTarget == 0) {
        {
          return false;
        }
      }
      // The invoke's producer must be this block's checked detector (the
      // status local and record write are spellable) or its transformed
      // call (the callee wrote the record; the edge branches on the call's
      // success bool).
      const MirInstruction *producer = nullptr;
      for (const MirInstruction &instruction : block.instructions) {
        if (instruction.id == block.terminator.invokeInstruction) {
          producer = &instruction;
        }
      }
      if (producer == nullptr) {
        {
          return false;
        }
      }
      if (producer->kind == MirInstructionKind::Drop) {
        if (!failureForm ||
            failureDestructorTarget(program_, body, *producer) == nullptr) {
          return false;
        }
        continue;
      }
      if (callableValueInvocation(*producer)) {
        // The instruction probe above accepted the failure form only for a
        // fused lambda whose recursively verified body carries the explicit
        // record convention. Its Invoke branches on the emitted success
        // local just like a transformed named callee.
        continue;
      }
      if (plainExternalBoundaryCallee(program_, *producer)) {
        // The plain declaration ABI has no status channel. If it returns, the
        // conservative invoke took its success edge; no caller cleanup edge
        // has been bypassed because no GTI failure record exists to route.
        continue;
      }
      if (terminallyContainedPlainCallee(program_, representations_,
                                         *producer) ||
          terminallyContainedMemberCallee(program_, representations_,
                                          *producer) ||
          (!failureForm &&
           producer->intrinsic == IntrinsicKind::AllocatePrefixStorage) ||
          (!failureForm && storageBoundsCheckCall(*producer))) {
        // These are compatibility containment points, not explicit MIR
        // propagation. They remain valid only in a plain compatibility
        // body; admitting them in the failure form would erase the current
        // frame's cleanup edge.
        if (!failureForm) {
          continue;
        }
        return false;
      }
      if (producer->kind == MirInstructionKind::Compute &&
          (!cppMirTerminalCheckedHelperSpelling(producer->operation).empty() ||
           producer->operation == MirOperation::Convert) &&
          !producer->localFailureSites.empty() &&
          (producer->info.type.kind == SemanticType::Float ||
           producer->info.type.kind == SemanticType::Double)) {
        // IEEE-754 nontrapping results are not defined runtime failures:
        // the floating site never fires, so the edge is a plain goto on
        // both forms.
        continue;
      }
      if (!failureForm) {
        // Plain shape: a Lambda's terminal checked compute, or a template
        // body's terminally-contained plain call, produce invokes whose
        // helpers never return on failure.
        if (callableTemplateBody &&
            producer->kind == MirInstructionKind::Call) {
          continue;
        }
        if (producer->kind == MirInstructionKind::Construct &&
            producer->localFailureSites.empty() &&
            producer->definedFailure.propagation ==
                FailurePropagationKind::Constructor) {
          // A propagating construction terminates at its own site on every
          // shipped path — the constructor failure ABI does not exist yet —
          // so the else edge is dead in the plain shape exactly as it is in
          // the failure form below.
          continue;
        }
        if (checkedConversionIntrinsicCall(*producer)) {
          // The plain shape spells the terminal numeric_cast, which never
          // returns on failure, so the else edge is dead.
          continue;
        }
        if (producer->intrinsic == IntrinsicKind::ExpectedValue ||
            producer->intrinsic == IntrinsicKind::ExpectedError) {
          // The expected extraction's spelled member contains the
          // wrong-state failure terminally; the else edge is dead.
          continue;
        }
        if (producer->kind == MirInstructionKind::Call &&
            wrapperContainedCallSite(program_, representations_, body, block,
                                     *producer)) {
          // The plain wrapper name terminates the escaping failure at the
          // callee's own boundary; the per-site proof shows the else path
          // only cleans up and terminates, so the edge is dead.
          continue;
        }
        // Outside a lambda the else edge must be provably terminal-only
        // (cleanup glue ending in the failure family): the terminal
        // helper never returns on failure, so the never-taken path stays
        // dead exactly like the compatibility spelling. The view element
        // read and the checked conversion carry terminal helpers too.
        if ((address.kind != MirBodyKind::Lambda &&
             (failureForm ||
              !failurePathTerminates(body, block.terminator.elseTarget))) ||
            producer->kind != MirInstructionKind::Compute ||
            (cppMirTerminalCheckedHelperSpelling(producer->operation).empty() &&
             producer->operation != MirOperation::Index &&
             producer->operation != MirOperation::Convert) ||
            producer->localFailureSites.size() != 1) {
          {
          }
          {
            return false;
          }
        }
        continue;
      }
      if (producer->kind == MirInstructionKind::Construct &&
          producer->localFailureSites.empty() &&
          producer->definedFailure.propagation ==
              FailurePropagationKind::Constructor) {
        const MirConstructorInstance *target =
            producer->constructorTarget
                ? program_.findConstructorInstance(*producer->constructorTarget)
                : nullptr;
        if (target != nullptr &&
            (failureConstructorBoundaryEligible(program_, *target) ||
             terminallyContainedPlainConstructor(program_, representations_,
                                                 *producer))) {
          continue;
        }
        return false;
      }
      if (producer->kind == MirInstructionKind::Call &&
          (producer->intrinsic == IntrinsicKind::ExpectedValue ||
           producer->intrinsic == IntrinsicKind::ExpectedError)) {
        continue;
      }
      if (producer->kind == MirInstructionKind::Call) {
        if (storageAllocationFailureCall(program_, *producer)) {
          // The allocation helper writes the canonical checked status that
          // drives this Invoke edge; successful publication remains in MIR.
          continue;
        }
        if (sparseStorageIntrinsic(producer->intrinsic) &&
            !producer->localFailureSites.empty()) {
          // The checked sparse-storage helper writes the exact status used by
          // this Invoke edge; the failure record is populated at the site.
          continue;
        }
        if (prefixStorageIntrinsic(producer->intrinsic) &&
            !producer->localFailureSites.empty()) {
          // The storage detector's own status local carries the fired
          // outcome; the edge branches on it like any checked detector.
          continue;
        }
        if (prefixStorageIntrinsic(producer->intrinsic) &&
            producer->localFailureSites.empty() &&
            producer->definedFailure.propagation ==
                FailurePropagationKind::None) {
          // A discharged storage read carries no site: flow analysis
          // proved the bound, so the invoke's else edge is unreachable
          // and the edge is a plain goto, exactly like the analysis's
          // discharged-read exemption.
          continue;
        }
        if (checkedConversionIntrinsicCall(*producer)) {
          continue;
        }
        if (storageBoundsCheckCall(*producer) &&
            storageBoundsDetailSpelling(program_, *producer)) {
          continue;
        }
        if (producer->intrinsic == IntrinsicKind::AllocateUniqueOwner &&
            !producer->functionTarget && !producer->receiver &&
            producer->definedFailure.propagation ==
                FailurePropagationKind::None &&
            producer->localFailureSites.size() == 1) {
          // The shipped allocation helper reports exhaustion terminally. Its
          // verified Invoke therefore has an unreachable failure successor in
          // both text forms, matching emitTerminator's unconditional success
          // edge until a recoverable try_make_* allocation contract exists.
          continue;
        }
        const MirFunctionInstance *target =
            producer->functionTarget
                ? program_.findFunctionInstance(*producer->functionTarget)
                : nullptr;
        if (target == nullptr || !target->mayRaiseDefinedFailure) {
          {
            return false;
          }
        }
        continue;
      }
      if ((producer->kind == MirInstructionKind::Load ||
           producer->kind == MirInstructionKind::Assign) &&
          producer->localFailureSites.size() == 1) {
        continue;
      }
      if (stringViewIndexFailureSite(program_, *producer)) {
        continue;
      }
      if (producer->kind != MirInstructionKind::Compute ||
          cppMirCheckedOperationHelperSpelling(producer->operation).empty() ||
          producer->localFailureSites.size() != 1) {
        {
          return false;
        }
      }
      continue;
    }
    case MirTerminatorKind::PropagateFailure:
      if (block.terminator.failureRecord == 0) {
        {
          return false;
        }
      }
      if (!failureForm && address.kind != MirBodyKind::Lambda &&
          !callableTemplateBody) {
        // A plain shape admits this terminator only with an unreachability
        // proof: walk from the entry following every edge except an
        // invoke's else edge (every invoke producer this probe admits in
        // the plain shape terminates at its own site on failure, so that
        // edge is dead), and require the walk never lands here. The block
        // then spells abort.
        bool reachable = false;
        std::vector<MirBlockId> stack{body.entry};
        std::vector<MirBlockId> seen;
        while (!stack.empty()) {
          const MirBlockId id = stack.back();
          stack.pop_back();
          if (std::find(seen.begin(), seen.end(), id) != seen.end()) {
            continue;
          }
          seen.push_back(id);
          if (id == block.id) {
            reachable = true;
            break;
          }
          for (const MirBlock &candidate : body.blocks) {
            if (candidate.id != id) {
              continue;
            }
            const MirTerminator &edge = candidate.terminator;
            if (edge.kind == MirTerminatorKind::Invoke) {
              stack.push_back(edge.target);
              continue;
            }
            if (edge.kind == MirTerminatorKind::Return ||
                edge.kind == MirTerminatorKind::Exit ||
                edge.kind == MirTerminatorKind::Unreachable ||
                edge.kind == MirTerminatorKind::PropagateFailure) {
              continue;
            }
            if (edge.target != 0) {
              stack.push_back(edge.target);
            }
            if (edge.elseTarget != 0) {
              stack.push_back(edge.elseTarget);
            }
            for (const MirSwitchTarget &target : edge.switchTargets) {
              stack.push_back(target.target);
            }
          }
        }
        if (reachable) {
          return false;
        }
      }
      continue;
    case MirTerminatorKind::TerminateCleanupFailure: {
      const MirFailureRecord *secondary =
          body.findFailureRecord(block.terminator.failureRecord);
      const MirBlock *producerBlock =
          secondary == nullptr ? nullptr
                               : body.findBlock(secondary->producerBlock);
      const MirInstruction *producer =
          secondary == nullptr
              ? nullptr
              : findInstruction(body, secondary->producerInstruction);
      if (!failureForm || !block.instructions.empty() ||
          block.activeFailure == 0 || block.failureParameter == 0 ||
          block.failureParameter != block.terminator.failureRecord ||
          block.activeFailure == block.failureParameter ||
          secondary == nullptr || producerBlock == nullptr ||
          producerBlock->activeFailure != block.activeFailure ||
          producer == nullptr || producer->kind != MirInstructionKind::Drop ||
          failureDestructorTarget(program_, body, *producer) == nullptr) {
        return false;
      }
      continue;
    }
    case MirTerminatorKind::Goto:
    case MirTerminatorKind::Unreachable:
      continue;
    case MirTerminatorKind::Exit:
      if (address.kind != MirBodyKind::Module || block.terminator.value ||
          block.terminator.returnLoan ||
          block.terminator.invokeInstruction != 0 ||
          block.terminator.failureRecord != 0 || block.terminator.target != 0 ||
          block.terminator.elseTarget != 0 ||
          !block.terminator.switchTargets.empty() ||
          !block.terminator.successLifecycle.empty()) {
        return false;
      }
      continue;
    case MirTerminatorKind::Branch:
      if (!block.terminator.value || !valueOperand(*block.terminator.value)) {
        {
          return false;
        }
      }
      continue;
    case MirTerminatorKind::Switch: {
      if (!block.terminator.value || !valueOperand(*block.terminator.value)) {
        {
          return false;
        }
      }
      bool targets = true;
      for (const MirSwitchTarget &target : block.terminator.switchTargets) {
        targets = targets && target.value && typeRow(target.value->type);
        // A payload-enum case must resolve its variant in the copied enum
        // row: the text spells the discriminant as the variant record's
        // alternative index, and an unproven index must fail closed here
        // rather than in the plain integral spelling.
        if (targets && target.value->kind == SwitchCaseKind::Enumerator &&
            target.value->enumOwner != 0) {
          const CppMirEnumRepresentation *enumRow = nullptr;
          for (const CppMirEnumRepresentation &row : representations_.enums()) {
            if (row.owner == target.value->enumOwner) {
              enumRow = &row;
            }
          }
          if (enumRow != nullptr && !enumRow->payloadVariants.empty()) {
            targets =
                targets && !target.value->value.negative &&
                std::any_of(
                    enumRow->payloadVariants.begin(),
                    enumRow->payloadVariants.end(),
                    [&](const CppMirPayloadVariantRepresentation &variant) {
                      return variant.index == target.value->value.magnitude;
                    });
          }
        }
      }
      if (!targets) {
        {
          return false;
        }
      }
      continue;
    }
    case MirTerminatorKind::Return: {
      const ClassSsaLifetimeSlot returnedClassSlot =
          block.terminator.value &&
                  block.terminator.value->kind == MirOperandKind::Value
              ? classSsaLifetimeSlot(program_, representations_, body,
                                     block.terminator.value->value)
              : ClassSsaLifetimeSlot{};
      if (block.terminator.returnLoan && *block.terminator.returnLoan != 0) {
        if (loanById(*block.terminator.returnLoan) == nullptr ||
            // A class-valued return published through a loan would copy
            // the borrowed referent — a deleted copy constructor rejects
            // it and moving from a borrowed place is unsound — so the
            // shape stays on the compatibility path until rows carry the
            // copyability proof. A return whose VALUE operand fuses its
            // construct is different: the loan only records the borrowed
            // reference escaping inside the constructed value, erased
            // per ADR 018, and nothing ever spells the loan pointer.
            (body.returnType.kind == SemanticType::Class &&
             !(block.terminator.value &&
               block.terminator.value->kind == MirOperandKind::Value &&
               (returnConstructDefinition(
                    body, block.terminator.value->value) != nullptr ||
                returnMoveDefinition(body, block.terminator.value->value) !=
                    nullptr)) &&
             !(returnedClassSlot.returnBlock == &block &&
               returnedClassSlot.consumerKind ==
                   ClassSsaSlotConsumerKind::Return))) {
          {
            return false;
          }
        }
        if (!(returnedClassSlot.returnBlock == &block &&
              returnedClassSlot.consumerKind ==
                  ClassSsaSlotConsumerKind::Return)) {
          continue;
        }
      }
      const auto passiveReturnOperand = [&]() {
        if (!block.terminator.value ||
            block.terminator.value->type != body.returnType) {
          return false;
        }
        const std::optional<CppMirTypeRepresentationKind> kind =
            expectedTypeRepresentation(block.terminator.value->type);
        if (!kind || *kind != CppMirTypeRepresentationKind::Scalar ||
            !typeRow(block.terminator.value->type)) {
          return false;
        }
        if (block.terminator.value->kind == MirOperandKind::Constant) {
          return literalSupported(block.terminator.value->literal,
                                  block.terminator.value->type);
        }
        if (block.terminator.value->kind != MirOperandKind::Copy ||
            block.terminator.value->place == 0) {
          return false;
        }
        const MirPlace *place = body.findPlace(block.terminator.value->place);
        return place != nullptr && place->type == block.terminator.value->type;
      };
      if (block.terminator.value && !valueOperand(*block.terminator.value) &&
          !passiveReturnOperand()) {
        {
          return false;
        }
      }
      if (failureForm && block.terminator.value &&
          expectedClassPlacementResultType(program_, representations_,
                                           body.returnType)) {
        const MirOperand &returned = *block.terminator.value;
        const MirInstruction *construct =
            returned.kind == MirOperandKind::Value
                ? returnConstructDefinition(body, returned.value)
                : nullptr;
        const MirInstruction *defaulted =
            construct == nullptr && returned.kind == MirOperandKind::Value
                ? returnDefaultConstructionDefinition(body, returned.value)
                : nullptr;
        const MirInstruction *moved =
            construct == nullptr && returned.kind == MirOperandKind::Value
                ? returnMoveDefinition(body, returned.value)
                : nullptr;
        const MirInstruction *unexpectedValue =
            returned.kind == MirOperandKind::Value
                ? unexpectedDefinition(body, returned.value)
                : nullptr;
        const MirInstruction *directExpectedCall =
            returned.kind == MirOperandKind::Value
                ? placementDirectReturnCall(program_, representations_, body,
                                            returned.value)
                : nullptr;
        const ExpectedPayloadReturnSlot expectedPayload =
            returned.kind == MirOperandKind::Value
                ? expectedPayloadReturnSlot(program_, representations_, body,
                                            returned.value)
                : ExpectedPayloadReturnSlot{};
        const SemanticType &payload = body.returnType.arguments.front();
        const SemanticType &error = body.returnType.arguments.back();
        bool publicationReady = false;
        if (returned.type == payload) {
          bool constructorFailureFree = true;
          if (construct != nullptr && construct->constructorTarget) {
            const MirConstructorInstance *target =
                program_.findConstructorInstance(*construct->constructorTarget);
            constructorFailureFree =
                target != nullptr &&
                (!target->mayRaiseDefinedFailure ||
                 failureConstructorBoundaryEligible(program_, *target));
          }
          publicationReady = expectedPayload || moved != nullptr ||
                             defaulted != nullptr ||
                             (construct != nullptr && constructorFailureFree);
        } else if (returned.type == body.returnType) {
          publicationReady = moved != nullptr || directExpectedCall != nullptr;
        } else if (returned.type.kind == SemanticType::Unexpected &&
                   returned.type.arguments.size() == 1 &&
                   returned.type.arguments.front() == error) {
          publicationReady = unexpectedValue != nullptr;
        }
        if (!publicationReady) {
          return false;
        }
      }
      if (failureForm && block.terminator.value &&
          block.terminator.value->type.kind == SemanticType::Class &&
          returnConstructDefinition(body, block.terminator.value->value) ==
              nullptr &&
          returnDefaultConstructionDefinition(
              body, block.terminator.value->value) == nullptr &&
          returnMoveDefinition(body, block.terminator.value->value) ==
              nullptr &&
          returnCopyLoadDefinition(body, block.terminator.value->value) ==
              nullptr &&
          placementDirectReturnCall(program_, representations_, body,
                                    block.terminator.value->value) == nullptr &&
          !expectedPayloadReturnSlot(program_, representations_, body,
                                     block.terminator.value->value) &&
          !(returnedClassSlot.returnBlock == &block &&
            returnedClassSlot.consumerKind ==
                ClassSsaSlotConsumerKind::Return)) {
        // A class value publishes only through its paired inline
        // construct; anything else has no local to spell.
        {
          return false;
        }
      }
      continue;
    }
    default: {
      return false;
    }
    }
  }
  return true;
} // namespace

CppMirBodyEmissionText
CppMirBodyEmitter::emitBodyText(MirBodyAddress address,
                                std::string_view familyLabel,
                                std::size_t indentation) const {
  CppMirBodyEmissionText result;
  result.analysis = analyze(address);
  if (!result.analysis.ready()) {
    return result;
  }
  switch (address.kind) {
  case MirBodyKind::Module:
    if (address.owner != 0) {
      throw std::logic_error(
          "general MIR module text emission received a nonzero owner");
    }
    result.text = ScalarBodyTextEmitter(program_, representations_, indentation)
                      .emitModule(familyLabel);
    return result;
  case MirBodyKind::Function: {
    const MirFunctionInstance *function =
        program_.findFunctionInstance(address.owner);
    if (function == nullptr) {
      throw std::logic_error(
          "general MIR body text emission lost its exact function instance");
    }
    result.text = ScalarBodyTextEmitter(program_, representations_, indentation)
                      .emit(*function, familyLabel);
    return result;
  }
  case MirBodyKind::Destructor: {
    const MirDestructorInstance *destructor =
        program_.findDestructorInstance(address.owner);
    if (destructor == nullptr) {
      throw std::logic_error(
          "general MIR body text emission lost its exact destructor instance");
    }
    result.text = ScalarBodyTextEmitter(program_, representations_, indentation)
                      .emit(*destructor, familyLabel);
    return result;
  }
  case MirBodyKind::Constructor: {
    const MirConstructorInstance *constructor =
        program_.findConstructorInstance(address.owner);
    if (constructor == nullptr) {
      throw std::logic_error(
          "general MIR body text emission lost its exact constructor "
          "instance");
    }
    result.text = ScalarBodyTextEmitter(program_, representations_, indentation)
                      .emit(*constructor, familyLabel);
    return result;
  }
  default:
    throw std::logic_error(
        "general MIR body text emission supports module, function, "
        "constructor, and destructor bodies only");
  }
}

CppMirBodyEmissionText
CppMirBodyEmitter::emitFailureBodyText(MirBodyAddress address,
                                       std::string_view familyLabel,
                                       std::size_t indentation) const {
  CppMirBodyEmissionText result;
  result.analysis = analyze(address);
  if (!result.analysis.ready()) {
    return result;
  }
  if (address.kind == MirBodyKind::Module) {
    if (address.owner != 0) {
      throw std::logic_error(
          "failure-form MIR module text emission received a nonzero owner");
    }
    result.text =
        ScalarBodyTextEmitter(program_, representations_, indentation, true)
            .emitModule(familyLabel);
    return result;
  }
  if (address.kind == MirBodyKind::Function) {
    const MirFunctionInstance *function =
        program_.findFunctionInstance(address.owner);
    if (function == nullptr) {
      throw std::logic_error(
          "failure-form MIR body text emission lost its function instance");
    }
    result.text =
        ScalarBodyTextEmitter(program_, representations_, indentation, true)
            .emit(*function, familyLabel);
    return result;
  }
  if (address.kind == MirBodyKind::Constructor) {
    const MirConstructorInstance *constructor =
        program_.findConstructorInstance(address.owner);
    if (constructor == nullptr) {
      throw std::logic_error(
          "failure-form MIR body text emission lost its constructor instance");
    }
    result.text =
        ScalarBodyTextEmitter(program_, representations_, indentation, true)
            .emit(*constructor, familyLabel);
    return result;
  }
  if (address.kind == MirBodyKind::Destructor) {
    const MirDestructorInstance *destructor =
        program_.findDestructorInstance(address.owner);
    if (destructor == nullptr) {
      throw std::logic_error(
          "failure-form MIR body text emission lost its destructor instance");
    }
    result.text =
        ScalarBodyTextEmitter(program_, representations_, indentation, true)
            .emit(*destructor, familyLabel);
    return result;
  }
  throw std::logic_error(
      "failure-form MIR body text emission supports module, function, "
      "constructor, and destructor bodies only");
}

bool CppMirBodyEmitter::supportsNativeContainedBaseConstruction(
    HirConstructorInstanceId constructor) const {
  const MirConstructorInstance *instance =
      program_.findConstructorInstance(constructor);
  const MirClassInstance *owner =
      instance == nullptr ? nullptr
                          : program_.findClassInstance(instance->owner);
  return instance != nullptr && owner != nullptr &&
         nativeContainedBaseConstruction(program_, representations_, *owner,
                                         *instance);
}

CppMirInitializerScheduleText
CppMirBodyEmitter::initializerSchedule(MirBodyAddress address) const {
  CppMirInitializerScheduleText result;
  result.analysis = analyze(address);
  if (!result.analysis.ready()) {
    return result;
  }
  const MirBody *body = nullptr;
  switch (address.kind) {
  case MirBodyKind::FieldInitializers:
  case MirBodyKind::StaticFieldInitializers: {
    const MirClassInstance *owner = program_.findClassInstance(address.owner);
    if (owner == nullptr) {
      return result;
    }
    body = address.kind == MirBodyKind::FieldInitializers
               ? &owner->fieldInitializers
               : &owner->staticFieldInitializers;
    break;
  }
  case MirBodyKind::Module:
    body = &program_.module();
    break;
  default:
    return result;
  }
  // The schedule is the straight-line chain of blocks from the entry:
  // literal materialization, exact symbol-storage reads,
  // terminally-contained checked computes (the compatibility helper family
  // that reports and never returns on failure, so each Invoke edge is
  // sequential and its else block is unreachable), exact non-failing class
  // construction, per-field Initialize stages, and lifecycle boundaries.
  // Every other shape stays with the compatibility route.
  if (body->blocks.empty() || body->entry != 1 || !body->loans.empty() ||
      !directOwningFieldInitializerTransfers(*body) ||
      !body->cleanupBoundaries.empty()) {
    return result;
  }
  std::vector<const MirInstruction *> schedule;
  {
    const MirBlock *block = &body->blocks.front();
    std::size_t visited = 0;
    for (;;) {
      if (++visited > body->blocks.size()) {
        return result;
      }
      for (const MirInstruction &instruction : block->instructions) {
        schedule.push_back(&instruction);
      }
      if (block->terminator.kind == MirTerminatorKind::Exit) {
        break;
      }
      // Program-initialization units chain by plain Goto; checked steps
      // chain by Invoke over a terminally-contained compute.
      if (block->terminator.kind == MirTerminatorKind::Goto) {
        const MirBlock *next = nullptr;
        for (const MirBlock &candidate : body->blocks) {
          if (candidate.id == block->terminator.target) {
            next = &candidate;
          }
        }
        if (next == nullptr) {
          return result;
        }
        block = next;
        continue;
      }
      if (block->terminator.kind != MirTerminatorKind::Invoke) {
        return result;
      }
      // The invoke's producer must be this block's terminally-contained
      // checked compute; its else target must be an empty propagate
      // block, which the helper's own containment makes unreachable.
      const MirInstruction *producer = nullptr;
      for (const MirInstruction &instruction : block->instructions) {
        if (instruction.id == block->terminator.invokeInstruction) {
          producer = &instruction;
        }
      }
      const MirBlock *elseBlock = nullptr;
      const MirBlock *next = nullptr;
      for (const MirBlock &candidate : body->blocks) {
        if (candidate.id == block->terminator.elseTarget) {
          elseBlock = &candidate;
        }
        if (candidate.id == block->terminator.target) {
          next = &candidate;
        }
      }
      const bool checkedCompute =
          producer != nullptr &&
          producer->kind == MirInstructionKind::Compute &&
          !cppMirTerminalCheckedHelperSpelling(producer->operation).empty() &&
          producer->localFailureSites.size() == 1;
      const bool containedConstruction =
          producer != nullptr && plainConstructorBodyContainsFailure(
                                     program_, representations_, *producer);
      if ((!checkedCompute && !containedConstruction) || next == nullptr ||
          elseBlock == nullptr || !elseBlock->instructions.empty() ||
          elseBlock->terminator.kind != MirTerminatorKind::PropagateFailure) {
        return result;
      }
      block = next;
    }
  }
  ScalarBodyTextEmitter writer(program_, representations_, 0);
  std::unordered_map<MirValueId, std::string> spellings;
  const auto dependentFieldTypeSpelling =
      [&](MirValueId value) -> const std::string * {
    if (address.kind != MirBodyKind::FieldInitializers || value == 0) {
      return nullptr;
    }
    const MirPlace *destination = nullptr;
    for (const MirInstruction *candidate : schedule) {
      if (candidate->kind != MirInstructionKind::Initialize ||
          !candidate->destination || candidate->operands.size() != 1 ||
          candidate->operands.front().kind != MirOperandKind::Value ||
          candidate->operands.front().value != value) {
        continue;
      }
      const MirPlace *found = body->findPlace(*candidate->destination);
      if (found == nullptr || destination != nullptr) {
        return nullptr;
      }
      destination = found;
    }
    if (destination == nullptr ||
        destination->root != MirPlaceRootKind::Binding ||
        destination->symbol == 0 || !destination->projections.empty()) {
      return nullptr;
    }
    const auto row = std::find_if(
        representations_.symbols().begin(), representations_.symbols().end(),
        [&](const CppMirSymbolRepresentation &candidate) {
          return candidate.kind == CppMirSymbolRepresentationKind::Field &&
                 candidate.owner == address.owner &&
                 candidate.symbol == destination->symbol &&
                 candidate.ordinal == 0 && candidate.type == destination->type;
        });
    return row == representations_.symbols().end() ||
                   row->declarationTypeSpelling.empty()
               ? nullptr
               : &row->declarationTypeSpelling;
  };
  const auto integralKind = [](const SemanticType &type) {
    switch (type.kind) {
    case SemanticType::Int8:
    case SemanticType::Int16:
    case SemanticType::Int32:
    case SemanticType::Int64:
    case SemanticType::UInt8:
    case SemanticType::UInt16:
    case SemanticType::UInt32:
    case SemanticType::UInt64:
    case SemanticType::Char:
      return true;
    default:
      return false;
    }
  };
  for (const MirInstruction *instruction : schedule) {
    switch (instruction->kind) {
    case MirInstructionKind::Lifecycle:
      continue;
    case MirInstructionKind::Compute: {
      if (!instruction->result) {
        return result;
      }
      if (instruction->operation == MirOperation::Literal) {
        if (!instruction->literal || !instruction->operands.empty() ||
            !instruction->localFailureSites.empty() ||
            !ScalarBodyTextEmitter::spellableLiteral(*instruction->literal,
                                                     instruction->info.type)) {
          return result;
        }
        spellings.emplace(*instruction->result,
                          writer.literalSpelling(*instruction->literal,
                                                 instruction->info.type));
        continue;
      }
      // The empty aggregate spells the row type's value initialization,
      // exactly like the compute vocabulary's Aggregate arm.
      if (instruction->operation == MirOperation::Aggregate) {
        const auto row = std::find_if(
            representations_.types().begin(), representations_.types().end(),
            [&](const CppMirTypeRepresentation &candidate) {
              return candidate.type == instruction->info.type;
            });
        if (!instruction->localFailureSites.empty() ||
            row == representations_.types().end() || row->spelling.empty()) {
          return result;
        }
        const std::string *dependent =
            dependentFieldTypeSpelling(*instruction->result);
        std::string spelled = dependent == nullptr ? row->spelling : *dependent;
        spelled += '{';
        for (std::size_t index = 0; index < instruction->operands.size();
             ++index) {
          const MirOperand &operand = instruction->operands[index];
          const auto found = operand.kind == MirOperandKind::Value
                                 ? spellings.find(operand.value)
                                 : spellings.end();
          if (row->kind != CppMirTypeRepresentationKind::FixedArray ||
              found == spellings.end()) {
            return result;
          }
          if (index != 0) {
            spelled += ", ";
          }
          spelled += found->second;
        }
        spelled += '}';
        spellings.emplace(*instruction->result, std::move(spelled));
        continue;
      }
      // A terminally-contained checked compute spells the compatibility
      // helper over its already-spelled operands, exactly as the
      // compatibility in-class initializer does.
      const std::string_view helper =
          cppMirTerminalCheckedHelperSpelling(instruction->operation);
      if (helper.empty() || instruction->localFailureSites.size() != 1 ||
          instruction->operands.empty() || instruction->operands.size() > 2 ||
          !integralKind(instruction->info.type)) {
        return result;
      }
      std::string spelled(helper);
      spelled += '(';
      for (std::size_t index = 0; index < instruction->operands.size();
           ++index) {
        const MirOperand &operand = instruction->operands[index];
        const auto found = operand.kind == MirOperandKind::Value
                               ? spellings.find(operand.value)
                               : spellings.end();
        if (found == spellings.end() || !integralKind(operand.type)) {
          return result;
        }
        if (index != 0) {
          spelled += ", ";
        }
        spelled += found->second;
      }
      spelled += ')';
      spellings.emplace(*instruction->result, std::move(spelled));
      continue;
    }
    case MirInstructionKind::CallInput: {
      if (!instruction->result || instruction->operands.size() != 1 ||
          instruction->operands.front().kind != MirOperandKind::Value ||
          !instruction->localFailureSites.empty()) {
        return result;
      }
      const auto source = spellings.find(instruction->operands.front().value);
      if (source == spellings.end() ||
          instruction->operands.front().type != instruction->info.type) {
        return result;
      }
      spellings.emplace(*instruction->result, source->second);
      continue;
    }
    case MirInstructionKind::Load: {
      const MirOperand *operand = instruction->operands.size() == 1
                                      ? &instruction->operands.front()
                                      : nullptr;
      const MirPlace *source = operand != nullptr &&
                                       operand->kind == MirOperandKind::Copy &&
                                       operand->place != 0
                                   ? body->findPlace(operand->place)
                                   : nullptr;
      const std::optional<HirClassInstanceId> concreteOwner =
          address.kind == MirBodyKind::FieldInitializers ||
                  address.kind == MirBodyKind::StaticFieldInitializers
              ? std::optional<HirClassInstanceId>{address.owner}
              : std::nullopt;
      const bool moduleBindingStorage =
          source != nullptr && address.kind == MirBodyKind::Module &&
          source->root == MirPlaceRootKind::Binding && source->binding != 0 &&
          source->symbol != 0 && source->capture == 0 &&
          source->projections.empty() && [&] {
            const MirProgramInitializationStep *step =
                program_.programInitializationPlan().findStepForSymbol(
                    source->symbol);
            return step != nullptr && step->binding == source->binding &&
                   step->storagePlace == source->id;
          }();
      const bool symbolStorage = source != nullptr &&
                                 source->root == MirPlaceRootKind::Symbol &&
                                 source->capture == 0 && source->symbol != 0 &&
                                 source->projections.empty();
      const CppMirSymbolRepresentation *storage =
          !moduleBindingStorage && !symbolStorage
              ? nullptr
              : storageRepresentationForBody(program_, representations_,
                                             concreteOwner, source->symbol);
      if (!instruction->result || source == nullptr || storage == nullptr ||
          storage->spelling.empty() || source->type != operand->type ||
          source->type != instruction->info.type ||
          storage->type != source->type || instruction->receiver ||
          instruction->destination || !instruction->localFailureSites.empty() ||
          !instruction->definedFailure.empty() ||
          !instruction->lifecycle.empty()) {
        return result;
      }
      spellings.emplace(*instruction->result, storage->spelling);
      continue;
    }
    case MirInstructionKind::Call: {
      const auto row = std::find_if(
          representations_.types().begin(), representations_.types().end(),
          [&](const CppMirTypeRepresentation &candidate) {
            return candidate.type == instruction->info.type;
          });
      if (!instruction->result || instruction->receiver ||
          instruction->functionTarget || instruction->constructorTarget ||
          instruction->lambdaTarget || instruction->bodyTarget ||
          instruction->callableInvocation ||
          (instruction->intrinsic != IntrinsicKind::None &&
           instruction->intrinsic !=
               IntrinsicKind::DefaultTypeParameterConstruction) ||
          !instruction->operands.empty() ||
          !instruction->callableArguments.empty() ||
          !instruction->localFailureSites.empty() ||
          row == representations_.types().end() || row->spelling.empty()) {
        return result;
      }
      const std::string *dependent =
          dependentFieldTypeSpelling(*instruction->result);
      spellings.emplace(*instruction->result,
                        (dependent == nullptr ? row->spelling : *dependent) +
                            "{}");
      continue;
    }
    case MirInstructionKind::Construct: {
      const MirConstructorInstance *target =
          instruction->constructorTarget ? program_.findConstructorInstance(
                                               *instruction->constructorTarget)
                                         : nullptr;
      const MirClassInstance *owner =
          target == nullptr ? nullptr
                            : program_.findClassInstance(target->owner);
      const auto row = std::find_if(
          representations_.types().begin(), representations_.types().end(),
          [&](const CppMirTypeRepresentation &candidate) {
            return candidate.type == instruction->info.type;
          });
      const bool containedFailure =
          target != nullptr && target->mayRaiseDefinedFailure &&
          plainConstructorBodyContainsFailure(program_, representations_,
                                              *instruction);
      if (!instruction->result || instruction->receiver ||
          instruction->destination ||
          instruction->constructorKind != ConstructorKind::Ordinary ||
          target == nullptr || owner == nullptr ||
          owner->type != instruction->info.type ||
          (target->mayRaiseDefinedFailure && !containedFailure) ||
          target->parameterTypes != instruction->parameterTypes ||
          instruction->operands.size() != target->parameterTypes.size() ||
          !instruction->localFailureSites.empty() ||
          row == representations_.types().end() ||
          row->kind != CppMirTypeRepresentationKind::Class ||
          row->spelling.empty()) {
        return result;
      }
      const std::string *dependent =
          dependentFieldTypeSpelling(*instruction->result);
      std::string spelled = dependent == nullptr ? row->spelling : *dependent;
      spelled += '(';
      for (std::size_t index = 0; index < instruction->operands.size();
           ++index) {
        const MirOperand &operand = instruction->operands[index];
        const auto found = operand.kind == MirOperandKind::Value
                               ? spellings.find(operand.value)
                               : spellings.end();
        if (found == spellings.end() ||
            operand.type != target->parameterTypes[index]) {
          return result;
        }
        if (index != 0) {
          spelled += ", ";
        }
        spelled += found->second;
      }
      spelled += ')';
      spellings.emplace(*instruction->result, std::move(spelled));
      continue;
    }
    case MirInstructionKind::Initialize: {
      const MirPlace *destination =
          instruction->destination ? body->findPlace(*instruction->destination)
                                   : nullptr;
      if (destination == nullptr ||
          destination->root != MirPlaceRootKind::Binding ||
          !destination->projections.empty() || destination->symbol == 0 ||
          !instruction->localFailureSites.empty() ||
          instruction->operands.size() > 1) {
        return result;
      }
      if (instruction->operands.empty()) {
        // The bare default: the field carries no in-class initializer text.
        result.fields.push_back({.field = destination->symbol});
        continue;
      }
      const MirOperand &operand = instruction->operands.front();
      if (operand.kind == MirOperandKind::Constant && operand.literal &&
          ScalarBodyTextEmitter::spellableLiteral(*operand.literal,
                                                  operand.type)) {
        // A frontend-evaluated constant carries its literal on the
        // operand itself.
        result.fields.push_back({.field = destination->symbol,
                                 .spelling = writer.literalSpelling(
                                     *operand.literal, operand.type)});
        continue;
      }
      const auto spelled = operand.kind == MirOperandKind::Value
                               ? spellings.find(operand.value)
                               : spellings.end();
      if (spelled == spellings.end()) {
        return result;
      }
      result.fields.push_back(
          {.field = destination->symbol, .spelling = spelled->second});
      continue;
    }
    default:
      return result;
    }
  }
  result.supported = true;
  return result;
}

CppMirBodyEmissionAnalysis
CppMirBodyEmitter::analyze(MirBodyAddress address) const {
  return BodyAnalysisBuilder(program_, representations_, address).run(true);
}

CppMirProgramEmissionAnalysis CppMirBodyEmitter::analyzeProgram() const {
  CppMirProgramEmissionAnalysis analysis;
  analysis.readiness = CppMirBodyEmissionReadiness::Ready;

  const std::vector<MirBodyAddress> addresses =
      enumerateMirBodyAddresses(program_);
  if (addresses.empty()) {
    analysis.readiness = CppMirBodyEmissionReadiness::Incoherent;
    analysis.issues.push_back(
        {.kind = CppMirBodyEmissionIssueKind::InvalidMirProgram,
         .detail = "core MIR body inventory is empty"});
    return analysis;
  }

  BodyAnalysisBuilder validation(program_, representations_, addresses.front());
  validation.validateProgram();
  validation.validateRepresentations();
  CppMirBodyEmissionAnalysis validationResult = validation.finishValidation();
  analysis.readiness =
      mergeReadiness(analysis.readiness, validationResult.readiness);
  analysis.issues = validationResult.issues;

  analysis.bodies.reserve(addresses.size());
  for (const MirBodyAddress address : addresses) {
    CppMirBodyEmissionAnalysis body;
    if (address.kind == MirBodyKind::HostedStartup) {
      // Hosted startup is a compiler-generated adapter schedule, not a
      // callable basic-block body. Its dedicated verifier below is its exact
      // text authority and intentionally needs no emitted body-name row.
      body.body = address;
      body.readiness = CppMirBodyEmissionReadiness::Ready;
    } else {
      body =
          BodyAnalysisBuilder(program_, representations_, address).run(false);
    }
    bool textSupported = false;
    if (body.ready()) {
      switch (address.kind) {
      case MirBodyKind::Module: {
        const bool executable = std::any_of(
            program_.programInitializationPlan().steps.begin(),
            program_.programInitializationPlan().steps.end(),
            [](const MirProgramInitializationStep &step) {
              return step.role == ProgramInitializationStepRole::Initializer;
            });
        const bool mayRaise = cppMirModuleMayRaiseDefinedFailure(program_);
        const bool passiveSchedule =
            !mayRaise && initializerSchedule(address).supported;
        textSupported =
            passiveSchedule ||
            (executable && (mayRaise ? supportsFailureBodyText(address)
                                     : supportsBodyText(address)));
        break;
      }
      case MirBodyKind::FieldInitializers:
      case MirBodyKind::StaticFieldInitializers:
        textSupported = initializerSchedule(address).supported;
        break;
      case MirBodyKind::Function: {
        const MirFunctionInstance *instance =
            program_.findFunctionInstance(address.owner);
        textSupported = instance != nullptr &&
                        (instance->definitionKind != MirDefinitionKind::Source
                             ? boundaryDeclarationBody(address)
                             : (instance->mayRaiseDefinedFailure
                                    ? supportsFailureBodyText(address) ||
                                          supportsBodyText(address)
                                    : supportsBodyText(address)));
        break;
      }
      case MirBodyKind::Constructor: {
        const MirConstructorInstance *instance =
            program_.findConstructorInstance(address.owner);
        textSupported = instance != nullptr &&
                        (instance->definitionKind != MirDefinitionKind::Source
                             ? boundaryDeclarationBody(address)
                             : (instance->mayRaiseDefinedFailure
                                    ? supportsFailureBodyText(address) ||
                                          supportsBodyText(address)
                                    : supportsBodyText(address)));
        break;
      }
      case MirBodyKind::Destructor: {
        const MirDestructorInstance *instance =
            program_.findDestructorInstance(address.owner);
        textSupported = instance != nullptr &&
                        (instance->definitionKind != MirDefinitionKind::Source
                             ? boundaryDeclarationBody(address)
                             : (instance->mayRaiseDefinedFailure
                                    ? supportsFailureBodyText(address) ||
                                          supportsBodyText(address)
                                    : supportsBodyText(address)));
        break;
      }
      case MirBodyKind::Lambda:
        textSupported =
            supportsFailureBodyText(address) || supportsBodyText(address);
        break;
      case MirBodyKind::HostedStartup:
        textSupported = cppMirHostedStartupNoArgumentsSchedule(program_) ||
                        cppMirHostedStartupFailureFreeSchedule(program_) ||
                        cppMirHostedStartupOwnedArgumentsSchedule(program_);
        break;
      }
    }
    if (body.ready() && !textSupported) {
      body.readiness = CppMirBodyEmissionReadiness::MissingRepresentation;
      std::string detail =
          "verified MIR body is outside the complete C++ text vocabulary";
      const auto name = std::find_if(
          representations_.bodies().begin(), representations_.bodies().end(),
          [&](const CppMirBodyNameRepresentation &row) {
            return row.address == address;
          });
      if (name != representations_.bodies().end() && !name->spelling.empty()) {
        detail += " for '" + name->spelling + "'";
      }
      body.issues.push_back(
          {.kind = CppMirBodyEmissionIssueKind::UnsupportedTextVocabulary,
           .body = address,
           .detail = std::move(detail)});
    }
    analysis.readiness = mergeReadiness(analysis.readiness, body.readiness);
    analysis.bodies.push_back(std::move(body));
  }
  return analysis;
}

} // namespace lang
