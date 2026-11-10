// Backend-neutral proofs for unrelated two-owner carriers. Each fixture must
// reach verified MIR with its exact publication structure, each forged MIR
// mutation must fail independent verification, and the shipped native
// backend must accept the program. The C++ body-planner admission checks for
// the same fixtures live in cpp_mir_user_dual_range_tests.cpp.

#include "llvm_backend_test_support.h"
#include "mir_test_fixture.h"

namespace {

lang::MirInstruction *instruction(lang::MirBody &body,
                                  lang::MirInstructionId id) {
  for (auto &block : body.blocks)
    for (auto &candidate : block.instructions)
      if (candidate.id == id)
        return &candidate;
  return nullptr;
}

std::string readFixture(const std::filesystem::path &path) {
  std::ifstream input(path);
  return {std::istreambuf_iterator<char>(input), {}};
}

void expectNativeEmission(const lang::FrontendResult &frontend,
                          std::string_view message) {
  std::string error;
  const bool emitted = gti_test::llvmEmits(
      frontend, lang::OptimizationLevel::O0, lang::TargetInfo::host(), &error);
  if (!emitted)
    std::cerr << error << '\n';
  expect(emitted, message);
}

// The parameter place an owned initializer moves from: its formal binding's
// unprojected root.
lang::MirPlaceId ownedSourcePlace(const lang::MirConstructorInstance &owner,
                                  const lang::MirConstructorInitializer &init) {
  if (!init.ownedParameter ||
      *init.ownedParameter >= owner.parameterBindings.size())
    return 0;
  const auto binding = owner.parameterBindings[*init.ownedParameter];
  for (const auto &place : owner.body.places)
    if (place.root == lang::MirPlaceRootKind::Binding &&
        place.binding == binding && place.projections.empty())
      return place.id;
  return 0;
}

lang::MirInstructionId initializerPublication(const lang::MirBody &body,
                                              std::size_t initializer) {
  for (const auto &block : body.blocks)
    for (const auto &candidate : block.instructions)
      if (candidate.constructorInitializer == initializer)
        return candidate.id;
  return 0;
}

void testMixedCarrierParameterPrefix() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/user_dual_range_direct.gti";
  const std::string source = readFixture(path);
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(!source.empty() && frontend.canGenerateCode(),
         "unrelated two-owner inline proxy fixture has verified MIR");
  if (!frontend.canGenerateCode())
    return;

  const lang::MirConstructorInstance *selected = nullptr;
  for (const auto &candidate : frontend.mir.constructorInstances()) {
    if (candidate.initializers.size() == 3 &&
        candidate.initializers[0].ownedParameter &&
        candidate.initializers[1].ownedParameter &&
        !candidate.initializers[2].ownedParameter &&
        candidate.initializers[2].targetType == lang::SemanticType::Int32) {
      expect(selected == nullptr, "the mixed carrier constructor is unique");
      selected = &candidate;
    }
  }
  expect(selected != nullptr,
         "fixture exercises two moved fields and scalar sibling");
  if (!selected)
    return;
  const auto firstSource =
      ownedSourcePlace(*selected, selected->initializers[0]);
  const auto secondSource =
      ownedSourcePlace(*selected, selected->initializers[1]);
  const auto firstPublication = initializerPublication(selected->body, 1);
  expect(firstSource && secondSource && firstSource != secondSource &&
             firstPublication,
         "both moved fields publish from distinct exact parameter places");
  if (!firstSource || !firstPublication)
    return;

  lang::MirInstructionId scalarLoad = 0, scalarInitialize = 0, scalarEnd = 0;
  for (const auto &block : selected->body.blocks)
    for (const auto &candidate : block.instructions) {
      if (candidate.kind == lang::MirInstructionKind::Load &&
          candidate.hirValue == selected->initializers[2].arguments.front())
        scalarLoad = candidate.id;
      if (candidate.constructorInitializer == 3)
        scalarInitialize = candidate.id;
      if (candidate.fullExpressionEnd != 0)
        for (const auto &expression : selected->body.fullExpressions)
          if (expression.id == candidate.fullExpressionEnd &&
              expression.constructorInitializer == 3)
            scalarEnd = candidate.id;
    }
  expect(scalarLoad && scalarInitialize && scalarEnd,
         "scalar sibling retains its exact executable publication triple");
  if (!scalarLoad || !scalarInitialize || !scalarEnd)
    return;

  expectNativeEmission(frontend,
                       "mixed carrier constructor reaches native emission");

  const auto reject = [&](auto mutate, std::string_view reason) {
    auto malformed = frontend.mir;
    auto *changed = const_cast<lang::MirConstructorInstance *>(
        malformed.findConstructorInstance(selected->id));
    mutate(*changed);
    (void)lang::rebuildMirValueUses(changed->body);
    expect(!lang::verifyMirProgram(malformed).valid(), reason);
  };
  reject(
      [&](auto &changed) {
        instruction(changed.body, scalarInitialize)->constructorInitializer = 0;
      },
      "scalar sibling cannot lose its exact initializer identity");
  reject(
      [&](auto &changed) {
        instruction(changed.body, scalarLoad)->operands.front().place =
            firstSource;
      },
      "scalar sibling cannot read a different typed parameter");
  reject(
      [&](auto &changed) {
        instruction(changed.body, scalarInitialize)->destination =
            instruction(changed.body, firstPublication)->destination;
      },
      "scalar sibling cannot publish into another field");
  reject(
      [&](auto &changed) {
        instruction(changed.body, scalarEnd)->fullExpressionEnd = 0;
      },
      "scalar sibling must retain its exact full-expression endpoint");
  reject(
      [&](auto &changed) {
        changed.initializers[1].ownedParameter =
            changed.initializers[0].ownedParameter;
      },
      "two moved fields cannot claim the same source parameter");
}

void testOwningReturnedReferenceReplacement() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/user_owning_reference_replacement.gti";
  const std::string source = readFixture(path);
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(!source.empty() && frontend.canGenerateCode(),
         "unrelated owning accessor replacement has verified MIR");
  if (!frontend.canGenerateCode())
    return;
  const lang::MirFunctionInstance *entry = nullptr;
  for (const auto &function : frontend.mir.functionInstances())
    if (function.entryKind != lang::ProgramEntryKind::None)
      entry = &function;
  expect(entry != nullptr, "owning accessor fixture has an entry point");
  if (!entry)
    return;
  const lang::MirInstruction *assignment = nullptr;
  for (const auto &block : entry->body.blocks)
    for (const auto &candidate : block.instructions) {
      const auto *destination =
          candidate.destination ? entry->body.findPlace(*candidate.destination)
                                : nullptr;
      if (candidate.kind == lang::MirInstructionKind::Assign && destination &&
          destination->root == lang::MirPlaceRootKind::Loan) {
        assignment = &candidate;
        break;
      }
    }
  expect(assignment && assignment->replacementPair &&
             assignment->lifecycle.size() == 1,
         "owning returned-reference write carries replacement and cleanup");
  if (!assignment || !assignment->replacementPair ||
      assignment->lifecycle.size() != 1)
    return;
  const auto *destination = entry->body.findPlace(*assignment->destination);
  const auto *loan = entry->body.findLoan(destination->loan);
  const lang::MirInstruction *accessor = nullptr;
  for (const auto &block : entry->body.blocks)
    for (const auto &candidate : block.instructions)
      if (candidate.kind == lang::MirInstructionKind::Call && loan &&
          candidate.loan == loan->id)
        accessor = &candidate;
  expect(loan && accessor && accessor->result,
         "replacement destination names one checked reference call");
  if (!loan || !accessor || !accessor->result)
    return;
  expect(loan->access == lang::AccessMode::Mutable &&
             accessor->borrowAccess == lang::AccessMode::Mutable,
         "owning source-call result replaces a mutable accessor referent");

  expectNativeEmission(
      frontend, "owning returned-reference assignment reaches native emission");
  const auto reject = [&](auto mutate, std::string_view reason) {
    auto malformed = frontend.mir;
    auto *changed = const_cast<lang::MirFunctionInstance *>(
        malformed.findFunctionInstance(entry->id));
    mutate(changed->body);
    (void)lang::rebuildMirValueUses(changed->body);
    expect(!lang::verifyMirProgram(malformed).valid(), reason);
  };
  reject(
      [&](auto &body) {
        auto *changed = const_cast<lang::MirLoan *>(body.findLoan(loan->id));
        changed->access = lang::AccessMode::ReadOnly;
      },
      "owning replacement cannot use a read-only returned loan");
  reject([&](auto &body) { instruction(body, accessor->id)->loan.reset(); },
         "owning replacement cannot lose exact accessor loan identity");
  reject(
      [&](auto &body) {
        instruction(body, accessor->id)->borrowAccess =
            lang::AccessMode::ReadOnly;
      },
      "source call must retain the declared mutable reference authority");
  reject(
      [&](auto &body) {
        instruction(body, assignment->id)->replacementPair = 0;
      },
      "owning returned-reference write must retain old-target cleanup");
  reject(
      [&](auto &body) {
        instruction(body, assignment->replacementPair)->replacementPair = 0;
      },
      "old-target cleanup must pair with the exact replacement");
  reject(
      [&](auto &body) {
        instruction(body, assignment->id)->lifecycle.front().source = 0;
      },
      "owning source-call stage transfers exactly its own obligation");
}

void testExclusiveGeneratedShell() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/user_mutable_inline_proxy_endpoint.gti";
  std::string source = readFixture(path);
  const auto main = source.find("\nint main(");
  expect(main != std::string::npos,
         "exclusive shell fixture has an entry point");
  if (main == std::string::npos)
    return;
  // The exact same source classes exercise shell retirement independently of
  // the structural range protocol and its separate endpoint marker proof.
  source.replace(main, std::string::npos, R"(
int main() {
  mut MutableOwnerRange range{0};
  { auto cursor = range.begin_mut(); }
  return range.at(std::size_t(0)) == 101 ? 0 : 1;
}
)");
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode(),
         "exclusive generated shell has verified MIR without range lowering");
  if (!frontend.canGenerateCode())
    return;
  const lang::MirConstructorInstance *cleanupConstructor = nullptr;
  lang::HirClassInstanceId exclusiveShell = 0;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    const auto *owner = frontend.mir.findClassInstance(constructor.owner);
    if (!owner || !owner->destructor || owner->declaredFields.size() != 1)
      continue;
    const lang::MirClassInstance *child = nullptr;
    for (const auto &candidate : frontend.mir.classInstances())
      if (candidate.type == owner->declaredFields.front().type)
        child = &candidate;
    if (!child || child->requiresActiveCleanup || child->destructor ||
        child->traits.copyable ||
        std::none_of(child->declaredFields.begin(), child->declaredFields.end(),
                     [](const auto &field) {
                       return field.type.kind ==
                                  lang::SemanticType::Reference &&
                              field.type.referenceAccess ==
                                  lang::AccessMode::Mutable;
                     }))
      continue;
    expect(cleanupConstructor == nullptr,
           "exclusive shell constructor fixture is unique");
    cleanupConstructor = &constructor;
    exclusiveShell = child->id;
  }
  expect(cleanupConstructor && exclusiveShell,
         "cleanup-bearing cursor uses an inline exclusive generated shell");
  if (!cleanupConstructor || !exclusiveShell)
    return;
  expectNativeEmission(frontend,
                       "exclusive generated shell constructor reaches native "
                       "emission with its rollback");
  const auto rejectShell = [&](auto mutate, std::string_view reason) {
    auto malformed = frontend.mir;
    auto *child = const_cast<lang::MirClassInstance *>(
        malformed.findClassInstance(exclusiveShell));
    auto *constructor = const_cast<lang::MirConstructorInstance *>(
        malformed.findConstructorInstance(cleanupConstructor->id));
    mutate(*child, *constructor);
    expect(!lang::verifyMirProgram(malformed).valid(), reason);
  };
  // Forged copyability or destructor status on the shell's class row is a
  // representation-planning concern (cpp_mir_user_dual_range); MIR itself
  // owns the exact field publication.
  rejectShell(
      [](auto &, auto &constructor) {
        constructor.initializers.front().field = 0;
      },
      "exclusive shell cannot bypass exact field publication");
}

void testRangeEndpointMarkerAuthority() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/user_mutable_inline_proxy_endpoint.gti";
  const std::string source = readFixture(path);
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode(),
         "exclusive range endpoint has verified MIR");
  if (!frontend.canGenerateCode())
    return;
  const lang::MirFunctionInstance *selected = nullptr;
  std::size_t rangeIndex = 0;
  for (const auto &function : frontend.mir.functionInstances())
    for (std::size_t index = 0; index < function.body.rangeIterations.size();
         ++index)
      if (function.body.rangeIterations[index].endpointParking) {
        expect(selected == nullptr,
               "exclusive endpoint parking fixture is unique");
        selected = &function;
        rangeIndex = index;
      }
  expect(selected != nullptr, "exclusive range records exact endpoint parking");
  if (!selected)
    return;
  auto body = selected->body;
  const auto &parking = *body.rangeIterations[rangeIndex].endpointParking;
  // Parking and restoring are pure markers: they carry no executable operand
  // and produce no runtime value.
  const auto *park = instruction(body, parking.park);
  const auto *restore = instruction(body, parking.restore);
  expect(park && restore && park->operands.empty() && !park->result &&
             restore->operands.empty() && !restore->result,
         "endpoint parking is a marker, not a runtime state change");
  expectNativeEmission(frontend,
                       "verified range endpoint markers reach native emission");
  const auto reject = [&](auto mutate, std::string_view reason) {
    auto malformed = frontend.mir;
    auto *changed = const_cast<lang::MirFunctionInstance *>(
        malformed.findFunctionInstance(selected->id));
    mutate(changed->body);
    (void)lang::rebuildMirValueUses(changed->body);
    expect(!lang::verifyMirProgram(malformed).valid(), reason);
  };
  reject(
      [&](auto &body) {
        body.rangeIterations[rangeIndex].endpointParking.reset();
      },
      "endpoint marker cannot lose its exact range inventory");
  reject([&](auto &body) { instruction(body, parking.park)->hirStatement = 0; },
         "endpoint marker retains source range identity");
  reject([&](auto &body) { instruction(body, parking.restore)->hirValue = 0; },
         "restore retains the selected endpoint identity");
  reject(
      [&](auto &body) {
        instruction(body, parking.park)
            ->operands.push_back({.kind = lang::MirOperandKind::Constant,
                                  .type = lang::SemanticType::Int32});
      },
      "endpoint marker cannot hide an executable operand");
  reject(
      [&](auto &body) {
        body.rangeIterations[rangeIndex]
            .endpointParking->failureRestores.push_back(parking.restore);
      },
      "restore has one exact normal or failure inventory membership");
}

void testScalarSiblingPositions() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/user_dual_range_scalar_siblings.gti";
  const std::string source = readFixture(path);
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode(),
         "all scalar sibling positions have verified MIR");
  if (!frontend.canGenerateCode())
    return;
  std::size_t checked = 0;
  std::vector<std::size_t> scalarPositions;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    if (constructor.initializers.size() != 3 ||
        std::count_if(constructor.initializers.begin(),
                      constructor.initializers.end(),
                      [](const auto &initializer) {
                        return initializer.ownedParameter.has_value();
                      }) != 2)
      continue;
    std::size_t scalar = 3;
    for (std::size_t index = 0; index != 3; ++index) {
      const auto &initializer = constructor.initializers[index];
      if (!initializer.ownedParameter)
        scalar = index;
      else
        expect(ownedSourcePlace(constructor, initializer) != 0 &&
                   initializerPublication(constructor.body, index + 1) != 0,
               "leading, middle and trailing scalar siblings preserve each "
               "exact parameter move publication");
    }
    scalarPositions.push_back(scalar);
    ++checked;
  }
  std::sort(scalarPositions.begin(), scalarPositions.end());
  expect(checked == 3 && scalarPositions == std::vector<std::size_t>{0, 1, 2},
         "all three scalar sibling positions are exercised");
  expectNativeEmission(frontend,
                       "mixed source view carriers reach native emission "
                       "through verified MIR");
}

void testSourceLiteralScalarSiblings() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/user_scalar_default_move.gti";
  const std::string source = readFixture(path);
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode(),
         "unrelated generic owners and view defaults have verified MIR");
  if (!frontend.canGenerateCode())
    return;
  const lang::MirConstructorInstance *mixed = nullptr;
  for (const auto &constructor : frontend.mir.constructorInstances())
    if (constructor.initializers.size() == 3 &&
        constructor.initializers[0].ownedParameter &&
        !constructor.initializers[1].ownedParameter &&
        constructor.initializers[1].targetType == lang::SemanticType::UInt64 &&
        constructor.initializers[2].ownedParameter) {
      expect(mixed == nullptr, "literal-between-view fixture is unique");
      mixed = &constructor;
    }
  expect(mixed != nullptr,
         "literal scalar sibling separates two parameter field moves");
  if (!mixed)
    return;
  lang::MirInstructionId literal = 0, initialize = 0, boundary = 0;
  for (const auto &block : mixed->body.blocks)
    for (const auto &member : block.instructions) {
      if (member.hirValue == mixed->initializers[1].arguments.front() &&
          member.kind == lang::MirInstructionKind::Compute &&
          member.operation == lang::MirOperation::Literal)
        literal = member.id;
      if (member.constructorInitializer == 2)
        initialize = member.id;
      if (member.fullExpressionEnd)
        for (const auto &expression : mixed->body.fullExpressions)
          if (expression.id == member.fullExpressionEnd &&
              expression.constructorInitializer == 2)
            boundary = member.id;
    }
  expect(literal && initialize && boundary,
         "source literal has an exact executable publication triple");
  if (!literal || !initialize || !boundary)
    return;
  std::size_t checked = 0;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    const auto *owner = frontend.mir.findClassInstance(constructor.owner);
    if (!owner)
      continue;
    const bool genericNode =
        constructor.initializers.size() == 3 &&
        owner->declaredFields.size() == 3 &&
        constructor.initializers[0].ownedParameter &&
        owner->declaredFields[1].type == lang::SemanticType::UInt64 &&
        owner->declaredFields[2].type == lang::SemanticType::UInt64;
    const bool leadingLiteral =
        constructor.initializers.size() == 2 &&
        !constructor.initializers[0].ownedParameter &&
        constructor.initializers[0].targetType == lang::SemanticType::UInt64 &&
        constructor.initializers[1].ownedParameter;
    if (!genericNode && !leadingLiteral && constructor.id != mixed->id)
      continue;
    const auto owned = std::count_if(
        constructor.initializers.begin(), constructor.initializers.end(),
        [&](const auto &initializer) {
          return ownedSourcePlace(constructor, initializer) != 0;
        });
    expect(owned == (constructor.id == mixed->id ? 2 : 1),
           "passive defaults preserve exact parameter move binding");
    // Moving the int payload cannot fail; the owning string payload can.
    const bool scalarPayload =
        genericNode &&
        constructor.parameterTypes ==
            std::vector<lang::SemanticType>{lang::SemanticType::Int32};
    expect(!scalarPayload || !constructor.mayRaiseDefinedFailure,
           "a moved scalar payload is failure-free");
    ++checked;
  }
  expect(checked == 4,
         "scalar, owning, leading and interleaved defaults are covered");
  expectNativeEmission(frontend,
                       "scalar and owning generic payloads beside source "
                       "defaults reach native emission");
  const auto reject = [&](auto mutate, std::string_view reason) {
    auto malformed = frontend.mir;
    auto *changed = const_cast<lang::MirConstructorInstance *>(
        malformed.findConstructorInstance(mixed->id));
    mutate(changed->body);
    (void)lang::rebuildMirValueUses(changed->body);
    expect(!lang::verifyMirProgram(malformed).valid(), reason);
  };
  reject([&](auto &body) { instruction(body, literal)->literal.reset(); },
         "literal sibling requires its exact typed value");
  reject(
      [&](auto &body) {
        instruction(body, literal)->operation = lang::MirOperation::Add;
      },
      "scalar prefix cannot hide runtime computation");
  reject(
      [&](auto &body) {
        instruction(body, literal)->literalProvenance.kind =
            lang::MirLiteralProvenanceKind::None;
      },
      "literal sibling requires canonical source provenance");
  reject(
      [&](auto &body) {
        instruction(body, initialize)->constructorInitializer = 0;
      },
      "literal publication retains its selected field identity");
  reject(
      [&](auto &body) { instruction(body, boundary)->fullExpressionEnd = 0; },
      "literal sibling retains its exact expression endpoint");
}

} // namespace

int main() {
  testMixedCarrierParameterPrefix();
  testOwningReturnedReferenceReplacement();
  testScalarSiblingPositions();
  testSourceLiteralScalarSiblings();
  testExclusiveGeneratedShell();
  testRangeEndpointMarkerAuthority();
  if (failures)
    return 1;
  std::cout << "Unrelated two-owner MIR proof mutations passed\n";
  return 0;
}
