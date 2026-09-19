#include "cpp_mir_body_emitter_test_fixture.h"

namespace {

void testOwningCheckedBodyEmitsWholeCleanupProof() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-body-owning-failure.gti", R"(
class FailureCleanup {
public:
  int32_t bit;
  FailureCleanup(int32_t value) : bit(value) {}
  ~FailureCleanup() { this.bit + 1; }
};

int32_t checked_increment(int32_t value) {
  FailureCleanup scope = FailureCleanup(1);
  return value + 1;
}
)");
  expect(frontend.canGenerateCode(),
         "the owning checked-operation fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const auto function = std::find_if(
      frontend.mir.functionInstances().begin(),
      frontend.mir.functionInstances().end(),
      [](const lang::MirFunctionInstance &candidate) {
        return !candidate.body.failureRecords.empty() &&
               std::any_of(candidate.body.dropObligations.begin(),
                           candidate.body.dropObligations.end(),
                           [](const lang::MirDropObligation &obligation) {
                             return obligation.dropType.requiresActiveCleanup;
                           });
      });
  expect(function != frontend.mir.functionInstances().end(),
         "the owning fixture should retain failure records and cleanup "
         "obligations in one function body");
  if (function == frontend.mir.functionInstances().end()) {
    return;
  }
  const lang::CppMirBodyEmissionMap map(completeRows(frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = function->id};
  const lang::CppMirBodyEmissionAnalysis analysis = emitter.analyze(address);
  expect(analysis.ready(),
         "an owning checked body should be ready once MIR carries its exact "
         "failure-cleanup drop");
  expect(emitter.supportsFailureBodyText(address),
         "an owning checked body should prove its failure-form cleanup text");
  if (!emitter.supportsFailureBodyText(address)) {
    return;
  }
  const lang::CppMirBodyEmissionText text =
      emitter.emitFailureBodyText(address, "owning-cleanup-test-v0", 1);
  expect(text.emitted() &&
             text.text.find(".destroy_with_failure(") != std::string::npos &&
             text.text.find("return false;") != std::string::npos,
         "the failure edge must run the engaged slot's fallible destructor "
         "before returning failure");
}

void testRichFieldInitializerSchedule() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-body-rich-field-initializers.gti", R"(
enum class Mode {
  Ready,
};

int32_t baseline = 11;

class Reading {
  int32_t value;
public:
  Reading(int32_t initial) : value(initial) {}
};

struct Marker {};

struct Component {
  mut uint64_t offset = 0;
  mut uint64_t length = 0;
};

class Dashboard {
  Mode mode = Mode::Ready;
  int32_t baseline_copy = baseline;
  Reading initial{7};
  int32_t samples[2] = {2, 3};
  Marker marker{};
  Component component{};
};

int main() {
  Dashboard dashboard = Dashboard();
  return 0;
}
)");
  expect(frontend.canGenerateCode(),
         "the rich field-initializer fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::CppMirInitializerScheduleText *selected = nullptr;
  std::optional<lang::CppMirInitializerScheduleText> schedule;
  for (const lang::MirClassInstance &instance : frontend.mir.classInstances()) {
    lang::CppMirInitializerScheduleText candidate = emitter.initializerSchedule(
        {.kind = lang::MirBodyKind::FieldInitializers, .owner = instance.id});
    if (candidate.supported && candidate.fields.size() == 6) {
      schedule = std::move(candidate);
      selected = &*schedule;
      break;
    }
  }
  expect(selected != nullptr,
         "the dashboard initializer body should expose its six-field MIR "
         "schedule");
  if (selected == nullptr) {
    return;
  }
  const auto hasSpelling = [&](std::string_view first,
                               std::string_view second = {}) {
    return std::any_of(
        selected->fields.begin(), selected->fields.end(),
        [&](const lang::CppMirFieldInitializerSpelling &field) {
          return field.spelling.find(first) != std::string::npos &&
                 (second.empty() ||
                  field.spelling.find(second) != std::string::npos);
        });
  };
  expect(hasSpelling("Mode::Ready"),
         "the enum field should use the copied enumerator storage row");
  expect(hasSpelling("::__gti_program::baseline"),
         "the global-backed field should use the copied program-storage "
         "row");
  expect(hasSpelling("Reading", "(static_cast<std::int32_t>(7))"),
         "the direct field construction should be reconstructed from the "
         "MIR Construct and CallInput stages");
  expect(hasSpelling("std::array<std::int32_t, 2>",
                     "{static_cast<std::int32_t>(2), "
                     "static_cast<std::int32_t>(3)}"),
         "the fixed-array field aggregate should preserve its staged element "
         "order");
  expect(hasSpelling("Marker{}"),
         "a targetless default Construct should publish an empty class field "
         "initializer from verified MIR");
  expect(hasSpelling("Component{}"),
         "a targetless generated default Construct should publish a non-empty "
         "class field initializer from verified MIR");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(artifact.contents.find("Marker marker =") != std::string::npos &&
             artifact.contents.find("Marker{}") != std::string::npos &&
             artifact.contents.find("Component component =") !=
                 std::string::npos &&
             artifact.contents.find("Component{}") != std::string::npos,
         "the complete C++ backend should emit verified targetless empty and "
         "non-empty class field initializers without a vocabulary error");
}

void testDependentFieldInitializerSchedule() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-body-dependent-field-initializers.gti", R"(
class StaticArray<T, uint64_t N> {
  T values[N] = {};
};

class WrappedArray<T, uint64_t N> {
  StaticArray<T, N> value = StaticArray<T, N>();
};

int main() {
  StaticArray<int32_t, 4> four = StaticArray<int32_t, 4>();
  StaticArray<int32_t, 8> eight = StaticArray<int32_t, 8>();
  WrappedArray<int32_t, 4> wrapped_four = WrappedArray<int32_t, 4>();
  WrappedArray<int32_t, 8> wrapped_eight = WrappedArray<int32_t, 8>();
  return 0;
}
)");
  expect(frontend.canGenerateCode(),
         "dependent field-initializer fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  std::size_t arrays = 0;
  std::size_t wrappers = 0;
  std::unordered_set<std::string> arraySpellings;
  std::unordered_set<std::string> wrapperSpellings;
  for (const lang::MirClassInstance &instance : frontend.mir.classInstances()) {
    const lang::ClassTypeInfo *info =
        frontend.semantics.findClassType(instance.declaration);
    if (info == nullptr || info->declaration == nullptr ||
        (info->declaration->name().lexeme != "StaticArray" &&
         info->declaration->name().lexeme != "WrappedArray")) {
      continue;
    }
    const lang::CppMirInitializerScheduleText schedule =
        emitter.initializerSchedule(
            {.kind = lang::MirBodyKind::FieldInitializers,
             .owner = instance.id});
    expect(schedule.supported && schedule.fields.size() == 1,
           "each dependent class instance should retain one verified field "
           "initializer");
    if (!schedule.supported || schedule.fields.size() != 1) {
      continue;
    }
    const std::string &spelling = schedule.fields.front().spelling;
    if (info->declaration->name().lexeme == "StaticArray") {
      expect((spelling == "std::array<std::int32_t, 4>{}" ||
              spelling == "std::array<std::int32_t, 8>{}"),
             "each direct StaticArray instance should project its concrete "
             "aggregate spelling");
      arraySpellings.insert(spelling);
      ++arrays;
    } else {
      expect(spelling.find("::__gti_type_") != std::string::npos &&
                 spelling.find("_StaticArray{}") != std::string::npos &&
                 spelling.find("<T, N>") == std::string::npos,
             "each direct WrappedArray instance should project its concrete "
             "nested-class default-construction spelling");
      wrapperSpellings.insert(spelling);
      ++wrappers;
    }
  }
  expect(arrays == 2 && wrappers == 2 && arraySpellings.size() == 2 &&
             wrapperSpellings.size() == 2,
         "the dependent initializer fixture should exercise both concrete "
         "instances and spell each physical representation distinctly");
}

void testContainedOwningFieldConstructor() {
  const lang::FrontendResult frontend = analyzeWithStandardLibrary(
      "cpp-mir-body-owning-field-constructor.gti", R"(
#include <std/vector>

class Holder {
  mut std::vector<int32_t> values = std::vector<int32_t>();

public:
  Holder() {}
};

int32_t main() {
  Holder value{};
  return 0;
}
)");
  expect(frontend.canGenerateCode(),
         "the owning field-constructor fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::HirClassInstance *holder = nullptr;
  for (const lang::HirClassInstance &instance : frontend.hir.classInstances()) {
    if (instance.source != nullptr &&
        instance.source->name().lexeme == "Holder") {
      holder = &instance;
      break;
    }
  }
  expect(holder != nullptr,
         "the fixture should retain the Holder HIR class instance");
  if (holder == nullptr) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirClassInstance *owner =
      frontend.mir.findClassInstance(holder->id);
  expect(owner != nullptr, "the fixture should retain the Holder MIR instance");
  if (owner == nullptr) {
    return;
  }
  const lang::CppMirInitializerScheduleText schedule =
      emitter.initializerSchedule(
          {.kind = lang::MirBodyKind::FieldInitializers, .owner = owner->id});
  expect(schedule.supported && schedule.fields.size() == 1,
         "Holder's native in-class field schedule should be verified by MIR");
  expect(owner->fieldInitializers.dropObligations.size() == 2 &&
             !owner->fieldInitializers.failureRecords.empty(),
         "Holder's field-initializer MIR should retain the temporary, field "
         "rollback, and checked construction edge erased by native direct "
         "initialization");

  const lang::MirConstructorInstance *ownerConstructor = nullptr;
  for (const lang::MirConstructorInstance &constructor :
       frontend.mir.constructorInstances()) {
    if (constructor.owner == owner->id) {
      ownerConstructor = &constructor;
    }
  }
  expect(ownerConstructor != nullptr &&
             ownerConstructor->mayRaiseDefinedFailure,
         "Holder's concrete constructor should retain the field-initializer "
         "failure effect even when its own source body is empty");
  if (ownerConstructor == nullptr) {
    return;
  }
  const std::size_t defaultFieldRollbacks = std::count_if(
      ownerConstructor->body.dropObligations.begin(),
      ownerConstructor->body.dropObligations.end(),
      [](const lang::MirDropObligation &drop) {
        return drop.kind == lang::MirDropObligationKind::ConstructionRollback &&
               drop.initiallyActive;
      });
  expect(defaultFieldRollbacks == 1,
         "the completed owning in-class initializer should enter Holder's "
         "constructor as one active rollback obligation");
  const lang::CppMirBodyEmissionAnalysis constructorAnalysis = emitter.analyze(
      {.kind = lang::MirBodyKind::Constructor, .owner = ownerConstructor->id});
  expect(constructorAnalysis.ready() &&
             !hasIssue(constructorAnalysis,
                       lang::CppMirBodyEmissionIssueKind::
                           MissingPartialConstructionRollbackMir),
         "the body emitter should consume the verified default-field "
         "rollback instead of reporting a construction gap");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const auto marker = [&](std::size_t constructor) {
    return artifact.contents.find("scalar-cfg-v1 constructor-instance " +
                                  std::to_string(constructor) + "\n") !=
           std::string::npos;
  };
  expect(marker(ownerConstructor->id),
         "the ordinary Holder constructor should publish from verified MIR "
         "after its checked field construction is contained at the native "
         "boundary");
}

void testContainedOwningStringFieldConstructor() {
  const lang::FrontendResult frontend = analyzeWithStandardLibrary(
      "cpp-mir-body-owning-string-field-constructor.gti", R"(
#include <std/string>

class StringHolder {
public:
  StringHolder() {}

  std::size_t size() {
    return text.size() + shadow.size();
  }

private:
  mut std::string text = std::string("");
  mut std::string shadow = std::string("");
};

int main() {
  mut StringHolder value = StringHolder();
  return value.size() == 0 ? 0 : 1;
}
)");
  expect(frontend.canGenerateCode(),
         "the owning string-field fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirClassInstance *owner = nullptr;
  for (const lang::MirClassInstance &instance : frontend.mir.classInstances()) {
    const lang::ClassTypeInfo *info =
        frontend.semantics.findClassType(instance.declaration);
    if (info != nullptr && info->declaration != nullptr &&
        info->declaration->name().lexeme == "StringHolder") {
      owner = &instance;
      break;
    }
  }
  expect(owner != nullptr,
         "the string-field fixture should retain its concrete MIR owner");
  if (owner == nullptr) {
    return;
  }

  const lang::MirInstruction *fieldConstruct = nullptr;
  std::size_t fieldConstructs = 0;
  for (const lang::MirBlock &block : owner->fieldInitializers.blocks) {
    for (const lang::MirInstruction &instruction : block.instructions) {
      if (instruction.kind != lang::MirInstructionKind::Construct ||
          !instruction.constructorTarget) {
        continue;
      }
      ++fieldConstructs;
      if (fieldConstruct == nullptr) {
        fieldConstruct = &instruction;
      }
    }
  }
  const lang::MirConstructorInstance *fieldConstructor =
      fieldConstruct == nullptr ? nullptr
                                : frontend.mir.findConstructorInstance(
                                      *fieldConstruct->constructorTarget);
  expect(fieldConstructs == 2 && fieldConstruct != nullptr &&
             fieldConstructor != nullptr &&
             fieldConstructor->mayRaiseDefinedFailure,
         "both std::string field constructions should retain their exact "
         "checked constructor target");
  if (fieldConstructs != 2 || fieldConstruct == nullptr ||
      fieldConstructor == nullptr) {
    return;
  }
  expect(owner->fieldInitializers.dropObligations.size() == 4 &&
             owner->fieldInitializers.failureRecords.size() == 2 &&
             owner->fieldInitializers.cleanupBoundaries.size() == 1 &&
             std::any_of(
                 owner->fieldInitializers.blocks.begin(),
                 owner->fieldInitializers.blocks.end(),
                 [](const lang::MirBlock &block) {
                   return std::any_of(
                       block.instructions.begin(), block.instructions.end(),
                       [](const lang::MirInstruction &instruction) {
                         return instruction.kind ==
                                    lang::MirInstructionKind::Drop &&
                                instruction.lifecycle.size() == 1 &&
                                instruction.lifecycle.front().failureCleanup;
                       });
                 }),
         "the second owning field should retain exact rollback MIR for the "
         "first completed field");

  lang::CppMirBodyEmissionMapRows rows =
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23);
  const auto contained = std::find_if(
      rows.containedConstructors.begin(), rows.containedConstructors.end(),
      [&](const lang::CppMirContainedConstructorRepresentation &row) {
        return row.constructor == fieldConstructor->id;
      });
  expect(contained != rows.containedConstructors.end() &&
             contained->ownerType == fieldConstruct->info.type &&
             contained->parameterTypes == fieldConstructor->parameterTypes,
         "the representation row builder should inventory the exact contained "
         "std::string constructor representation");
  if (contained == rows.containedConstructors.end()) {
    return;
  }
  const std::string tagSpelling = contained->tagSpelling;
  const std::string stateSpelling = contained->stateSpelling;

  lang::CppMirBodyEmissionMapRows duplicateRows = rows;
  duplicateRows.containedConstructors.push_back(*contained);
  const lang::CppMirBodyEmissionMap duplicateMap(std::move(duplicateRows));
  const lang::CppMirProgramEmissionAnalysis duplicateAnalysis =
      lang::CppMirBodyEmitter(frontend.mir, duplicateMap).analyzeProgram();
  expect(std::any_of(duplicateAnalysis.issues.begin(),
                     duplicateAnalysis.issues.end(),
                     [](const lang::CppMirBodyEmissionIssue &issue) {
                       return issue.kind ==
                              lang::CppMirBodyEmissionIssueKind::
                                  DuplicateContainedConstructorRepresentation;
                     }),
         "a duplicate contained-constructor row should invalidate the copied "
         "representation map");

  const lang::CppMirBodyEmissionMap map(rows);
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::CppMirInitializerScheduleText schedule =
      emitter.initializerSchedule(
          {.kind = lang::MirBodyKind::FieldInitializers, .owner = owner->id});
  expect(
      schedule.supported && schedule.fields.size() == 2 &&
          std::all_of(schedule.fields.begin(), schedule.fields.end(),
                      [&](const lang::CppMirFieldInitializerSpelling &field) {
                        return field.spelling.find(tagSpelling) !=
                                   std::string::npos &&
                               field.spelling.find(stateSpelling) !=
                                   std::string::npos;
                      }),
      "the field schedule should select the exact terminal-containment "
      "constructor overload for both owning fields and prove the retained "
      "cleanup-only failure branch");

  std::erase_if(rows.containedConstructors,
                [&](const lang::CppMirContainedConstructorRepresentation &row) {
                  return row.constructor == fieldConstructor->id;
                });
  const lang::CppMirBodyEmissionMap missingRowMap(std::move(rows));
  const lang::CppMirBodyEmitter missingRowEmitter(frontend.mir, missingRowMap);
  expect(
      !missingRowEmitter
           .initializerSchedule({.kind = lang::MirBodyKind::FieldInitializers,
                                 .owner = owner->id})
           .supported,
      "removing the exact contained-constructor row should fail closed");

  const lang::BackendArtifact artifact = gti_test::emitCpp(frontend);
  expect(artifact.contents.find(tagSpelling) != std::string::npos &&
             artifact.contents.find(stateSpelling) != std::string::npos &&
             artifact.contents.find(lang::cppMirFailureConstructorTagSpelling(
                 fieldConstructor->id)) != std::string::npos &&
             artifact.contents.find("&__gti_mir_contained_state.success, "
                                    "&__gti_mir_contained_state.record)") !=
                 std::string::npos &&
             artifact.contents.find("gti_rt_failure_terminate_v1") !=
                 std::string::npos,
         "the C++ artifact should emit the contained overload, delegate to "
         "the exact failure constructor, and terminate with its record");
}

void testBodyOrderedOwnedParameterConstructor() {
  const lang::FrontendResult frontend = analyzeWithStandardLibrary(
      "cpp-mir-body-ordered-owned-parameter-constructor.gti", R"(
#include <std/string>

class OrderedPath {
public:
  OrderedPath(std::string value)
      : first(value.clone()), second(std::move(value)) {
    first.append("!");
  }

  std::size_t first_size() {
    return first.size();
  }

  std::size_t second_size() {
    return second.size();
  }

private:
  mut std::string first = std::string("");
  mut std::string second = std::string("");
};

int main() {
  mut OrderedPath path = OrderedPath(std::string("hello"));
  if (path.first_size() != 6 || path.second_size() != 5) {
    return 1;
  }
  return 0;
}
)");
  expect(frontend.canGenerateCode(),
         "the body-ordered owned-parameter constructor should pass the "
         "frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirConstructorInstance *constructor = nullptr;
  for (const lang::MirConstructorInstance &candidate :
       frontend.mir.constructorInstances()) {
    if (candidate.initializers.size() == 2 &&
        !candidate.initializers.front().ownedParameter &&
        candidate.initializers.back().ownedParameter) {
      constructor = &candidate;
      break;
    }
  }
  expect(constructor != nullptr && constructor->mayRaiseDefinedFailure,
         "the ordered constructor should retain its clone, move, and body "
         "failure schedule");
  if (constructor == nullptr) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Constructor,
                                     .owner = constructor->id};
  expect(emitter.analyze(address).ready() &&
             emitter.supportsFailureBodyText(address),
         "the transformed constructor should preserve body-ordered clone and "
         "owned-parameter publication");
  if (!emitter.supportsFailureBodyText(address)) {
    return;
  }

  const std::string text =
      emitter
          .emitFailureBodyText(address,
                               "body-ordered-owned-parameter-constructor-v0", 1)
          .text;
  const std::size_t clone = text.find("clone__gti_mir_failure");
  const std::size_t parameterMove = text.find("std::move(__gti_mir_p_", clone);
  expect(clone != std::string::npos && parameterMove != std::string::npos &&
             clone < parameterMove &&
             text.find("construction rollback deferred") != std::string::npos,
         "the emitted constructor should clone before moving the parameter "
         "and retain exact later-failure rollback");

  const lang::BackendArtifact artifact = gti_test::emitCpp(frontend);
  expect(!artifact.contents.empty() &&
             artifact.contents.find("mir_failure_constructor_tag_v1") !=
                 std::string::npos &&
             artifact.contents.find("mir_empty_state_v1") != std::string::npos,
         "production emission should build the transformed initializer list "
         "without dereferencing absent native owned-field bindings");

  lang::MirProgram mismatchedReceiver = frontend.mir;
  auto &constructors = const_cast<std::vector<lang::MirConstructorInstance> &>(
      mismatchedReceiver.constructorInstances());
  bool mutated = false;
  for (lang::MirConstructorInstance &candidate : constructors) {
    if (candidate.id != constructor->id) {
      continue;
    }
    for (lang::MirBlock &block : candidate.body.blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::CallInput &&
            instruction.callInputRole == lang::MirCallInputRole::Receiver) {
          instruction.callInputRole = lang::MirCallInputRole::Argument;
          mutated = true;
          break;
        }
      }
      if (mutated) {
        break;
      }
    }
  }
  expect(mutated,
         "the near-miss fixture should expose the clone receiver stage");
  if (mutated) {
    const lang::CppMirBodyEmitter mismatchedEmitter(mismatchedReceiver, map);
    expect(!mismatchedEmitter.supportsFailureBodyText(address),
           "member-call field publication should fail closed when its exact "
           "receiver schedule is absent");
  }
}

void testPassiveOwnedParameterConstructorFields() {
  const lang::FrontendResult frontend = analyzeWithStandardLibrary(
      "cpp-mir-passive-owned-parameter-constructor.gti", R"(
struct Pair<T, U> {
  mut T first;
  mut U second;

  Pair(T first_value, U second_value)
      : first(std::move(first_value)), second(std::move(second_value)) {}
};

int main() {
  mut Pair<int32_t, int64_t> value =
      Pair<int32_t, int64_t>(int32_t(3), int64_t(4));
  return value.first + int32_t(value.second) - 7;
}
)");
  expect(frontend.canGenerateCode(),
         "the passive owned-parameter constructor should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirConstructorInstance *constructor = nullptr;
  for (const lang::MirConstructorInstance &candidate :
       frontend.mir.constructorInstances()) {
    if (candidate.parameterTypes ==
            std::vector<lang::SemanticType>{lang::SemanticType::Int32,
                                            lang::SemanticType::Int64} &&
        candidate.initializers.size() == 2 &&
        std::all_of(candidate.initializers.begin(),
                    candidate.initializers.end(),
                    [](const lang::MirConstructorInitializer &initializer) {
                      return initializer.ownedParameter.has_value();
                    })) {
      constructor = &candidate;
      break;
    }
  }
  expect(constructor != nullptr && constructor->mayRaiseDefinedFailure,
         "the concrete generic constructor should retain both conservative "
         "owned-parameter initializers");
  if (constructor == nullptr) {
    return;
  }

  const auto bindings =
      lang::cppMirOwnedParameterFieldBindings(frontend.mir, *constructor);
  expect(bindings && bindings->size() == 2 &&
             std::all_of(
                 bindings->begin(), bindings->end(),
                 [](const lang::CppMirOwnedParameterFieldBinding &binding) {
                   return binding.sourcePlace != 0 && binding.movedValue != 0 &&
                          binding.moveInstruction != 0 &&
                          binding.initializeInstruction != 0 &&
                          binding.dropInstruction == 0 &&
                          binding.parameterDrop == 0;
                 }),
         "passive moved parameters should bind both fields while proving the "
         "absence of lexical drop records");
  if (!bindings || bindings->empty()) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Constructor,
                                     .owner = constructor->id};
  expect(emitter.supportsFailureBodyText(address) &&
             emitter.analyzeProgram().ready(),
         "the exact passive move schedule should make the constructor and "
         "its caller ready for C++ text emission");

  const lang::BackendArtifact artifact = gti_test::emitCpp(frontend);
  expect(!artifact.contents.empty() &&
             artifact.contents.find("first(std::move(__gti_mir_arg_0))") !=
                 std::string::npos &&
             artifact.contents.find("second(std::move(__gti_mir_arg_1))") !=
                 std::string::npos,
         "production emission should publish both passive fields directly "
         "from their moved constructor parameters");

  lang::MirProgram malformed = frontend.mir;
  auto &constructors = const_cast<std::vector<lang::MirConstructorInstance> &>(
      malformed.constructorInstances());
  auto changed =
      std::find_if(constructors.begin(), constructors.end(),
                   [&](const lang::MirConstructorInstance &candidate) {
                     return candidate.id == constructor->id;
                   });
  if (changed != constructors.end()) {
    auto &places =
        const_cast<std::vector<lang::MirPlace> &>(changed->body.places);
    auto source = std::find_if(
        places.begin(), places.end(), [&](const lang::MirPlace &place) {
          return place.id == bindings->front().sourcePlace;
        });
    if (source != places.end()) {
      source->traits.drop = lang::DropKind::Lexical;
    }
  }
  const lang::MirConstructorInstance *changedConstructor =
      malformed.findConstructorInstance(constructor->id);
  expect(changedConstructor != nullptr &&
             !lang::cppMirOwnedParameterFieldBindings(malformed,
                                                      *changedConstructor),
         "a lexical parameter with no drop schedule should still fail closed");
}

void testMoveOnlyClassResultReceiverFieldTransfer() {
  lang::MirBlock proof;
  lang::MirInstruction move;
  move.id = 1;
  move.kind = lang::MirInstructionKind::Move;
  move.lifecycle.push_back(
      {.kind = lang::MirLifecycleEventKind::Move, .source = 1, .target = 2});
  lang::MirInstruction drop;
  drop.id = 2;
  drop.kind = lang::MirInstructionKind::Drop;
  drop.lifecycle.push_back(
      {.kind = lang::MirLifecycleEventKind::Drop, .source = 1});
  proof.instructions = {move, drop};
  expect(lang::mirDropFollowsDefiniteMove(proof, 1, 2) &&
             !lang::mirDropFollowsDefiniteMove(proof, 2, 2) &&
             !lang::mirDropFollowsDefiniteMove(proof, 1) &&
             !lang::mirDropFollowsDefiniteMove(proof, 0, 2) &&
             !lang::mirDropFollowsDefiniteMove(proof, 1, 99),
         "moved-shell proof must name the exact still-moved source before "
         "its retirement");
  for (int mutation = 0; mutation < 6; ++mutation) {
    lang::MirBlock changed = proof;
    auto &instruction = changed.instructions.front();
    auto &event = instruction.lifecycle.front();
    switch (mutation) {
    case 0:
      event.conditional = true;
      break;
    case 1:
      event.kind = lang::MirLifecycleEventKind::Reparent;
      break;
    case 2:
      event.kind = lang::MirLifecycleEventKind::TransferOut;
      break;
    case 3:
      instruction.tombstone = true;
      break;
    case 4:
      instruction.lifecycle.push_back(
          {.kind = lang::MirLifecycleEventKind::Initialize, .target = 1});
      break;
    case 5:
      event.kind = lang::MirLifecycleEventKind::Replace;
      event.target = 1;
      break;
    }
    expect(!lang::mirDropFollowsDefiniteMove(changed, 1, 2),
           "conditional, retired, tombstoned, or reinitialized owners must "
           "not acquire a moved-shell proof");
  }

  const lang::FrontendResult classFields =
      analyzeWithStandardLibrary("cpp-mir-class-owned-parameter-fields.gti", R"(
#include <std/memory>
class Passive {
public:
  int32_t value;
  Passive() : value(0) {}
  Passive(int32_t initial) : value(initial) {}
  Passive(Passive& other) = delete;
  Passive(Passive&& other) = default;
};
class Active {
public:
  int32_t value;
  Active() : value(0) {}
  Active(int32_t initial) : value(initial) {}
  Active(Active& other) = delete;
  Active(Active&& other) = default;
  ~Active() { std::println(this.value); }
};
struct Node<T> {
  mut T value;
  mut std::unique_ptr<Node<T>> next = std::unique_ptr<Node<T>>();
  Node() requires std::default_initializable<T> : value(T()) {}
  Node(T initial, std::unique_ptr<Node<T>> following)
      : value(std::move(initial)), next(std::move(following)) {}
};
int main() {
  auto passive = std::make_unique<Node<Passive>>(
      Passive(1), std::unique_ptr<Node<Passive>>());
  auto active = std::make_unique<Node<Active>>(
      Active(2), std::unique_ptr<Node<Active>>());
  auto defaultPassive = std::make_unique<Node<Passive>>();
  auto defaultActive = std::make_unique<Node<Active>>();
  return 0;
}
)");
  expect(classFields.canGenerateCode(),
         "mixed and cleanup-owning node constructors should reach MIR");
  std::size_t classFieldConstructors = 0;
  for (const lang::MirConstructorInstance &constructor :
       classFields.mir.constructorInstances()) {
    if (constructor.parameterTypes.size() != 2 ||
        constructor.parameterTypes.front().kind != lang::SemanticType::Class ||
        constructor.initializers.size() != 2) {
      continue;
    }
    const auto bindings =
        lang::cppMirOwnedParameterFieldBindings(classFields.mir, constructor);
    expect(bindings && bindings->size() == 2,
           "the field-order proof must accept passive class moves and a "
           "shared transfer of two active field obligations");
    if (!bindings || bindings->size() != 2) {
      continue;
    }
    ++classFieldConstructors;
    const auto &first = bindings->front();
    const auto &second = bindings->back();
    bool sawMovedDrop = false;
    for (const auto &block : constructor.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.id == first.dropInstruction && first.fieldDrop != 0) {
          sawMovedDrop = true;
          expect(lang::mirDropFollowsDefiniteMove(block, first.parameterDrop,
                                                  instruction.id) &&
                     instruction.definedFailure.empty(),
                 "a definitely moved constructor parameter must retire its "
                 "shell without a fallible source-destructor edge");
        }
      }
    }
    if (first.fieldDrop == 0) {
      expect(first.initializeInstruction == 0 && first.parameterDrop != 0 &&
                 second.initializeInstruction != 0,
             "a passive lexical class publishes through initializer metadata "
             "without inventing an active field obligation");
    } else {
      expect(sawMovedDrop,
             "the active class parameter must retain its shell drop");
      expect(first.fieldDrop != second.fieldDrop &&
                 first.fieldTransferInstruction ==
                     second.fieldTransferInstruction,
             "distinct active field obligations may share one transfer record");
    }
    for (int mutation = 0; mutation < 2; ++mutation) {
      lang::MirProgram malformed = classFields.mir;
      auto &constructors =
          const_cast<std::vector<lang::MirConstructorInstance> &>(
              malformed.constructorInstances());
      for (auto &candidate : constructors) {
        if (candidate.id != constructor.id) {
          continue;
        }
        for (auto &block : candidate.body.blocks) {
          for (auto &instruction : block.instructions) {
            if (instruction.id != second.fieldTransferInstruction) {
              continue;
            }
            for (auto &event : instruction.lifecycle) {
              if (event.source != second.fieldDrop) {
                continue;
              }
              if (mutation == 0) {
                event.conditional = true;
              } else {
                event.source = first.parameterDrop;
              }
            }
          }
        }
        expect(
            !lang::cppMirOwnedParameterFieldBindings(malformed, candidate),
            "conditional or retargeted batch field transfer must fail closed");
      }
    }
  }
  expect(classFieldConstructors == 2,
         "both concrete node-constructor families need an exact field proof");
  std::size_t defaultFieldConstructors = 0;
  for (const auto &constructor : classFields.mir.constructorInstances()) {
    if (!constructor.parameterTypes.empty() ||
        constructor.initializers.size() != 1 ||
        constructor.initializers.front().targetType.kind !=
            lang::SemanticType::Class) {
      continue;
    }
    const auto bindings = lang::cppMirConstructedFieldInitializerBindings(
        classFields.mir, constructor);
    expect(bindings.size() == 1,
           "zero-argument class construction may precede an implicit empty "
           "owner without losing its exact field publication");
    if (bindings.size() != 1) {
      continue;
    }
    ++defaultFieldConstructors;
    for (int mutation = 0; mutation < 3; ++mutation) {
      lang::MirProgram malformed = classFields.mir;
      auto *changed = const_cast<lang::MirConstructorInstance *>(
          malformed.findConstructorInstance(constructor.id));
      auto *owner = const_cast<lang::MirClassInstance *>(
          malformed.findClassInstance(constructor.owner));
      if (mutation == 0) {
        for (auto &block : changed->body.blocks) {
          for (auto &instruction : block.instructions) {
            if (instruction.id == bindings.front().constructInstruction) {
              instruction.callSite = instruction.hirValue + 10000;
            }
          }
        }
      } else if (mutation == 1) {
        for (auto &drop : changed->body.dropObligations) {
          if (drop.initiallyActive) {
            drop.initiallyActive = false;
          }
        }
      } else {
        for (auto &block : owner->fieldInitializers.blocks) {
          for (auto &instruction : block.instructions) {
            if (instruction.kind == lang::MirInstructionKind::Call) {
              instruction.functionTarget = 1;
            }
          }
        }
      }
      expect(
          lang::cppMirConstructedFieldInitializerBindings(malformed, *changed)
              .empty(),
          "forged constructor identity, inactive default-field cleanup, "
          "and observable declaration defaults must fail the field proof");
    }
  }
  expect(defaultFieldConstructors == 2,
         "both passive and destructor-bearing defaults need an exact proof");
  if (classFieldConstructors == 2) {
    expect(!gti_test::emitCpp(classFields).contents.empty(),
           "mixed and active node constructors must have production emission");
  }

  const lang::FrontendResult frontend = analyzeWithStandardLibrary(
      "cpp-mir-move-only-result-field-transfer.gti", R"(
#include <std/memory>

class Node {
public:
  mut int32_t value;
  mut std::unique_ptr<Node> next = std::unique_ptr<Node>();

  Node(int32_t initial, std::unique_ptr<Node> following)
      : value(std::move(initial)), next(std::move(following)) {}
};

class List {
  mut std::unique_ptr<Node> head = std::unique_ptr<Node>();

public:
  List() {}

  void push(int32_t value) mut {
    this.head =
        std::make_unique<Node>(std::move(value), std::move(this.head));
  }

  int32_t front() { return this.head->value; }
};

int main() {
  mut List values = List();
  values.push(1);
  return values.front() - 1;
}
)");
  expect(frontend.canGenerateCode(),
         "the unique-owner replacement fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirConstructorInstance *nodeConstructor = nullptr;
  for (const lang::MirConstructorInstance &candidate :
       frontend.mir.constructorInstances()) {
    if (candidate.parameterTypes.size() == 2 &&
        candidate.parameterTypes.front() == lang::SemanticType::Int32 &&
        candidate.parameterTypes.back().kind == lang::SemanticType::Class &&
        candidate.initializers.size() == 2 &&
        std::all_of(candidate.initializers.begin(),
                    candidate.initializers.end(),
                    [](const lang::MirConstructorInitializer &initializer) {
                      return initializer.ownedParameter.has_value();
                    })) {
      nodeConstructor = &candidate;
      break;
    }
  }
  const auto constructorBindings =
      nodeConstructor == nullptr
          ? std::optional<std::vector<lang::CppMirOwnedParameterFieldBinding>>{}
          : lang::cppMirOwnedParameterFieldBindings(frontend.mir,
                                                    *nodeConstructor);
  expect(nodeConstructor != nullptr && constructorBindings &&
             constructorBindings->size() == 2 &&
             constructorBindings->front().parameterDrop == 0 &&
             constructorBindings->front().valueDrop == 0 &&
             constructorBindings->front().fieldDrop == 0 &&
             constructorBindings->front().fieldTransferInstruction == 0 &&
             constructorBindings->back().parameterDrop != 0 &&
             constructorBindings->back().valueDrop != 0 &&
             constructorBindings->back().fieldDrop != 0 &&
             constructorBindings->back().fieldTransferInstruction != 0,
         "the mixed constructor should distinguish its passive field from "
         "the exact active Move/Reparent/TransferOut field schedule");

  const lang::MirFunctionInstance *push = nullptr;
  const lang::MirInstruction *store = nullptr;
  const lang::MirInstruction *resultDrop = nullptr;
  const lang::MirPlace *resultShell = nullptr;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    for (const lang::MirBlock &block : function.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (push != nullptr ||
            instruction.kind != lang::MirInstructionKind::Assign ||
            !instruction.destination || instruction.operands.size() != 1 ||
            instruction.operands.front().kind != lang::MirOperandKind::Value ||
            instruction.operands.front().type.kind !=
                lang::SemanticType::Class) {
          continue;
        }
        const lang::MirPlace *destination =
            function.body.findPlace(*instruction.destination);
        if (destination == nullptr ||
            destination->root != lang::MirPlaceRootKind::This) {
          continue;
        }
        const lang::MirValue *assigned =
            function.body.findValue(instruction.operands.front().value);
        const lang::MirInstruction *producer = nullptr;
        if (assigned != nullptr) {
          for (const lang::MirBlock &candidateBlock : function.body.blocks) {
            for (const lang::MirInstruction &candidate :
                 candidateBlock.instructions) {
              if (candidate.id == assigned->definition) {
                producer = &candidate;
              }
            }
          }
        }
        const lang::MirFunctionInstance *factory =
            producer != nullptr && producer->functionTarget
                ? frontend.mir.findFunctionInstance(*producer->functionTarget)
                : nullptr;
        const bool allocationFactory =
            factory != nullptr &&
            std::any_of(factory->body.blocks.begin(),
                        factory->body.blocks.end(),
                        [](const lang::MirBlock &candidateBlock) {
                          return std::any_of(
                              candidateBlock.instructions.begin(),
                              candidateBlock.instructions.end(),
                              [](const lang::MirInstruction &candidate) {
                                return candidate.intrinsic ==
                                       lang::IntrinsicKind::AllocateUniqueOwner;
                              });
                        });
        if (!allocationFactory) {
          continue;
        }
        const lang::MirPlace *shell = nullptr;
        for (const lang::MirPlace &place : function.body.places) {
          if (place.root == lang::MirPlaceRootKind::Value &&
              place.value == instruction.operands.front().value &&
              place.projections.empty()) {
            shell = &place;
          }
        }
        const lang::MirInstruction *drop = nullptr;
        if (shell != nullptr) {
          for (const lang::MirBlock &candidateBlock : function.body.blocks) {
            for (const lang::MirInstruction &candidate :
                 candidateBlock.instructions) {
              if (candidate.kind == lang::MirInstructionKind::Drop &&
                  candidate.destination == shell->id) {
                drop = &candidate;
              }
            }
          }
        }
        if (shell != nullptr && drop != nullptr) {
          push = &function;
          store = &instruction;
          resultShell = shell;
          resultDrop = drop;
        }
      }
    }
  }
  expect(push != nullptr && store != nullptr && resultShell != nullptr &&
             resultDrop != nullptr,
         "the replacement should retain its consuming field store and the "
         "moved-from result-shell drop");
  if (push == nullptr || store == nullptr || resultShell == nullptr ||
      resultDrop == nullptr) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = push->id};
  const lang::CppMirBodyEmissionAnalysis bodyAnalysis =
      emitter.analyze(address);
  const lang::CppMirProgramEmissionAnalysis programAnalysis =
      emitter.analyzeProgram();
  const bool failureBody = emitter.supportsFailureBodyText(address);
  if (!failureBody || !bodyAnalysis.ready() || !programAnalysis.ready()) {
    for (const lang::CppMirBodyEmissionIssue &issue : bodyAnalysis.issues) {
      std::cerr << "move-only field body issue: " << issue.detail << '\n';
    }
    for (const lang::CppMirBodyEmissionIssue &issue : programAnalysis.issues) {
      std::cerr << "move-only field program issue: " << issue.detail << '\n';
    }
    std::cerr << "move-only field plain=" << emitter.supportsBodyText(address)
              << " failure=" << failureBody << '\n';
  }
  expect(failureBody && bodyAnalysis.ready() && programAnalysis.ready(),
         "the verified move-only class store should admit the transformed "
         "body");
  if (!failureBody) {
    return;
  }

  const std::string text =
      emitter
          .emitFailureBodyText(address,
                               "move-only-result-receiver-field-transfer-v0", 1)
          .text;
  const std::string destinationMove =
      "__gti_mir_p_" + std::to_string(*store->destination) +
      " = std::move(__gti_mir_p_" + std::to_string(resultShell->id) + ".get())";
  const std::string shellDestroy =
      "__gti_mir_p_" + std::to_string(resultShell->id) + ".destroy();";
  expect(text.find(destinationMove) != std::string::npos &&
             text.find(shellDestroy) != std::string::npos,
         "the field store should move from the placement result and destroy "
         "only its moved-from shell");

  const lang::BackendArtifact artifact = gti_test::emitCpp(frontend);
  expect(!artifact.contents.empty() &&
             artifact.contents.find(destinationMove) != std::string::npos &&
             artifact.contents.find(shellDestroy) != std::string::npos,
         "production emission should accept the same field publication");

  lang::MirProgram malformedStore = frontend.mir;
  auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
      malformedStore.functionInstances());
  auto changed = std::find_if(functions.begin(), functions.end(),
                              [&](const lang::MirFunctionInstance &candidate) {
                                return candidate.id == push->id;
                              });
  bool forgedPublication = false;
  if (changed != functions.end()) {
    for (lang::MirBlock &block : changed->body.blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (instruction.id == store->id && instruction.lifecycle.size() == 1) {
          instruction.lifecycle.front().conditional = true;
          forgedPublication = true;
        }
      }
    }
  }
  expect(forgedPublication,
         "the near-miss fixture should expose the field publication event");
  if (forgedPublication) {
    const lang::CppMirBodyEmitter malformedEmitter(malformedStore, map);
    expect(!malformedEmitter.supportsFailureBodyText(address),
           "a conditional move-only field publication should fail closed");
  }

  if (nodeConstructor != nullptr && constructorBindings &&
      !constructorBindings->empty()) {
    lang::MirProgram malformedConstructor = frontend.mir;
    auto &constructors =
        const_cast<std::vector<lang::MirConstructorInstance> &>(
            malformedConstructor.constructorInstances());
    auto changedConstructor =
        std::find_if(constructors.begin(), constructors.end(),
                     [&](const lang::MirConstructorInstance &candidate) {
                       return candidate.id == nodeConstructor->id;
                     });
    bool forgedTransfer = false;
    if (changedConstructor != constructors.end()) {
      for (lang::MirBlock &block : changedConstructor->body.blocks) {
        for (lang::MirInstruction &instruction : block.instructions) {
          if (instruction.id ==
                  constructorBindings->back().fieldTransferInstruction &&
              instruction.lifecycle.size() == 1) {
            instruction.lifecycle.front().conditional = true;
            forgedTransfer = true;
          }
        }
      }
    }
    const lang::MirConstructorInstance *malformedNode =
        malformedConstructor.findConstructorInstance(nodeConstructor->id);
    expect(forgedTransfer && malformedNode != nullptr &&
               !lang::cppMirOwnedParameterFieldBindings(malformedConstructor,
                                                        *malformedNode),
           "a conditional rollback transfer should invalidate the complete "
           "mixed-field constructor proof");
  }
}

void testMoveOnlyReceiverFieldLifecycleSchedules() {
  const lang::FrontendResult frontend = analyzeWithStandardLibrary(
      "cpp-mir-forward-list-field-lifecycles.gti", R"(
#include <std/forward_list>
#include <std/memory>

class ResetOwner {
  mut std::unique_ptr<int32_t> value = std::unique_ptr<int32_t>();
public:
  void reset() mut { this.value = std::unique_ptr<int32_t>(); }
};

int main() {
  mut ResetOwner owner = ResetOwner();
  owner.reset();
  mut std::forward_list<int32_t> values = std::forward_list<int32_t>();
  values.push_front(1);
  values.pop_front();
  values.clear();
  return values.empty() ? 0 : 1;
}
)");
  expect(frontend.canGenerateCode(),
         "the forward-list lifecycle fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  struct FieldTransfer {
    const lang::MirFunctionInstance *function = nullptr;
    const lang::MirInstruction *producer = nullptr;
    const lang::MirInstruction *consumer = nullptr;
    const lang::MirInstruction *destroy = nullptr;
    const lang::MirPlace *slot = nullptr;
    const lang::MirPlace *source = nullptr;
    lang::MirInstructionId successEndBorrow = 0;
  };
  FieldTransfer movedField;
  FieldTransfer defaultField;

  const auto definitionFor = [](const lang::MirBody &body,
                                lang::MirInstructionId id) {
    const lang::MirInstruction *found = nullptr;
    for (const lang::MirBlock &block : body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.id == id) {
          if (found != nullptr) {
            return static_cast<const lang::MirInstruction *>(nullptr);
          }
          found = &instruction;
        }
      }
    }
    return found;
  };

  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    for (const lang::MirBlock &block : function.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Assign ||
            !instruction.destination || instruction.operands.size() != 1 ||
            instruction.operands.front().kind != lang::MirOperandKind::Value ||
            instruction.operands.front().type.kind !=
                lang::SemanticType::Class) {
          continue;
        }
        const lang::MirPlace *destination =
            function.body.findPlace(*instruction.destination);
        const lang::MirValue *assigned =
            function.body.findValue(instruction.operands.front().value);
        const lang::MirInstruction *producer =
            assigned == nullptr
                ? nullptr
                : definitionFor(function.body, assigned->definition);
        if (destination == nullptr ||
            destination->root != lang::MirPlaceRootKind::This ||
            destination->projections.size() != 1 || producer == nullptr) {
          continue;
        }
        const lang::MirPlace *slot = nullptr;
        for (const lang::MirPlace &place : function.body.places) {
          if (place.root == lang::MirPlaceRootKind::Value &&
              place.value == assigned->id && place.projections.empty()) {
            if (slot != nullptr) {
              slot = nullptr;
              break;
            }
            slot = &place;
          }
        }
        const lang::MirInstruction *destroy = nullptr;
        if (slot != nullptr) {
          for (const lang::MirBlock &candidateBlock : function.body.blocks) {
            for (const lang::MirInstruction &candidate :
                 candidateBlock.instructions) {
              if (candidate.kind == lang::MirInstructionKind::Drop &&
                  candidate.destination == slot->id) {
                if (destroy != nullptr) {
                  destroy = nullptr;
                  break;
                }
                destroy = &candidate;
              }
            }
          }
        }
        if (slot == nullptr || destroy == nullptr) {
          continue;
        }
        if (producer->kind == lang::MirInstructionKind::Call &&
            !producer->functionTarget && !producer->constructorTarget &&
            !producer->receiver && producer->operands.empty() &&
            producer->intrinsic == lang::IntrinsicKind::None) {
          defaultField = {.function = &function,
                          .producer = producer,
                          .consumer = &instruction,
                          .destroy = destroy,
                          .slot = slot};
          continue;
        }
        const lang::MirPlace *source =
            producer->kind == lang::MirInstructionKind::Move &&
                    producer->operands.size() == 1
                ? function.body.findPlace(producer->operands.front().place)
                : nullptr;
        if (source == nullptr || source->root != lang::MirPlaceRootKind::Loan ||
            source->projections.size() != 1) {
          continue;
        }
        lang::MirInstructionId successEndBorrow = 0;
        for (const lang::MirBlock &candidateBlock : function.body.blocks) {
          const auto producerPosition =
              std::find_if(candidateBlock.instructions.begin(),
                           candidateBlock.instructions.end(),
                           [&](const lang::MirInstruction &candidate) {
                             return candidate.id == producer->id;
                           });
          if (producerPosition == candidateBlock.instructions.end()) {
            continue;
          }
          const auto consumerPosition = std::next(producerPosition);
          if (consumerPosition == candidateBlock.instructions.end() ||
              consumerPosition->id != instruction.id) {
            continue;
          }
          const auto endBorrow = std::find_if(
              std::next(consumerPosition), candidateBlock.instructions.end(),
              [&](const lang::MirInstruction &candidate) {
                return candidate.kind == lang::MirInstructionKind::EndBorrow &&
                       candidate.loan && *candidate.loan == source->loan;
              });
          const auto drop =
              std::find_if(endBorrow, candidateBlock.instructions.end(),
                           [&](const lang::MirInstruction &candidate) {
                             return candidate.id == destroy->id;
                           });
          if (endBorrow != candidateBlock.instructions.end() &&
              drop != candidateBlock.instructions.end() && endBorrow < drop) {
            successEndBorrow = endBorrow->id;
          }
        }
        movedField = {.function = &function,
                      .producer = producer,
                      .consumer = &instruction,
                      .destroy = destroy,
                      .slot = slot,
                      .source = source,
                      .successEndBorrow = successEndBorrow};
      }
    }
  }

  expect(movedField.function != nullptr && movedField.producer != nullptr &&
             movedField.consumer != nullptr && movedField.destroy != nullptr &&
             movedField.slot != nullptr && movedField.source != nullptr &&
             movedField.successEndBorrow != 0,
         "pop_front should expose the exact borrowed-field move, replace, "
         "end-borrow, and shell-drop schedule");
  expect(defaultField.function != nullptr && defaultField.producer != nullptr &&
             defaultField.consumer != nullptr &&
             defaultField.destroy != nullptr && defaultField.slot != nullptr,
         "reset should expose the exact default-construction, receiver-field "
         "replace, and shell-drop schedule");
  if (movedField.function == nullptr || movedField.consumer == nullptr ||
      movedField.slot == nullptr || movedField.successEndBorrow == 0 ||
      defaultField.function == nullptr || defaultField.producer == nullptr ||
      defaultField.consumer == nullptr || defaultField.slot == nullptr) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress movedAddress{.kind = lang::MirBodyKind::Function,
                                          .owner = movedField.function->id};
  const lang::MirBodyAddress defaultAddress{.kind = lang::MirBodyKind::Function,
                                            .owner = defaultField.function->id};
  expect(emitter.supportsFailureBodyText(movedAddress) &&
             emitter.supportsFailureBodyText(defaultAddress) &&
             emitter.analyzeProgram().ready(),
         "the two exact receiver-field lifecycle schedules should be ready "
         "for transformed C++ emission");

  const std::string movedText =
      emitter
          .emitFailureBodyText(movedAddress,
                               "move-only-borrowed-field-transfer-v0", 1)
          .text;
  const std::string defaultText =
      emitter
          .emitFailureBodyText(defaultAddress,
                               "move-only-default-field-transfer-v0", 1)
          .text;
  const auto destinationMove = [](const FieldTransfer &transfer) {
    return "__gti_mir_p_" + std::to_string(*transfer.consumer->destination) +
           " = std::move(__gti_mir_p_" + std::to_string(transfer.slot->id) +
           ".get())";
  };
  const auto shellDestroy = [](const FieldTransfer &transfer) {
    return "__gti_mir_p_" + std::to_string(transfer.slot->id) + ".destroy();";
  };
  expect(movedText.find("__gti_mir_p_" + std::to_string(movedField.slot->id) +
                        ".construct(std::move(") != std::string::npos &&
             movedText.find(destinationMove(movedField)) != std::string::npos &&
             movedText.find(shellDestroy(movedField)) != std::string::npos,
         "pop_front should construct from the borrowed field, replace the "
         "receiver field, and destroy only the moved-from shell");
  expect(defaultText.find("__gti_mir_p_" +
                          std::to_string(defaultField.slot->id) +
                          ".construct();") != std::string::npos &&
             defaultText.find(destinationMove(defaultField)) !=
                 std::string::npos &&
             defaultText.find(shellDestroy(defaultField)) != std::string::npos,
         "reset should construct the empty owner in its MIR slot, replace "
         "the receiver field, and destroy only the moved-from shell");

  lang::MirProgram malformedMove = frontend.mir;
  auto &moveFunctions = const_cast<std::vector<lang::MirFunctionInstance> &>(
      malformedMove.functionInstances());
  auto changedMove =
      std::find_if(moveFunctions.begin(), moveFunctions.end(),
                   [&](const lang::MirFunctionInstance &candidate) {
                     return candidate.id == movedField.function->id;
                   });
  bool removedEndBorrow = false;
  if (changedMove != moveFunctions.end()) {
    for (lang::MirBlock &block : changedMove->body.blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (instruction.id == movedField.successEndBorrow) {
          instruction.loan.reset();
          removedEndBorrow = true;
        }
      }
    }
  }
  expect(removedEndBorrow,
         "the pop_front near miss should expose its success end-borrow");
  if (removedEndBorrow) {
    const lang::CppMirBodyEmitter malformedEmitter(malformedMove, map);
    expect(!malformedEmitter.supportsFailureBodyText(movedAddress),
           "a borrowed-field move without the sequenced success end-borrow "
           "should fail closed");
  }

  lang::MirProgram malformedDefault = frontend.mir;
  auto &defaultFunctions = const_cast<std::vector<lang::MirFunctionInstance> &>(
      malformedDefault.functionInstances());
  auto changedDefault =
      std::find_if(defaultFunctions.begin(), defaultFunctions.end(),
                   [&](const lang::MirFunctionInstance &candidate) {
                     return candidate.id == defaultField.function->id;
                   });
  bool forgedActivation = false;
  if (changedDefault != defaultFunctions.end()) {
    for (lang::MirBlock &block : changedDefault->body.blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (instruction.id == defaultField.producer->id &&
            instruction.lifecycle.size() == 1) {
          instruction.lifecycle.front().conditional = true;
          forgedActivation = true;
        }
      }
    }
  }
  expect(forgedActivation,
         "the clear near miss should expose its slot activation event");
  if (forgedActivation) {
    const lang::CppMirBodyEmitter malformedEmitter(malformedDefault, map);
    expect(!malformedEmitter.supportsFailureBodyText(defaultAddress),
           "a conditional default-value activation should fail closed");
  }
}

void testNestedReceiverFieldProjection() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-body-nested-receiver-field.gti", R"(
#include <std/string>

struct Component {
  mut std::size_t offset = 0;
  mut std::size_t length = 0;
};

class Path {
public:
  std::string filename() {
    mut std::string result = std::string("");
    auto offset = this.component.offset;
    return std::move(result);
  }

private:
  mut std::string text = std::string("");
  mut Component component{};
};

int main() {
  mut Path path = Path();
  std::string name = path.filename();
  return int32_t(name.size());
}
)");
  expect(frontend.canGenerateCode(),
         "the nested receiver-field fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *filename = nullptr;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    const lang::HirFunctionInstance *hir =
        frontend.hir.findFunctionInstance(function.id);
    if (hir != nullptr && hir->source != nullptr && function.owner &&
        hir->source->name().lexeme == "filename") {
      filename = &function;
      break;
    }
  }
  expect(filename != nullptr && filename->mayRaiseDefinedFailure,
         "filename should retain its owning local's exact failure effect");
  if (filename == nullptr) {
    return;
  }

  const lang::MirPlace *nested = nullptr;
  for (const lang::MirPlace &place : filename->body.places) {
    if (place.root == lang::MirPlaceRootKind::This &&
        place.projections.size() == 2 &&
        std::all_of(place.projections.begin(), place.projections.end(),
                    [](const lang::MirPlaceProjection &projection) {
                      return projection.kind == lang::MirProjectionKind::Field;
                    })) {
      nested = &place;
      break;
    }
  }
  expect(nested != nullptr,
         "filename MIR should retain the component.offset projection chain");
  if (nested == nullptr) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = filename->id};
  expect(emitter.analyze(address).ready() &&
             emitter.supportsFailureBodyText(address),
         "a failure-form method should admit an exact nested receiver-field "
         "projection");
  if (!emitter.supportsFailureBodyText(address)) {
    return;
  }

  const lang::CppMirBodyEmissionText text =
      emitter.emitFailureBodyText(address, "nested-receiver-field-test-v0", 1);
  expect(text.emitted() && text.text.find(" = (*this).") != std::string::npos,
         "the emitted method should bind the nested receiver field from the "
         "live object");

  const lang::BackendArtifact artifact = gti_test::emitCpp(frontend);
  expect(artifact.contents.find(" = (*this).") != std::string::npos,
         "production C++ emission should retain the verified nested field "
         "chain without a vocabulary error");
}

void testContainedAbstractBaseConstructor() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-body-contained-abstract-base.gti", R"(
#include <std/memory>
#include <std/vector>

class Item {
public:
  Item() {}
};

class Base {
  mut std::unique_ptr<Item> item;

public:
  Base() : item(std::make_unique<Item>()) {}
  virtual void run() mut = 0;
};

class Derived : public Base {
  mut bool ready = false;

public:
  Derived() : Base() {}

  void run() mut override {
    this.ready = true;
  }
};

void instantiate() {
  Derived value{};
}
)");
  expect(frontend.canGenerateCode(),
         "the contained abstract-base fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::HirClassInstance *baseHir = nullptr;
  const lang::HirClassInstance *derivedHir = nullptr;
  for (const lang::HirClassInstance &instance : frontend.hir.classInstances()) {
    if (instance.source == nullptr) {
      continue;
    }
    if (instance.source->name().lexeme == "Base") {
      baseHir = &instance;
    } else if (instance.source->name().lexeme == "Derived") {
      derivedHir = &instance;
    }
  }
  expect(baseHir != nullptr && derivedHir != nullptr,
         "the fixture should retain both concrete HIR class instances");
  if (baseHir == nullptr || derivedHir == nullptr) {
    return;
  }

  const lang::MirClassInstance *base =
      frontend.mir.findClassInstance(baseHir->id);
  const lang::MirClassInstance *derived =
      frontend.mir.findClassInstance(derivedHir->id);
  expect(base != nullptr && derived != nullptr,
         "the fixture should retain both MIR class instances");
  if (base == nullptr || derived == nullptr) {
    return;
  }
  expect(base->abstract && base->polymorphic && base->requiresActiveCleanup,
         "the base should retain its abstract polymorphic cleanup metadata");
  expect(derived->bases.size() == 1 &&
             derived->bases.front().instance == base->id,
         "the derived class should retain its exact concrete base instance");

  const lang::MirConstructorInstance *baseConstructor = nullptr;
  const lang::MirConstructorInstance *derivedConstructor = nullptr;
  for (const lang::MirConstructorInstance &constructor :
       frontend.mir.constructorInstances()) {
    if (constructor.owner == base->id) {
      baseConstructor = &constructor;
    } else if (constructor.owner == derived->id) {
      derivedConstructor = &constructor;
    }
  }
  expect(baseConstructor != nullptr && derivedConstructor != nullptr,
         "the fixture should retain both source constructor instances");
  if (baseConstructor == nullptr || derivedConstructor == nullptr) {
    return;
  }
  expect(baseConstructor->mayRaiseDefinedFailure &&
             derivedConstructor->mayRaiseDefinedFailure,
         "the owning base initializer failure effect should reach both "
         "constructors");

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  expect(
      emitter.supportsNativeContainedBaseConstruction(derivedConstructor->id),
      "the derived constructor should prove its exact native base "
      "construction from MIR");

  const lang::CppMirBodyEmissionText baseText = emitter.emitBodyText(
      {.kind = lang::MirBodyKind::Constructor, .owner = baseConstructor->id},
      "contained-abstract-base-v0", 1);
  const lang::CppMirBodyEmissionText derivedText = emitter.emitBodyText(
      {.kind = lang::MirBodyKind::Constructor, .owner = derivedConstructor->id},
      "contained-derived-v0", 1);
  expect(baseText.emitted(),
         "an abstract base's source constructor must remain directly "
         "spellable from MIR");
  expect(derivedText.emitted(),
         "the derived source constructor must remain directly spellable "
         "from MIR");
  expect(baseText.text.find("__gti_mir_boundary_failure_") != std::string::npos,
         "the base field call should contain its transformed failure at the "
         "ordinary constructor boundary");
}

// Sweeps the shipped example corpus through the generic body-emission gate.
//
// This exists because the readiness figure is otherwise measured only by
// ad-hoc tooling, and a change that breaks compilation silently "improves" it:
// a rejected source contributes no bodies at all. Asserting that every example
// still reaches MIR keeps that failure mode visible, and asserting that no
// frontend-produced body is Incoherent keeps the gate honest about the
// difference between "needs more MIR authority" and "this MIR is wrong".

void testCleanupFixtureFunctionBodiesAreReady() {
  const std::filesystem::path fixture =
      std::filesystem::path(__FILE__).parent_path() / "fixtures" /
      "mir_backend_class_default_cleanup.gti";
  std::ifstream input(fixture);
  std::stringstream buffer;
  buffer << input.rdbuf();
  const lang::FrontendResult frontend =
      lang::Frontend().analyze(fixture.string(), buffer.str());
  expect(frontend.canGenerateCode(),
         "the class-default-cleanup fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  bool functionBodiesReady = true;
  std::size_t scalarFunctionBodies = 0;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (function.entryKind != lang::ProgramEntryKind::None ||
        function.mayRaiseDefinedFailure) {
      continue;
    }
    ++scalarFunctionBodies;
    const lang::CppMirBodyEmissionAnalysis analysis = emitter.analyze(
        {.kind = lang::MirBodyKind::Function, .owner = function.id});
    if (!analysis.ready()) {
      functionBodiesReady = false;
      std::cerr << "cleanup function-instance " << function.id
                << " is not ready: "
                << (analysis.issues.empty() ? std::string("<no issue>")
                                            : analysis.issues.front().detail)
                << '\n';
    }
  }
  expect(scalarFunctionBodies >= 3 && functionBodiesReady,
         "every non-entry failure-free cleanup-fixture function body should "
         "be analysis-Ready under production rows");

  // The destructor-definition route dissolved into the same per-body
  // admission: every failure-free declared destructor body is Ready and
  // inside the text vocabulary, and the checked-arithmetic near miss
  // (CheckedCleanup) stays outside admission.
  std::size_t admittedDestructorBodies = 0;
  std::size_t declinedDestructorBodies = 0;
  for (const lang::MirDestructorInstance &destructor :
       frontend.mir.destructorInstances()) {
    if (destructor.definitionKind != lang::MirDefinitionKind::Source) {
      continue;
    }
    const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Destructor,
                                       .owner = destructor.id};
    if (emitter.analyze(address).ready() && emitter.supportsBodyText(address)) {
      ++admittedDestructorBodies;
    } else {
      ++declinedDestructorBodies;
    }
  }
  expect(admittedDestructorBodies == 4 && declinedDestructorBodies == 1,
         "exactly the four ordinary declared destructor bodies should be "
         "admitted and the checked-arithmetic near miss declined");
}

// Milestone gate for the rollback campaign: every constructor and
// field-initializer body of the owned-lifecycle fixture is analysis-Ready
// under production rows, and the scalar user destructor summarizes
// provably failure-free since the effect-proof widening. Native-closing
// prelude destructors and the prelude function bodies legitimately keep
// their containment flags and withheld capability rows.

void testOwnedLifecycleConstructionBodiesReady() {
  const std::filesystem::path repository =
      std::filesystem::path(__FILE__).parent_path().parent_path();
  const std::filesystem::path fixture =
      repository / "tests" / "fixtures" / "mir_backend_owned_lifecycle.gti";
  const std::filesystem::path standardLibrary = repository / "stdlib";
  std::ifstream input(fixture);
  std::stringstream buffer;
  buffer << input.rdbuf();
  const lang::FrontendResult frontend = lang::Frontend().analyze(
      fixture.string(), buffer.str(), {standardLibrary / "prelude.gti"}, {},
      {standardLibrary});
  expect(frontend.canGenerateCode(),
         "the owned-lifecycle fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::CppMirProgramEmissionAnalysis program = emitter.analyzeProgram();
  bool constructionReady = program.issues.empty();
  std::size_t constructionBodies = 0;
  std::size_t readyDestructors = 0;
  for (const lang::CppMirBodyEmissionAnalysis &body : program.bodies) {
    if (body.body.kind == lang::MirBodyKind::Destructor) {
      readyDestructors += body.ready() ? 1 : 0;
      continue;
    }
    if (body.body.kind != lang::MirBodyKind::Constructor &&
        body.body.kind != lang::MirBodyKind::FieldInitializers) {
      continue;
    }
    ++constructionBodies;
    if (!body.ready()) {
      constructionReady = false;
      std::cerr << "owned-lifecycle construction body kind="
                << static_cast<int>(body.body.kind)
                << " owner=" << body.body.owner << " not ready: "
                << (body.issues.empty() ? std::string("<no issue>")
                                        : body.issues.front().detail)
                << '\n';
    }
  }
  expect(constructionBodies >= 3 && constructionReady,
         "every owned-lifecycle constructor and field-initializer body must "
         "stay analysis-Ready under production rows");
  expect(readyDestructors >= 1,
         "the scalar user destructor should summarize provably failure-free "
         "and reach analysis-Ready; native-closing prelude destructors "
         "legitimately keep their containment flags");
}

// A safety discharge proves every local failure outcome impossible even when
// the optimized CFG still carries its structural Invoke. The terminator emits
// only the success edge, so the read must use the unchecked helper and omit a
// dead status carrier. Near misses have no discharge and remain checked.

void testDefaultConstructionPreparedParameterStaging() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-default-prepared-parameter.gti", R"(
struct Scalar {
  int value = 0;
};

T transfer_value<T>(T value) {
  return std::move(value);
}

int main() {
  Scalar scalar = transfer_value(Scalar());
  expected<Scalar, int> result = Scalar();
  return scalar.value;
}
)");
  expect(frontend.canGenerateCode(),
         "the default prepared-parameter fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const auto entry =
      std::find_if(frontend.mir.functionInstances().begin(),
                   frontend.mir.functionInstances().end(),
                   [](const lang::MirFunctionInstance &function) {
                     return function.entryKind != lang::ProgramEntryKind::None;
                   });
  expect(entry != frontend.mir.functionInstances().end(),
         "the default prepared-parameter fixture should retain its entry");
  if (entry == frontend.mir.functionInstances().end()) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = entry->id};
  expect(emitter.analyze(address).ready() &&
             emitter.supportsFailureBodyText(address),
         "a targetless default construction should publish through its exact "
         "prepared-parameter lifecycle");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(artifact.contents.find("function-instance " +
                                std::to_string(entry->id) + "\n") !=
                 std::string::npos &&
             artifact.contents.find(".construct();") != std::string::npos &&
             artifact.contents.find("stages a placement class result") !=
                 std::string::npos &&
             artifact.contents.find("publishes at its Expected initializer") !=
                 std::string::npos &&
             artifact.contents.find(".construct(::__gti_program::Scalar{});") !=
                 std::string::npos,
         "production emission should publish targetless default values "
         "directly through both verified destination schedules");
}

// A generic member on a non-generic class cannot use the generic-owner
// explicit-specialization spelling. Its transformed ABI is one concrete
// overload declared on the class and defined without a template<> prefix.

void testReferenceReturnFailureAbi() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-reference-return.gti", R"(
class tally {
public:
  mut int total = 3;

  mut int& bump(int amount) mut {
    this.total += amount;
    return this.total;
  }
};

int main() {
  mut tally counter = tally();
  mut int& first = counter.bump(4);
  first = 17;
  return counter.bump(0) - 17;
}
)");
  expect(frontend.canGenerateCode(),
         "the reference-return fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::MirFunctionInstance *bump = nullptr;
  const lang::MirFunctionInstance *entry = nullptr;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (function.returnType.kind == lang::SemanticType::Reference &&
        function.mayRaiseDefinedFailure) {
      bump = &function;
    }
    if (function.entryKind != lang::ProgramEntryKind::None) {
      entry = &function;
    }
  }
  expect(bump != nullptr && entry != nullptr,
         "the fixture should lower the loan-returning member and its caller");
  if (bump == nullptr || entry == nullptr) {
    return;
  }
  const lang::MirPlace *referenceCarrier = nullptr;
  for (const lang::MirPlace &place : entry->body.places) {
    const bool parameter =
        std::find(entry->parameterBindings.begin(),
                  entry->parameterBindings.end(),
                  place.binding) != entry->parameterBindings.end();
    if (!parameter && place.root == lang::MirPlaceRootKind::Binding &&
        place.projections.empty() &&
        place.type.kind == lang::SemanticType::Reference) {
      referenceCarrier = &place;
    }
  }
  const lang::MirLoan *retained = nullptr;
  std::size_t carrierLoans = 0;
  if (referenceCarrier != nullptr) {
    for (const lang::MirLoan &loan : entry->body.loans) {
      if (std::find(loan.carriers.begin(), loan.carriers.end(),
                    referenceCarrier->binding) != loan.carriers.end()) {
        ++carrierLoans;
        retained = &loan;
      }
    }
  }
  bool initializesFromRetainedLoan = false;
  if (referenceCarrier != nullptr && retained != nullptr) {
    for (const lang::MirBlock &block : entry->body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        initializesFromRetainedLoan |=
            instruction.kind == lang::MirInstructionKind::Initialize &&
            instruction.destination == referenceCarrier->id &&
            instruction.operands.size() == 1 &&
            instruction.operands.front().kind == lang::MirOperandKind::Loan &&
            instruction.operands.front().loan == retained->id;
      }
    }
  }
  expect(referenceCarrier != nullptr && carrierLoans == 1 &&
             retained != nullptr &&
             retained->kind == lang::MirLoanKind::CallResult &&
             retained->parent == 0 && retained->semanticLoan != 0 &&
             initializesFromRetainedLoan,
         "a retained root call result should preserve its exact loan pointer "
         "as the local reference carrier");
  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = bump->id};
  expect(emitter.supportsFailureBodyText(address),
         "the loan-returning body should prove its transformed text");
  const lang::MirBodyAddress entryAddress{.kind = lang::MirBodyKind::Function,
                                          .owner = entry->id};
  expect(emitter.supportsFailureBodyText(entryAddress),
         "the retained-reference caller should prove its transformed text");
  if (!emitter.supportsFailureBodyText(address) ||
      !emitter.supportsFailureBodyText(entryAddress)) {
    return;
  }
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const std::string &text = artifact.contents;
  const auto contains = [&](std::string_view needle) {
    return text.find(needle) != std::string::npos;
  };
  expect(contains("std::int32_t **__gti_mir_out_result"),
         "the transformed signature should carry the pointer out-parameter");
  expect(contains("*__gti_mir_out_result = __gti_mir_loan_"),
         "the Return-with-loan should publish through the out-parameter");
  expect(contains("std::int32_t *__gti_mir_p_") &&
             contains(" = __gti_mir_loan_") && contains("&__gti_mir_loan_"),
         "the caller should retain the published pointer in its reference "
         "carrier");
  expect(!contains("__gti_mir_boundary_result"),
         "the direct transformed call must not retain a terminal boundary "
         "wrapper");
}

void testGenericOwnerReferenceReturnFailureAbi() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-generic-reference-return.gti", R"(
class box<T> {
  T value;

public:
  box(T initial) : value(initial) {}

  T& get() {
    return this.value;
  }
};

int main() {
  box<int> value{7};
  return value.get() - 7;
}
)");
  expect(frontend.canGenerateCode(),
         "the generic reference-return fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::MirFunctionInstance *getter = nullptr;
  const lang::MirFunctionInstance *entry = nullptr;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (function.returnType.kind == lang::SemanticType::Reference) {
      getter = &function;
    }
    if (function.entryKind == lang::ProgramEntryKind::NoArguments) {
      entry = &function;
    }
  }
  expect(getter != nullptr && entry != nullptr,
         "the fixture should lower the generic member and hosted entry");
  if (getter == nullptr || entry == nullptr) {
    return;
  }

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const auto contains = [&](std::string_view needle) {
    return artifact.contents.find(needle) != std::string::npos;
  };
  expect(!contains("const T **__gti_mir_out_result") &&
             contains("const std::int32_t **__gti_mir_out_result") &&
             contains("__gti_type_") && contains("_box") &&
             contains("__gti_member_") && contains("_get__gti_mir_failure"),
         "the selected direct generic class member should publish its "
         "concrete reference through the pointer carrier without retaining a "
         "native dependent primary");
  expect(contains("scalar-cfg-failure-v1 function-instance " +
                  std::to_string(getter->id)) &&
             contains("scalar-cfg-failure-v1 function-instance " +
                      std::to_string(entry->id)),
         "the generic reference-return chain should remain closed through "
         "the hosted entry");
}

// The stores-reference constructor schedule (ADR 018): each reference
// field pairs bijectively with one Stored loan whose source is the
// dereference carrier of a reference parameter; the emitted C++ binds the
// field in the member initializer list, the Borrow spells as a comment,
// and no loan pointer materializes. A nonzero constructor borrow origin
// is that schedule's caller-side lifetime fact.

void testStoredReferenceConstructorSchedule() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-stored-reference.gti", R"(
class holder {
  int& target;

public:
  holder(int& value) : target(value) {}

  int read() { return this.target; }
};

int main() {
  mut int cell = 42;
  holder bound = holder(cell);
  return bound.read() - 42;
}
)");
  expect(frontend.canGenerateCode(),
         "the stored-reference fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::MirConstructorInstance *constructor = nullptr;
  for (const lang::MirConstructorInstance &candidate :
       frontend.mir.constructorInstances()) {
    if (!candidate.body.loans.empty()) {
      constructor = &candidate;
    }
  }
  expect(constructor != nullptr,
         "the fixture should lower the loan-carrying constructor");
  if (constructor == nullptr) {
    return;
  }
  const std::optional<std::vector<lang::CppMirStoredReferenceBinding>>
      bindings = lang::cppMirStoredReferenceBindings(*constructor);
  expect(bindings.has_value() && bindings->size() == 1 &&
             (*bindings)[0].parameter == 0,
         "the schedule should pair the reference field with the reference "
         "parameter");
  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Constructor,
                                     .owner = constructor->id};
  expect(emitter.analyze(address).ready() && emitter.supportsBodyText(address),
         "the stored-reference constructor should be ready and in the text "
         "vocabulary");
  if (!emitter.supportsBodyText(address)) {
    return;
  }
  const lang::CppMirBodyEmissionText text =
      emitter.emitBodyText(address, "stored-reference-test-v0", 1);
  expect(text.text.find("binds its reference field in the initializer "
                        "list") != std::string::npos &&
             text.text.find("__gti_mir_loan_") == std::string::npos,
         "the Borrow should spell as a comment with no loan pointer local");

  // The full backend chain binds the field in the emitted member
  // initializer list of the selected constructor definition.
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(artifact.contents.find(" : target(__gti_mir_arg_0) ") !=
             std::string::npos,
         "the emitted constructor should bind the reference field in its "
         "member initializer list");
  expect(artifact.contents.find("constructor-instance " +
                                std::to_string(constructor->id)) !=
             std::string::npos,
         "the selected constructor should carry its verified-MIR banner");
}

// A constructor of a generic class publishes inside its GTI-owned concrete
// class and carries the same verified schedule. The concrete declaration must
// still precede any later class initializer that constructs the application.

void testGenericOwnerConstructorSpecialization() {
  const lang::FrontendResult frontend = analyze("cpp-mir-generic-ctor.gti", R"(
class wrapper<T> {
  T& target;

public:
  wrapper(T& value) : target(value) {}

  T read() { return this.target; }
};

class reading<T> {
  T value;

public:
  reading(T initial) : value(initial) {}

  T read() { return this.value; }
};

class dashboard {
  reading<int> initial{7};

public:
  int read() { return this.initial.read(); }
};

int main() {
  mut int cell = 7;
  wrapper<int> bound = wrapper<int>(cell);
  dashboard display{};
  return bound.read() + display.read() - 14;
}
)");
  expect(frontend.canGenerateCode(),
         "the generic-owner fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const auto contains = [&](std::string_view needle) {
    return artifact.contents.find(needle) != std::string::npos;
  };
  expect(contains("_wrapper(const std::int32_t & __gti_mir_arg_0);") &&
             contains("class __gti_type_") &&
             !contains("template <> __gti_program::wrapper"),
         "the selected wrapper constructor should declare on its ordinary "
         "GTI-owned concrete class");
  const std::size_t readingDeclaration =
      artifact.contents.find("_reading(std::int32_t __gti_mir_arg_0);");
  const std::size_t dashboardDefinition =
      artifact.contents.find("class dashboard {");
  expect(readingDeclaration != std::string::npos &&
             dashboardDefinition != std::string::npos &&
             readingDeclaration < dashboardDefinition,
         "a direct generic constructor should be declared before a later "
         "class initializer uses its concrete application");
  expect(contains(" : target(__gti_mir_arg_0) "),
         "the concrete constructor should bind the reference field in its "
         "member initializer list");
}

// Generic source constructors and generic members use concrete private
// overloads for their transformed ABI. The source templates remain available
// for compatibility emission, while each admitted MIR instance owns one
// independently nameable definition.

void testConcreteGenericConstructorAndMemberPackPublication() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-concrete-generic-overloads.gti", R"(
#include <std/vector>

class extent {
  uint64_t count;

public:
  extent<uint64_t N>(int32_t values[N]) : count(N) {}

  uint64_t size() { return this.count; }
};

int32_t main() {
  extent fixed = extent({1, 2});
  mut std::vector<int32_t> values{};
  [[discard]] values.emplace_back(4);
  return int32_t(fixed.size()) + values[std::size_t(0)] - 6;
}
)");
  expect(frontend.canGenerateCode(),
         "the concrete generic overload fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirConstructorInstance *genericConstructor = nullptr;
  for (const lang::MirConstructorInstance &constructor :
       frontend.mir.constructorInstances()) {
    const lang::HirConstructorInstance *source =
        frontend.hir.findConstructorInstance(constructor.id);
    if (source != nullptr && source->source != nullptr &&
        !source->source->genericParameters().empty()) {
      genericConstructor = &constructor;
      break;
    }
  }
  const lang::MirFunctionInstance *genericMember = nullptr;
  const lang::MirFunctionInstance *entry = nullptr;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (function.entryKind != lang::ProgramEntryKind::None) {
      entry = &function;
    }
    const lang::HirFunctionInstance *source =
        frontend.hir.findFunctionInstance(function.id);
    if (source != nullptr && source->source != nullptr && function.owner &&
        std::any_of(source->source->genericParameters().begin(),
                    source->source->genericParameters().end(),
                    [](const lang::GenericParameter &parameter) {
                      return parameter.pack;
                    })) {
      genericMember = &function;
    }
  }
  expect(genericConstructor != nullptr && genericMember != nullptr &&
             entry != nullptr,
         "the fixture should retain concrete generic constructor, member-pack, "
         "and entry bodies");
  if (genericConstructor == nullptr || genericMember == nullptr ||
      entry == nullptr) {
    return;
  }

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const auto containsMarker = [&](std::string_view kind, std::size_t id) {
    return artifact.contents.find(std::string(kind) + " " + std::to_string(id) +
                                  "\n") != std::string::npos;
  };
  expect(containsMarker("constructor-instance", genericConstructor->id) &&
             containsMarker("function-instance", genericMember->id) &&
             containsMarker("function-instance", entry->id),
         "the concrete generic overloads and their entry caller should all "
         "publish verified-MIR bodies");
  expect(artifact.contents.find("mir_failure_constructor_tag_v1<" +
                                std::to_string(genericConstructor->id) + ">") !=
             std::string::npos,
         "the generic constructor should publish through its unique private "
         "tag overload");
}

void testPassiveConstructorFailureSummary() {
  const lang::FrontendResult frontend = analyze("cpp-mir-passive-ctor.gti", R"(
class counter {
  mut int value;

public:
  counter(int initial) : value(initial) {}

  int tick() mut {
    this.value += 1;
    return this.value;
  }
};

int main() {
  mut counter value{0};
  return value.tick() - 1;
}
)");
  expect(frontend.canGenerateCode(),
         "the passive-constructor fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::MirConstructorInstance *constructor =
      frontend.mir.constructorInstances().empty()
          ? nullptr
          : &frontend.mir.constructorInstances().front();
  const auto entry = std::find_if(
      frontend.mir.functionInstances().begin(),
      frontend.mir.functionInstances().end(), [](const auto &function) {
        return function.entryKind == lang::ProgramEntryKind::NoArguments;
      });
  const lang::MirDefinedFailureEffects effects =
      lang::deriveMirDefinedFailureEffects(frontend.mir);
  expect(constructor != nullptr && !constructor->mayRaiseDefinedFailure &&
             constructor->id <= effects.constructors.size() &&
             !effects.constructors[constructor->id - 1],
         "a constructor covering trivial scalar fields from matching scalar "
         "parameters should have an exact failure-free MIR summary");
  expect(entry != frontend.mir.functionInstances().end(),
         "the fixture should lower its no-argument entry");
  if (constructor == nullptr ||
      entry == frontend.mir.functionInstances().end()) {
    return;
  }
  bool exactConstruction = false;
  for (const lang::MirBlock &block : entry->body.blocks) {
    for (const lang::MirInstruction &instruction : block.instructions) {
      exactConstruction =
          exactConstruction ||
          (instruction.kind == lang::MirInstructionKind::Construct &&
           instruction.constructorTarget == constructor->id &&
           instruction.definedFailure.empty() &&
           instruction.localFailureSites.empty());
    }
  }
  expect(exactConstruction,
         "a call to the proved constructor should lower without a failure "
         "edge in its caller");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(artifact.contents.find("constructor-instance " +
                                std::to_string(constructor->id)) !=
                 std::string::npos &&
             artifact.contents.find("function-instance " +
                                    std::to_string(entry->id)) !=
                 std::string::npos,
         "the proved constructor and its failure-capable caller should both "
         "publish from verified MIR");
}

void testPassiveFixedArrayFailureClosure() {
  const lang::FrontendResult frontend = analyze("cpp-mir-fixed-array.gti", R"(
int32_t sum(int32_t values[4]) {
  mut int32_t total = 0;
  for (mut int32_t index = 0; index < 4; index++) {
    total += values[index];
  }
  return total;
}

class pair<T> {
  T values[2];

public:
  pair(T left, T right) : values({left, right}) {}

  T first() { return this.values[0]; }
};

int32_t main() {
  int32_t values[4] = {1, 2, 3, 4};
  int32_t grid[2][2] = {{1, 2}, {3, 4}};
  pair<int32_t> value = pair<int32_t>(7, 8);
  return sum(values) + grid[1][0] + value.first() - 20;
}
)");
  expect(frontend.canGenerateCode(),
         "the passive fixed-array fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  expect(frontend.mir.constructorInstances().size() == 1,
         "the fixed-array fixture should lower one constructor");
  if (frontend.mir.constructorInstances().size() != 1) {
    return;
  }
  const lang::MirConstructorInstance &constructor =
      frontend.mir.constructorInstances().front();
  const lang::MirDefinedFailureEffects effects =
      lang::deriveMirDefinedFailureEffects(frontend.mir);
  expect(!constructor.mayRaiseDefinedFailure &&
             constructor.id <= effects.constructors.size() &&
             !effects.constructors[constructor.id - 1],
         "a complete passive fixed-array initializer should be proved "
         "failure-free");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const auto contains = [&](std::string_view needle) {
    return artifact.contents.find(needle) != std::string::npos;
  };
  expect(contains("scalar-cfg-v1 constructor-instance " +
                  std::to_string(constructor.id)),
         "the passive fixed-array constructor should publish from verified "
         "MIR");
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    expect(contains("scalar-cfg-failure-v1 function-instance " +
                    std::to_string(function.id)),
           "every function in the fixed-array failure component should use "
           "the transformed ABI");
  }
  expect(contains("[static_cast<std::size_t>(1)], UINT64_C(0), "
                  "&__gti_mir_v_"),
         "a nested array read should check the terminal index over its "
         "proved-safe parent projection");
  expect(contains("mir_checked_array_read_v1((*this).values, UINT64_C(0), "
                  "&__gti_mir_v_"),
         "a receiver fixed-array field read should use the checked failure "
         "operation");
}

void testOwningFixedArrayAggregateAndCheckedMove() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-owning-fixed-array.gti", R"(
#include <std/memory>

struct record {
  int32_t value;
  record(int32_t initial) : value(initial) {}
};

int32_t partial_drop() {
  mut std::unique_ptr<record> owners[2] = {
      std::make_unique<record>(7), std::make_unique<record>(8)};
  auto first = std::move(owners[0]);
  return first->value;
}

int32_t main() {
  return partial_drop() == 7 ? 0 : 1;
}
)");
  expect(frontend.canGenerateCode(),
         "the owning fixed-array fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *selected = nullptr;
  const lang::MirInstruction *checkedMove = nullptr;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    bool owningAggregate = false;
    const lang::MirInstruction *candidateMove = nullptr;
    for (const lang::MirBlock &block : function.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        owningAggregate =
            owningAggregate ||
            (instruction.kind == lang::MirInstructionKind::Compute &&
             instruction.operation == lang::MirOperation::Aggregate &&
             instruction.info.type.kind == lang::SemanticType::Array &&
             instruction.info.type.arguments.size() == 1 &&
             instruction.info.type.arguments.front().kind ==
                 lang::SemanticType::Class);
        if (instruction.kind == lang::MirInstructionKind::Move &&
            instruction.localFailureSites.size() == 1) {
          candidateMove = &instruction;
        }
      }
    }
    if (owningAggregate && candidateMove != nullptr) {
      selected = &function;
      checkedMove = candidateMove;
      break;
    }
  }
  expect(selected != nullptr && checkedMove != nullptr,
         "the owning fixed-array fixture should retain its aggregate and "
         "checked element move in MIR");
  if (selected == nullptr || checkedMove == nullptr) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = selected->id};
  expect(emitter.analyze(address).ready() &&
             emitter.supportsFailureBodyText(address),
         "the owning aggregate and checked move should support transformed "
         "MIR emission");
  if (!emitter.supportsFailureBodyText(address)) {
    return;
  }

  const std::string text =
      emitter.emitFailureBodyText(address, "owning-fixed-array-test-v0", 1)
          .text;
  expect(text.find("mir_lifetime_slot<") != std::string::npos &&
             text.find(".construction_address()") != std::string::npos &&
             text.find("std::array<") != std::string::npos &&
             text.find("std::move(__gti_mir_v_") != std::string::npos &&
             text.find("::gti_internal::backend::array_at(") !=
                 std::string::npos &&
             text.find("failure cleanup drop-obligation") != std::string::npos,
         "the transformed body should stage both owners in exact lifetime "
         "slots, roll back a partial aggregate, and bounds-check the move");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(artifact.contents.find("scalar-cfg-failure-v1 function-instance " +
                                std::to_string(selected->id) + "\n") !=
             std::string::npos,
         "production emission should select the owning fixed-array body from "
         "verified MIR");
}

void testPolymorphicClassArrayFieldLifecycleSchedule() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-polymorphic-class-array-field-lifecycle.gti", R"(
mut int32_t constructed = 0;
mut int32_t destroyed = 0;

interface readable {
  int32_t read();
};

class passive_item : public readable {
public:
  passive_item() {}
  int32_t read() override { return 7; }
};

class counted_item : public readable {
public:
  counted_item() {
    if (constructed == 0) { constructed = 1; }
    else { constructed = 2; }
  }
  ~counted_item() {
    if (destroyed == 0) { destroyed = 1; }
    else { destroyed = 2; }
  }
  int32_t read() override { return 9; }
};

class owner {
public:
  owner() {}
  mut passive_item passive[2] = {};
  mut counted_item counted[2] = {};
};

int main() {
  { mut owner value = owner(); }
  return constructed == 2 && destroyed == 2 ? 0 : 1;
}
)");
  expect(frontend.canGenerateCode(),
         "the polymorphic class-array field fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirClassInstance *owner = nullptr;
  for (const lang::MirClassInstance &instance : frontend.mir.classInstances()) {
    if (instance.declaredFields.size() == 2 &&
        std::all_of(instance.declaredFields.begin(),
                    instance.declaredFields.end(),
                    [](const lang::MirClassFieldInfo &field) {
                      return field.type.kind == lang::SemanticType::Array &&
                             field.type.arguments.size() == 1 &&
                             field.type.arguments.front().kind ==
                                 lang::SemanticType::Class;
                    })) {
      owner = &instance;
      break;
    }
  }
  expect(owner != nullptr,
         "the fixture should retain one exact two-array field owner");
  if (owner == nullptr) {
    return;
  }

  const lang::MirBodyAddress address{
      .kind = lang::MirBodyKind::FieldInitializers, .owner = owner->id};
  const lang::MirBody &body = owner->fieldInitializers;
  std::size_t aggregates = 0;
  std::size_t fixedArrayDrops = 0;
  std::size_t containedDestructorDrops = 0;
  for (const lang::MirDropObligation &drop : body.dropObligations) {
    fixedArrayDrops += drop.dropType.fixedArray.has_value();
  }
  for (const lang::MirBlock &block : body.blocks) {
    for (const lang::MirInstruction &instruction : block.instructions) {
      aggregates += instruction.kind == lang::MirInstructionKind::Compute &&
                    instruction.operation == lang::MirOperation::Aggregate;
      containedDestructorDrops +=
          instruction.kind == lang::MirInstructionKind::Drop &&
          instruction.definedFailure.propagation ==
              lang::FailurePropagationKind::Destructor &&
          instruction.lifecycle.size() == 1 &&
          instruction.lifecycle.front().failureCleanup;
    }
  }
  expect(aggregates == 2 && fixedArrayDrops == 4 &&
             containedDestructorDrops == 1,
         "the field body should retain two array aggregates, their result and "
         "rollback obligations, and one status-proven destructor cleanup");

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirInitializerScheduleText schedule =
      lang::CppMirBodyEmitter(frontend.mir, map).initializerSchedule(address);
  expect(schedule.supported && schedule.fields.size() == 2,
         "interface-only passive and destructor-bearing class arrays should "
         "publish from one verified field-initializer schedule");

  lang::MirProgram malformed = frontend.mir;
  lang::MirBody *malformedBody = lang::findMirBody(malformed, address);
  lang::MirInstruction *malformedDrop = nullptr;
  if (malformedBody != nullptr) {
    for (lang::MirBlock &block : malformedBody->blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Drop &&
            instruction.definedFailure.propagation ==
                lang::FailurePropagationKind::Destructor) {
          malformedDrop = &instruction;
          break;
        }
      }
      if (malformedDrop != nullptr) {
        break;
      }
    }
  }
  if (malformedDrop != nullptr) {
    malformedDrop->definedFailure.propagation =
        lang::FailurePropagationKind::DirectCall;
  }
  expect(malformedDrop != nullptr && !lang::CppMirBodyEmitter(malformed, map)
                                          .initializerSchedule(address)
                                          .supported,
         "retargeting the contained destructor propagation must fail closed");
}

void testPassiveClassArrayMoveAssignmentSchedule() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-passive-class-array-move.gti", R"(
struct record {
  int32_t value;
  record(int32_t initial) : value(initial) {}
};

int32_t main() {
  mut record values[2] = {record(1), record(2)};
  record first = std::move(values[0]);
  record second = std::move(values[1]);
  values[0] = std::move(second);
  values[1] = std::move(first);
  if (values[0].value == 2 and values[1].value == 1) {
    return 0;
  }
  return 1;
}
)");
  expect(frontend.canGenerateCode(),
         "the passive class-array move fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *selected = nullptr;
  const lang::MirInstruction *aggregate = nullptr;
  std::vector<const lang::MirInstruction *> assignmentMoves;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    const lang::MirInstruction *candidateAggregate = nullptr;
    std::vector<const lang::MirInstruction *> candidateMoves;
    for (const lang::MirBlock &block : function.body.blocks) {
      for (std::size_t index = 0; index < block.instructions.size(); ++index) {
        const lang::MirInstruction &instruction = block.instructions[index];
        if (instruction.kind == lang::MirInstructionKind::Compute &&
            instruction.operation == lang::MirOperation::Aggregate &&
            instruction.info.type.kind == lang::SemanticType::Array &&
            instruction.info.type.arguments.size() == 1 &&
            instruction.info.type.arguments.front().kind ==
                lang::SemanticType::Class) {
          candidateAggregate = &instruction;
        }
        if (index + 1 >= block.instructions.size() ||
            instruction.kind != lang::MirInstructionKind::Move ||
            !instruction.result) {
          continue;
        }
        const lang::MirInstruction &consumer = block.instructions[index + 1];
        const lang::MirPlace *destination =
            consumer.destination
                ? function.body.findPlace(*consumer.destination)
                : nullptr;
        if (consumer.kind == lang::MirInstructionKind::Assign &&
            consumer.operands.size() == 1 &&
            consumer.operands.front().kind == lang::MirOperandKind::Value &&
            consumer.operands.front().value == *instruction.result &&
            destination != nullptr &&
            std::any_of(destination->projections.begin(),
                        destination->projections.end(),
                        [](const lang::MirPlaceProjection &projection) {
                          return projection.kind ==
                                 lang::MirProjectionKind::Index;
                        })) {
          candidateMoves.push_back(&instruction);
        }
      }
    }
    if (candidateAggregate != nullptr && candidateMoves.size() == 2) {
      selected = &function;
      aggregate = candidateAggregate;
      assignmentMoves = std::move(candidateMoves);
      break;
    }
  }
  expect(selected != nullptr && aggregate != nullptr &&
             assignmentMoves.size() == 2,
         "the fixture should retain its passive aggregate and two adjacent "
         "class Move/Assign pairs");
  if (selected == nullptr || aggregate == nullptr ||
      assignmentMoves.size() != 2) {
    return;
  }

  const lang::MirPlace *arraySlot = nullptr;
  for (const lang::MirBlock &block : selected->body.blocks) {
    for (const lang::MirInstruction &instruction : block.instructions) {
      if (instruction.kind == lang::MirInstructionKind::Initialize &&
          instruction.destination && instruction.operands.size() == 1 &&
          instruction.operands.front().kind == lang::MirOperandKind::Value &&
          instruction.operands.front().value == *aggregate->result) {
        arraySlot = selected->body.findPlace(*instruction.destination);
      }
    }
  }
  expect(arraySlot != nullptr,
         "the passive aggregate should initialize one exact array binding");
  if (arraySlot == nullptr) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = selected->id};
  expect(emitter.analyze(address).ready() &&
             emitter.supportsFailureBodyText(address),
         "the passive class-array move schedule should support transformed "
         "MIR emission");
  if (!emitter.supportsFailureBodyText(address)) {
    return;
  }

  const std::string text =
      emitter
          .emitFailureBodyText(address, "passive-class-array-move-test-v0", 1)
          .text;
  const std::string arrayStorage =
      "__gti_mir_p_" + std::to_string(arraySlot->id);
  bool erasedMoveLocals = true;
  for (const lang::MirInstruction *move : assignmentMoves) {
    erasedMoveLocals =
        erasedMoveLocals && move != nullptr && move->result &&
        text.find("__gti_mir_v_" + std::to_string(*move->result)) ==
            std::string::npos;
  }
  expect(text.find(arrayStorage + ".construction_address()") !=
                 std::string::npos &&
             text.find("stages into fixed-array assignment") !=
                 std::string::npos &&
             text.find("mir_checked_array_write_v1") != std::string::npos &&
             text.find(", std::move(__gti_mir_p_") != std::string::npos &&
             text.find(arrayStorage + ".destroy();") != std::string::npos &&
             text.find("].value") != std::string::npos && erasedMoveLocals,
         "the transformed body should construct the non-default array in its "
         "slot, move through checked assignments without SSA objects, read "
         "constant element fields, and destroy the slot on return");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(artifact.contents.find("scalar-cfg-failure-v1 function-instance " +
                                std::to_string(selected->id) + "\n") !=
             std::string::npos,
         "production emission should select the passive class-array body "
         "from verified MIR");
}

void testCheckedClassArrayElementAssignmentFamily() {
  const lang::FrontendResult frontend = analyzeWithStandardLibrary(
      "cpp-mir-checked-class-array-assignment.gti", R"(
class leaf {
public:
  leaf() {}
  mut int32_t v = 1;
};

int main() {
  mut leaf items[4] = {};
  mut leaf source = leaf();
  items[0].v = 2;
  int32_t observed = items[0].v;
  items[1] = source;
  items[2] = std::move(source);
  items[3] = leaf();
  return int(observed + items[1].v + items[2].v + items[3].v) - 5;
}
)");
  expect(frontend.canGenerateCode(),
         "the five-row class-array assignment fixture should pass the "
         "frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const auto entry = std::find_if(
      frontend.mir.functionInstances().begin(),
      frontend.mir.functionInstances().end(), [](const auto &function) {
        return function.entryKind == lang::ProgramEntryKind::NoArguments;
      });
  expect(entry != frontend.mir.functionInstances().end(),
         "the five-row fixture should lower its entry function");
  if (entry == frontend.mir.functionInstances().end()) {
    return;
  }

  const lang::MirInstruction *copyLoad = nullptr;
  const lang::MirInstruction *copyAssignment = nullptr;
  const lang::MirInstruction *move = nullptr;
  const lang::MirInstruction *moveAssignment = nullptr;
  const lang::MirInstruction *freshConstruct = nullptr;
  const lang::MirInstruction *freshAssignment = nullptr;
  const lang::MirPlace *copyDestination = nullptr;
  const lang::MirPlace *freshDestination = nullptr;
  bool fieldRead = false;
  bool fieldWrite = false;
  const auto wholeElement = [](const lang::MirPlace *place) {
    return place != nullptr && place->type.kind == lang::SemanticType::Class &&
           place->projections.size() == 1 &&
           place->projections.front().kind == lang::MirProjectionKind::Index;
  };
  const auto elementField = [](const lang::MirPlace *place) {
    return place != nullptr && place->projections.size() == 2 &&
           place->projections.front().kind == lang::MirProjectionKind::Index &&
           place->projections.back().kind == lang::MirProjectionKind::Field;
  };
  for (const lang::MirBlock &block : entry->body.blocks) {
    for (std::size_t index = 0; index < block.instructions.size(); ++index) {
      const lang::MirInstruction &instruction = block.instructions[index];
      if (instruction.kind == lang::MirInstructionKind::Load &&
          instruction.operands.size() == 1) {
        fieldRead = fieldRead || elementField(entry->body.findPlace(
                                     instruction.operands.front().place));
      }
      if (instruction.kind == lang::MirInstructionKind::Assign &&
          instruction.destination) {
        fieldWrite =
            fieldWrite ||
            elementField(entry->body.findPlace(*instruction.destination));
      }
      if (index + 1 >= block.instructions.size() || !instruction.result) {
        continue;
      }
      const lang::MirInstruction &consumer = block.instructions[index + 1];
      const lang::MirPlace *destination =
          consumer.destination ? entry->body.findPlace(*consumer.destination)
                               : nullptr;
      if (consumer.kind != lang::MirInstructionKind::Assign ||
          !consumer.result || consumer.operands.size() != 1 ||
          consumer.operands.front().kind != lang::MirOperandKind::Value ||
          consumer.operands.front().value != *instruction.result ||
          !wholeElement(destination)) {
        continue;
      }
      if (instruction.kind == lang::MirInstructionKind::Load &&
          instruction.operands.front().kind == lang::MirOperandKind::Copy) {
        copyLoad = &instruction;
        copyAssignment = &consumer;
        copyDestination = destination;
      } else if (instruction.kind == lang::MirInstructionKind::Move) {
        move = &instruction;
        moveAssignment = &consumer;
      } else if (instruction.kind == lang::MirInstructionKind::Construct &&
                 instruction.constructorKind ==
                     lang::ConstructorKind::Ordinary) {
        freshConstruct = &instruction;
        freshAssignment = &consumer;
        freshDestination = destination;
      }
    }
  }

  const auto exactBoundsFailure = [&](const lang::MirInstruction *assignment) {
    if (assignment == nullptr || assignment->localFailureSites.size() != 1 ||
        assignment->definedFailure.localOrigins.size() != 1) {
      return false;
    }
    const lang::DefinedFailureOrigin &origin =
        assignment->definedFailure.localOrigins.front();
    const std::vector<lang::DefinedFailureOutcome> expected = {
        {.code = lang::DefinedFailureCode::IndexOutOfBounds,
         .detail = lang::DefinedFailureDetail::FixedArray}};
    const std::optional<lang::FailureSiteId> site =
        frontend.mir.failureMetadata().siteFor(origin);
    const lang::FailureSiteDescriptor *descriptor =
        frontend.mir.failureMetadata().findSite(
            assignment->localFailureSites.front());
    return site && *site == assignment->localFailureSites.front() &&
           origin.outcomes == expected && descriptor != nullptr &&
           descriptor->outcomes == expected;
  };
  const auto mutableArrayBase = [&](const lang::MirPlace *destination) {
    return destination != nullptr &&
           std::any_of(entry->body.places.begin(), entry->body.places.end(),
                       [&](const lang::MirPlace &candidate) {
                         return candidate.root == destination->root &&
                                candidate.binding == destination->binding &&
                                candidate.projections.empty() &&
                                candidate.type.kind ==
                                    lang::SemanticType::Array &&
                                candidate.access == lang::AccessMode::Mutable;
                       });
  };
  expect(fieldRead && fieldWrite && copyLoad != nullptr &&
             copyAssignment != nullptr && move != nullptr &&
             moveAssignment != nullptr && freshConstruct != nullptr &&
             freshAssignment != nullptr && copyDestination != nullptr &&
             freshDestination != nullptr,
         "MIR should retain the field read/write and adjacent copy, move, and "
         "fresh-value whole-element assignment families");
  if (copyLoad == nullptr || copyAssignment == nullptr || move == nullptr ||
      moveAssignment == nullptr || freshConstruct == nullptr ||
      freshAssignment == nullptr || copyDestination == nullptr ||
      freshDestination == nullptr) {
    return;
  }
  expect(copyDestination->access == lang::AccessMode::ReadOnly &&
             freshDestination->access == lang::AccessMode::ReadOnly &&
             mutableArrayBase(copyDestination) &&
             mutableArrayBase(freshDestination) &&
             exactBoundsFailure(copyAssignment) &&
             exactBoundsFailure(moveAssignment) &&
             exactBoundsFailure(freshAssignment),
         "checked whole-element stores should use projected read-only places, "
         "mutable sibling array storage, and one paired fixed-array bounds "
         "site");

  const bool fusedValuesHaveCleanup =
      std::any_of(entry->body.places.begin(), entry->body.places.end(),
                  [&](const lang::MirPlace &place) {
                    return place.root == lang::MirPlaceRootKind::Value &&
                           (place.value == *copyLoad->result ||
                            place.value == *copyAssignment->result ||
                            place.value == *freshConstruct->result ||
                            place.value == *freshAssignment->result);
                  }) ||
      std::any_of(entry->body.dropObligations.begin(),
                  entry->body.dropObligations.end(),
                  [&](const lang::MirDropObligation &drop) {
                    return drop.generatedValue == *copyLoad->result ||
                           drop.generatedValue == *copyAssignment->result ||
                           drop.generatedValue == *freshConstruct->result ||
                           drop.generatedValue == *freshAssignment->result;
                  });
  expect(!fusedValuesHaveCleanup && copyLoad->lifecycle.empty() &&
             copyAssignment->lifecycle.empty() &&
             freshConstruct->lifecycle.empty() &&
             freshAssignment->lifecycle.empty(),
         "the copy and fresh-value fusions should own no value-rooted place, "
         "drop, or lifecycle cleanup");

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = entry->id};
  expect(emitter.analyze(address).ready() &&
             emitter.supportsFailureBodyText(address),
         "the five-row checked class-array body should support transformed "
         "MIR emission");
  if (!emitter.supportsFailureBodyText(address)) {
    return;
  }

  const std::string text =
      emitter.emitFailureBodyText(address, "class-array-assignment-test-v0", 1)
          .text;
  std::size_t checkedWrites = 0;
  for (std::size_t position = 0;
       (position =
            text.find(" = ::gti_internal::backend::mir_checked_array_write_v1(",
                      position)) != std::string::npos;
       ++checkedWrites, ++position) {
  }
  const std::string copyValue =
      "__gti_mir_v_" + std::to_string(*copyLoad->result);
  const std::string freshValue =
      "__gti_mir_v_" + std::to_string(*freshConstruct->result);
  expect(checkedWrites == 3 &&
             text.find("class copy load " + std::to_string(*copyLoad->result) +
                       " fuses into assignment " +
                       std::to_string(copyAssignment->id)) !=
                 std::string::npos &&
             text.find("construct " + std::to_string(*freshConstruct->result) +
                       " fuses into checked array assignment") !=
                 std::string::npos &&
             text.find("stages into fixed-array assignment") !=
                 std::string::npos &&
             text.find(copyValue) == std::string::npos &&
             text.find(freshValue) == std::string::npos,
         "the copy, move, and fresh rows should each spell one checked write "
         "without materializing copy/fresh SSA objects");

  const lang::FrontendResult invalid =
      lang::Frontend().analyze("cpp-mir-move-only-array-copy.gti", R"(
class move_only {
public:
  move_only() {}
  move_only(move_only& other) = delete;
  move_only(move_only&& other) = default;
};

int main() {
  mut move_only items[2] = {};
  move_only source = move_only();
  items[1] = source;
  return 0;
}
)");
  const bool copyDiagnostic = std::any_of(
      invalid.diagnostics.begin(), invalid.diagnostics.end(),
      [](const lang::Diagnostic &diagnostic) {
        return diagnostic.code == "GTI-S2003" &&
               diagnostic.message.find("array element") != std::string::npos;
      });
  expect(!invalid.canGenerateCode() && copyDiagnostic,
         "a named move-only class value should remain rejected as an array "
         "element copy");
}

void testGeneratedSpecialMemberConstructionSchedule() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-generated-special-members.gti", R"(
class value {
  int32_t payload;

public:
  value(int32_t initial) : payload(initial) {}
  value(value& other) = default;
  value(value&& other) = default;
};

int32_t main() {
  value original = value(7);
  value copied = value(original);
  mut value movable = value(9);
  value moved = value(std::move(movable));
  return 0;
}
)");
  expect(frontend.canGenerateCode(),
         "the generated special-member fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const auto entry = std::find_if(
      frontend.mir.functionInstances().begin(),
      frontend.mir.functionInstances().end(), [](const auto &function) {
        return function.entryKind == lang::ProgramEntryKind::NoArguments;
      });
  expect(entry != frontend.mir.functionInstances().end(),
         "the generated special-member fixture should lower its entry");
  if (entry == frontend.mir.functionInstances().end()) {
    return;
  }

  std::size_t copies = 0;
  std::size_t moves = 0;
  bool copyUsesSourcePlace = false;
  bool moveUsesExplicitMove = false;
  bool everyResultActivatesBinding = true;
  for (const lang::MirBlock &block : entry->body.blocks) {
    for (const lang::MirInstruction &instruction : block.instructions) {
      if (instruction.kind != lang::MirInstructionKind::Construct ||
          instruction.constructorKind == lang::ConstructorKind::Ordinary) {
        continue;
      }
      copies += instruction.constructorKind == lang::ConstructorKind::Copy;
      moves += instruction.constructorKind == lang::ConstructorKind::Move;
      if (instruction.operands.size() == 1) {
        const lang::MirOperand &operand = instruction.operands.front();
        copyUsesSourcePlace =
            copyUsesSourcePlace ||
            (instruction.constructorKind == lang::ConstructorKind::Copy &&
             operand.kind == lang::MirOperandKind::BorrowRead &&
             entry->body.findPlace(operand.place) != nullptr);
        if (instruction.constructorKind == lang::ConstructorKind::Move &&
            operand.kind == lang::MirOperandKind::Value) {
          const lang::MirValue *value = entry->body.findValue(operand.value);
          for (const lang::MirBlock &candidate : entry->body.blocks) {
            for (const lang::MirInstruction &definition :
                 candidate.instructions) {
              moveUsesExplicitMove =
                  moveUsesExplicitMove ||
                  (value != nullptr && definition.id == value->definition &&
                   definition.kind == lang::MirInstructionKind::Move);
            }
          }
        }
      }

      bool activated = false;
      if (instruction.result) {
        for (const lang::MirValueUse &use :
             entry->body.usesOf(*instruction.result)) {
          for (const lang::MirBlock &candidate : entry->body.blocks) {
            for (const lang::MirInstruction &consumer :
                 candidate.instructions) {
              activated =
                  activated ||
                  (consumer.id == use.instruction &&
                   consumer.kind == lang::MirInstructionKind::Initialize &&
                   consumer.destination && consumer.lifecycle.size() == 1 &&
                   consumer.lifecycle.front().kind ==
                       lang::MirLifecycleEventKind::Initialize &&
                   consumer.lifecycle.front().target != 0);
            }
          }
        }
      }
      everyResultActivatesBinding = everyResultActivatesBinding && activated;
    }
  }
  expect(copies == 1 && moves == 1 && copyUsesSourcePlace &&
             moveUsesExplicitMove && everyResultActivatesBinding,
         "generated copy/move MIR should retain exact source, ownership, and "
         "destination-lifetime authority");

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = entry->id};
  const bool supported = entry->mayRaiseDefinedFailure
                             ? emitter.supportsFailureBodyText(address)
                             : emitter.supportsBodyText(address);
  expect(emitter.analyze(address).ready() && supported,
         "the fully scheduled generated special members should enter the "
         "general MIR text path");
  if (!supported) {
    return;
  }
  const lang::CppMirBodyEmissionText text =
      entry->mayRaiseDefinedFailure
          ? emitter.emitFailureBodyText(address,
                                        "generated-special-member-test-v0", 1)
          : emitter.emitBodyText(address, "generated-special-member-test-v0",
                                 1);
  expect(text.text.find("generated special-member construct") !=
                 std::string::npos &&
             text.text.find(".construct(std::move(") != std::string::npos,
         "generated copy/move emission should publish through the proved slot "
         "and move only the explicit move source");
}

void testConditionalClassReturnAndPreparedResultStage() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-conditional-class-return.gti", R"(
#include <std/memory>

struct Reading {
  int32_t value;

  Reading(int32_t initial) : value(initial) {}
};

std::unique_ptr<Reading> choose_owner(
    bool use_left,
    std::unique_ptr<Reading> left,
    std::unique_ptr<Reading> right) {
  return use_left ? std::move(left) : std::move(right);
}

int32_t main() {
  auto reading = choose_owner(
      true,
      std::make_unique<Reading>(7),
      std::make_unique<Reading>(11));
  return reading->value - 7;
}
)");
  expect(frontend.canGenerateCode(),
         "the conditional class-return fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *conditional = nullptr;
  const lang::MirFunctionInstance *entry = nullptr;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (function.entryKind != lang::ProgramEntryKind::None) {
      entry = &function;
    }
    const bool hasBranch = std::any_of(
        function.body.blocks.begin(), function.body.blocks.end(),
        [](const lang::MirBlock &block) {
          return block.terminator.kind == lang::MirTerminatorKind::Branch;
        });
    if (function.parameterTypes.size() == 3 &&
        function.parameterTypes.front().kind == lang::SemanticType::Bool &&
        function.returnType.kind == lang::SemanticType::Class && hasBranch) {
      conditional = &function;
    }
  }
  expect(conditional != nullptr && entry != nullptr,
         "the fixture should lower its conditional owner return and entry");
  if (conditional == nullptr || entry == nullptr) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);

  const lang::MirBodyAddress conditionalAddress{
      .kind = lang::MirBodyKind::Function, .owner = conditional->id};
  expect(emitter.analyze(conditionalAddress).ready() &&
             emitter.supportsFailureBodyText(conditionalAddress),
         "the conditional class return should enter the transformed "
         "failure-body path");
  if (emitter.supportsFailureBodyText(conditionalAddress)) {
    const lang::CppMirBodyEmissionText text = emitter.emitFailureBodyText(
        conditionalAddress, "conditional-class-return-test-v0", 1);
    expect(text.emitted() &&
               text.text.find("mir_lifetime_slot<") != std::string::npos &&
               text.text.find("std::construct_at(") != std::string::npos &&
               text.text.find("conditional return transfer-out") !=
                   std::string::npos,
           "the conditional arms should converge through one owned slot "
           "whose final move explicitly transfers its drop");
  }

  const lang::MirBodyAddress entryAddress{.kind = lang::MirBodyKind::Function,
                                          .owner = entry->id};
  expect(emitter.analyze(entryAddress).ready() &&
             emitter.supportsFailureBodyText(entryAddress),
         "the caller should enter the transformed failure-body path");
  if (emitter.supportsFailureBodyText(entryAddress)) {
    const lang::CppMirBodyEmissionText text = emitter.emitFailureBodyText(
        entryAddress, "prepared-class-result-test-v0", 1);
    expect(text.emitted() &&
               text.text.find(".construction_address()") != std::string::npos &&
               text.text.find(".mark_constructed()") != std::string::npos &&
               text.text.find("stages a placement class result") !=
                   std::string::npos,
           "nested class results should construct in their prepared call "
           "slots and activate those slots only after success");
  }
}

void testConditionalFailureDestructorDrop() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-conditional-drop.gti", R"(
class Cleanup {
public:
  Cleanup() {}
  ~Cleanup() { std::println("cleanup"); }
  operator bool() { return true; }
};

int main() {
  bool skipped = false && Cleanup();
  return skipped ? 1 : 0;
}
)");
  expect(frontend.canGenerateCode(),
         "the conditional class-drop fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *entry = nullptr;
  const lang::MirInstruction *conditionalDrop = nullptr;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (function.entryKind == lang::ProgramEntryKind::None) {
      continue;
    }
    entry = &function;
    for (const lang::MirBlock &block : function.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Drop &&
            instruction.destination && instruction.lifecycle.size() == 1 &&
            instruction.lifecycle.front().kind ==
                lang::MirLifecycleEventKind::Drop &&
            instruction.lifecycle.front().conditional) {
          conditionalDrop = &instruction;
        }
      }
    }
  }
  expect(entry != nullptr && conditionalDrop != nullptr,
         "short-circuit cleanup should lower one conditional MIR Drop");
  if (entry == nullptr || conditionalDrop == nullptr) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = entry->id};
  expect(emitter.analyze(address).ready() &&
             emitter.supportsFailureBodyText(address),
         "the conditional class-drop fixture should enter transformed MIR "
         "emission");
  if (!emitter.supportsFailureBodyText(address)) {
    return;
  }

  const std::string text =
      emitter.emitFailureBodyText(address, "conditional-drop-test-v0", 1).text;
  const std::string conditionalDestroy =
      "__gti_mir_p_" + std::to_string(*conditionalDrop->destination) +
      ".destroy_with_failure_if_engaged(";
  expect(text.find(conditionalDestroy) != std::string::npos,
         "a conditional fallible Drop should succeed without touching an "
         "unengaged lifetime slot");
}

void testVirtualFailureInterfaceFamily() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-virtual-failure-family.gti", R"(
interface Iterator<T> {
  T& operator*();
  void operator++() mut;
  int32_t stable_value();
};

class Counter : public Iterator<int32_t> {
  mut int32_t value;

public:
  Counter(int32_t initial) : value(initial) {}
  int32_t& operator*() override { return this.value; }
  void operator++() mut override { this.value++; }
  int32_t stable_value() override { return 7; }
};

class Unrelated {
public:
  Unrelated() {}
  int32_t marker() { return 9; }
};

int32_t direct_stable() {
  mut Counter counter{1};
  return counter.stable_value();
}

int32_t main() {
  mut Counter counter{1};
  Unrelated unrelated{};
  mut Iterator<int32_t>& iterator = counter;
  int32_t first = *iterator;
  ++iterator;
  return *iterator - first - 1 + iterator.stable_value() - 7 +
         direct_stable() - 7 + unrelated.marker() - 9;
}
)");
  expect(frontend.canGenerateCode(),
         "the virtual failure-family fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *dereferenceRoot = nullptr;
  const lang::MirFunctionInstance *dereferenceOverride = nullptr;
  const lang::MirFunctionInstance *incrementRoot = nullptr;
  const lang::MirFunctionInstance *incrementOverride = nullptr;
  const lang::MirFunctionInstance *stableRoot = nullptr;
  const lang::MirFunctionInstance *stableOverride = nullptr;
  const lang::MirFunctionInstance *unrelatedMarker = nullptr;
  const lang::MirFunctionInstance *directCaller = nullptr;
  const lang::MirFunctionInstance *entry = nullptr;
  lang::HirFunctionInstanceId unrelatedMarkerId = 0;
  lang::HirFunctionInstanceId directCallerId = 0;
  for (const lang::HirFunctionInstance &function :
       frontend.hir.functionInstances()) {
    if (function.source == nullptr) {
      continue;
    }
    if (function.source->name().lexeme == "marker") {
      unrelatedMarkerId = function.id;
    } else if (function.source->name().lexeme == "direct_stable") {
      directCallerId = function.id;
    }
  }
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (function.entryKind != lang::ProgramEntryKind::None) {
      entry = &function;
    }
    if (function.id == directCallerId) {
      directCaller = &function;
    }
    if (!function.overloadedOperator) {
      if (function.id == unrelatedMarkerId) {
        unrelatedMarker = &function;
      } else if (function.pureVirtual) {
        stableRoot = &function;
      } else if (function.overrideMethod) {
        stableOverride = &function;
      }
      continue;
    }
    const bool dereference =
        *function.overloadedOperator == lang::OverloadedOperator::Dereference;
    if (function.pureVirtual) {
      (dereference ? dereferenceRoot : incrementRoot) = &function;
    } else if (function.overrideMethod) {
      (dereference ? dereferenceOverride : incrementOverride) = &function;
    }
  }
  expect(dereferenceRoot != nullptr && dereferenceOverride != nullptr &&
             incrementRoot != nullptr && incrementOverride != nullptr &&
             stableRoot != nullptr && stableOverride != nullptr &&
             unrelatedMarker != nullptr && directCaller != nullptr &&
             entry != nullptr,
         "the fixture should lower all pure contracts, all overrides, and "
         "its unrelated control and entry body");
  if (dereferenceRoot == nullptr || dereferenceOverride == nullptr ||
      incrementRoot == nullptr || incrementOverride == nullptr ||
      stableRoot == nullptr || stableOverride == nullptr ||
      unrelatedMarker == nullptr || directCaller == nullptr ||
      entry == nullptr) {
    return;
  }
  expect(
      lang::cppMirVirtualFailureContractRoot(frontend.mir, *dereferenceRoot) ==
              dereferenceRoot &&
          lang::cppMirVirtualFailureContractRoot(
              frontend.mir, *dereferenceOverride) == dereferenceRoot &&
          lang::cppMirVirtualFailureContractRoot(
              frontend.mir, *incrementRoot) == incrementRoot &&
          lang::cppMirVirtualFailureContractRoot(
              frontend.mir, *incrementOverride) == incrementRoot &&
          lang::cppMirVirtualFailureContractRoot(frontend.mir, *stableRoot) ==
              stableRoot &&
          lang::cppMirVirtualFailureContractRoot(frontend.mir,
                                                 *stableOverride) == stableRoot,
      "each override should resolve to its exact pure interface contract");
  expect(!stableOverride->mayRaiseDefinedFailure,
         "the stable override should exercise a failure-free implementation "
         "of a conservatively failure-capable virtual contract");

  const lang::MirInstruction *directStableCall = nullptr;
  for (const lang::MirBlock &block : directCaller->body.blocks) {
    const auto found = std::find_if(
        block.instructions.begin(), block.instructions.end(),
        [&](const lang::MirInstruction &instruction) {
          return instruction.kind == lang::MirInstructionKind::Call &&
                 instruction.functionTarget == stableOverride->id;
        });
    if (found != block.instructions.end()) {
      directStableCall = &*found;
      break;
    }
  }
  const lang::MirClassInstance *stableOwner =
      stableOverride->owner
          ? frontend.mir.findClassInstance(*stableOverride->owner)
          : nullptr;
  const lang::MirClassInstance *unrelatedOwner =
      unrelatedMarker->owner
          ? frontend.mir.findClassInstance(*unrelatedMarker->owner)
          : nullptr;
  expect(directStableCall != nullptr && stableOwner != nullptr &&
             unrelatedOwner != nullptr &&
             directStableCall->dispatch == lang::CallDispatch::Virtual &&
             directStableCall->dispatchOwner == stableOwner->type &&
             directStableCall->definedFailure.propagation ==
                 lang::FailurePropagationKind::VirtualCall,
         "a concrete override call should retain its exact target, virtual "
         "dispatch owner, and conservative invoke edge in MIR");
  if (directStableCall == nullptr || stableOwner == nullptr ||
      unrelatedOwner == nullptr || unrelatedMarker == nullptr) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress directAddress{.kind = lang::MirBodyKind::Function,
                                           .owner = directCaller->id};
  for (const lang::MirFunctionInstance *function :
       {dereferenceOverride, incrementOverride, stableOverride, entry}) {
    const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                       .owner = function->id};
    expect(emitter.analyze(address).ready() &&
               emitter.supportsFailureBodyText(address),
           "the complete virtual family should support failure-form MIR "
           "emission");
  }
  expect(emitter.analyze(directAddress).ready() &&
             (emitter.supportsBodyText(directAddress) ||
              emitter.supportsFailureBodyText(directAddress)),
         "the direct concrete override caller should remain inside the "
         "complete C++ MIR vocabulary");

  const auto mutableFunction = [](lang::MirProgram &program,
                                  lang::HirFunctionInstanceId id) {
    auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
        program.functionInstances());
    const auto found =
        std::find_if(functions.begin(), functions.end(),
                     [&](const lang::MirFunctionInstance &candidate) {
                       return candidate.id == id;
                     });
    return found == functions.end() ? nullptr : &*found;
  };
  const auto mutableInstruction = [&](lang::MirProgram &program,
                                      lang::MirInstructionId id) {
    lang::MirFunctionInstance *function =
        mutableFunction(program, directCaller->id);
    if (function == nullptr) {
      return static_cast<lang::MirInstruction *>(nullptr);
    }
    for (lang::MirBlock &block : function->body.blocks) {
      const auto found =
          std::find_if(block.instructions.begin(), block.instructions.end(),
                       [&](const lang::MirInstruction &instruction) {
                         return instruction.id == id;
                       });
      if (found != block.instructions.end()) {
        return &*found;
      }
    }
    return static_cast<lang::MirInstruction *>(nullptr);
  };
  const auto rejects = [&](lang::MirProgram program, std::string_view message) {
    const lang::CppMirBodyEmitter malformed(program, map);
    expect(!malformed.supportsBodyText(directAddress) &&
               !malformed.supportsFailureBodyText(directAddress),
           message);
  };

  lang::MirProgram missingOwner = frontend.mir;
  mutableFunction(missingOwner, stableRoot->id)->owner.reset();
  rejects(std::move(missingOwner),
          "a virtual-family candidate without an owner must fail closed");

  lang::MirProgram unrelatedTargetOwner = frontend.mir;
  mutableFunction(unrelatedTargetOwner, stableOverride->id)->owner =
      unrelatedOwner->id;
  rejects(std::move(unrelatedTargetOwner),
          "a concrete virtual target with an unrelated owner must fail "
          "closed");

  lang::MirProgram forgedRoot = frontend.mir;
  mutableFunction(forgedRoot, stableOverride->id)->virtualRoots = {
      unrelatedMarker->declaration};
  rejects(std::move(forgedRoot),
          "a concrete virtual target with a forged contract root must fail "
          "closed");

  lang::MirProgram unrelatedDispatchOwner = frontend.mir;
  mutableInstruction(unrelatedDispatchOwner, directStableCall->id)
      ->dispatchOwner = unrelatedOwner->type;
  rejects(std::move(unrelatedDispatchOwner),
          "a concrete virtual call with an unrelated dispatch owner must "
          "fail closed");

  lang::MirProgram forgedTarget = frontend.mir;
  mutableInstruction(forgedTarget, directStableCall->id)->functionTarget =
      unrelatedMarker->id;
  rejects(std::move(forgedTarget),
          "a concrete virtual call with a forged target must fail closed");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const std::string &text = artifact.contents;
  expect(text.find("virtual bool "
                   "__gti_operator_dereference__gti_mir_failure(") !=
                 std::string::npos &&
             text.find("__gti_operator_dereference__gti_mir_failure("
                       "const std::int32_t **__gti_mir_out_result, "
                       "::gti_failure_record_v1 *__gti_mir_failure_record) "
                       "const override;") != std::string::npos,
         "the interface and concrete class should publish one virtual status "
         "ABI");
  expect(text.find("const std::int32_t &Counter::"
                   "__gti_operator_dereference() const {") !=
                 std::string::npos &&
             text.find("void Counter::__gti_operator_pre_increment() {") !=
                 std::string::npos,
         "ordinary virtual entries should be defined as terminal boundary "
         "wrappers for non-MIR callers");
  expect(text.find("scalar-cfg-failure-v1 function-instance " +
                   std::to_string(entry->id)) != std::string::npos &&
             text.find(".__gti_operator_dereference__gti_mir_failure(") !=
                 std::string::npos &&
             text.find("stable_value__gti_mir_failure(") != std::string::npos &&
             text.find(".stable_value()") != std::string::npos &&
             text.find(".Iterator<std::int32_t>::"
                       "__gti_operator_dereference__gti_mir_failure(") ==
                 std::string::npos,
         "the MIR entry should call the unqualified status member so native "
         "virtual dispatch remains active");
}

void testConcreteOnlyGenericVirtualContractRoots() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-concrete-only-virtual-roots.gti", R"(
interface Iterator<T> {
  T& operator*();
  void operator++() mut;
};

class Counter : public Iterator<int32_t> {
  mut int32_t value;

public:
  Counter(int32_t initial) : value(initial) {}
  int32_t& operator*() override { return this.value; }
  void operator++() mut override { this.value++; }
  bool operator!=(Counter& other) {
    return this.value != other.value;
  }
};

class Range {
  int32_t first;
  int32_t last;

public:
  Range(int32_t first, int32_t last) : first(first), last(last) {}
  Counter begin() { return Counter(this.first); }
  Counter end() { return Counter(this.last); }
};

int32_t main() {
  Range values{1, 4};
  mut int32_t total = 0;
  for (auto& value : values) {
    total += value;
  }
  return total - 6;
}
)");
  expect(frontend.canGenerateCode(),
         "the concrete-only virtual contract fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *dereferenceRoot = nullptr;
  const lang::MirFunctionInstance *incrementRoot = nullptr;
  const lang::MirFunctionInstance *entry = nullptr;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (function.entryKind != lang::ProgramEntryKind::None) {
      entry = &function;
    }
    if (!function.pureVirtual || !function.overloadedOperator) {
      continue;
    }
    if (*function.overloadedOperator == lang::OverloadedOperator::Dereference) {
      dereferenceRoot = &function;
    } else if (*function.overloadedOperator ==
               lang::OverloadedOperator::PreIncrement) {
      incrementRoot = &function;
    }
  }
  expect(dereferenceRoot != nullptr && incrementRoot != nullptr &&
             entry != nullptr,
         "concrete overrides should materialize their exact generic pure "
         "contract roots without an interface-typed source use");
  if (dereferenceRoot == nullptr || incrementRoot == nullptr ||
      entry == nullptr) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress entryAddress{.kind = lang::MirBodyKind::Function,
                                          .owner = entry->id};
  expect(emitter.analyze(entryAddress).ready() &&
             emitter.supportsFailureBodyText(entryAddress),
         "the concrete-only range caller should retain its transformed MIR "
         "body once the virtual contract family is complete");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(artifact.contents.find("scalar-cfg-failure-v1 function-instance " +
                                std::to_string(entry->id)) !=
                 std::string::npos &&
             artifact.contents.find(
                 ".__gti_operator_dereference__gti_mir_failure(") !=
                 std::string::npos,
         "the production backend should emit the concrete-only range caller "
         "through the virtual failure ABI");
}

void testAbstractBaseFailureFamilyAndInheritedMembers() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-abstract-base-failure-family.gti", R"(
class Base {
public:
  mut int32_t state = 0;

  virtual bool advance() mut = 0;
  void stop() mut { this.state = 9; }
  bool run() mut { return this.advance(); }
};

class Derived : public Base {
public:
  Derived() {}

  bool advance() mut override {
    this.state++;
    this.stop();
    return true;
  }

  bool complete() { return this.state == 9; }
};

int32_t main() {
  mut Derived value{};
  if (!value.run()) {
    return 1;
  }
  return 0;
}
)");
  expect(frontend.canGenerateCode(),
         "the abstract-base failure-family fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *root = nullptr;
  const lang::MirFunctionInstance *override = nullptr;
  const lang::MirFunctionInstance *inheritedCaller = nullptr;
  const lang::MirFunctionInstance *fieldReader = nullptr;
  const lang::MirFunctionInstance *entry = nullptr;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (function.entryKind != lang::ProgramEntryKind::None) {
      entry = &function;
    } else if (function.pureVirtual) {
      root = &function;
    } else if (function.overrideMethod) {
      override = &function;
      inheritedCaller = &function;
    } else if (function.owner) {
      const bool callsBaseMember = std::any_of(
          function.body.blocks.begin(), function.body.blocks.end(),
          [&](const lang::MirBlock &block) {
            return std::any_of(block.instructions.begin(),
                               block.instructions.end(),
                               [&](const lang::MirInstruction &instruction) {
                                 if (!instruction.functionTarget) {
                                   return false;
                                 }
                                 const lang::MirFunctionInstance *target =
                                     frontend.mir.findFunctionInstance(
                                         *instruction.functionTarget);
                                 return target != nullptr && target->owner &&
                                        *target->owner != *function.owner;
                               });
          });
      if (callsBaseMember) {
        inheritedCaller = &function;
      } else if (override != nullptr && function.owner == override->owner &&
                 std::any_of(
                     function.body.places.begin(), function.body.places.end(),
                     [](const lang::MirPlace &place) {
                       return place.root == lang::MirPlaceRootKind::This &&
                              !place.projections.empty() &&
                              place.projections.front().kind ==
                                  lang::MirProjectionKind::Field;
                     })) {
        fieldReader = &function;
      }
    }
  }
  const lang::MirConstructorInstance *constructor = nullptr;
  if (override != nullptr && override->owner) {
    for (const lang::MirConstructorInstance &candidate :
         frontend.mir.constructorInstances()) {
      if (candidate.owner == *override->owner) {
        constructor = &candidate;
        break;
      }
    }
  }
  expect(root != nullptr && override != nullptr && inheritedCaller != nullptr &&
             fieldReader != nullptr && entry != nullptr &&
             constructor != nullptr,
         "the fixture should lower its abstract contract, override, inherited "
         "member call, inherited field read, constructor, and entry");
  if (root == nullptr || override == nullptr || inheritedCaller == nullptr ||
      fieldReader == nullptr || entry == nullptr || constructor == nullptr) {
    return;
  }
  expect(lang::cppMirVirtualFailureContractRoot(frontend.mir, *root) == root &&
             lang::cppMirVirtualFailureContractRoot(frontend.mir, *override) ==
                 root,
         "a pure virtual declared on an abstract class should own the same "
         "status ABI as its concrete override");

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  for (const lang::MirFunctionInstance *function :
       {override, inheritedCaller, fieldReader, entry}) {
    const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                       .owner = function->id};
    expect(emitter.analyze(address).ready() &&
               (emitter.supportsBodyText(address) ||
                emitter.supportsFailureBodyText(address)),
           "abstract-base family body " + std::to_string(function->id) +
               " should have verified MIR text");
  }
  const lang::MirBodyAddress constructorAddress{
      .kind = lang::MirBodyKind::Constructor, .owner = constructor->id};
  expect(emitter.analyze(constructorAddress).ready() &&
             emitter.supportsFailureBodyText(constructorAddress),
         "the passive default-base constructor should have verified failure "
         "MIR text");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(artifact.contents.find("virtual bool advance__gti_mir_failure(") !=
                 std::string::npos &&
             artifact.contents.find("advance__gti_mir_failure(") !=
                 artifact.contents.rfind("advance__gti_mir_failure(") &&
             artifact.contents.find("constructor-instance " +
                                    std::to_string(constructor->id) + "\n") !=
                 std::string::npos &&
             artifact.contents.find("function-instance " +
                                    std::to_string(entry->id) + "\n") !=
                 std::string::npos,
         "an abstract class and its override should publish the virtual "
         "failure contract, passive constructor, and entry implementation");
}

void testUniqueOwnerUpcastFailureBody() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-unique-owner-upcast.gti", R"(
#include <std/memory>

interface Base {
  int32_t value();
};

class Derived : public Base {
public:
  Derived() {}
  int32_t value() override { return 7; }
};

class Other : public Base {
public:
  Other() {}
  int32_t value() override { return 5; }
};

int32_t main() {
  auto scalar = std::make_unique<int32_t>(7);
  auto zero = std::make_unique<int32_t>();
  if (*scalar != 7 || *zero != 0) { return 1; }
  mut std::unique_ptr<Derived> derived = std::make_unique<Derived>();
  mut std::unique_ptr<Base> base =
      std::upcast_unique<Base, Derived>(std::move(derived));
  mut std::unique_ptr<Other> other = std::make_unique<Other>();
  mut std::unique_ptr<Base> other_base =
      std::upcast_unique<Base, Other>(std::move(other));
  return base->value() + other_base->value() - 12;
}
)");
  expect(frontend.canGenerateCode(),
         "the unique-owner upcast fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *upcast = nullptr;
  const lang::MirInstruction *upcastIntrinsic = nullptr;
  const lang::MirInstruction *discardedNullAssignment = nullptr;
  const lang::MirInstruction *returnMove = nullptr;
  const auto findInstruction = [](const lang::MirBody &body,
                                  lang::MirInstructionId id) {
    for (const lang::MirBlock &block : body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.id == id) {
          return &instruction;
        }
      }
    }
    return static_cast<const lang::MirInstruction *>(nullptr);
  };
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    for (const lang::MirBlock &block : function.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.intrinsic == lang::IntrinsicKind::UniqueOwnerUpcast) {
          upcast = &function;
          upcastIntrinsic = &instruction;
        }
        if (instruction.kind == lang::MirInstructionKind::Assign &&
            instruction.result && instruction.operands.size() == 1 &&
            instruction.operands.front().type.kind ==
                lang::SemanticType::NullPtr &&
            function.body.usesOf(*instruction.result).empty()) {
          discardedNullAssignment = &instruction;
        }
      }
      if (upcast == &function &&
          block.terminator.kind == lang::MirTerminatorKind::Return &&
          block.terminator.value &&
          block.terminator.value->kind == lang::MirOperandKind::Value) {
        const lang::MirValue *returned =
            function.body.findValue(block.terminator.value->value);
        const lang::MirInstruction *construct =
            returned == nullptr
                ? nullptr
                : findInstruction(function.body, returned->definition);
        if (construct != nullptr &&
            construct->kind == lang::MirInstructionKind::Construct &&
            construct->operands.size() == 1 &&
            construct->operands.front().kind == lang::MirOperandKind::Value) {
          const lang::MirValue *moved =
              function.body.findValue(construct->operands.front().value);
          const lang::MirInstruction *definition =
              moved == nullptr
                  ? nullptr
                  : findInstruction(function.body, moved->definition);
          if (definition != nullptr &&
              definition->kind == lang::MirInstructionKind::Move) {
            returnMove = definition;
          }
        }
      }
    }
  }
  expect(upcast != nullptr && upcastIntrinsic != nullptr &&
             discardedNullAssignment != nullptr && returnMove != nullptr,
         "the fixture should lower the consuming intrinsic, discarded null "
         "assignment, and fused owner return");
  if (upcast == nullptr || upcastIntrinsic == nullptr ||
      discardedNullAssignment == nullptr || returnMove == nullptr) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = upcast->id};
  expect(emitter.analyze(address).ready() &&
             emitter.supportsFailureBodyText(address),
         "the unique-owner upcast body should support transformed MIR "
         "emission");
  if (!emitter.supportsFailureBodyText(address)) {
    return;
  }
  const lang::CppMirBodyEmissionText text =
      emitter.emitFailureBodyText(address, "unique-owner-upcast-test-v0", 1);
  std::size_t checkedBorrows = 0;
  std::size_t checkedAllocations = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.intrinsic == lang::IntrinsicKind::AllocateUniqueOwner &&
            instruction.info.type.arguments ==
                std::vector<lang::SemanticType>{lang::SemanticType::Int32}) {
          ++checkedAllocations;
          const lang::MirBodyAddress allocationAddress{
              .kind = lang::MirBodyKind::Function, .owner = function.id};
          expect(emitter.analyze(allocationAddress).ready() &&
                     !emitter.supportsBodyText(allocationAddress) &&
                     emitter.supportsFailureBodyText(allocationAddress),
                 "scalar allocation must require the MIR failure ABI");
          if (emitter.supportsFailureBodyText(allocationAddress)) {
            const auto allocationText = emitter.emitFailureBodyText(
                allocationAddress, "scalar-allocation-test-v0", 1);
            expect(allocationText.text.find("mir_allocate_unique_scalar_v1<") !=
                           std::string::npos &&
                       allocationText.text.find(
                           ".code == GTI_FAILURE_CODE_NONE_V1") !=
                           std::string::npos &&
                       allocationText.text.find("*__gti_mir_failure_record = "
                                                "::gti_failure_record_v1{") !=
                           std::string::npos &&
                       allocationText.text.find("return false;") !=
                           std::string::npos &&
                       allocationText.text.find("::backend::make_unique<") ==
                           std::string::npos,
                   "scalar allocation must inspect status and retain its "
                   "failure return");
          }
        }
        if (instruction.intrinsic != lang::IntrinsicKind::UniqueOwnerBorrow &&
            instruction.intrinsic !=
                lang::IntrinsicKind::UniqueOwnerBorrowMut) {
          continue;
        }
        ++checkedBorrows;
        const lang::MirBodyAddress borrowAddress{
            .kind = lang::MirBodyKind::Function, .owner = function.id};
        expect(
            emitter.analyze(borrowAddress).ready() &&
                !emitter.supportsBodyText(borrowAddress) &&
                emitter.supportsFailureBodyText(borrowAddress),
            "owner borrowing must require the cleanup-preserving failure ABI");
        if (!emitter.supportsFailureBodyText(borrowAddress)) {
          continue;
        }
        const auto borrowText = emitter.emitFailureBodyText(
            borrowAddress, "unique-owner-borrow-test-v0", 1);
        const auto check =
            borrowText.text.find("GTI_FAILURE_CODE_EMPTY_OWNER_ACCESS_V1");
        const auto publication =
            borrowText.text.find("*__gti_mir_out_result =");
        expect(check != std::string::npos && publication != std::string::npos &&
                   check < publication &&
                   borrowText.text.find(".get();", publication) !=
                       std::string::npos &&
                   borrowText.text.find("return false;") != std::string::npos &&
                   borrowText.text.find("owner_access(") == std::string::npos,
               "the MIR detector must precede success publication and preserve "
               "a failure return");
      }
    }
  }
  expect(checkedBorrows != 0,
         "the upcast client must exercise checked owner access");
  expect(checkedAllocations == 2,
         "both scalar allocation forms need checked production emission");
  const std::size_t upcastCall = text.text.find("unique_owner_upcast<");
  const std::size_t movedArgument = text.text.find(">(std::move(", upcastCall);
  expect(text.text.find("unique_owner_upcast<") != std::string::npos &&
             movedArgument != std::string::npos &&
             text.text.find("__gti_mir_v_" +
                            std::to_string(*discardedNullAssignment->result) +
                            " =") == std::string::npos &&
             text.text.find("std::move(__gti_mir_v_" +
                            std::to_string(*returnMove->result)) ==
                 std::string::npos &&
             text.text.find(
                 "std::construct_at(__gti_mir_out_result, std::move(") !=
                 std::string::npos,
         "the upcast should consume its owner, omit the discarded assignment "
         "readback, and publish the fused move from its source place");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const std::size_t primary = artifact.contents.find("class unique_ptr");
  const std::size_t nextMember = artifact.contents.find("const T &", primary);
  std::vector<std::string> declarations;
  if (primary != std::string::npos && nextMember != std::string::npos) {
    std::size_t cursor = primary;
    while ((cursor = artifact.contents.find("upcast__gti_mir_failure(",
                                            cursor)) != std::string::npos &&
           cursor < nextMember) {
      const std::size_t nameStart =
          artifact.contents.rfind("__gti_member_", cursor);
      if (nameStart == std::string::npos || nameStart < primary) {
        break;
      }
      declarations.push_back(artifact.contents.substr(
          nameStart, cursor - nameStart +
                         std::string_view("upcast__gti_mir_failure").size()));
      ++cursor;
    }
  }
  const auto occurrenceCount = [&](const std::string &name) {
    std::size_t count = 0;
    std::size_t cursor = 0;
    while ((cursor = artifact.contents.find(name + '(', cursor)) !=
           std::string::npos) {
      ++count;
      ++cursor;
    }
    return count;
  };
  const auto ownerCount = [&](const std::string &name) {
    std::size_t count = 0;
    for (std::string_view owner : {"Derived", "Other"}) {
      count += artifact.contents.find(
                   "unique_ptr<::__gti_program::" + std::string(owner) +
                   ">::" + name + '(') != std::string::npos
                   ? 1
                   : 0;
    }
    return count;
  };
  expect(declarations.size() == 2 && declarations[0] != declarations[1] &&
             std::all_of(declarations.begin(), declarations.end(),
                         [&](const std::string &name) {
                           return occurrenceCount(name) == 4 &&
                                  ownerCount(name) == 1;
                         }),
         "each selected generic upcast on a retained owner carrier should "
         "publish one distinct owner-qualified declaration, specialization, "
         "call, and definition");
}

void testExpectedLifetimeSlotObserverFusion() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-expected-slot-observer.gti", R"(
#include <std/format>
#include <std/string>

int32_t main() {
  auto formatted = std::format("value={}", int32_t(7));
  if (!formatted) {
    return 1;
  }
  std::println(formatted.value());
  return 0;
}
)");
  expect(frontend.canGenerateCode(),
         "the expected-slot observer fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *selected = nullptr;
  lang::MirValueId observerValue = 0;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    for (const lang::MirBlock &block : function.body.blocks) {
      for (const lang::MirInstruction &load : block.instructions) {
        if (load.kind != lang::MirInstructionKind::Load || !load.result ||
            load.info.type.kind != lang::SemanticType::Expected ||
            load.info.type.arguments.size() != 2 ||
            load.info.type.arguments.front().kind !=
                lang::SemanticType::Class ||
            load.operands.size() != 1 || load.operands.front().place == 0) {
          continue;
        }
        const bool ownedSlot =
            std::any_of(function.body.dropObligations.begin(),
                        function.body.dropObligations.end(),
                        [&](const lang::MirDropObligation &drop) {
                          return drop.place == load.operands.front().place;
                        });
        bool observer = false;
        bool extraction = false;
        for (const lang::MirBlock &candidate : function.body.blocks) {
          for (const lang::MirInstruction &instruction :
               candidate.instructions) {
            observer = observer ||
                       (instruction.kind == lang::MirInstructionKind::Compute &&
                        instruction.operation ==
                            lang::MirOperation::ExpectedHasValue &&
                        instruction.operands.size() == 1 &&
                        instruction.operands.front().kind ==
                            lang::MirOperandKind::Value &&
                        instruction.operands.front().value == *load.result);
            extraction =
                extraction ||
                (instruction.kind == lang::MirInstructionKind::Call &&
                 instruction.receiver &&
                 instruction.receiver->place == load.operands.front().place &&
                 (instruction.intrinsic == lang::IntrinsicKind::ExpectedValue ||
                  instruction.intrinsic == lang::IntrinsicKind::ExpectedError));
          }
        }
        if (ownedSlot && observer && extraction) {
          selected = &function;
          observerValue = *load.result;
        }
      }
    }
  }
  expect(selected != nullptr,
         "the fixture should lower an owned Expected slot with an observer "
         "and extraction");
  if (selected == nullptr) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = selected->id};
  expect(emitter.analyze(address).ready() &&
             emitter.supportsFailureBodyText(address),
         "the owned Expected slot should enter the transformed failure-body "
         "path");
  if (!emitter.supportsFailureBodyText(address)) {
    return;
  }
  const lang::CppMirBodyEmissionText text =
      emitter.emitFailureBodyText(address, "expected-slot-observer-test-v0", 1);
  const std::string observerLocal =
      "__gti_mir_v_" + std::to_string(observerValue) + "{}";
  const bool extractionUsesLiveValue =
      text.text.find(".get().value()") != std::string::npos ||
      text.text.find(".get().error()") != std::string::npos ||
      (text.text.find("mir_expected_") != std::string::npos &&
       text.text.find(".get(), &__gti_mir_") != std::string::npos);
  expect(text.text.find(".get().has_value()") != std::string::npos &&
             extractionUsesLiveValue,
         "Expected observers and extraction helpers should read the live "
         "value inside its lifetime slot");
  expect(text.text.find(observerLocal) == std::string::npos,
         "the Expected observer's explicit MIR load must not materialize a "
         "copying SSA local");
}

void testExpectedOwningPayloadPublicationAndForwarding() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-expected-owning-payload.gti", R"(
#include <std/tcp>

int main() {
  auto opened = std::tcp::open();
  if (!opened) {
    return 1;
  }
  return opened.value().is_open() ? 0 : 2;
}
)");
  expect(frontend.canGenerateCode(),
         "the owning Expected payload fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *payloadProducer = nullptr;
  const lang::MirFunctionInstance *forwarder = nullptr;
  const lang::MirFunctionInstance *entry = nullptr;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (function.entryKind != lang::ProgramEntryKind::None) {
      entry = &function;
    }
    if (function.returnType.kind != lang::SemanticType::Expected ||
        function.returnType.arguments.size() != 2 ||
        function.returnType.arguments.front().kind !=
            lang::SemanticType::Class) {
      continue;
    }
    for (const lang::MirBlock &block : function.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Construct &&
            instruction.result &&
            instruction.info.type == function.returnType.arguments.front()) {
          payloadProducer = &function;
        }
        if (instruction.kind == lang::MirInstructionKind::Call &&
            instruction.result &&
            instruction.info.type == function.returnType) {
          forwarder = &function;
        }
      }
    }
  }
  expect(payloadProducer != nullptr && forwarder != nullptr && entry != nullptr,
         "the fixture should lower a payload constructor, an unchanged "
         "Expected forwarder, and a hosted caller");
  if (payloadProducer == nullptr || forwarder == nullptr || entry == nullptr) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  for (const lang::MirFunctionInstance *function :
       {payloadProducer, forwarder, entry}) {
    const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                       .owner = function->id};
    expect(emitter.analyze(address).ready() &&
               emitter.supportsFailureBodyText(address),
           "each owning Expected call-chain body should support transformed "
           "failure emission");
  }

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const std::string &text = artifact.contents;
  const auto hasMarker = [&](const lang::MirFunctionInstance &function) {
    return text.find("scalar-cfg-failure-v1 function-instance " +
                     std::to_string(function.id)) != std::string::npos;
  };
  expect(hasMarker(*payloadProducer) && hasMarker(*forwarder) &&
             hasMarker(*entry),
         "production emission should select the complete owning Expected "
         "call chain");
  expect(text.find("handle(std::move(__gti_mir_arg_0))") != std::string::npos &&
             text.find(".construct_from([&]() { return ") !=
                 std::string::npos &&
             text.find(", true> __gti_mir_v_") != std::string::npos,
         "the private owned field should initialize in place and the payload "
         "should publish through one representation-cleanup slot");
  expect(text.find("placement value published at its call") !=
                 std::string::npos &&
             text.find("(__gti_mir_out_result, __gti_mir_failure_record)") !=
                 std::string::npos,
         "the unchanged Expected forwarder should pass its caller's result "
         "storage directly to the transformed callee");
}

void testCleanupOwningTemporaryArgumentTransfer() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-cleanup-temporary-argument.gti", R"(
class cleanup_t {
public:
  cleanup_t(int32_t initial) : value(initial) {}
  cleanup_t(cleanup_t& other) = delete;
  cleanup_t(cleanup_t&& other) = default;
  ~cleanup_t() {}

  int32_t value;
};

int32_t read(cleanup_t value) { return value.value; }

expected<cleanup_t, int32_t> make_ready(int32_t value) {
  return cleanup_t(value);
}

int main() {
  int32_t direct = read(cleanup_t(3));
  expected<cleanup_t, int32_t> ready = make_ready(4);
  return direct + read(std::move(ready).value()) - 7;
}
)");
  expect(frontend.canGenerateCode(),
         "cleanup-owning constructor and Expected results should pass the "
         "frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const auto entry = std::find_if(
      frontend.mir.functionInstances().begin(),
      frontend.mir.functionInstances().end(), [](const auto &function) {
        return function.entryKind == lang::ProgramEntryKind::NoArguments;
      });
  expect(entry != frontend.mir.functionInstances().end(),
         "the cleanup-temporary fixture should retain its entry function");
  if (entry == frontend.mir.functionInstances().end()) {
    return;
  }

  const auto findInstruction = [](const lang::MirBody &body,
                                  lang::MirInstructionId id) {
    for (const lang::MirBlock &block : body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.id == id) {
          return &instruction;
        }
      }
    }
    return static_cast<const lang::MirInstruction *>(nullptr);
  };
  const lang::MirInstruction *directStage = nullptr;
  const lang::MirInstruction *expectedStage = nullptr;
  const lang::MirInstruction *expectedProducer = nullptr;
  for (const lang::MirBlock &block : entry->body.blocks) {
    for (const lang::MirInstruction &instruction : block.instructions) {
      if (instruction.kind != lang::MirInstructionKind::CallInput ||
          instruction.callInputKind != lang::HirCallInputKind::MoveValue ||
          instruction.operands.size() != 1 ||
          instruction.operands.front().kind != lang::MirOperandKind::Value) {
        continue;
      }
      const lang::MirValue *source =
          entry->body.findValue(instruction.operands.front().value);
      const lang::MirInstruction *producer =
          source == nullptr ? nullptr
                            : findInstruction(entry->body, source->definition);
      if (producer != nullptr &&
          producer->kind == lang::MirInstructionKind::Construct) {
        directStage = &instruction;
      }
      if (producer != nullptr &&
          producer->kind == lang::MirInstructionKind::Call &&
          producer->intrinsic == lang::IntrinsicKind::ExpectedValue) {
        expectedStage = &instruction;
        expectedProducer = producer;
      }
    }
  }

  const lang::MirDropObligation *sourceDrop =
      expectedStage == nullptr || expectedStage->lifecycle.size() != 1
          ? nullptr
          : entry->body.findDropObligation(
                expectedStage->lifecycle.front().source);
  const lang::MirDropObligation *preparedDrop =
      expectedStage == nullptr || !expectedStage->preparedParameterDrop
          ? nullptr
          : entry->body.findDropObligation(
                *expectedStage->preparedParameterDrop);
  const lang::MirPlace *sourcePlace =
      sourceDrop == nullptr ? nullptr
                            : entry->body.findPlace(sourceDrop->place);
  const lang::MirPlace *preparedPlace =
      preparedDrop == nullptr ? nullptr
                              : entry->body.findPlace(preparedDrop->place);
  const lang::MirPlace *directPlace =
      directStage == nullptr || !directStage->destination
          ? nullptr
          : entry->body.findPlace(*directStage->destination);
  expect(directStage != nullptr && expectedStage != nullptr &&
             expectedProducer != nullptr && sourceDrop != nullptr &&
             preparedDrop != nullptr && sourcePlace != nullptr &&
             preparedPlace != nullptr && directPlace != nullptr &&
             sourceDrop->kind == lang::MirDropObligationKind::Value &&
             preparedDrop->kind ==
                 lang::MirDropObligationKind::PreparedParameter &&
             sourcePlace->root == lang::MirPlaceRootKind::Value &&
             sourcePlace->projections.empty() &&
             preparedPlace->root == lang::MirPlaceRootKind::Temporary &&
             preparedPlace->projections.empty() &&
             sourceDrop->dropType == preparedDrop->dropType &&
             sourceDrop->dropType.requiresActiveCleanup &&
             expectedStage->lifecycle.front().kind ==
                 lang::MirLifecycleEventKind::Reparent &&
             expectedStage->lifecycle.front().target == preparedDrop->id,
         "the owning extraction should retain one exact Value-drop to "
         "PreparedParameter reparent chain");
  if (directStage == nullptr || expectedStage == nullptr ||
      expectedProducer == nullptr || sourceDrop == nullptr ||
      preparedDrop == nullptr || sourcePlace == nullptr ||
      preparedPlace == nullptr || directPlace == nullptr) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = entry->id};
  expect(emitter.analyze(address).ready() &&
             emitter.supportsFailureBodyText(address),
         "the verified cleanup-owning transfers should support transformed "
         "failure emission");
  if (!emitter.supportsFailureBodyText(address)) {
    return;
  }

  const std::string text =
      emitter.emitFailureBodyText(address, "cleanup-temporary-argument-v0", 1)
          .text;
  const auto count = [&](std::string_view needle) {
    std::size_t occurrences = 0;
    for (std::size_t at = text.find(needle); at != std::string::npos;
         at = text.find(needle, at + needle.size())) {
      ++occurrences;
    }
    return occurrences;
  };
  const std::string sourceSlot =
      "__gti_mir_p_" + std::to_string(sourcePlace->id);
  const std::string sourceValue =
      "__gti_mir_v_" + std::to_string(expectedStage->operands.front().value);
  const std::string preparedSlot =
      "__gti_mir_p_" + std::to_string(preparedPlace->id);
  const std::string directSlot =
      "__gti_mir_p_" + std::to_string(directPlace->id);
  expect(count("mir_expected_value_move_v1") == 1 &&
             count("&" + preparedSlot) == 1 &&
             count("std::move(" + preparedSlot + ".get())") == 1 &&
             count(preparedSlot + ".destroy();") == 1 &&
             text.find(sourceSlot) == std::string::npos &&
             text.find(sourceValue) == std::string::npos,
         "the Expected extraction should construct the prepared slot once, "
         "consume and retire it once, and emit no source storage");
  expect(count(directSlot + ".construct(") == 1 &&
             count("std::move(" + directSlot + ".get())") == 1 &&
             count(directSlot + ".destroy();") == 1,
         "the direct constructor temporary should use one prepared slot and "
         "one post-call retirement");

  const auto mutableEntry = [&](lang::MirProgram &program) {
    auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
        program.functionInstances());
    const auto found =
        std::find_if(functions.begin(), functions.end(),
                     [&](const auto &item) { return item.id == entry->id; });
    return found == functions.end() ? nullptr : &*found;
  };
  const auto mutableInstruction = [](lang::MirBody &body,
                                     lang::MirInstructionId id) {
    for (lang::MirBlock &block : body.blocks) {
      const auto found =
          std::find_if(block.instructions.begin(), block.instructions.end(),
                       [&](const lang::MirInstruction &instruction) {
                         return instruction.id == id;
                       });
      if (found != block.instructions.end()) {
        return &*found;
      }
    }
    return static_cast<lang::MirInstruction *>(nullptr);
  };
  const auto rejected = [&](const lang::MirProgram &program) {
    const lang::CppMirBodyEmissionMap changedMap(completeRows(program));
    return !lang::CppMirBodyEmitter(program, changedMap)
                .supportsFailureBodyText(address);
  };

  lang::MirProgram duplicateUse = frontend.mir;
  if (lang::MirFunctionInstance *function = mutableEntry(duplicateUse)) {
    if (lang::MirInstruction *stage =
            mutableInstruction(function->body, expectedStage->id)) {
      stage->operands.push_back(stage->operands.front());
      (void)lang::rebuildMirValueUses(function->body);
    }
  }
  expect(rejected(duplicateUse),
         "an owning extraction with a duplicate staged use should remain "
         "unsupported");

  lang::MirProgram projectedSource = frontend.mir;
  if (lang::MirFunctionInstance *function = mutableEntry(projectedSource)) {
    if (lang::MirPlace *place = const_cast<lang::MirPlace *>(
            function->body.findPlace(sourcePlace->id))) {
      place->projections.push_back(
          {.kind = lang::MirProjectionKind::Field, .field = 0});
    }
  }
  expect(rejected(projectedSource),
         "a projected source drop identity should remain unsupported");

  lang::MirProgram aliasedSource = frontend.mir;
  if (lang::MirFunctionInstance *function = mutableEntry(aliasedSource)) {
    lang::MirLoanId nextLoan = 1;
    for (const lang::MirLoan &loan : function->body.loans) {
      nextLoan = std::max(nextLoan, loan.id + 1);
    }
    function->body.loans.push_back({.id = nextLoan,
                                    .source = sourcePlace->id,
                                    .access = lang::AccessMode::ReadOnly});
  }
  expect(rejected(aliasedSource),
         "an aliased source drop identity should remain unsupported");

  lang::MirProgram mismatchedDrop = frontend.mir;
  if (lang::MirFunctionInstance *function = mutableEntry(mismatchedDrop)) {
    for (lang::MirDropObligation &drop : function->body.dropObligations) {
      if (drop.id == preparedDrop->id) {
        drop.dropType.type = lang::SemanticType::Int32;
      }
    }
  }
  expect(rejected(mismatchedDrop),
         "a prepared parameter with mismatched drop metadata should remain "
         "unsupported");

  lang::MirProgram unprovenFailure = frontend.mir;
  if (lang::MirFunctionInstance *function = mutableEntry(unprovenFailure)) {
    if (lang::MirInstruction *producer =
            mutableInstruction(function->body, expectedProducer->id)) {
      producer->localFailureSites.clear();
    }
  }
  expect(rejected(unprovenFailure),
         "an owning extraction without its exact failure proof should remain "
         "unsupported");
}

void testConstructedArgumentTemporaryBorrow() {
  const lang::FrontendResult frontend = analyzeWithStandardLibrary(
      "cpp-mir-constructed-argument-temporary-borrow.gti", R"(
#include <std/string>
class Text {
  std::string data;
public:
  Text(std::string value) : data(value.clone()) {}
  std::string get() { return this.data.clone(); }
  std::string update() mut { return this.data.clone(); }
};
int32_t observe(Text& value) { return 1; }
int32_t main() {
  std::println(Text(std::string("read")).get());
  std::println((Text(std::string("mutable"))).update());
  return observe(Text(std::string("borrow"))) - 1;
}
)");
  expect(frontend.canGenerateCode(),
         "constructed owning arguments of borrowed temporaries should pass "
         "the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const auto entry = std::find_if(
      frontend.mir.functionInstances().begin(),
      frontend.mir.functionInstances().end(), [](const auto &function) {
        return function.entryKind == lang::ProgramEntryKind::NoArguments;
      });
  expect(entry != frontend.mir.functionInstances().end(),
         "the constructed-argument fixture should lower its entry");
  if (entry == frontend.mir.functionInstances().end()) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = entry->id};
  expect(emitter.analyze(address).ready() &&
             emitter.supportsFailureBodyText(address),
         "a moved constructor argument should not require naming the outer "
         "temporary receiver or read-borrowed argument");
  if (!emitter.supportsFailureBodyText(address)) {
    return;
  }
  const std::string text =
      emitter.emitFailureBodyText(address, "constructed-argument-test-v0", 1)
          .text;
  std::size_t retirements = 0;
  for (std::size_t at = 0;
       (at = text.find("end transferred argument representation", at)) !=
       std::string::npos;
       ++at) {
    ++retirements;
  }
  expect(retirements == 3 &&
             text.find(".construction_address()") != std::string::npos &&
             text.find(".mark_constructed();") != std::string::npos,
         "each of the three inner owners should publish once and retire its "
         "moved-from representation after outer construction");

  const lang::MirInstruction *stage = nullptr;
  const lang::MirInstruction *outer = nullptr;
  bool readReceiver = false;
  bool mutableReceiver = false;
  bool readArgument = false;
  for (const lang::MirBlock &block : entry->body.blocks) {
    for (const lang::MirInstruction &instruction : block.instructions) {
      if (instruction.kind != lang::MirInstructionKind::CallInput) {
        continue;
      }
      if (instruction.callInputKind == lang::HirCallInputKind::MoveValue &&
          instruction.lifecycle.size() == 1 &&
          instruction.lifecycle.front().kind ==
              lang::MirLifecycleEventKind::TransferOut) {
        stage = &instruction;
      }
      readReceiver |=
          instruction.callInputKind ==
              lang::HirCallInputKind::ReadTemporaryBorrow &&
          instruction.callInputRole == lang::MirCallInputRole::Receiver;
      mutableReceiver |=
          instruction.callInputKind ==
              lang::HirCallInputKind::MutableTemporaryBorrow &&
          instruction.callInputRole == lang::MirCallInputRole::Receiver;
      readArgument |=
          instruction.callInputKind ==
              lang::HirCallInputKind::ReadTemporaryBorrow &&
          instruction.callInputRole == lang::MirCallInputRole::Argument;
    }
  }
  expect(stage != nullptr && readReceiver && mutableReceiver && readArgument,
         "MIR should retain the exact transfer and all three temporary borrow "
         "roles");
  if (stage == nullptr || !stage->result || stage->operands.size() != 1) {
    return;
  }
  for (const lang::MirBlock &block : entry->body.blocks) {
    for (const lang::MirInstruction &instruction : block.instructions) {
      if (instruction.kind == lang::MirInstructionKind::Construct &&
          std::any_of(instruction.operands.begin(), instruction.operands.end(),
                      [&](const lang::MirOperand &operand) {
                        return operand.kind == lang::MirOperandKind::Value &&
                               operand.value == *stage->result;
                      })) {
        outer = &instruction;
      }
    }
  }
  const lang::MirValue *inner =
      entry->body.findValue(stage->operands.front().value);
  expect(inner != nullptr && outer != nullptr && outer->successResultDrop,
         "the transfer should connect one inner producer and one activated "
         "outer temporary");
  if (inner == nullptr || outer == nullptr || !outer->successResultDrop) {
    return;
  }
  const auto mutate = [&](const auto &edit, std::string_view message) {
    lang::MirProgram changed = frontend.mir;
    auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
        changed.functionInstances());
    auto function = std::find_if(
        functions.begin(), functions.end(),
        [&](const auto &candidate) { return candidate.id == entry->id; });
    edit(function->body);
    expect(!lang::verifyMirProgram(changed).valid(), message);
    // Keep the representation rows from the valid program: lowering an
    // invalid mutation would reject before this independent backend probe.
    expect(
        !lang::CppMirBodyEmitter(changed, map).supportsFailureBodyText(address),
        message);
  };
  mutate(
      [&](lang::MirBody &body) {
        for (lang::MirBlock &block : body.blocks) {
          for (lang::MirInstruction &instruction : block.instructions) {
            if (instruction.id == stage->id) {
              instruction.lifecycle.clear();
            }
          }
        }
      },
      "an inner argument without its transfer must fail closed");
  for (lang::MirInstructionId producer : {inner->definition, outer->id}) {
    mutate(
        [&](lang::MirBody &body) {
          for (lang::MirBlock &block : body.blocks) {
            if (block.terminator.invokeInstruction == producer) {
              block.terminator.successLifecycle.clear();
            }
          }
        },
        "an inner or outer owner without success activation must fail closed");
  }
  mutate(
      [&](lang::MirBody &body) {
        for (lang::MirBlock &block : body.blocks) {
          for (lang::MirInstruction &instruction : block.instructions) {
            if (instruction.kind == lang::MirInstructionKind::Drop &&
                instruction.lifecycle.size() == 1 &&
                instruction.lifecycle.front().source ==
                    *outer->successResultDrop) {
              instruction.lifecycle.clear();
            }
          }
        }
      },
      "an outer temporary without scheduled cleanup must fail closed");
  mutate(
      [&](lang::MirBody &body) {
        for (lang::MirPlace &place : body.places) {
          if (place.root == lang::MirPlaceRootKind::Value &&
              place.value == *outer->result) {
            place.access = lang::AccessMode::ReadOnly;
          }
        }
      },
      "an outer temporary without mutable materialized storage must fail "
      "closed");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(artifact.contents.find("scalar-cfg-failure-v1 function-instance " +
                                std::to_string(entry->id) + "\n") !=
             std::string::npos,
         "production must emit the complete entry from verified MIR");
}

void testFailureCapableClassReturnTemporaryReceiver() {
  const lang::FrontendResult frontend = analyzeWithStandardLibrary(
      "cpp-mir-class-return-temporary-receiver.gti", R"(
#include <std/string>

int32_t main() {
  if (std::to_string(int8_t(0)) != "0") {
    return 1;
  }
  return 0;
}
)");
  expect(
      frontend.canGenerateCode(),
      "the class-return temporary-receiver fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const auto entry = std::find_if(
      frontend.mir.functionInstances().begin(),
      frontend.mir.functionInstances().end(), [](const auto &function) {
        return function.entryKind == lang::ProgramEntryKind::NoArguments;
      });
  expect(entry != frontend.mir.functionInstances().end(),
         "the fixture should lower its no-argument entry");
  if (entry == frontend.mir.functionInstances().end()) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = entry->id};
  expect(emitter.analyze(address).ready() &&
             emitter.supportsFailureBodyText(address),
         "a fallible class-return call used as a fallible operator receiver "
         "should enter transformed MIR emission");
  if (!emitter.supportsFailureBodyText(address)) {
    return;
  }

  const std::string text =
      emitter.emitFailureBodyText(address, "class-return-receiver-test-v0", 1)
          .text;
  expect(text.find("mir_lifetime_slot<") != std::string::npos &&
             text.find("to_string__gti_mir_failure(") != std::string::npos &&
             text.find(".construction_address()") != std::string::npos &&
             text.find(".mark_constructed();") != std::string::npos &&
             text.find("string::__gti_mir_op_ne__gti_mir_failure(") !=
                 std::string::npos &&
             text.find(".get().") != std::string::npos &&
             text.find(".destroy();") != std::string::npos &&
             text.find("failure cleanup drop-obligation") != std::string::npos,
         "the returned class should publish into one MIR lifetime slot, serve "
         "as the operator receiver, and retire on success and failure");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(artifact.contents.find("scalar-cfg-failure-v1 function-instance " +
                                std::to_string(entry->id) + "\n") !=
             std::string::npos,
         "production emission should select the temporary-receiver entry from "
         "verified MIR");
}

void testFailureCapableClassReturnFieldRead() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-class-return-field-read.gti", R"(
struct Packet {
  int32_t value;
  Packet(int32_t initial) : value(initial) {}
};

Packet make_packet() {
  return Packet(7);
}

int32_t main() { return make_packet().value - 7; }
)");
  expect(frontend.canGenerateCode(),
         "a field read from a returned class value should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const auto entry = std::find_if(
      frontend.mir.functionInstances().begin(),
      frontend.mir.functionInstances().end(), [](const auto &function) {
        return function.entryKind == lang::ProgramEntryKind::NoArguments;
      });
  expect(entry != frontend.mir.functionInstances().end(),
         "the returned-field fixture should lower its entry function");
  if (entry == frontend.mir.functionInstances().end()) {
    return;
  }

  lang::MirValueId classResult = 0;
  for (const lang::MirBlock &block : entry->body.blocks) {
    for (const lang::MirInstruction &instruction : block.instructions) {
      if (instruction.kind == lang::MirInstructionKind::Call &&
          instruction.functionTarget && instruction.result &&
          instruction.info.type.kind == lang::SemanticType::Class) {
        classResult = *instruction.result;
      }
    }
  }
  expect(classResult != 0,
         "the returned-field fixture should retain its class call result");

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = entry->id};
  expect(emitter.analyze(address).ready() &&
             emitter.supportsFailureBodyText(address),
         "the returned class field read should enter transformed MIR "
         "emission");
  if (!emitter.supportsFailureBodyText(address)) {
    return;
  }

  const std::string text =
      emitter.emitFailureBodyText(address, "class-return-field-read-v0", 1)
          .text;
  const std::string slot = "__gti_mir_v_" + std::to_string(classResult);
  expect(text.find("mir_lifetime_slot<") != std::string::npos &&
             text.find(slot + ".construction_address()") != std::string::npos &&
             text.find(slot + ".mark_constructed()") != std::string::npos &&
             text.find(slot + ".get().") != std::string::npos,
         "the returned class should be constructed into one representation "
         "slot and projected from that slot");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(!artifact.contents.empty(),
         "the complete backend should emit a returned-class field read");

  lang::MirProgram duplicateProjectionUse = frontend.mir;
  auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
      duplicateProjectionUse.functionInstances());
  lang::MirFunctionInstance *mutatedEntry = nullptr;
  for (lang::MirFunctionInstance &function : functions) {
    if (function.id == entry->id) {
      mutatedEntry = &function;
    }
  }
  lang::MirPlace *projection = nullptr;
  if (mutatedEntry != nullptr) {
    for (lang::MirPlace &place : mutatedEntry->body.places) {
      if (place.root == lang::MirPlaceRootKind::Value &&
          place.value == classResult && !place.projections.empty()) {
        projection = &place;
      }
    }
  }
  bool replacedReturn = false;
  if (mutatedEntry != nullptr && projection != nullptr) {
    for (lang::MirBlock &block : mutatedEntry->body.blocks) {
      if (block.terminator.kind != lang::MirTerminatorKind::Return) {
        continue;
      }
      block.terminator.value =
          lang::MirOperand{.kind = lang::MirOperandKind::Copy,
                           .place = projection->id,
                           .type = projection->type};
      replacedReturn = true;
    }
    (void)lang::rebuildMirValueUses(mutatedEntry->body);
  }
  expect(replacedReturn &&
             lang::verifyMirProgram(duplicateProjectionUse).valid(),
         "the returned-field near miss should remain structurally valid MIR");
  if (replacedReturn &&
      lang::verifyMirProgram(duplicateProjectionUse).valid()) {
    const lang::CppMirBodyEmissionMap duplicateMap(
        completeRows(duplicateProjectionUse));
    const lang::CppMirBodyEmitter duplicateEmitter(duplicateProjectionUse,
                                                   duplicateMap);
    expect(!duplicateEmitter.supportsFailureBodyText(address),
           "the class lifetime-slot selector should reject a projected place "
           "with a second direct consumer");
  }
}

void testCleanupFreeConstructorTemporaryFieldRead() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-constructor-temporary-field-read.gti", R"(
class plain_t {
public:
  plain_t() {}
  mut int32_t value = 7;
};

int32_t main() { return plain_t().value - 7; }
)");
  expect(frontend.canGenerateCode(),
         "a direct field read from a cleanup-free constructor temporary "
         "should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const auto entry = std::find_if(
      frontend.mir.functionInstances().begin(),
      frontend.mir.functionInstances().end(), [](const auto &function) {
        return function.entryKind == lang::ProgramEntryKind::NoArguments;
      });
  expect(entry != frontend.mir.functionInstances().end(),
         "the constructor-temporary fixture should lower its entry function");
  if (entry == frontend.mir.functionInstances().end()) {
    return;
  }

  const lang::MirInstruction *construct = nullptr;
  for (const lang::MirBlock &block : entry->body.blocks) {
    for (const lang::MirInstruction &instruction : block.instructions) {
      if (instruction.kind == lang::MirInstructionKind::Construct &&
          instruction.constructorTarget && instruction.result &&
          instruction.info.type.kind == lang::SemanticType::Class) {
        expect(construct == nullptr,
               "the canonical fixture should have one class construction");
        construct = &instruction;
      }
    }
  }
  expect(construct != nullptr,
         "the direct constructor receiver should remain an ordinary MIR "
         "Construct result");
  if (construct == nullptr) {
    return;
  }

  const lang::MirValueId classResult = *construct->result;
  const lang::MirValue *constructed = entry->body.findValue(classResult);
  const lang::MirConstructorInstance *constructor =
      frontend.mir.findConstructorInstance(*construct->constructorTarget);
  const lang::MirClassInstance *owner =
      constructor == nullptr
          ? nullptr
          : frontend.mir.findClassInstance(constructor->owner);
  const std::vector<lang::MirValueUse> uses = entry->body.usesOf(classResult);
  const lang::MirPlace *projection =
      uses.size() == 1 && uses.front().kind == lang::MirValueUseKind::PlaceRoot
          ? entry->body.findPlace(uses.front().place)
          : nullptr;
  const lang::MirInstruction *load = nullptr;
  if (projection != nullptr) {
    for (const lang::MirBlock &block : entry->body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Load &&
            instruction.operands.size() == 1 &&
            instruction.operands.front().kind == lang::MirOperandKind::Copy &&
            instruction.operands.front().place == projection->id) {
          expect(load == nullptr,
                 "the projected field should have one scalar Load consumer");
          load = &instruction;
        }
      }
    }
  }

  expect(
      constructed != nullptr && constructor != nullptr && owner != nullptr &&
          constructed->info.category == lang::ValueCategory::Value &&
          construct->constructorKind == lang::ConstructorKind::Ordinary &&
          !construct->destination && !construct->receiver &&
          construct->localFailureSites.empty() &&
          construct->definedFailure.localOrigins.empty() &&
          construct->lifecycle.empty() && construct->fullExpressionEnd == 0 &&
          construct->cleanupBoundaryEnd == 0 && !construct->successResultDrop &&
          !construct->successResultDestination &&
          constructor->owner == owner->id &&
          owner->type == constructed->info.type &&
          constructor->parameterTypes == construct->parameterTypes &&
          construct->parameterTypes.size() == construct->operands.size() &&
          owner->destructorStatus == lang::SpecialMemberStatus::Generated &&
          !owner->destructor && !owner->requiresActiveDropState &&
          !owner->requiresActiveCleanup,
      "the construction should carry exact target/type/parameter metadata "
      "without semantic cleanup obligations");
  expect(uses.size() == 1 && projection != nullptr &&
             projection->root == lang::MirPlaceRootKind::Value &&
             projection->value == classResult &&
             projection->projections.size() == 1 &&
             projection->projections.front().kind ==
                 lang::MirProjectionKind::Field &&
             load != nullptr && load->result &&
             load->info.type == projection->type &&
             load->info.traits.drop == lang::DropKind::Trivial &&
             load->info.traits.copyable && load->lifecycle.empty() &&
             load->localFailureSites.empty() && load->definedFailure.empty(),
         "the construction result should have one value-rooted Field place "
         "and one exact trivial scalar copy Load");
  expect(std::none_of(entry->body.dropObligations.begin(),
                      entry->body.dropObligations.end(),
                      [&](const lang::MirDropObligation &drop) {
                        return drop.generatedValue == classResult ||
                               (projection != nullptr &&
                                drop.place == projection->id);
                      }),
         "the cleanup-free constructor value and field view should own no MIR "
         "drop obligations");

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = entry->id};
  expect(emitter.analyze(address).ready() &&
             emitter.supportsFailureBodyText(address),
         "the exact constructor field read should enter transformed MIR "
         "emission");
  if (!emitter.supportsFailureBodyText(address)) {
    return;
  }

  const std::string text =
      emitter.emitFailureBodyText(address, "constructor-field-read-v0", 1).text;
  const std::string slot = "__gti_mir_v_" + std::to_string(classResult);
  const auto count = [&](std::string_view needle) {
    std::size_t occurrences = 0;
    for (std::size_t at = text.find(needle); at != std::string::npos;
         at = text.find(needle, at + needle.size())) {
      ++occurrences;
    }
    return occurrences;
  };
  expect(text.find("mir_lifetime_slot<") != std::string::npos &&
             count("std::construct_at(" + slot + ".construction_address()") ==
                 1 &&
             count(slot + ".mark_constructed()") == 1 &&
             count(slot + ".get().") == 1 && count(slot + ".destroy()") == 0 &&
             count("std::move(" + slot + ".get())") == 0,
         "the constructor should publish once into its value slot and the "
         "field Load should read it once without an invented copy or drop");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(!artifact.contents.empty(),
         "the complete backend should emit the direct constructor field read");

  lang::MirProgram duplicateProjectionUse = frontend.mir;
  auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
      duplicateProjectionUse.functionInstances());
  lang::MirFunctionInstance *mutatedEntry = nullptr;
  for (lang::MirFunctionInstance &function : functions) {
    if (function.id == entry->id) {
      mutatedEntry = &function;
    }
  }
  bool replacedReturn = false;
  if (mutatedEntry != nullptr && projection != nullptr) {
    for (lang::MirBlock &block : mutatedEntry->body.blocks) {
      if (block.terminator.kind != lang::MirTerminatorKind::Return ||
          !block.terminator.value) {
        continue;
      }
      block.terminator.value =
          lang::MirOperand{.kind = lang::MirOperandKind::Copy,
                           .place = projection->id,
                           .type = projection->type};
      replacedReturn = true;
    }
    (void)lang::rebuildMirValueUses(mutatedEntry->body);
  }
  expect(replacedReturn &&
             lang::verifyMirProgram(duplicateProjectionUse).valid(),
         "the duplicate field consumer near miss should remain valid MIR");
  if (replacedReturn &&
      lang::verifyMirProgram(duplicateProjectionUse).valid()) {
    const lang::CppMirBodyEmissionMap duplicateMap(
        completeRows(duplicateProjectionUse));
    const lang::CppMirBodyEmitter duplicateEmitter(duplicateProjectionUse,
                                                   duplicateMap);
    expect(!duplicateEmitter.supportsFailureBodyText(address),
           "a constructor result field place with a second consumer should "
           "remain outside the slot representation");
  }

  const lang::FrontendResult nested =
      analyze("cpp-mir-constructor-temporary-nested-field.gti", R"(
struct inner_t { int32_t value = 7; };
class outer_t {
public:
  outer_t() {}
  inner_t inner = inner_t();
};
int32_t main() { return outer_t().inner.value - 7; }
)");
  expect(nested.canGenerateCode(),
         "the nested constructor field near miss should pass the frontend");
  if (nested.canGenerateCode()) {
    const auto nestedEntry = std::find_if(
        nested.mir.functionInstances().begin(),
        nested.mir.functionInstances().end(), [](const auto &function) {
          return function.entryKind == lang::ProgramEntryKind::NoArguments;
        });
    expect(nestedEntry != nested.mir.functionInstances().end(),
           "the nested constructor field near miss should lower its entry");
    if (nestedEntry != nested.mir.functionInstances().end()) {
      const lang::CppMirBodyEmissionMap nestedMap(
          buildRows(nested, nested.mir, lang::CppStandard::Cpp23));
      const lang::CppMirBodyEmitter nestedEmitter(nested.mir, nestedMap);
      expect(
          !nestedEmitter.supportsFailureBodyText(
              {.kind = lang::MirBodyKind::Function, .owner = nestedEntry->id}),
          "a two-field constructor projection should remain unsupported");
    }
  }

  const lang::FrontendResult cleanup =
      analyze("cpp-mir-constructor-temporary-cleanup-field.gti", R"(
class cleanup_t {
public:
  cleanup_t() {}
  ~cleanup_t() {}
  int32_t value = 7;
};
int32_t main() { return cleanup_t().value - 7; }
)");
  expect(cleanup.canGenerateCode(),
         "the cleanup-owning constructor field near miss should pass the "
         "frontend");
  if (cleanup.canGenerateCode()) {
    const auto cleanupEntry = std::find_if(
        cleanup.mir.functionInstances().begin(),
        cleanup.mir.functionInstances().end(), [](const auto &function) {
          return function.entryKind == lang::ProgramEntryKind::NoArguments;
        });
    expect(cleanupEntry != cleanup.mir.functionInstances().end(),
           "the cleanup-owning constructor near miss should lower its entry");
    if (cleanupEntry != cleanup.mir.functionInstances().end()) {
      const lang::CppMirBodyEmissionMap cleanupMap(
          buildRows(cleanup, cleanup.mir, lang::CppStandard::Cpp23));
      const lang::CppMirBodyEmitter cleanupEmitter(cleanup.mir, cleanupMap);
      expect(
          !cleanupEmitter.supportsFailureBodyText(
              {.kind = lang::MirBodyKind::Function, .owner = cleanupEntry->id}),
          "an explicit-destructor constructor temporary should remain "
          "outside the cleanup-free slot representation");
    }
  }
}

constexpr NamedTest kTests[] = {
    {"testOwningCheckedBodyEmitsWholeCleanupProof",
     testOwningCheckedBodyEmitsWholeCleanupProof},
    {"testRichFieldInitializerSchedule", testRichFieldInitializerSchedule},
    {"testDependentFieldInitializerSchedule",
     testDependentFieldInitializerSchedule},
    {"testContainedOwningFieldConstructor",
     testContainedOwningFieldConstructor},
    {"testContainedOwningStringFieldConstructor",
     testContainedOwningStringFieldConstructor},
    {"testBodyOrderedOwnedParameterConstructor",
     testBodyOrderedOwnedParameterConstructor},
    {"testPassiveOwnedParameterConstructorFields",
     testPassiveOwnedParameterConstructorFields},
    {"testMoveOnlyClassResultReceiverFieldTransfer",
     testMoveOnlyClassResultReceiverFieldTransfer},
    {"testMoveOnlyReceiverFieldLifecycleSchedules",
     testMoveOnlyReceiverFieldLifecycleSchedules},
    {"testNestedReceiverFieldProjection", testNestedReceiverFieldProjection},
    {"testContainedAbstractBaseConstructor",
     testContainedAbstractBaseConstructor},
    {"testCleanupFixtureFunctionBodiesAreReady",
     testCleanupFixtureFunctionBodiesAreReady},
    {"testOwnedLifecycleConstructionBodiesReady",
     testOwnedLifecycleConstructionBodiesReady},
    {"testDefaultConstructionPreparedParameterStaging",
     testDefaultConstructionPreparedParameterStaging},
    {"testReferenceReturnFailureAbi", testReferenceReturnFailureAbi},
    {"testGenericOwnerReferenceReturnFailureAbi",
     testGenericOwnerReferenceReturnFailureAbi},
    {"testStoredReferenceConstructorSchedule",
     testStoredReferenceConstructorSchedule},
    {"testGenericOwnerConstructorSpecialization",
     testGenericOwnerConstructorSpecialization},
    {"testConcreteGenericConstructorAndMemberPackPublication",
     testConcreteGenericConstructorAndMemberPackPublication},
    {"testPassiveConstructorFailureSummary",
     testPassiveConstructorFailureSummary},
    {"testPassiveFixedArrayFailureClosure",
     testPassiveFixedArrayFailureClosure},
    {"testOwningFixedArrayAggregateAndCheckedMove",
     testOwningFixedArrayAggregateAndCheckedMove},
    {"testPolymorphicClassArrayFieldLifecycleSchedule",
     testPolymorphicClassArrayFieldLifecycleSchedule},
    {"testPassiveClassArrayMoveAssignmentSchedule",
     testPassiveClassArrayMoveAssignmentSchedule},
    {"testCheckedClassArrayElementAssignmentFamily",
     testCheckedClassArrayElementAssignmentFamily},
    {"testGeneratedSpecialMemberConstructionSchedule",
     testGeneratedSpecialMemberConstructionSchedule},
    {"testConditionalClassReturnAndPreparedResultStage",
     testConditionalClassReturnAndPreparedResultStage},
    {"testConditionalFailureDestructorDrop",
     testConditionalFailureDestructorDrop},
    {"testVirtualFailureInterfaceFamily", testVirtualFailureInterfaceFamily},
    {"testConcreteOnlyGenericVirtualContractRoots",
     testConcreteOnlyGenericVirtualContractRoots},
    {"testAbstractBaseFailureFamilyAndInheritedMembers",
     testAbstractBaseFailureFamilyAndInheritedMembers},
    {"testUniqueOwnerUpcastFailureBody", testUniqueOwnerUpcastFailureBody},
    {"testExpectedLifetimeSlotObserverFusion",
     testExpectedLifetimeSlotObserverFusion},
    {"testExpectedOwningPayloadPublicationAndForwarding",
     testExpectedOwningPayloadPublicationAndForwarding},
    {"testCleanupOwningTemporaryArgumentTransfer",
     testCleanupOwningTemporaryArgumentTransfer},
    {"testConstructedArgumentTemporaryBorrow",
     testConstructedArgumentTemporaryBorrow},
    {"testFailureCapableClassReturnTemporaryReceiver",
     testFailureCapableClassReturnTemporaryReceiver},
    {"testFailureCapableClassReturnFieldRead",
     testFailureCapableClassReturnFieldRead},
    {"testCleanupFreeConstructorTemporaryFieldRead",
     testCleanupFreeConstructorTemporaryFieldRead},
};

} // namespace

int main(int argc, char **argv) {
  return runNamedTests(argc, argv, kTests, "lifecycle");
}
