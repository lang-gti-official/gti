#include "../src/compiler/mir_storage_effects.h"
#include "gti/frontend.h"
#include "gti/mir_editor.h"
#include "gti/mir_effects.h"
#include "gti/mir_lowering.h"
#include "gti/mir_verifier.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {
bool passed = true;
void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    passed = false;
  }
}

lang::FrontendResult fixture(std::string_view storage, bool cleanup) {
  const std::string declarations =
      std::string(storage) + " mut int32_t first = 1;\n" +
      std::string(storage) + " mut int32_t second = 2;\n";
  const std::string source = cleanup ? R"(
class Worker {
public:
  Worker() = default;
  ~Worker() { second = 9; }
};
int main() {
  int32_t& borrowed = first;
  { Worker task{}; }
  return borrowed;
}
)"
                                     : R"(
void change() { second = 9; }
void relay() { change(); }
int main() {
  int32_t& borrowed = first;
  relay();
  return borrowed;
}
)";
  return lang::Frontend().analyze("/tmp/gti-mir-persistent-storage/main.gti",
                                  declarations + source);
}

lang::SymbolId symbol(const lang::FrontendResult &frontend,
                      std::string_view name) {
  for (const auto &record : frontend.semantics.database().symbols())
    if (record.name == name)
      return record.id;
  return 0;
}

void independentHiddenEffects(std::string_view storage, bool cleanup) {
  const auto frontend = fixture(storage, cleanup);
  for (const auto &diagnostic : frontend.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(frontend.canGenerateCode(),
         "a live persistent borrow permits disjoint transitive mutation");
  if (!frontend.canGenerateCode())
    return;
  expect(lang::verifyMirProgram(frontend.mir).valid(),
         "unmodified disjoint persistent effects independently verify");
  const auto first = symbol(frontend, "first");
  const auto second = symbol(frontend, "second");
  expect(first != 0 && second != 0 && first != second,
         "fixture owns distinct structural storage identities");
  if (!first || !second)
    return;
  auto forged = frontend.mir;
  bool changed = false;
  for (const auto address : lang::enumerateMirBodyAddresses(forged)) {
    if (address.kind != lang::MirBodyKind::Function &&
        address.kind != lang::MirBodyKind::Destructor)
      continue;
    auto *body = lang::findMirBody(forged, address);
    bool mutated = false;
    for (auto &place : body->places)
      if (place.root == lang::MirPlaceRootKind::Symbol &&
          place.symbol == second) {
        place.symbol = first;
        if (place.key && place.key->root == second)
          place.key->root = first;
        changed = mutated = true;
      }
    for (auto &block : body->blocks)
      for (auto &instruction : block.instructions)
        if (instruction.kind == lang::MirInstructionKind::EnsureThreadLocal &&
            instruction.threadLocalStorage == second) {
          instruction.threadLocalStorage = first;
          instruction.bodyTarget = lang::MirBodyAddress{
              .kind = lang::MirBodyKind::ThreadLocalInitializer,
              .owner = first};
        }
    if (mutated)
      expect(lang::verifyMirBody(*body, address.owner).valid(),
             "callee mutation is body-local-valid; program proof must catch "
             "alias");
  }
  expect(changed, "mutation reaches executable callee storage places");
  const auto proof = lang::verifyMirProgram(forged);
  expect(!proof.valid(),
         "independent MIR proof rejects a substituted hidden transitive write");
  expect(std::any_of(proof.errors.begin(), proof.errors.end(),
                     [](const auto &error) {
                       return error.message.find(
                                  "ambient source call or cleanup") !=
                              std::string::npos;
                     }),
         "rejection comes from exact ambient access versus live loan proof");
}

void nonLexicalAndFieldBoundaries() {
  const auto frontend = lang::Frontend().analyze(
      "/tmp/gti-mir-persistent-storage/endpoints.gti", R"(
class Counters {
public:
  mut int32_t left = 1;
  mut int32_t right = 2;
};
thread_local mut Counters state{};
void change_right() { state.right = 3; }
void change_left() { state.left = 4; }
int main() {
  int32_t& borrowed = state.left;
  change_right();
  int32_t copied = borrowed;
  change_left();
  return copied;
}
)");
  for (const auto &diagnostic : frontend.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(frontend.canGenerateCode(),
         "field-disjoint calls and a later write after last use are accepted");
  if (frontend.canGenerateCode())
    expect(lang::verifyMirProgram(frontend.mir).valid(),
           "MIR hidden effects preserve field projections and EndBorrow");
}
void readySlotThroughNestedCalls() {
  const auto frontend =
      lang::Frontend().analyze("/tmp/gti-mir-persistent-storage/ready.gti", R"(
thread_local mut int32_t first = 1;
int32_t initialize() { first = 7; return 2; }
thread_local int32_t second = initialize();
int32_t read_second() { return second; }
int32_t relay() { return read_second(); }
int main() {
  int32_t& ready = second;
  int32_t& protected_first = first;
  int32_t copied = relay();
  return ready + protected_first + copied;
}
)");
  for (const auto &diagnostic : frontend.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(frontend.canGenerateCode(),
         "an existing exact TLS loan proves initialization completed through "
         "nested helpers");
  if (frontend.canGenerateCode())
    expect(lang::verifyMirProgram(frontend.mir).valid(),
           "MIR readiness retains the conditional initializer edge, not a "
           "blanket call exemption");
}
void stagedInputBoundaries(std::string_view storage, bool branch) {
  const std::string source = std::string(storage) + R"( mut int32_t first = 1;
)" + std::string(storage) + R"( mut int32_t second = 2;
int32_t change() { second = 9; return 3; }
int32_t read(int32_t& input, int32_t copied) { return input + copied; }
int main() { return read(first, )" +
                             (branch ? "true ? change() : 0" : "change()") +
                             R"(); }
)";
  const auto frontend = lang::Frontend().analyze(
      "/tmp/gti-mir-persistent-storage/staged.gti", source);
  for (const auto &diagnostic : frontend.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(frontend.canGenerateCode(),
         "later input may mutate storage disjoint from an earlier binding");
  if (!frontend.canGenerateCode())
    return;
  expect(lang::verifyMirProgram(frontend.mir).valid(),
         "independent staged-input proof accepts disjoint later arguments");
  auto forged = frontend.mir;
  const auto first = symbol(frontend, "first");
  const auto second = symbol(frontend, "second");
  bool changed = false;
  for (const auto address : lang::enumerateMirBodyAddresses(forged)) {
    auto *body = lang::findMirBody(forged, address);
    if (address.kind != lang::MirBodyKind::Function)
      continue;
    for (auto &place : body->places)
      if (place.root == lang::MirPlaceRootKind::Symbol &&
          place.symbol == second) {
        place.symbol = first;
        if (place.key && place.key->root == second)
          place.key->root = first;
        changed = true;
      }
    for (auto &block : body->blocks)
      for (auto &instruction : block.instructions)
        if (instruction.kind == lang::MirInstructionKind::EnsureThreadLocal &&
            instruction.threadLocalStorage == second) {
          instruction.threadLocalStorage = first;
          instruction.bodyTarget = lang::MirBodyAddress{
              .kind = lang::MirBodyKind::ThreadLocalInitializer,
              .owner = first};
        }
  }
  expect(changed, "staged-input fixture has an independently mutable callee");
  const auto proof = lang::verifyMirProgram(forged);
  expect(!proof.valid() &&
             std::any_of(proof.errors.begin(), proof.errors.end(),
                         [](const auto &error) {
                           return error.message.find("persistent call input") !=
                                  std::string::npos;
                         }),
         "independent proof rejects hidden invalidation after input binding");
}
void persistentTransferIsIndependentlyRejected() {
  const auto frontend =
      lang::Frontend().analyze("/tmp/gti-mir-persistent-storage/move.gti", R"(
thread_local mut int32_t value = 1;
int main() { return value; }
)");
  expect(frontend.canGenerateCode(), "persistent observation fixture is valid");
  if (!frontend.canGenerateCode())
    return;
  auto forged = frontend.mir;
  bool changed = false;
  for (const auto address : lang::enumerateMirBodyAddresses(forged)) {
    auto *body = lang::findMirBody(forged, address);
    if (address.kind != lang::MirBodyKind::Function)
      continue;
    for (auto &block : body->blocks)
      for (auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Load ||
            instruction.operands.size() != 1)
          continue;
        auto &operand = instruction.operands.front();
        const auto *place = body->findPlace(operand.place);
        if (operand.kind == lang::MirOperandKind::Copy && place &&
            place->root == lang::MirPlaceRootKind::Symbol &&
            place->symbol == symbol(frontend, "value")) {
          instruction.kind = lang::MirInstructionKind::Move;
          operand.kind = lang::MirOperandKind::Move;
          changed = true;
        }
      }
  }
  expect(changed, "malformed fixture changes an actual persistent load");
  expect(!lang::verifyMirProgram(forged).valid(),
         "program verification rejects the malformed persistent move");
  const auto proof = lang::verifyMirPersistentStorageEffects(forged);
  expect(!proof.valid() &&
             std::any_of(proof.errors.begin(), proof.errors.end(),
                         [](const auto &error) {
                           return error.message.find(
                                      "destructively transferred") !=
                                  std::string::npos;
                         }),
         "independent storage proof rejects an unrepresented destructive "
         "transfer");
}
void stagedBorrowMayPublishItsOriginalLifetime() {
  const auto frontend = lang::Frontend().analyze(
      "/tmp/gti-mir-persistent-storage/staged-return.gti", R"(
thread_local mut int32_t value = 1;
int32_t& retain(int32_t& input, int32_t copied) { return input; }
int main() { int32_t& result = retain(value, 0); return result; }
)");
  for (const auto &diagnostic : frontend.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(frontend.canGenerateCode(),
         "an ordered persistent input may publish its actual storage lifetime");
  if (frontend.canGenerateCode())
    expect(lang::verifyMirProgram(frontend.mir).valid(),
           "input protection does not invent a shorter-lived result parent");
}
void nativeBoundaryIsNotAnUnknownSourceBody() {
  const auto frontend = lang::Frontend().analyze(
      "/tmp/gti-mir-persistent-storage/native-input.gti", R"(
extern "C" { int32_t number(); }
thread_local mut int32_t value = 1;
void write(mut int32_t& target, int32_t copied) { target = copied; }
int main() { write(value, number()); return value; }
)");
  for (const auto &diagnostic : frontend.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(frontend.canGenerateCode(),
         "declared native inputs retain their foreign-boundary obligations");
  if (!frontend.canGenerateCode())
    return;
  expect(lang::verifyMirProgram(frontend.mir).valid(),
         "a scalar native input is not an arbitrary hidden GTI storage write");
  auto forged = frontend.mir;
  bool changed = false;
  for (const auto &instance : forged.functionInstances()) {
    auto &function = const_cast<lang::MirFunctionInstance &>(instance);
    if (function.linkage == lang::LanguageLinkage::C &&
        function.definitionKind != lang::MirDefinitionKind::Source) {
      function.linkage = lang::LanguageLinkage::Gti;
      function.externalSymbol.clear();
      changed = true;
    }
  }
  expect(changed, "fixture owns a declared native target");
  const auto proof = lang::verifyMirPersistentStorageEffects(forged);
  expect(!proof.valid(),
         "removing the native boundary cannot authorize an unknown GTI effect");
}
void exactRestorationRemainsAvailable() {
  const auto library =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const auto frontend = lang::Frontend().analyze(
      "/tmp/gti-mir-persistent-storage/restore.gti", R"(
#include <std/utility>
thread_local mut int value = 1;
int main() { int previous = std::exchange(value, 2); return previous + value; }
)",
      {library / "prelude.gti"}, {}, {library});
  for (const auto &diagnostic : frontend.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(frontend.canGenerateCode(),
         "persistent storage may use the existing exact restoration operation");
  if (frontend.canGenerateCode())
    expect(lang::verifyMirProgram(frontend.mir).valid(),
           "restoration proof is not confused with an unrepresented raw Move");
}

void generatedDefaultHasExactEmptyTargets(std::string_view storage) {
  const auto frontend = lang::Frontend().analyze(
      "/tmp/gti-mir-persistent-storage/generated-default.gti",
      std::string(storage) + " mut int32_t first = 1;\n" +
          std::string(storage) + " mut int32_t second = 2;\n" + R"(
struct Cleanup { ~Cleanup() { second = 9; } };
void inspect(Cleanup& input) {}
void execute() { inspect(Cleanup()); }
int main() { int32_t& borrowed = first; execute(); return borrowed; }
)");
  for (const auto &diagnostic : frontend.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(frontend.canGenerateCode(),
         "generated default construction does not invent unrelated persistent "
         "writes while exact destructor effects remain disjoint");
  if (!frontend.canGenerateCode())
    return;

  std::optional<lang::MirBodyAddress> producerBody;
  lang::MirInstructionId producerId = 0;
  for (const auto address : lang::enumerateMirBodyAddresses(frontend.mir)) {
    const auto *body = lang::findMirBody(frontend.mir, address);
    for (const auto &block : body->blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.kind == lang::MirInstructionKind::Call &&
            instruction.info.type.kind == lang::SemanticType::Class &&
            !instruction.functionTarget && !instruction.constructorTarget &&
            instruction.intrinsic == lang::IntrinsicKind::None) {
          producerBody = address;
          producerId = instruction.id;
          const auto targets =
              lang::mirInvocationTargets(frontend.mir, *body, instruction);
          expect(!targets.boundary && targets.addresses.empty(),
                 "an independently typed generated default has no hidden "
                 "source invocation");
        }
  }
  expect(producerBody.has_value(),
         "fixture reaches a targetless generated default-construction call");
  if (!producerBody)
    return;

  for (const bool scheduled : {false, true}) {
    auto forged = frontend.mir;
    const auto *body = lang::findMirBody(forged, *producerBody);
    const lang::MirInstruction *producer = nullptr;
    for (const auto &block : body->blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.id == producerId)
          producer = &instruction;
    expect(producer != nullptr, "the exact generated producer is preserved");
    if (producer == nullptr)
      continue;
    const auto owner =
        std::find_if(forged.classInstances().begin(),
                     forged.classInstances().end(), [&](const auto &candidate) {
                       return candidate.type == producer->info.type;
                     });
    expect(owner != forged.classInstances().end(),
           "generated producer has an exact concrete owner");
    if (owner == forged.classInstances().end())
      continue;
    auto &changed = const_cast<lang::MirClassInstance &>(*owner);
    if (scheduled)
      changed.generatedDefaultRequiresSchedule = true;
    else
      changed.defaultConstructor = lang::SpecialMemberStatus::Deleted;
    const auto targets = lang::mirInvocationTargets(forged, *body, *producer);
    expect(targets.boundary,
           "missing generated status or an omitted concrete schedule cannot "
           "claim an empty hidden target set");
    expect(!lang::verifyMirPersistentStorageEffects(forged).valid(),
           "forged default-construction authority cannot suppress a hidden "
           "persistent conflict");
  }
}

void generatedDefaultSchedulesRetainEffects(std::string_view storage) {
  for (const auto shape : {"struct Outer { int32_t field = initialize(); };",
                           "struct Member { Member() { mutate(); } }; "
                           "struct Outer { Member member; };",
                           "struct Base { int32_t field = initialize(); }; "
                           "struct Outer : public Base {};"}) {
    const auto source = [&](std::string_view target) {
      return std::string(storage) + " mut int32_t first = 1;\n" +
             std::string(storage) + " mut int32_t second = 2;\n" +
             "void mutate() { " + std::string(target) + " = 9; } " +
             "int32_t initialize() { mutate(); return 0; } " + shape + R"(
void inspect(Outer& input) {}
void execute() { inspect(Outer()); }
int main() { int32_t& borrowed = first; execute(); return borrowed; }
)";
    };
    const auto allowed = lang::Frontend().analyze(
        "/tmp/gti-mir-persistent-storage/generated-schedule.gti",
        source("second"));
    for (const auto &diagnostic : allowed.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    expect(allowed.canGenerateCode(),
           "effectful explicit/implicit field and inherited-base schedules "
           "preserve disjoint persistent access");
    bool exactSchedule = false;
    for (const auto &constructor : allowed.mir.constructorInstances()) {
      if (!constructor.generatedDefault)
        continue;
      const auto *owner = allowed.mir.findClassInstance(constructor.owner);
      exactSchedule |= owner && owner->generatedDefaultRequiresSchedule;
    }
    expect(exactSchedule,
           "generated field/base work is represented by a required concrete "
           "constructor schedule, not a targetless default");
    const auto rejected = lang::Frontend().analyze(
        "/tmp/gti-mir-persistent-storage/generated-schedule-alias.gti",
        source("first"));
    const auto hasCode = [&](std::string_view code) {
      return std::any_of(
          rejected.diagnostics.begin(), rejected.diagnostics.end(),
          [&](const auto &diagnostic) { return diagnostic.code == code; });
    };
    expect(!rejected.semanticValid && hasCode("GTI-S2045") &&
               !hasCode("GTI-B0001"),
           "an effectful generated field/base schedule cannot mutate a live "
           "persistent loan and is rejected before MIR");
  }
}
void referenceRootDoesNotAcquireExactVirtualType(bool localAlias) {
  const std::string source =
      R"(
class Source {
public:
  virtual int32_t read() { return 1; }
};
class Reader : public Source {
public:
  int32_t read() override { return 3; }
};
Source& relay(Source& input) { return input; }
int32_t owned_copy(Source& input) {
  Source copy = relay(input);
  return copy.read();
}
)" +
      std::string(
          localAlias
              ? R"(int32_t inspect(Source& input) {
  Source& alias = relay(input);
  return alias.read();
}
)"
              : "int32_t inspect(Source& input) { return input.read(); }\n") +
      "int main() { Reader local{}; return inspect(local) + owned_copy(local); "
      "}\n";
  const auto frontend = lang::Frontend().analyze(
      "/tmp/gti-mir-persistent-storage/reference-virtual.gti", source);
  for (const auto &diagnostic : frontend.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(frontend.canGenerateCode(), "dynamic reference dispatch is valid "
                                     "without a conflicting persistent loan");
  if (!frontend.canGenerateCode())
    return;
  std::optional<lang::MirBodyAddress> caller;
  lang::MirInstructionId callId = 0;
  for (const auto address : lang::enumerateMirBodyAddresses(frontend.mir))
    for (const auto &block : lang::findMirBody(frontend.mir, address)->blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.kind == lang::MirInstructionKind::Call &&
            instruction.dispatch == lang::CallDispatch::Virtual) {
          const auto *body = lang::findMirBody(frontend.mir, address);
          const auto *value = instruction.receiver
                                  ? body->findValue(instruction.receiver->value)
                                  : nullptr;
          const auto *stageBlock =
              value ? body->findBlock(value->definitionBlock) : nullptr;
          bool owned = false;
          if (stageBlock)
            for (const auto &stage : stageBlock->instructions)
              if (stage.id == value->definition && stage.operands.size() == 1) {
                const auto *place =
                    body->findPlace(stage.operands.front().place);
                owned = place &&
                        place->root == lang::MirPlaceRootKind::Binding &&
                        place->projections.empty() &&
                        place->type.kind == lang::SemanticType::Class;
              }
          const auto targets =
              lang::mirInvocationTargets(frontend.mir, *body, instruction);
          if (owned) {
            expect(!targets.boundary && targets.addresses.size() == 1,
                   "explicit Load/Copy publishes an exact owning class from a "
                   "reference");
            continue;
          }
          expect(targets.boundary,
                 "a base reference is not its declared class's exact object");
          caller = address;
          callId = instruction.id;
        }
  expect(caller && callId, "fixture owns one dynamic reference receiver");
  if (!caller || !callId)
    return;
  for (int mutation = 0; mutation != (localAlias ? 3 : 2); ++mutation) {
    auto forged = frontend.mir;
    auto *body = lang::findMirBody(forged, *caller);
    lang::MirInstruction *call = nullptr;
    for (auto &block : body->blocks)
      for (auto &instruction : block.instructions)
        if (instruction.id == callId)
          call = &instruction;
    const auto *value = call && call->receiver
                            ? body->findValue(call->receiver->value)
                            : nullptr;
    auto *block = value ? body->findBlock(value->definitionBlock) : nullptr;
    lang::MirInstruction *stage = nullptr;
    if (block)
      for (auto &instruction : block->instructions)
        if (instruction.id == value->definition)
          stage = &instruction;
    if (!stage || stage->operands.size() != 1) {
      expect(false, "reference fixture has one exact receiver stage");
      continue;
    }
    auto &place = body->places.at(stage->operands.front().place - 1);
    expect(place.root == lang::MirPlaceRootKind::Binding &&
               place.projections.size() == 1 &&
               place.projections.front().kind ==
                   lang::MirProjectionKind::Dereference,
           "reference receiver retains its physical dereference");
    place.projections.clear();
    if (mutation != 0)
      for (auto &view : body->places)
        if (view.root == lang::MirPlaceRootKind::Binding &&
            view.binding == place.binding && view.projections.empty())
          view.type = place.type;
    if (mutation == 2) {
      // A borrowed reference-returning call has an SSA Value too. Omitting
      // the source compiler's explicit Load/Copy must not publish an object.
      lang::MirValueId borrowedValue = 0;
      for (const auto &candidate : body->values)
        if (candidate.info.type == place.type &&
            candidate.info.category == lang::ValueCategory::Place)
          borrowedValue = candidate.id;
      bool replaced = false;
      for (auto &candidateBlock : body->blocks)
        for (auto &instruction : candidateBlock.instructions) {
          if (instruction.kind != lang::MirInstructionKind::Initialize ||
              !instruction.destination)
            continue;
          const auto *destination = body->findPlace(*instruction.destination);
          if (!destination ||
              destination->root != lang::MirPlaceRootKind::Binding ||
              destination->binding != place.binding ||
              !destination->projections.empty())
            continue;
          expect(instruction.operands.size() == 1 &&
                     instruction.operands.front().kind ==
                         lang::MirOperandKind::Loan,
                 "reference alias originally publishes one exact Loan address");
          instruction.operands = {{.kind = lang::MirOperandKind::Value,
                                   .value = borrowedValue,
                                   .type = place.type}};
          replaced = true;
        }
      expect(borrowedValue != 0 && replaced,
             "local alias control has an actual borrowed SSA call result");
    }
    expect(lang::mirInvocationTargets(forged, *body, *call).boundary,
           "stripping reference indirection cannot manufacture exact dynamic "
           "type");
    expect(!lang::verifyMirProgram(forged).valid(),
           "declared formal or typed local publication rejects a forged class "
           "root (local=" +
               std::to_string(localAlias) +
               ", mutation=" + std::to_string(mutation) + ")");
  }
}

void ordinaryBindingsRetainPhysicalStorageTypes() {
  const auto frontend = lang::Frontend().analyze(
      "/tmp/gti-mir-persistent-storage/ordinary-bindings.gti", R"(
class Sample {
public:
  int32_t member = 3;
};
Sample& identity(Sample& input) { return input; }
int32_t read(Sample& input) {
  Sample& alias = identity(input);
  Sample copy = alias;
  return copy.member;
}
int main() { Sample sample{}; return read(sample); }
)");
  for (const auto &diagnostic : frontend.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(frontend.canGenerateCode(),
         "ordinary reference aliases and explicit owning copies remain valid");
  if (!frontend.canGenerateCode())
    return;
  std::optional<lang::MirBodyAddress> caller;
  lang::MirPlaceId referent = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    const auto *declaration =
        frontend.semantics.findFunction(function.declaration);
    if (!declaration || !declaration->declaration ||
        declaration->declaration->name().lexeme != "read")
      continue;
    for (const auto &place : function.body.places)
      if (place.root == lang::MirPlaceRootKind::Binding &&
          place.type.kind == lang::SemanticType::Class &&
          place.projections.size() == 1 &&
          place.projections.front().kind ==
              lang::MirProjectionKind::Dereference &&
          std::find(function.parameterBindings.begin(),
                    function.parameterBindings.end(),
                    place.binding) == function.parameterBindings.end()) {
        caller = lang::MirBodyAddress{.kind = lang::MirBodyKind::Function,
                                      .owner = function.id};
        referent = place.id;
        break;
      }
  }
  expect(caller && referent, "nonvirtual local reference has an explicit view");
  if (!caller || !referent)
    return;
  for (const bool retagStorage : {false, true}) {
    auto forged = frontend.mir;
    auto *body = lang::findMirBody(forged, *caller);
    auto &view = body->places.at(referent - 1);
    view.projections.clear();
    if (retagStorage)
      for (auto &place : body->places)
        if (place.root == lang::MirPlaceRootKind::Binding &&
            place.binding == view.binding && place.projections.empty())
          place.type = view.type;
    const auto bodyProof = lang::verifyMirBody(*body, caller->owner);
    expect(!bodyProof.valid() && !lang::verifyMirProgram(forged).valid(),
           "ordinary reference storage cannot become an owned object by "
           "stripping or retagging its view");
    if (!retagStorage)
      expect(std::any_of(bodyProof.errors.begin(), bodyProof.errors.end(),
                         [](const auto &error) {
                           return error.message.find("stored type") !=
                                  std::string::npos;
                         }),
             "reference view rejection owns the physical stored-type rule");
  }
}

void exactWholeObjectVirtualEffects(std::string_view storage) {
  const std::string source = std::string(storage) + R"( mut int32_t first = 1;
)" + std::string(storage) + R"( mut int32_t second = 2;
class Source {
public:
  virtual int32_t read() { return 1; }
  virtual int32_t other() { return 0; }
};
class Reader : public Source {
public:
  int32_t read() override { second = 9; return 3; }
  int32_t other() override { return 0; }
};
class Sibling : public Source {
public:
  int32_t read() override { return 4; }
};
Reader reader{};
Sibling sibling{};
int32_t read_owned(Reader input) {
  Reader local{};
  return input.read() + local.read();
}
class UsesDefault {
  int32_t kept = reader.read();
public:
  UsesDefault() {}
  int32_t read() { return this.kept; }
};
int main() {
  int32_t& held = first;
  UsesDefault selected{};
  return held + selected.read() + read_owned(reader);
}
)";
  const auto frontend = lang::Frontend().analyze(
      "/tmp/gti-mir-persistent-storage/exact-virtual.gti", source);
  for (const auto &diagnostic : frontend.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(frontend.canGenerateCode(),
         "whole-object virtual declaration defaults preserve disjoint effects");
  if (!frontend.canGenerateCode()) {
    std::cerr << "Exact virtual fixture storage=" << storage
              << " first=" << symbol(frontend, "first")
              << " second=" << symbol(frontend, "second")
              << " reader=" << symbol(frontend, "reader") << '\n';
    for (const auto address : lang::enumerateMirBodyAddresses(frontend.mir)) {
      const auto &body = *lang::findMirBody(frontend.mir, address);
      for (const auto &block : body.blocks)
        for (const auto &instruction : block.instructions) {
          const auto targets =
              lang::mirInvocationTargets(frontend.mir, body, instruction);
          if (instruction.kind != lang::MirInstructionKind::Call &&
              instruction.kind != lang::MirInstructionKind::Construct &&
              instruction.kind != lang::MirInstructionKind::CallBody &&
              instruction.kind != lang::MirInstructionKind::EnsureThreadLocal)
            continue;
          std::cerr << "body " << static_cast<int>(address.kind) << ':'
                    << address.owner << " instruction " << instruction.id
                    << " kind=" << static_cast<int>(instruction.kind)
                    << " boundary=" << targets.boundary << " targets=";
          for (const auto target : targets.addresses)
            std::cerr << static_cast<int>(target.kind) << ':' << target.owner
                      << ',';
          std::cerr << '\n';
          if (!targets.boundary || !instruction.receiver)
            continue;
          const auto *value = body.findValue(instruction.receiver->value);
          const auto *stageBlock =
              value ? body.findBlock(value->definitionBlock) : nullptr;
          if (!stageBlock)
            continue;
          for (const auto &stage : stageBlock->instructions) {
            if (stage.id != value->definition || stage.operands.size() != 1)
              continue;
            const auto *place = body.findPlace(stage.operands.front().place);
            if (!place)
              continue;
            std::cerr << " receiver root=" << static_cast<int>(place->root)
                      << " binding=" << place->binding
                      << " symbol=" << place->symbol
                      << " type=" << static_cast<int>(place->type.kind) << ':'
                      << place->type.classId
                      << " projections=" << place->projections.size()
                      << " capture=" << place->capture
                      << " foreign=" << place->foreignStorage
                      << " stage=" << static_cast<int>(stage.callInputKind)
                      << " function=" << instruction.functionTarget.value_or(0)
                      << '\n';
            const auto *step =
                frontend.mir.programInitializationPlan().findStepForSymbol(
                    place->symbol);
            if (!step)
              continue;
            const auto *stored =
                frontend.mir.module().findPlace(step->storagePlace);
            std::cerr << " storage step=" << step->id
                      << " binding=" << step->binding
                      << " place=" << step->storagePlace;
            if (stored)
              std::cerr << " root=" << static_cast<int>(stored->root)
                        << " binding=" << stored->binding
                        << " symbol=" << stored->symbol
                        << " type=" << static_cast<int>(stored->type.kind)
                        << ':' << stored->type.classId
                        << " projections=" << stored->projections.size();
            std::cerr << '\n';
          }
        }
    }
    return;
  }
  expect(
      lang::verifyMirProgram(frontend.mir).valid(),
      "whole-object virtual effects independently verify before optimization");
  std::optional<lang::MirBodyAddress> caller;
  lang::MirInstructionId callId = 0;
  for (const auto address : lang::enumerateMirBodyAddresses(frontend.mir))
    for (const auto &block : lang::findMirBody(frontend.mir, address)->blocks)
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Call ||
            instruction.dispatch != lang::CallDispatch::Virtual)
          continue;
        const auto targets = lang::mirInvocationTargets(
            frontend.mir, *lang::findMirBody(frontend.mir, address),
            instruction);
        expect(
            !targets.boundary && targets.addresses.size() == 1 &&
                targets.addresses.front().kind == lang::MirBodyKind::Function &&
                instruction.functionTarget &&
                targets.addresses.front().owner == *instruction.functionTarget,
            "exact virtual effect edge preserves selected concrete identity");
        expect(instruction.definedFailure.propagation ==
                   lang::FailurePropagationKind::VirtualCall,
               "effect proof does not erase dynamic failure propagation");
        const auto *value = instruction.receiver
                                ? lang::findMirBody(frontend.mir, address)
                                      ->findValue(instruction.receiver->value)
                                : nullptr;
        const auto *stageBlock = value
                                     ? lang::findMirBody(frontend.mir, address)
                                           ->findBlock(value->definitionBlock)
                                     : nullptr;
        if (stageBlock)
          for (const auto &stage : stageBlock->instructions) {
            if (stage.id != value->definition || stage.operands.size() != 1)
              continue;
            const auto *place = lang::findMirBody(frontend.mir, address)
                                    ->findPlace(stage.operands.front().place);
            if (place && place->root == lang::MirPlaceRootKind::Symbol &&
                place->symbol == symbol(frontend, "reader")) {
              caller = address;
              callId = instruction.id;
            }
          }
      }
  expect(caller && callId != 0,
         "fixture contains the constructor-default virtual call");
  if (!caller || callId == 0)
    return;

  const auto first = symbol(frontend, "first");
  const auto second = symbol(frontend, "second");
  auto conflicting = frontend.mir;
  bool changed = false;
  for (const auto address : lang::enumerateMirBodyAddresses(conflicting)) {
    if (address.kind != lang::MirBodyKind::Function)
      continue;
    auto *body = lang::findMirBody(conflicting, address);
    for (auto &place : body->places)
      if (place.root == lang::MirPlaceRootKind::Symbol &&
          place.symbol == second) {
        place.symbol = first;
        if (place.key && place.key->root == second)
          place.key->root = first;
        changed = true;
      }
    for (auto &block : body->blocks)
      for (auto &instruction : block.instructions)
        if (instruction.kind == lang::MirInstructionKind::EnsureThreadLocal &&
            instruction.threadLocalStorage == second) {
          instruction.threadLocalStorage = first;
          instruction.bodyTarget = lang::MirBodyAddress{
              .kind = lang::MirBodyKind::ThreadLocalInitializer,
              .owner = first};
        }
  }
  expect(changed, "virtual override contains an actual persistent write");
  const auto conflict = lang::verifyMirProgram(conflicting);
  expect(!conflict.valid() &&
             std::any_of(conflict.errors.begin(), conflict.errors.end(),
                         [](const auto &error) {
                           return error.message.find(
                                      "ambient source call or cleanup") !=
                                  std::string::npos;
                         }),
         "exact readonly virtual call retains conflicting transitive writes");

  for (int mutation = 0; mutation != 14; ++mutation) {
    auto forged = frontend.mir;
    auto *body = lang::findMirBody(forged, *caller);
    lang::MirInstruction *call = nullptr;
    for (auto &block : body->blocks)
      for (auto &instruction : block.instructions)
        if (instruction.id == callId)
          call = &instruction;
    if (!call || !call->receiver || !call->functionTarget) {
      expect(false, "virtual mutation has exact fixture identities");
      continue;
    }
    const auto *value = body->findValue(call->receiver->value);
    auto *block = value ? body->findBlock(value->definitionBlock) : nullptr;
    lang::MirInstruction *stage = nullptr;
    if (block)
      for (auto &input : block->instructions)
        if (input.id == value->definition)
          stage = &input;
    if (!stage || stage->operands.size() != 1) {
      expect(false, "virtual fixture contains one staged receiver borrow");
      continue;
    }
    auto &place = body->places.at(stage->operands.front().place - 1);
    auto *function = const_cast<lang::MirFunctionInstance *>(
        forged.findFunctionInstance(*call->functionTarget));
    if (!function) {
      expect(false, "virtual fixture retains the selected function instance");
      continue;
    }
    switch (mutation) {
    case 0:
      ++stage->callSite;
      break;
    case 1:
      stage->callInputRole = lang::MirCallInputRole::Argument;
      break;
    case 2:
      place.capture = 1;
      break;
    case 3:
      place.projections.push_back(
          {.kind = lang::MirProjectionKind::Dereference});
      break;
    case 4:
      function->virtualRoots.clear();
      break;
    case 5:
      function->definitionKind = lang::MirDefinitionKind::Declaration;
      break;
    case 6: {
      // Coherently retarget the declaration and dispatch owner to the base.
      // The actual Reader still has its writing override: a harmless base
      // summary cannot stand in for that dynamic effect.
      const auto root = function->virtualRoots.front();
      bool substituted = false;
      for (const auto &candidate : forged.functionInstances())
        if (candidate.declaration == root && candidate.owner &&
            !candidate.overrideMethod) {
          call->functionTarget = candidate.id;
          call->dispatchOwner =
              forged.findClassInstance(*candidate.owner)->type;
          substituted = true;
          break;
        }
      expect(substituted, "fixture exposes a coherent base declaration");
      break;
    }
    case 7: {
      bool substituted = false;
      for (const auto &candidate : forged.functionInstances())
        if (candidate.owner && candidate.owner != function->owner &&
            candidate.overrideMethod &&
            candidate.virtualRoots == function->virtualRoots) {
          call->functionTarget = candidate.id;
          call->dispatchOwner =
              forged.findClassInstance(*candidate.owner)->type;
          substituted = true;
          break;
        }
      expect(substituted, "fixture exposes a sibling's harmless override");
      break;
    }
    case 8: {
      bool substituted = false;
      for (const auto &candidate : forged.functionInstances())
        if (candidate.id != function->id &&
            candidate.owner == function->owner && candidate.virtualMethod &&
            candidate.virtualRoots != function->virtualRoots) {
          function->virtualRoots = candidate.virtualRoots;
          substituted = true;
          break;
        }
      expect(substituted, "fixture exposes an unrelated virtual slot");
      break;
    }
    case 9: {
      const auto root = function->virtualRoots.front();
      bool substituted = false;
      for (const auto &candidate : forged.functionInstances())
        if (candidate.declaration == root && candidate.owner &&
            !candidate.overrideMethod) {
          const auto type = forged.findClassInstance(*candidate.owner)->type;
          place.type = type;
          stage->info.type = type;
          stage->operands.front().type = type;
          call->receiver->type = type;
          body->values.at(call->receiver->value - 1).info.type = type;
          call->functionTarget = candidate.id;
          call->dispatchOwner = type;
          substituted = true;
          break;
        }
      expect(substituted,
             "consumer type can be forged independently of storage");
      break;
    }
    case 10:
      place.root = lang::MirPlaceRootKind::This;
      place.symbol = 0;
      place.binding = 0;
      break;
    case 11:
      place.foreignStorage = true;
      break;
    case 12:
      stage->result.reset();
      break;
    case 13: {
      auto &plan = const_cast<lang::MirProgramInitializationPlan &>(
          forged.programInitializationPlan());
      bool removed = false;
      for (auto &step : plan.steps)
        if (step.symbol == place.symbol) {
          step.symbol = 0;
          removed = true;
        }
      expect(removed, "receiver has an authoritative storage declaration");
      break;
    }
    }
    expect(
        lang::mirInvocationTargets(forged, *body, *call).boundary,
        "incoherent/open virtual evidence cannot grant an exact effect edge");
    expect(!lang::verifyMirProgram(forged).valid(),
           "independent program verification rejects forged virtual evidence");
  }
}

void foldHirProof() {
  const auto frontend = lang::Frontend().analyze(
      "/tmp/gti-mir-persistent-storage/fold-hir.gti", R"(
void touch<T>(mut int32_t& count, T& value) { count += 1; }
void folded<Args...>(mut int32_t& count, Args... args) {
  (touch(count, args), ...);
  (touch(count, args), ...);
}
int main() { mut int32_t count = 0; folded(count, 1, 2); return count - 4; }
)");
  expect(frontend.canGenerateCode(),
         "two unrelated user folds have ordinary concrete call plans");
  if (!frontend.canGenerateCode())
    return;
  lang::HirFunctionInstanceId owner = 0;
  std::vector<lang::HirValueId> folds;
  for (const auto &function : frontend.hir.functionInstances())
    for (const auto &value : function.body.values)
      if (value.kind == lang::HirValueKind::PackFold) {
        owner = function.id;
        folds.push_back(value.id);
      }
  expect(owner && folds.size() == 2,
         "typed expansion fixture has two exact fold owners");
  if (!owner || folds.size() != 2)
    return;
  for (int mutation = 0; mutation != 14; ++mutation) {
    auto forged = frontend.hir;
    auto &body =
        const_cast<lang::HirBody &>(forged.findFunctionInstance(owner)->body);
    auto *fold = const_cast<lang::HirValue *>(body.findValue(folds.front()));
    auto *call = const_cast<lang::HirValue *>(
        body.findValue(fold->packFoldElements.front().call));
    auto *prefix =
        const_cast<lang::HirValue *>(body.findValue(call->operands.front()));
    auto *element =
        const_cast<lang::HirValue *>(body.findValue(call->operands.back()));
    switch (mutation) {
    case 0:
      fold->packFoldElements.front().call = 0;
      break;
    case 1:
      fold->packFoldElements.front().call =
          body.findValue(folds.back())->packFoldElements.front().call;
      fold->operands[fold->packFoldArgument + 1] =
          fold->packFoldElements.front().call;
      break;
    case 2:
      fold->packFoldElements.back().call = fold->packFoldElements.front().call;
      fold->operands.back() = fold->packFoldElements.front().call;
      break;
    case 3:
      std::swap(fold->packFoldElements.front().call,
                fold->packFoldElements.back().call);
      std::swap(fold->operands[fold->packFoldArgument + 1],
                fold->operands.back());
      break;
    case 4:
      ++prefix->symbol;
      break;
    case 5:
      prefix->info.type = lang::SemanticType::Bool;
      break;
    case 6:
      call->callPlan->arguments.front().kind = lang::HirCallInputKind::Value;
      break;
    case 7:
      element->packElement = 2;
      break;
    case 8:
      element->info.type = lang::SemanticType::Bool;
      break;
    case 9:
      element->symbol = 0;
      break;
    case 10:
      element->operands.front() = fold->operands.front();
      break;
    case 11:
      element->info.access = lang::AccessMode::Mutable;
      break;
    case 12:
      element->info.traits.copyable = !element->info.traits.copyable;
      break;
    case 13:
      call->callPlan->arguments.front().kind =
          lang::HirCallInputKind::ReadBorrow;
      break;
    }
    if (mutation == 12) {
      expect(
          !lang::MirLowerer().lower(forged, frontend.failureMetadata).valid(),
          "pack-element traits must match independently resolved MIR types");
    } else {
      expect(!lang::verifyHirProgramPlans(frontend.semantics, forged).valid(),
             "HIR fold rejects a missing, substituted, duplicated, reordered "
             "or untyped expansion component");
    }
  }
}

void referenceInputPhysicalType(bool mutableInput) {
  const std::string source =
      std::string("void observe(") + (mutableInput ? "mut " : "") +
      "uint16_t& value) {}\n"
      "int main() {\n"
      "  mut uint16_t value = uint16_t(2); value += uint16_t(1);\n"
      "  uint16_t readonly = uint16_t(3);\n"
      "  int8_t different = int8_t(1);\n"
      "  observe(value); return int32_t(different) - 1;\n"
      "}\n";
  const auto frontend = lang::Frontend().analyze(
      "/tmp/gti-mir-persistent-storage/reference-pointee.gti", source);
  expect(frontend.canGenerateCode(),
         "ordinary shared and mutable scalar reference inputs verify");
  if (!frontend.canGenerateCode())
    return;
  for (const auto &function : frontend.mir.functionInstances()) {
    const auto *info = frontend.semantics.findFunction(function.declaration);
    if (!info || !info->declaration ||
        info->declaration->name().lexeme != "main")
      continue;
    for (int mutation = 0; mutation != (mutableInput ? 2 : 1); ++mutation) {
      auto forged = frontend.mir;
      auto *body = lang::findMirBody(
          forged, {.kind = lang::MirBodyKind::Function, .owner = function.id});
      lang::MirPlaceId replacement = 0;
      for (const auto &place : body->places)
        if (place.root == lang::MirPlaceRootKind::Binding &&
            place.projections.empty() &&
            (mutation == 0 ? place.type == lang::SemanticType::Int8
                           : place.type == lang::SemanticType::UInt16 &&
                                 place.access == lang::AccessMode::ReadOnly))
          replacement = place.id;
      bool changed = false;
      for (auto &block : body->blocks)
        for (auto &instruction : block.instructions)
          if (instruction.kind == lang::MirInstructionKind::CallInput &&
              instruction.callInputRole == lang::MirCallInputRole::Argument &&
              instruction.info.type.kind == lang::SemanticType::Reference &&
              instruction.info.type.arguments ==
                  std::vector<lang::SemanticType>{lang::SemanticType::UInt16}) {
            instruction.operands.front().place = replacement;
            changed = true;
          }
      expect(replacement && changed && !lang::verifyMirProgram(forged).valid(),
             "ordinary staged references reject a wrong physical pointee or "
             "read-only source for a mutable parameter");
    }
  }
}

void referenceInputBaseConversion() {
  const auto frontend = lang::Frontend().analyze(
      "/tmp/gti-mir-persistent-storage/reference-base.gti", R"(
class Base { public: Base() {} };
class Derived : public Base { public: Derived() : Base() {} };
class Unrelated { public: Unrelated() {} };
void observe(Base& value) {}
void change(mut Base& value) {}
int main() {
  mut Derived owner{};
  mut Unrelated unrelated{};
  observe(owner); change(owner); return 0;
}
)");
  expect(frontend.canGenerateCode(),
         "shared and mutable staged class references preserve verified public "
         "base conversion");
  if (!frontend.canGenerateCode())
    return;
  for (const auto &function : frontend.mir.functionInstances()) {
    const auto *info = frontend.semantics.findFunction(function.declaration);
    if (!info || !info->declaration ||
        info->declaration->name().lexeme != "main")
      continue;
    auto forged = frontend.mir;
    auto *body = lang::findMirBody(
        forged, {.kind = lang::MirBodyKind::Function, .owner = function.id});
    lang::MirPlaceId unrelated = 0;
    const auto unrelatedSymbol = symbol(frontend, "unrelated");
    for (const auto &place : body->places)
      if (place.root == lang::MirPlaceRootKind::Binding &&
          place.symbol == unrelatedSymbol && place.projections.empty())
        unrelated = place.id;
    bool changed = false;
    for (auto &block : body->blocks)
      for (auto &instruction : block.instructions)
        if (instruction.kind == lang::MirInstructionKind::CallInput &&
            instruction.callInputRole == lang::MirCallInputRole::Argument &&
            instruction.callInputKind == lang::HirCallInputKind::ReadBorrow) {
          instruction.operands.front().place = unrelated;
          changed = true;
        }
    expect(unrelated && changed && !lang::verifyMirProgram(forged).valid(),
           "a staged class reference cannot substitute an unrelated physical "
           "owner for a public-base view");
  }
}

void foldColdInputEffects() {
  const auto frontend = lang::Frontend().analyze(
      "/tmp/gti-mir-persistent-storage/fold-cold.gti", R"(
thread_local mut int32_t first = 1;
thread_local mut int32_t disjoint = 3;
int32_t initialize_second() { disjoint = 9; return 2; }
thread_local mut int32_t second = initialize_second();
void observe<T>(int32_t& earlier, int32_t& later, T& element) {}
void folded<Args...>(Args... args) {
  (observe(first, second, args), ...);
}
int main() { folded(1, 2); return 0; }
)");
  expect(frontend.canGenerateCode(),
         "a later TLS input may initialize disjoint persistent storage");
  if (!frontend.canGenerateCode())
    return;
  auto forged = frontend.mir;
  const auto first = symbol(frontend, "first");
  const auto disjoint = symbol(frontend, "disjoint");
  bool changed = false;
  for (const auto &function : forged.functionInstances()) {
    const auto *info = frontend.semantics.findFunction(function.declaration);
    if (!info || !info->declaration ||
        info->declaration->name().lexeme != "initialize_second")
      continue;
    auto *body = lang::findMirBody(
        forged, {.kind = lang::MirBodyKind::Function, .owner = function.id});
    for (auto &place : body->places)
      if (place.root == lang::MirPlaceRootKind::Symbol &&
          place.symbol == disjoint) {
        place.symbol = first;
        if (place.key && place.key->root == disjoint)
          place.key->root = first;
        changed = true;
      }
    for (auto &block : body->blocks)
      for (auto &instruction : block.instructions)
        if (instruction.kind == lang::MirInstructionKind::EnsureThreadLocal &&
            instruction.threadLocalStorage == disjoint) {
          instruction.threadLocalStorage = first;
          instruction.bodyTarget = lang::MirBodyAddress{
              .kind = lang::MirBodyKind::ThreadLocalInitializer,
              .owner = first};
        }
  }
  const auto proof = lang::verifyMirProgram(forged);
  expect(changed && !proof.valid() &&
             std::any_of(proof.errors.begin(), proof.errors.end(),
                         [](const auto &error) {
                           return error.message.find("persistent call input") !=
                                  std::string::npos;
                         }),
         "an earlier staged fold input remains protected while a later TLS "
         "initializer executes");
}

void scalarInputAvailability(bool mutableInput) {
  const std::string source =
      std::string("void observe(") + (mutableInput ? "mut " : "") +
      "int32_t& value) {}\n"
      "int main() {\n"
      "  mut int32_t first = 1; first += 1;\n"
      "  observe(first);\n"
      "  mut int32_t later = 2; later += 1; return later;\n"
      "}\n";
  const auto frontend = lang::Frontend().analyze(
      "/tmp/gti-mir-persistent-storage/scalar-input.gti", source);
  expect(frontend.canGenerateCode(),
         "an ordinary initialized scalar accepts a checked reference input");
  if (!frontend.canGenerateCode())
    return;
  std::optional<lang::MirBodyAddress> main;
  for (const auto &function : frontend.mir.functionInstances()) {
    const auto *info = frontend.semantics.findFunction(function.declaration);
    if (info && info->declaration && info->declaration->name().lexeme == "main")
      main = lang::MirBodyAddress{.kind = lang::MirBodyKind::Function,
                                  .owner = function.id};
  }
  expect(main.has_value(), "scalar input fixture has one concrete entry");
  if (!main)
    return;
  auto forged = frontend.mir;
  auto *body = lang::findMirBody(forged, *main);
  lang::MirInstructionId checkpoint = 0;
  for (const auto &block : body->blocks)
    for (const auto &instruction : block.instructions)
      if (instruction.kind == lang::MirInstructionKind::CallInput &&
          instruction.operands.size() == 1 &&
          instruction.operands.front().kind ==
              (mutableInput ? lang::MirOperandKind::BorrowWrite
                            : lang::MirOperandKind::BorrowRead))
        checkpoint = instruction.id;
  lang::MirPlaceId future = 0;
  for (const auto &block : body->blocks)
    for (const auto &instruction : block.instructions) {
      if (instruction.kind != lang::MirInstructionKind::Initialize ||
          instruction.id <= checkpoint || !instruction.destination)
        continue;
      const auto *place = body->findPlace(*instruction.destination);
      if (place && place->root == lang::MirPlaceRootKind::Binding &&
          place->projections.empty() &&
          place->type == lang::SemanticType::Int32)
        future = place->id;
    }
  expect(checkpoint && future,
         "ordinary scalar fixture stages before its future binding exists");
  if (!checkpoint || !future)
    return;
  for (auto &block : body->blocks)
    for (auto &instruction : block.instructions)
      if (instruction.id == checkpoint)
        instruction.operands.front().place = future;
  const auto proof = lang::verifyMirProgram(forged);
  expect(!proof.valid() &&
             std::any_of(proof.errors.begin(), proof.errors.end(),
                         [](const auto &error) {
                           return error.message.find(
                                      "definitely initialized scalar") !=
                                  std::string::npos;
                         }),
         "reference input independently requires an initialized scalar");
}

void foldPersistentInputs(std::string_view storage, bool mutableInput,
                          bool hiddenWrite) {
  const std::string source =
      std::string(storage) + " mut int32_t first = 1;\n" +
      std::string(storage) + " mut int32_t second = 2;\n" +
      (hiddenWrite ? "void hidden() { second = 9; }\n"
                   : "void hidden() { int32_t observed = second; }\n") +
      "void touch<T>(" + (mutableInput ? "mut " : "") +
      "int32_t& prefix, T& element) { hidden(); }\n"
      "void folded<Args...>(Args... args) {\n"
      "  int32_t seeded = first;\n"
      "  mut int32_t prefix = seeded;\n"
      "  (touch(prefix, args), ...);\n"
      "  mut int32_t later = seeded; later += 1;\n"
      "}\n"
      "int main() { folded(1, 2); return 0; }\n";
  const auto frontend = lang::Frontend().analyze(
      "/tmp/gti-mir-persistent-storage/compact-inputs.gti", source);
  for (const auto &diagnostic : frontend.diagnostics)
    if (diagnostic.severity == lang::DiagnosticSeverity::Error)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(frontend.canGenerateCode(),
         "unrelated user fold has exact compact source-call targets");
  if (!frontend.canGenerateCode())
    return;
  const auto first = symbol(frontend, "first");
  const auto second = symbol(frontend, "second");
  std::optional<lang::MirBodyAddress> owner;
  lang::HirFunctionInstanceId wrongTarget = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    const auto *info = frontend.semantics.findFunction(function.declaration);
    if (!info || !info->declaration)
      continue;
    if (info->declaration->name().lexeme == "folded")
      owner = lang::MirBodyAddress{.kind = lang::MirBodyKind::Function,
                                   .owner = function.id};
    if (info->declaration->name().lexeme == "main")
      wrongTarget = function.id;
  }
  if (!owner || !first || !second || !wrongTarget) {
    expect(false, "compact fixture owns its concrete function and storage");
    return;
  }
  auto disjoint = frontend.mir;
  auto *body = lang::findMirBody(disjoint, *owner);
  lang::MirPlaceId input = 0;
  lang::MirPlaceId expired = 0;
  for (const auto &place : body->places) {
    if (place.root == lang::MirPlaceRootKind::Symbol && place.symbol == first &&
        place.projections.empty())
      input = place.id;
  }
  std::vector<lang::MirInstructionId> calls;
  std::vector<lang::MirInstructionId> inputs;
  const auto findInstruction = [](lang::MirBody &owner,
                                  lang::MirInstructionId id) {
    for (auto &block : owner.blocks)
      for (auto &instruction : block.instructions)
        if (instruction.id == id)
          return &instruction;
    return static_cast<lang::MirInstruction *>(nullptr);
  };
  for (auto &block : body->blocks)
    for (auto &instruction : block.instructions) {
      if (instruction.kind != lang::MirInstructionKind::Call ||
          !instruction.functionTarget || instruction.operands.empty())
        continue;
      const auto *function =
          disjoint.findFunctionInstance(*instruction.functionTarget);
      const auto *info =
          function ? frontend.semantics.findFunction(function->declaration)
                   : nullptr;
      if (!info || !info->declaration ||
          info->declaration->name().lexeme != "touch")
        continue;
      const auto *value = body->findValue(instruction.operands.front().value);
      auto *stage = value ? findInstruction(*body, value->definition) : nullptr;
      expect(instruction.callSite == instruction.hirValue &&
                 instruction.callSite != 0 && stage &&
                 stage->kind == lang::MirInstructionKind::CallInput &&
                 stage->operands.size() == 1 &&
                 stage->operands.front().kind ==
                     (mutableInput ? lang::MirOperandKind::BorrowWrite
                                   : lang::MirOperandKind::BorrowRead),
             "fold element calls retain ordinary ordered reference inputs");
      if (!stage || stage->operands.size() != 1)
        return;
      stage->operands.front().place = input;
      inputs.push_back(stage->id);
      calls.push_back(instruction.id);
    }
  if (!calls.empty())
    for (const auto &block : body->blocks)
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Initialize ||
            instruction.id <= calls.back() || !instruction.destination)
          continue;
        const auto *place = body->findPlace(*instruction.destination);
        if (place && place->root == lang::MirPlaceRootKind::Binding &&
            place->projections.empty() &&
            place->type == lang::SemanticType::Int32)
          expired = place->id;
      }
  expect(input && expired && calls.size() == 2 && body->loans.empty(),
         "fold protection uses ordinary stages without a named source loan");
  if (!input || !expired || calls.size() != 2)
    return;
  expect(lang::verifyMirProgram(disjoint).valid(),
         "fold persistent call input permits disjoint hidden effects");
  auto overlap = disjoint;
  for (const auto address : lang::enumerateMirBodyAddresses(overlap)) {
    if (address.kind != lang::MirBodyKind::Function)
      continue;
    auto *callee = lang::findMirBody(overlap, address);
    for (auto &place : callee->places)
      if (place.root == lang::MirPlaceRootKind::Symbol &&
          place.symbol == second) {
        place.symbol = first;
        if (place.key && place.key->root == second)
          place.key->root = first;
      }
    for (auto &block : callee->blocks)
      for (auto &instruction : block.instructions)
        if (instruction.kind == lang::MirInstructionKind::EnsureThreadLocal &&
            instruction.threadLocalStorage == second) {
          instruction.threadLocalStorage = first;
          instruction.bodyTarget = lang::MirBodyAddress{
              .kind = lang::MirBodyKind::ThreadLocalInitializer,
              .owner = first};
        }
  }
  const auto proof = lang::verifyMirProgram(overlap);
  const bool conflicts = mutableInput || hiddenWrite;
  expect(proof.valid() == !conflicts,
         "ordered fold input access governs hidden same-root conflicts");
  if (conflicts)
    expect(std::any_of(proof.errors.begin(), proof.errors.end(),
                       [](const auto &error) {
                         return error.message.find("persistent call input") !=
                                std::string::npos;
                       }),
           "fold rejection comes from exact persistent input protection");
  for (int mutation = 0; mutation != 9; ++mutation) {
    if (mutation == 5 && storage.empty())
      continue;
    auto forged = disjoint;
    auto *target = lang::findMirBody(forged, *owner);
    for (auto &block : target->blocks)
      for (auto &instruction : block.instructions) {
        if (mutation == 5 &&
            instruction.kind == lang::MirInstructionKind::EnsureThreadLocal &&
            instruction.threadLocalStorage == first)
          instruction.threadLocalStorage = second;
        if (mutation == 0 && instruction.id == calls.front())
          instruction.functionTarget.reset();
        if (mutation == 1 && instruction.id == calls.front())
          instruction.functionTarget = wrongTarget;
        if (mutation == 2 && instruction.id == inputs.front())
          instruction.operands.front().place = 0;
        if (mutation == 3 && instruction.id == inputs.front())
          instruction.operands.front().type = lang::SemanticType::Bool;
        if (mutation == 4 && instruction.id == inputs.front())
          instruction.operands.front().place = expired;
        if (mutation == 6 && instruction.id == calls.front()) {
          const auto *next = findInstruction(*target, calls.back());
          instruction.operands.front() = next->operands.front();
        }
        if (mutation == 7 && instruction.id == calls.back())
          instruction.callSite =
              findInstruction(*target, calls.front())->callSite;
        if (mutation == 8 && instruction.id == calls.front())
          instruction.callSite = 0;
      }
    expect(lang::rebuildMirValueUses(*target),
           "forged invocation operands retain a rebuilt use inventory");
    const auto forgedProof = lang::verifyMirProgram(forged);
    if (forgedProof.valid())
      std::cerr << "fold forged input accepted: storage=" << storage
                << " mutable=" << mutableInput << " write=" << hiddenWrite
                << " mutation=" << mutation << '\n';
    expect(!forgedProof.valid(),
           "fold inputs require target, available typed source, exact TLS "
           "guard and unique invocation");
  }
  const auto compact = [&](lang::MirProgram &program) {
    auto *ownerBody = lang::findMirBody(program, *owner);
    for (const auto call : calls) {
      auto *invocation = findInstruction(*ownerBody, call);
      for (auto &operand : invocation->operands) {
        const auto *value = ownerBody->findValue(operand.value);
        auto *stage =
            value ? findInstruction(*ownerBody, value->definition) : nullptr;
        if (!stage || stage->kind != lang::MirInstructionKind::CallInput ||
            stage->operands.size() != 1)
          return false;
        operand = stage->operands.front();
        *stage = lang::makeMirInstructionTombstone(*stage);
      }
      invocation->callSite = 0;
    }
    return lang::rebuildMirValueUses(*ownerBody);
  };
  auto compactDisjoint = disjoint;
  expect(compact(compactDisjoint) &&
             lang::verifyMirProgram(compactDisjoint).valid(),
         "legacy compact vocabulary has a coherent disjoint control");
  auto compactOverlap = overlap;
  const bool collapsed = compact(compactOverlap);
  const auto compactProof = lang::verifyMirProgram(compactOverlap);
  expect(collapsed && compactProof.valid() == !conflicts,
         "removing staging cannot bypass invocation-time ambient protection");
}

void provisionalInitializationKeepsFailureCleanup() {
  const auto frontend = lang::Frontend().analyze(
      "/tmp/gti-mir-persistent-storage/initializer-cleanup.gti", R"(
thread_local mut int32_t source = 2147483647;
thread_local mut int32_t cleanup_count = 0;
class Guard {
public:
  Guard() {}
  ~Guard() { cleanup_count = 1; }
};
int32_t initialize() {
  Guard armed{};
  return source + 1;
}
int32_t initialized = initialize();
int main() { return initialized; }
)");
  for (const auto &diagnostic : frontend.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(frontend.canGenerateCode(),
         "provisional module lowering preserves initializer failures and "
         "armed local cleanup");
  if (!frontend.canGenerateCode())
    return;
  bool moduleInvoke = false;
  for (const auto &block : frontend.mir.module().blocks)
    moduleInvoke |= block.terminator.kind == lang::MirTerminatorKind::Invoke;
  bool cleanup = false;
  for (const auto &function : frontend.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.kind == lang::MirInstructionKind::Drop)
          for (const auto &event : instruction.lifecycle)
            cleanup |= event.kind == lang::MirLifecycleEventKind::Drop &&
                       event.failureCleanup;
  expect(moduleInvoke && cleanup,
         "real process initializer keeps its Invoke and failure-side drop");
  auto forged = frontend.mir;
  bool removed = false;
  for (const auto address : lang::enumerateMirBodyAddresses(forged)) {
    if (address.kind != lang::MirBodyKind::Function)
      continue;
    auto *body = lang::findMirBody(forged, address);
    for (auto &block : body->blocks)
      for (auto &instruction : block.instructions)
        if (instruction.kind == lang::MirInstructionKind::Drop)
          for (const auto &event : instruction.lifecycle)
            if (event.kind == lang::MirLifecycleEventKind::Drop &&
                event.failureCleanup) {
              instruction.lifecycle.clear();
              removed = true;
              break;
            }
  }
  expect(removed && !lang::verifyMirProgram(forged).valid(),
         "an initializer cannot omit the armed local's failure cleanup");
}
} // namespace

int main() {
  for (const bool cleanup : {false, true}) {
    independentHiddenEffects("thread_local", cleanup);
    independentHiddenEffects("", cleanup);
  }
  nonLexicalAndFieldBoundaries();
  readySlotThroughNestedCalls();
  for (const auto storage : {"thread_local", ""})
    for (const bool branch : {false, true})
      stagedInputBoundaries(storage, branch);
  persistentTransferIsIndependentlyRejected();
  stagedBorrowMayPublishItsOriginalLifetime();
  nativeBoundaryIsNotAnUnknownSourceBody();
  exactRestorationRemainsAvailable();
  generatedDefaultHasExactEmptyTargets("thread_local");
  generatedDefaultHasExactEmptyTargets("");
  generatedDefaultSchedulesRetainEffects("thread_local");
  generatedDefaultSchedulesRetainEffects("");
  exactWholeObjectVirtualEffects("thread_local");
  exactWholeObjectVirtualEffects("");
  referenceRootDoesNotAcquireExactVirtualType(false);
  referenceRootDoesNotAcquireExactVirtualType(true);
  ordinaryBindingsRetainPhysicalStorageTypes();
  provisionalInitializationKeepsFailureCleanup();
  foldHirProof();
  foldColdInputEffects();
  referenceInputPhysicalType(false);
  referenceInputPhysicalType(true);
  referenceInputBaseConversion();
  scalarInputAvailability(false);
  scalarInputAvailability(true);
  for (const auto storage : {"thread_local", ""})
    for (const bool mutableInput : {false, true})
      for (const bool hiddenWrite : {false, true})
        foldPersistentInputs(storage, mutableInput, hiddenWrite);
  return passed ? 0 : 1;
}
