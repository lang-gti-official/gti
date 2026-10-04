#include "cpp_mir_body_emitter_test_fixture.h"

namespace {

void testPassiveRecordComposition() {
  for (const std::string name :
       {"passive_expected_records", "passive_callable_records",
        "temporary_operator_records", "nested_receiver_array"}) {
    const auto path = std::filesystem::path(__FILE__).parent_path() /
                      "fixtures" / (name + ".gti");
    std::ifstream input(path);
    expect(input.good(), "the passive record composition fixture must exist");
    const std::string source{std::istreambuf_iterator<char>(input), {}};
    const auto frontend = analyzeWithStandardLibrary(path.string(), source);
    expect(frontend.canGenerateCode(), "record compositions must verify");
    if (!frontend.canGenerateCode())
      continue;
    const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
    // Production planning also applies the exact deduced-callable template
    // overlays. A raw representation map alone is not that whole-program plan.
    expect(
        !gti_test::emitCpp(frontend, frontend.mir, frontend.mir)
             .contents.empty(),
        "record compositions must have a complete verified-MIR representation");
    bool mutated = false;
    for (const auto &function : frontend.mir.functionInstances()) {
      if (mutated)
        break;
      auto malformed = frontend.mir;
      auto *changed = const_cast<lang::MirFunctionInstance *>(
          malformed.findFunctionInstance(function.id));
      for (auto &place : changed->body.places) {
        if (name == "nested_receiver_array" &&
            place.root == lang::MirPlaceRootKind::This &&
            place.projections.size() >= 3 &&
            place.projections.back().kind == lang::MirProjectionKind::Index) {
          place.projections.front().field = 999999;
          mutated = true;
          break;
        }
      }
      for (auto &block : changed->body.blocks) {
        for (auto &instruction : block.instructions) {
          if (mutated)
            break;
          if (name == "passive_callable_records" && instruction.lambdaTarget &&
              instruction.callableInvocation &&
              instruction.info.type == changed->returnType &&
              changed->returnType.kind == lang::SemanticType::Class) {
            // A passive result still requires the exact invoked lambda's
            // result type; absence of a semantic drop is not permission to
            // publish some other representation into caller storage.
            instruction.info.type = lang::SemanticType::Int32;
            mutated = true;
          } else if (name == "temporary_operator_records" &&
                     instruction.kind == lang::MirInstructionKind::Call &&
                     instruction.receiver && instruction.callSite == 0 &&
                     instruction.receiver->kind ==
                         lang::MirOperandKind::BorrowWrite) {
            instruction.receiver->kind = lang::MirOperandKind::BorrowRead;
            mutated = true;
          } else if (name == "passive_expected_records" &&
                     instruction.operation == lang::MirOperation::Unexpected &&
                     instruction.operands.size() == 1 &&
                     instruction.operands.front().type.kind ==
                         lang::SemanticType::Class) {
            instruction.operands.push_back(instruction.operands.front());
            mutated = true;
          }
        }
      }
      if (mutated) {
        const lang::CppMirBodyEmitter invalid(malformed, map);
        expect(!invalid.supportsFailureBodyText(
                   {lang::MirBodyKind::Function, function.id}),
               "record staging must reject mismatched fields, receivers, "
               "payloads, and results");
        bool rejected = false;
        try {
          static_cast<void>(gti_test::emitCpp(frontend, malformed, malformed));
        } catch (const std::exception &) {
          rejected = true;
        }
        expect(rejected,
               "the production plan must reject the same mutation after "
               "accepting the original program with its callable overlays");
      }
    }
    expect(mutated,
           "each record regression must exercise its exact negative proof");
  }
}

void testGeneratedDefaultOwningFieldInitializer() {
  const auto frontend = analyze("generated-default-owning-field.gti", R"(
class Resource {
  int value = 4;
public:
  ~Resource() {}
};
class Holder { Resource value{}; };
int main() { Holder holder{}; return 0; }
)");
  expect(frontend.canGenerateCode(),
         "generated default construction of an owning field must verify");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  bool checked = false;
  for (const auto &owner : frontend.hir.classInstances()) {
    if (!owner.source || owner.source->name().lexeme != "Holder")
      continue;
    const lang::MirBodyAddress address{lang::MirBodyKind::FieldInitializers,
                                       owner.id};
    const auto schedule = emitter.initializerSchedule(address);
    expect(schedule.supported && schedule.fields.size() == 1,
           "braced default construction preserves the exact owning field "
           "schedule");
    auto malformed = frontend.mir;
    auto *changed = const_cast<lang::MirClassInstance *>(
        malformed.findClassInstance(owner.id));
    expect(changed && changed->fieldInitializers.dropObligations.size() == 2,
           "fixture retains both temporary and field rollback identities");
    if (changed && changed->fieldInitializers.dropObligations.size() == 2) {
      auto &drops = changed->fieldInitializers.dropObligations;
      drops.front().place = drops.back().place;
      const lang::CppMirBodyEmitter invalid(malformed, map);
      expect(!invalid.initializerSchedule(address).supported,
             "field publication cannot erase or conflate its source lifetime");
    }
    checked = true;
  }
  expect(checked, "fixture must reach Holder's field initializer body");
}

const lang::MirInstruction *testInstruction(const lang::MirBody &body,
                                            lang::MirInstructionId id) {
  for (const auto &block : body.blocks)
    for (const auto &instruction : block.instructions)
      if (instruction.id == id)
        return &instruction;
  return nullptr;
}

const lang::MirInstruction *
testFieldPublication(const lang::MirBody &body,
                     const lang::MirInstruction &producer) {
  if (!producer.result)
    return nullptr;
  const lang::MirInstruction *publication = nullptr;
  for (const auto &use : body.usesOf(*producer.result)) {
    if (use.kind != lang::MirValueUseKind::InstructionOperand)
      continue;
    const auto *instruction = testInstruction(body, use.instruction);
    if (publication || !instruction || use.operandIndex != 0 ||
        instruction->kind != lang::MirInstructionKind::Initialize ||
        instruction->constructorInitializer == 0)
      return nullptr;
    publication = instruction;
  }
  return publication;
}

void testPackParameterCleanup() {
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/pack_parameter_cleanup_failure.gti";
  std::ifstream input(fixture);
  expect(input.good(), "the owning-pack cleanup regression must exist");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
  expect(frontend.canGenerateCode(),
         "owning pack parameter cleanup must verify");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  expect(emitter.analyzeProgram().ready(),
         "owning pack cleanup must have a complete C++ representation");
  std::size_t owningPacks = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    std::vector<lang::MirDropObligationId> elements;
    for (const auto &drop : function.body.dropObligations) {
      const auto *place = function.body.findPlace(drop.place);
      if (drop.kind == lang::MirDropObligationKind::Binding &&
          place != nullptr && place->projections.size() == 1 &&
          place->projections.front().kind ==
              lang::MirProjectionKind::PackElement)
        elements.push_back(drop.id);
    }
    if (elements.empty())
      continue;
    ++owningPacks;
    expect(elements.size() == 2,
           "both owning pack elements need entry obligations");
    if (elements.size() != 2)
      continue;
    for (const auto id : elements) {
      const auto *drop = function.body.findDropObligation(id);
      expect(drop->initiallyActive && drop->hirObligation != 0 &&
                 drop->dropType.requiresActiveCleanup,
             "each element must retain its concrete HIR cleanup identity");
    }
    const lang::MirBodyAddress address{lang::MirBodyKind::Function,
                                       function.id};
    expect(emitter.supportsFailureBodyText(address),
           "fallible pack cleanup must use the status ABI");
    if (emitter.supportsFailureBodyText(address)) {
      const auto text =
          emitter.emitFailureBodyText(address, "pack-cleanup", 1).text;
      expect(text.find(".construct(std::move(__gti_mir_arg_1))") !=
                     std::string::npos &&
                 text.find(".construct(std::move(__gti_mir_arg_2))") !=
                     std::string::npos,
             "pack parameters must enter distinct explicit lifetime slots");
    }
    for (int mutation = 0; mutation != 3; ++mutation) {
      auto malformed = frontend.mir;
      auto *changed = const_cast<lang::MirFunctionInstance *>(
          malformed.findFunctionInstance(function.id));
      auto &first = changed->body.dropObligations[elements.front() - 1];
      auto &second = changed->body.dropObligations[elements.back() - 1];
      if (mutation == 0)
        first.initiallyActive = false;
      else if (mutation == 1)
        second.place = first.place;
      else
        std::swap(first.constructionOrder, second.constructionOrder);
      expect(!lang::verifyMirProgram(malformed).valid(),
             "pack entry lifetime, element coverage, and cleanup order must "
             "reject forgery");
    }
  }
  expect(owningPacks == 1,
         "the regression must exercise one two-element owning pack");

  const auto copyFixture = fixture.parent_path() / "pack_parameter_copies.gti";
  std::ifstream copyInput(copyFixture);
  expect(copyInput.good(), "the repeated pack-copy regression must exist");
  const std::string copySource{std::istreambuf_iterator<char>(copyInput), {}};
  const auto copies =
      analyzeWithStandardLibrary(copyFixture.string(), copySource);
  expect(copies.canGenerateCode(),
         "repeated pack copies must preserve entry-owned source lifetimes");
  if (!copies.canGenerateCode())
    return;
  std::size_t copyStages = 0;
  for (const auto &function : copies.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::CallInput ||
            instruction.callInputKind != lang::HirCallInputKind::CopyValue ||
            instruction.operands.size() != 1)
          continue;
        const auto *place =
            function.body.findPlace(instruction.operands.front().place);
        if (place == nullptr || place->projections.size() != 1 ||
            place->projections.front().kind !=
                lang::MirProjectionKind::PackElement)
          continue;
        ++copyStages;
        expect(place->sourceValue == 0 && instruction.lifecycle.size() == 1 &&
                   instruction.lifecycle.front().kind ==
                       lang::MirLifecycleEventKind::Initialize &&
                   instruction.lifecycle.front().source == 0,
               "pack copy stages initialize new owners without consuming "
               "the shared entry lifetime");
        auto malformed = copies.mir;
        auto *body = lang::findMirBody(
            malformed, {lang::MirBodyKind::Function, function.id});
        body->places[place->id - 1].sourceValue = instruction.hirValue + 1;
        expect(!lang::verifyMirProgram(malformed).valid(),
               "entry-pack copy admission must not permit forged expression "
               "provenance");
      }
    }
  }
  expect(copyStages == 6,
         "two forwarding expansions and allocation must independently copy "
         "both pack elements");

  const auto borrowFixture =
      fixture.parent_path() / "pack_parameter_borrows.gti";
  std::ifstream borrowInput(borrowFixture);
  expect(borrowInput.good(), "the pack-fold borrowing regression must exist");
  const std::string borrowSource{std::istreambuf_iterator<char>(borrowInput),
                                 {}};
  const auto borrows =
      analyzeWithStandardLibrary(borrowFixture.string(), borrowSource);
  expect(borrows.canGenerateCode(), "owning pack folds must verify");
  if (!borrows.canGenerateCode())
    return;
  std::size_t borrowedElements = 0;
  for (const auto &function : borrows.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Call)
          continue;
        for (const auto &operand : instruction.operands) {
          const auto *place = function.body.findPlace(operand.place);
          if (operand.kind != lang::MirOperandKind::BorrowRead ||
              place == nullptr || place->projections.size() != 1 ||
              place->projections.front().kind !=
                  lang::MirProjectionKind::PackElement)
            continue;
          ++borrowedElements;
          expect(std::any_of(function.body.dropObligations.begin(),
                             function.body.dropObligations.end(),
                             [&](const auto &drop) {
                               return drop.place == place->id &&
                                      drop.initiallyActive;
                             }),
                 "a fold must borrow the live owning slot, not a moved "
                 "native parameter shell");
          auto malformed = borrows.mir;
          auto *body = lang::findMirBody(
              malformed, {lang::MirBodyKind::Function, function.id});
          auto alias = *place;
          alias.id = body->places.size() + 1;
          body->places.push_back(alias);
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "an alternate owning pack projection without its entry "
                 "lifetime must be rejected");
        }
      }
    }
  }
  expect(borrowedElements == 4,
         "two folds must borrow both owning elements from their live slots");
}

void testForwardListEmplacementPublication() {
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/forward_list_emplacement_cleanup.gti";
  std::ifstream input(fixture);
  expect(input.good(), "the emplacement cleanup regression must exist");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
  expect(frontend.canGenerateCode(), "fallible emplacement must verify");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  expect(emitter.analyzeProgram().ready(),
         "a conditional argument must preserve the staged list receiver");
  std::size_t directFields = 0;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    const lang::MirBodyAddress address{lang::MirBodyKind::Constructor,
                                       constructor.id};
    if (!emitter.supportsFailureBodyText(address))
      continue;
    const auto text =
        emitter.emitFailureBodyText(address, "emplacement", 1).text;
    if (text.find("in-place constructor field reparent") == std::string::npos)
      continue;
    ++directFields;
    const lang::MirInstruction *elementPublication = nullptr;
    for (const auto &block : constructor.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Initialize &&
            instruction.constructorInitializer != 0 &&
            instruction.destination &&
            instruction.info.type.kind == lang::SemanticType::Class &&
            !instruction.info.traits.movable)
          elementPublication = &instruction;
      }
    }
    const std::string elementPlace =
        elementPublication == nullptr
            ? ""
            : "__gti_mir_p_" + std::to_string(*elementPublication->destination);
    expect(
        elementPublication != nullptr &&
            text.find(
                "::new (static_cast<void *>(std::addressof(" + elementPlace +
                "))) ::__gti_program::Element("
                "::gti_internal::backend::mir_failure_constructor_tag_v1<") !=
                std::string::npos &&
            text.find(elementPlace + " = std::move(") == std::string::npos &&
            text.find("mir_lifetime_slot<::__gti_program::Element>") ==
                std::string::npos,
        "fallible immovable elements construct in their final field, not "
        "in a temporary followed by assignment");
    auto malformed = frontend.mir;
    auto *body = lang::findMirBody(malformed, address);
    bool mutated = false;
    for (auto &block : body->blocks) {
      for (auto &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Initialize &&
            instruction.constructorInitializer != 0 &&
            instruction.info.type.kind == lang::SemanticType::Class) {
          instruction.lifecycle.clear();
          mutated = true;
        }
      }
    }
    expect(mutated && !lang::verifyMirProgram(malformed).valid(),
           "in-place publication still requires exact rollback reparenting");
    expect(!lang::CppMirBodyEmitter(malformed, map)
                .supportsFailureBodyText(address),
           "the backend must reject missing in-place rollback publication");
  }
  expect(directFields == 1, "the node needs one in-place element publication");
  std::size_t abandonedReturns = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      if (block.activeFailure == 0)
        continue;
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::EndBorrow ||
            !instruction.loan)
          continue;
        const auto loan = std::find_if(
            function.body.loans.begin(), function.body.loans.end(),
            [&](const auto &item) { return item.id == *instruction.loan; });
        if (loan == function.body.loans.end() ||
            loan->kind != lang::MirLoanKind::Return)
          continue;
        ++abandonedReturns;
        auto malformed = frontend.mir;
        auto *body = lang::findMirBody(
            malformed, {lang::MirBodyKind::Function, function.id});
        for (auto &changed : body->blocks) {
          std::erase_if(changed.instructions, [&](const auto &item) {
            return item.id == instruction.id;
          });
        }
        expect(!lang::verifyMirProgram(malformed).valid(),
               "a failed return cleanup must retire its unpublished loan");
      }
    }
  }
  expect(abandonedReturns != 0,
         "emplacement must exercise unpublished-return cleanup");
}

void testContainerSwapStorageAndMemberResults() {
  const auto frontend =
      analyzeWithStandardLibrary("container-swap-slots.gti", R"(
#include <std/vector>
#include <std/forward_list>
class Item {
public:
  int key;
  Item(int value) : key(value) {}
  Item(Item& other) = delete;
  Item(Item&& other) = default;
  ~Item() { std::println(this.key); }
};
int main() {
  mut std::vector<Item> a = std::vector<Item>();
  mut std::vector<Item> b = std::vector<Item>();
  a.push_back(Item(1));
  a.swap(b);
  mut std::forward_list<Item> left = std::forward_list<Item>();
  mut std::forward_list<Item> right = std::forward_list<Item>();
  left.push_front(Item(2));
  left.swap(right);
  return 0;
}
)");
  expect(frontend.canGenerateCode(), "container swaps must pass the frontend");
  if (!frontend.canGenerateCode())
    return;
  for (const auto standard :
       {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23}) {
    auto rows = buildRows(frontend, frontend.mir, standard);
    const lang::CppMirBodyEmissionMap map(rows);
    const lang::CppMirBodyEmitter emitter(frontend.mir, map);
    expect(emitter.analyzeProgram().ready(),
           "storage-return and owned member-result swaps must be MIR-ready");
    std::size_t storageReturns = 0;
    std::size_t memberPublications = 0;
    for (const auto &function : frontend.mir.functionInstances()) {
      const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                         .owner = function.id};
      if (function.returnType.kind == lang::SemanticType::PrefixStorage &&
          function.definitionKind == lang::MirDefinitionKind::Source) {
        ++storageReturns;
        expect(emitter.supportsFailureBodyText(address),
               "private storage returns must use the explicit status ABI");
        const auto text =
            emitter.emitFailureBodyText(address, "storage-return-test", 1);
        expect(text.emitted() &&
                   text.text.find("*__gti_mir_out_result = std::move(") !=
                       std::string::npos,
               "storage publication must transfer rather than copy a handle");
        auto missing = rows;
        std::erase_if(missing.types, [&](const auto &row) {
          return row.type == function.returnType;
        });
        const lang::CppMirBodyEmissionMap missingMap(std::move(missing));
        expect(!lang::CppMirBodyEmitter(frontend.mir, missingMap)
                    .supportsFailureBodyText(address),
               "storage returns require their exact representation row");
      }
      for (const auto &block : function.body.blocks) {
        for (const auto &instruction : block.instructions) {
          if (instruction.kind != lang::MirInstructionKind::Call ||
              !instruction.receiver || !instruction.functionTarget ||
              !instruction.successResultDrop ||
              instruction.info.type.kind != lang::SemanticType::Class)
            continue;
          const bool replacesField = std::any_of(
              function.body.blocks.begin(), function.body.blocks.end(),
              [&](const auto &candidate) {
                return std::any_of(
                    candidate.instructions.begin(),
                    candidate.instructions.end(), [&](const auto &user) {
                      return user.kind == lang::MirInstructionKind::Assign &&
                             std::any_of(
                                 user.lifecycle.begin(), user.lifecycle.end(),
                                 [&](const auto &event) {
                                   return event.kind ==
                                              lang::MirLifecycleEventKind::
                                                  Replace &&
                                          event.source ==
                                              *instruction.successResultDrop &&
                                          event.target == 0;
                                 });
                    });
              });
          if (!replacesField)
            continue;
          ++memberPublications;
          for (int mutation = 0; mutation != 3; ++mutation) {
            auto malformed = frontend.mir;
            auto *changed = const_cast<lang::MirFunctionInstance *>(
                malformed.findFunctionInstance(function.id));
            for (auto &changedBlock : changed->body.blocks) {
              for (auto &changedInstruction : changedBlock.instructions) {
                if (changedInstruction.id != instruction.id)
                  continue;
                if (mutation == 0)
                  changedInstruction.receiver.reset();
                else if (mutation == 1)
                  changedInstruction.successResultDrop.reset();
                else
                  changedInstruction.callSite += 10000;
              }
            }
            expect(!lang::CppMirBodyEmitter(malformed, map)
                        .supportsFailureBodyText(address),
                   "member field publication rejects missing receivers, "
                   "activation, "
                   "and retargeted input schedules");
          }
        }
      }
    }
    expect(storageReturns > 0 && memberPublications > 0,
           "both new result representations must be exercised");
  }
}

void testToyEngineConstructorInitializers() {
  const auto frontend =
      analyzeWithStandardLibrary("toy-engine-initializers.gti", R"(
#include <std/memory>
mut int destroyed = 0;
class Resource { public: Resource() {} ~Resource() { destroyed += 1; } };
class Point { float x; public: Point(float value) : x(value) {} };
class Camera { Point position; public: Camera() : position(Point(5.0)) {} };
enum class Mode { idle, running };
class EnumState {
  Mode direct;
  Mode grouped;
public: EnumState(Mode initial) : direct(initial), grouped((initial)) {}
};
class Engine {
  std::unique_ptr<Resource> first = nullptr;
  std::unique_ptr<Resource> second = nullptr;
public: Engine() {}
};
int main() {
  Camera camera = Camera();
  Engine engine = Engine();
  EnumState state = EnumState(Mode::running);
  return 0;
}
)");
  expect(frontend.canGenerateCode(),
         "toy engine initializer cases pass the frontend");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  std::size_t constructedFields = 0;
  std::size_t floatingStages = 0;
  std::size_t enumStages = 0;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    for (const auto &block : constructor.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Construct &&
            instruction.parameterTypes ==
                std::vector<lang::SemanticType>{lang::SemanticType::Float} &&
            testFieldPublication(constructor.body, instruction))
          ++constructedFields;
        if (instruction.kind != lang::MirInstructionKind::Initialize ||
            (instruction.info.type != lang::SemanticType::Float &&
             instruction.info.type.kind != lang::SemanticType::Enum))
          continue;
        if (instruction.info.type.kind == lang::SemanticType::Enum)
          ++enumStages;
        else
          ++floatingStages;
        auto malformed = frontend.mir;
        auto *changed = lang::findMirBody(
            malformed,
            {.kind = lang::MirBodyKind::Constructor, .owner = constructor.id});
        for (auto &candidate : changed->blocks) {
          std::erase_if(candidate.instructions, [&](const auto &item) {
            return item.id == instruction.id;
          });
        }
        expect(lang::rebuildMirValueUses(*changed),
               "initializer mutation should retain coherent use lists");
        expect(!lang::verifyMirProgram(malformed).valid(),
               "removing an explicit scalar field publication must "
               "invalidate MIR");
      }
    }
  }
  expect(constructedFields == 1 && floatingStages == 1,
         "the nested Point initializer must retain one exact floating "
         "publication");
  expect(enumStages == 2,
         "direct and grouped enum fields require explicit MIR publication");
  std::size_t cleanupMutations = 0;
  for (const auto &owner : frontend.mir.classInstances()) {
    bool mutated = false;
    auto malformed = frontend.mir;
    const lang::MirBodyAddress address{
        .kind = lang::MirBodyKind::FieldInitializers, .owner = owner.id};
    auto *body = lang::findMirBody(malformed, address);
    for (auto &block : body->blocks) {
      for (auto &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Drop &&
            instruction.definedFailure.propagation ==
                lang::FailurePropagationKind::Destructor) {
          instruction.definedFailure.propagation =
              lang::FailurePropagationKind::DirectCall;
          mutated = true;
        }
      }
    }
    if (mutated) {
      ++cleanupMutations;
      expect(!lang::CppMirBodyEmitter(malformed, map)
                  .initializerSchedule(address)
                  .supported,
             "unreachable owner cleanup must still reject forged propagation "
             "metadata");
    }
  }
  expect(cleanupMutations != 0,
         "the null-owner case must exercise unreachable fallible cleanup");
  expect(emitter.analyzeProgram().ready(),
         "toy engine field and constructor initializers have complete MIR "
         "emission");
}

void testFallibleFieldInitializerPropagation() {
  const auto frontend =
      analyzeWithStandardLibrary("fallible-field-initializer.gti", R"(
#include <std/memory>
class Resource { public: Resource() {} };
class Application {
  std::unique_ptr<Resource> resource = std::make_unique<Resource>();
  std::unique_ptr<Resource> shadow = std::make_unique<Resource>();
public: Application() {}
};
int main() { Application application = Application(); return 0; }
)");
  expect(frontend.canGenerateCode(),
         "a fallible field initializer is valid GTI");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  bool found = false;
  for (const auto &owner : frontend.mir.classInstances()) {
    const lang::MirBodyAddress address{
        .kind = lang::MirBodyKind::FieldInitializers, .owner = owner.id};
    const auto analysis =
        lang::CppMirBodyEmitter(frontend.mir, map).analyze(address);
    if (owner.declaredFields.size() != 2 ||
        owner.declaredFields.front().type.kind != lang::SemanticType::Class)
      continue;
    const auto constructor = std::find_if(
        frontend.mir.constructorInstances().begin(),
        frontend.mir.constructorInstances().end(), [&](const auto &item) {
          return item.owner == owner.id && item.initializers.size() == 2 &&
                 item.initializers.front().field ==
                     owner.declaredFields.front().symbol;
        });
    if (constructor == frontend.mir.constructorInstances().end())
      continue;
    found = true;
    expect(analysis.ready(),
           "covered declaration defaults are nonexecuting metadata");
    const auto schedule =
        lang::CppMirBodyEmitter(frontend.mir, map).initializerSchedule(address);
    expect(schedule.supported && schedule.fields.size() == 2 &&
               std::all_of(
                   schedule.fields.begin(), schedule.fields.end(),
                   [](const auto &field) { return field.spelling.empty(); }),
           "source constructor defaults must not execute again as native field "
           "defaults");
    expect(constructor->mayRaiseDefinedFailure &&
               !constructor->body.dropObligations.empty(),
           "the selected constructor owns the default's failure and rollback "
           "schedule");
    for (int mutation = 0; mutation < 4; ++mutation) {
      auto damaged = frontend.mir;
      auto &constructors =
          const_cast<std::vector<lang::MirConstructorInstance> &>(
              damaged.constructorInstances());
      auto &selected = constructors[constructor->id - 1];
      if (mutation == 0)
        selected.initializers.clear();
      else if (mutation == 1)
        selected.initializers.push_back(selected.initializers.front());
      else if (mutation == 2)
        std::swap(selected.initializers.front(), selected.initializers.back());
      else
        selected.initializers.front().targetType = lang::SemanticType::Int32;
      expect(!lang::verifyMirProgram(damaged).valid(),
             "missing, duplicate, reordered, or mistyped selected fields must "
             "fail verification");
      expect(!lang::CppMirBodyEmitter(damaged, map)
                  .initializerSchedule(address)
                  .supported,
             "incomplete selected constructor coverage cannot erase defaults");
    }
    auto malformed = frontend.mir;
    auto *body = lang::findMirBody(malformed, address);
    for (auto &block : body->blocks) {
      for (auto &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Call &&
            instruction.functionTarget) {
          instruction.functionTarget = 999999;
        }
      }
    }
    expect(!lang::verifyMirProgram(malformed).valid(),
           "a corrupt call target remains invalid MIR, not a known limitation");
  }
  expect(found, "the owning declaration default must be selected into its "
                "source constructor");
  bool emitted = false;
  try {
    const auto artifact = gti_test::emitCpp(frontend);
    emitted = !artifact.contents.empty();
  } catch (const std::exception &) {
  }
  expect(emitted,
         "the complete backend must emit the source-constructor field call");

  const auto mixed =
      analyzeWithStandardLibrary("mixed-generic-field-initializers.gti", R"(
#include <std/memory>
class Record<T> {
  T value;
  int status = 7;
public:
  Record(T value) : value(std::move(value)) {}
  int get() { return this.status; }
};
int inspect<T>(T value) {
  Record<T> record{std::move(value)};
  return record.get();
}
int main() {
  auto identity = [](int value) -> int { return value; };
  std::unique_ptr<Record<int>> absent{};
  Record<std::unique_ptr<int>> present{std::make_unique<int>(5)};
  return inspect(std::move(identity)) + present.get() - 14;
}
)");
  expect(mixed.canGenerateCode(),
         "mixed constructed and type-only generic instances are valid GTI");
  if (!mixed.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap mixedMap(buildRows(mixed, mixed.mir));
  std::size_t typeOnly = 0;
  std::size_t constructed = 0;
  for (const auto &owner : mixed.mir.classInstances()) {
    if (owner.declaredFields.size() != 2 ||
        owner.declaredFields.back().type != lang::SemanticType::Int32)
      continue;
    const bool selected = std::any_of(
        mixed.mir.constructorInstances().begin(),
        mixed.mir.constructorInstances().end(),
        [&](const auto &constructor) { return constructor.owner == owner.id; });
    selected ? ++constructed : ++typeOnly;
    expect(owner.defaultConstructor != lang::SpecialMemberStatus::Generated,
           "type-only catalog erasure must exclude generated construction");
    const lang::MirBodyAddress address{
        .kind = lang::MirBodyKind::FieldInitializers, .owner = owner.id};
    const auto schedule = lang::CppMirBodyEmitter(mixed.mir, mixedMap)
                              .initializerSchedule(address);
    expect(schedule.supported && schedule.fields.size() == 2 &&
               std::all_of(
                   schedule.fields.begin(), schedule.fields.end(),
                   [](const auto &field) { return field.spelling.empty(); }),
           "constructed and unconstructed instances share nonexecuting "
           "declaration-default catalogs");
  }
  expect(typeOnly == 1 && constructed == 2,
         "the regression must retain one unconstructed and two constructed "
         "applications of the same generic owner");
  bool mixedEmitted = false;
  try {
    mixedEmitted = !gti_test::emitCpp(mixed).contents.empty();
  } catch (const std::exception &) {
  }
  expect(mixedEmitted,
         "retained generic carriers must not reject type-only sibling "
         "initializer catalogs");
}

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
  const std::size_t selectedFieldRollbacks = std::count_if(
      ownerConstructor->body.dropObligations.begin(),
      ownerConstructor->body.dropObligations.end(),
      [](const lang::MirDropObligation &drop) {
        return drop.kind == lang::MirDropObligationKind::ConstructionRollback &&
               !drop.initiallyActive;
      });
  expect(defaultFieldRollbacks == 0 && selectedFieldRollbacks == 1 &&
             ownerConstructor->initializers.size() == 1 &&
             schedule.fields.front().spelling.empty(),
         "the source constructor must start with an inactive field obligation "
         "and execute its selected default before publishing the field");
  const lang::CppMirBodyEmissionAnalysis constructorAnalysis = emitter.analyze(
      {.kind = lang::MirBodyKind::Constructor, .owner = ownerConstructor->id});
  expect(constructorAnalysis.ready() &&
             !hasIssue(constructorAnalysis,
                       lang::CppMirBodyEmissionIssueKind::
                           MissingPartialConstructionRollbackMir),
         "the body emitter should consume the verified default-field "
         "rollback instead of reporting a construction gap");
  const lang::MirBodyAddress constructorAddress{
      .kind = lang::MirBodyKind::Constructor, .owner = ownerConstructor->id};
  expect(emitter.supportsFailureBodyText(constructorAddress) &&
             !emitter.supportsBodyText(constructorAddress),
         "an in-place failure constructor must not advertise a plain body "
         "that addresses its erased intermediate value slot");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const auto marker = [&](std::size_t constructor) {
    return artifact.contents.find(
               "scalar-cfg-constructor-failure-v1 constructor-instance " +
               std::to_string(constructor) + "\n") != std::string::npos;
  };
  expect(marker(ownerConstructor->id),
         "the ordinary Holder constructor should publish from verified MIR "
         "with checked field construction in its selected initialization "
         "schedule");
}

void testPreparedConstructorReplacement() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/recursive_cleanup_failure.gti";
  std::ifstream input(path);
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode(), "recursive replacement fixture lowers");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  bool found = false;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &producer : block.instructions) {
        if (producer.kind != lang::MirInstructionKind::Construct ||
            !producer.result || !producer.successResultDrop ||
            producer.lifecycle.size() != 1 ||
            producer.lifecycle.front().kind !=
                lang::MirLifecycleEventKind::TransferOut)
          continue;
        bool replacement = false;
        for (const auto &use : function.body.usesOf(*producer.result))
          for (const auto &candidateBlock : function.body.blocks)
            for (const auto &consumer : candidateBlock.instructions)
              if (consumer.id == use.instruction &&
                  consumer.kind == lang::MirInstructionKind::Assign)
                replacement = true;
        if (!replacement)
          continue;
        found = true;
        const lang::MirBodyAddress address{lang::MirBodyKind::Function,
                                           function.id};
        expect(lang::CppMirBodyEmitter(frontend.mir, map)
                   .supportsFailureBodyText(address),
               "prepared arguments transfer at invocation while the result "
               "activates only on success before replacement cleanup");
        for (int mutation = 0; mutation != 4; ++mutation) {
          auto malformed = frontend.mir;
          auto *body = lang::findMirBody(malformed, address);
          for (auto &changed : body->findBlock(block.id)->instructions) {
            if (changed.id != producer.id)
              continue;
            if (mutation == 0)
              changed.lifecycle.clear();
            if (mutation == 1)
              changed.lifecycle.front().source = *producer.successResultDrop;
            if (mutation == 2)
              changed.lifecycle.push_back(changed.lifecycle.front());
            if (mutation == 3)
              changed.lifecycle.front().conditional = true;
          }
          expect(!lang::CppMirBodyEmitter(malformed, map)
                      .supportsFailureBodyText(address),
                 "missing, mismatched, duplicate or conditional input handoff "
                 "cannot publish a replacement constructor result");
        }
        return;
      }
    }
  }
  expect(found, "fixture exercises prepared constructor replacement");
}

void testExpectedArgumentLoopTransport() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/expected_argument_loop.gti";
  std::ifstream input(path);
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode(), "expected loop arguments must verify");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  expect(emitter.analyzeProgram().ready(), "expected argument loops must emit");
  size_t retired = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    const lang::MirBodyAddress address{lang::MirBodyKind::Function,
                                       function.id};
    for (const auto &block : function.body.blocks) {
      for (const auto &stage : block.instructions) {
        if (stage.kind != lang::MirInstructionKind::CallInput ||
            !stage.preparedParameterDrop || stage.lifecycle.size() != 1)
          continue;
        const auto &event = stage.lifecycle.front();
        const auto *drop = function.body.findDropObligation(event.source);
        const auto *place =
            drop ? function.body.findPlace(drop->place) : nullptr;
        if (event.kind != lang::MirLifecycleEventKind::Reparent || !place ||
            place->root != lang::MirPlaceRootKind::Temporary)
          continue;
        ++retired;
        const auto text =
            emitter.emitFailureBodyText(address, "expected-loop", 1).text;
        expect(text.find("__gti_mir_p_" + std::to_string(place->id) +
                         ".destroy();") != std::string::npos,
               "reparented temporary carrier must retire before loop reuse");
        for (int mutation = 0; mutation != 2; ++mutation) {
          auto malformed = frontend.mir;
          auto *body = lang::findMirBody(malformed, address);
          for (auto &candidate : body->blocks)
            for (auto &instruction : candidate.instructions)
              if (instruction.id == stage.id) {
                if (mutation == 0)
                  instruction.lifecycle.clear();
                else
                  instruction.lifecycle.front().source =
                      *stage.preparedParameterDrop;
              }
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "prepared transfer must retain its exact temporary lifetime");
        }
      }
    }
  }
  expect(retired == 2,
         "free and member loop arguments must retire temporary carriers");
}

void testCopiedBorrowedArgumentAcrossInvoke() {
  testExpectedArgumentLoopTransport();
  {
    const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                         "fixtures/expected_temporary_observer.gti";
    std::ifstream input(fixture);
    const std::string source{std::istreambuf_iterator<char>(input), {}};
    const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
    expect(frontend.canGenerateCode(),
           "temporary state-only observers must verify");
    if (!frontend.canGenerateCode())
      return;
    for (const auto standard :
         {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23}) {
      const lang::CppMirBodyEmissionMap map(
          buildRows(frontend, frontend.mir, standard));
      expect(
          lang::CppMirBodyEmitter(frontend.mir, map).analyzeProgram().ready(),
          "state-only observation retains one placed Expected on both "
          "standards");
      size_t checked = 0;
      for (const auto &function : frontend.mir.functionInstances()) {
        const lang::MirBodyAddress address{lang::MirBodyKind::Function,
                                           function.id};
        for (const auto &block : function.body.blocks) {
          for (const auto &producer : block.instructions) {
            if (producer.kind != lang::MirInstructionKind::Call ||
                !producer.functionTarget || !producer.successResultDrop ||
                producer.info.type.kind != lang::SemanticType::Expected)
              continue;
            ++checked;
            for (int mutation = 0; mutation != 5; ++mutation) {
              auto malformed = frontend.mir;
              auto *body = lang::findMirBody(malformed, address);
              const auto *resultDrop =
                  body->findDropObligation(*producer.successResultDrop);
              const bool conditional =
                  body->findPlace(resultDrop->place)->root ==
                  lang::MirPlaceRootKind::Temporary;
              if (mutation >= 3 && !conditional)
                continue;
              for (auto &candidate : body->blocks)
                for (auto &instruction : candidate.instructions) {
                  if (mutation == 0 && instruction.id == producer.id)
                    instruction.successResultDrop.reset();
                  if (mutation == 1 &&
                      instruction.kind == lang::MirInstructionKind::Drop &&
                      !instruction.lifecycle.empty() &&
                      instruction.lifecycle.front().source ==
                          *producer.successResultDrop)
                    instruction.lifecycle.clear();
                  if (mutation == 4 &&
                      instruction.kind == lang::MirInstructionKind::Drop &&
                      !instruction.lifecycle.empty() &&
                      instruction.lifecycle.front().source ==
                          *producer.successResultDrop)
                    instruction.lifecycle.front().conditional = false;
                }
              if (mutation == 2) {
                auto *drop =
                    body->findDropObligation(*producer.successResultDrop);
                body->places[drop->place - 1].initiallyAvailable = true;
              }
              if (mutation == 3)
                body->places[resultDrop->place - 1].sourceValue = 0;
              if (mutation == 0 || mutation == 1 || mutation == 4)
                expect(
                    !lang::verifyMirProgram(malformed).valid(),
                    "observed temporary requires success-only publication and "
                    "exact cleanup");
              if (mutation != 1 && mutation != 4)
                expect(!lang::CppMirBodyEmitter(malformed, map)
                            .supportsFailureBodyText(address),
                       "observer storage proof must reject missing publication "
                       "or pre-live storage");
            }
          }
        }
      }
      expect(checked == 4,
             "both success and error temporary observers need exact storage");
    }
  }
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/owner_tied_by_value_argument.gti";
  std::ifstream input(path);
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode(), "borrowed copied arguments lower");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  bool found = false;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &value : function.body.values) {
      const auto uses = function.body.usesOf(value.id);
      if (value.info.type.kind != lang::SemanticType::Class ||
          !value.info.traits.containsBorrowedState || uses.size() != 1 ||
          uses.front().kind != lang::MirValueUseKind::InstructionOperand ||
          uses.front().block == value.definitionBlock)
        continue;
      const auto *block = function.body.findBlock(value.definitionBlock);
      const auto producer =
          std::find_if(block->instructions.begin(), block->instructions.end(),
                       [&](const auto &instruction) {
                         return instruction.id == value.definition;
                       });
      if (producer == block->instructions.end() ||
          producer->kind != lang::MirInstructionKind::CallInput ||
          producer->callInputKind != lang::HirCallInputKind::CopyValue ||
          !producer->preparedParameterDrop || producer->operands.size() != 1 ||
          producer->operands.front().kind != lang::MirOperandKind::Copy)
        continue;
      found = true;
      const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                         .owner = function.id};
      expect(
          lang::CppMirBodyEmitter(frontend.mir, map)
              .supportsFailureBodyText(address),
          "copy before a later argument Invoke has a prepared-parameter slot");
      for (int mutation = 0; mutation != 3; ++mutation) {
        auto malformed = frontend.mir;
        auto *body = lang::findMirBody(malformed, address);
        for (auto &candidateBlock : body->blocks)
          for (auto &instruction : candidateBlock.instructions)
            if (instruction.id == producer->id) {
              if (mutation == 0)
                instruction.operands.front().place = 0;
              if (mutation == 1)
                instruction.operands.front().kind = lang::MirOperandKind::Move;
            }
        if (mutation == 2)
          for (const auto &candidate : malformed.classInstances())
            if (candidate.type == value.info.type)
              const_cast<lang::MirClassInstance &>(candidate)
                  .requiresActiveCleanup = true;
        expect(!lang::verifyMirProgram(malformed).valid(),
               "copied argument requires exact Copy source and matching "
               "prepared cleanup before backend admission");
      }
      break;
    }
    if (found)
      break;
  }
  expect(found,
         "fixture must retain a prepared borrowed Copy across Invoke blocks");
}

void testNonFailingOwningDeclarationInitializer() {
  const auto frontend =
      analyzeWithStandardLibrary("non-failing-owning-default.gti", R"gti(
class Cleanup { public: Cleanup() {} ~Cleanup() {} };
class Holder {
  Cleanup cleanup = Cleanup();
public:
  int operator()(int amount) mut { return amount; }
};
interface Marker { int marker(); };
class Passive : public Marker {
public:
  Passive() {}
  int marker() override { return 1; }
};
class Mixed {
  int tag = 7;
  Passive value = Passive();
public:
  Mixed() {}
};
int main() { Mixed mixed{}; Holder value{}; return std::move(value)(3) - 3; }
)gti");
  expect(frontend.canGenerateCode(), "owning generated-default fixture lowers");
  if (!frontend.canGenerateCode())
    return;
  const lang::MirClassInstance *owner = nullptr;
  for (const auto &candidate : frontend.mir.classInstances())
    if (candidate.fieldInitializers.dropObligations.size() == 2)
      owner = &candidate;
  expect(owner != nullptr, "field default has value and rollback obligations");
  if (!owner)
    return;
  const lang::MirBodyAddress address{
      .kind = lang::MirBodyKind::FieldInitializers, .owner = owner->id};
  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const auto schedule =
      lang::CppMirBodyEmitter(frontend.mir, map).initializerSchedule(address);
  expect(schedule.supported && schedule.fields.size() == 1 &&
             !schedule.fields.front().spelling.empty() &&
             owner->fieldInitializers.failureRecords.empty(),
         "non-failing construction publishes its exact owning field default");
  expect(!gti_test::emitCppText(frontend).empty(),
         "production emission accepts the generated constructor and consuming "
         "fallback");
  const lang::MirConstructorInstance *mixed = nullptr;
  for (const auto &constructor : frontend.mir.constructorInstances())
    if (constructor.initializers.size() == 2 &&
        constructor.initializers.front().targetType ==
            lang::SemanticType::Int32)
      mixed = &constructor;
  expect(mixed != nullptr, "mixed declaration-default constructor exists");
  if (mixed) {
    const lang::MirBodyAddress mixedAddress{lang::MirBodyKind::Constructor,
                                            mixed->id};
    const auto *entry = mixed->body.findBlock(mixed->body.entry);
    const auto *success =
        entry && entry->terminator.kind == lang::MirTerminatorKind::Invoke
            ? mixed->body.findBlock(entry->terminator.target)
            : nullptr;
    expect(lang::CppMirBodyEmitter(frontend.mir, map)
                   .supportsFailureBodyText(mixedAddress) &&
               entry && success && !success->instructions.empty() &&
               success->instructions.front().kind ==
                   lang::MirInstructionKind::Initialize &&
               success->instructions.front().constructorInitializer == 2 &&
               std::count_if(
                   entry->instructions.begin(), entry->instructions.end(),
                   [](const auto &i) {
                     return i.kind == lang::MirInstructionKind::Initialize &&
                            i.constructorInitializer == 1;
                   }) == 1,
           "scalar default precedes construction; passive field publication "
           "follows its exact Invoke success edge");
    for (int mutation = 0; mutation != 4; ++mutation) {
      auto malformed = frontend.mir;
      auto &constructor = const_cast<lang::MirConstructorInstance &>(
          *malformed.findConstructorInstance(mixed->id));
      if (mutation == 0)
        std::swap(constructor.initializers[0], constructor.initializers[1]);
      if (mutation == 3 && entry) {
        auto &terminator = constructor.body.findBlock(entry->id)->terminator;
        terminator.kind = lang::MirTerminatorKind::Goto;
        terminator.invokeInstruction = 0;
        terminator.failureRecord = 0;
        terminator.elseTarget = 0;
      }
      for (auto &block : constructor.body.blocks)
        for (auto &instruction : block.instructions) {
          if (mutation == 1 && instruction.constructorInitializer == 1 &&
              instruction.kind == lang::MirInstructionKind::Initialize)
            instruction.operands.clear();
          if (mutation == 2 &&
              instruction.operation == lang::MirOperation::Literal)
            instruction.literal.reset();
        }
      expect(!lang::verifyMirProgram(malformed).valid(),
             "field order, exact publication, literal identity and the "
             "constructor failure edge remain mandatory");
    }
  }
  for (int mutation = 0; mutation != 4; ++mutation) {
    auto malformed = frontend.mir;
    auto *body = lang::findMirBody(malformed, address);
    for (auto &block : body->blocks) {
      for (auto &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Construct) {
          if (mutation == 0)
            instruction.lifecycle.clear();
          if (mutation == 3)
            instruction.lifecycle.front().conditional = true;
        }
        if (mutation == 1 &&
            instruction.kind == lang::MirInstructionKind::Initialize)
          instruction.lifecycle.clear();
        if (mutation == 2 &&
            instruction.kind == lang::MirInstructionKind::Lifecycle &&
            !instruction.lifecycle.empty())
          instruction.lifecycle.clear();
      }
    }
    expect(!lang::CppMirBodyEmitter(malformed, map)
                .initializerSchedule(address)
                .supported,
           "missing activation, reparent, handoff or conditional activation "
           "cannot erase cleanup");
  }
}

void testContainedOwningStringFieldConstructor() {
  const lang::FrontendResult frontend = analyzeWithStandardLibrary(
      "cpp-mir-body-owning-string-field-constructor.gti", R"(
#include <std/string>

class StringHolder {
public:
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
             artifact.contents.find("gti_rt_failure_terminate_v2") !=
                 std::string::npos,
         "the C++ artifact should emit the contained overload, delegate to "
         "the exact failure constructor, and terminate with its record");
}

void testBodyOrderedOwnedParameterConstructor() {
  for (const bool declaredDestructor : {false, true}) {
    std::string source = R"(
#include <std/string>

class OrderedPath {
public:
  OrderedPath(std::string value)
      : first(value.clone()), second(std::move(value)) {
    first.append("!");
  }
  // destructor-stage

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
)";
    if (declaredDestructor) {
      const std::string marker = "// destructor-stage";
      source.replace(source.find(marker), marker.size(), "~OrderedPath() {}");
    }
    const lang::FrontendResult frontend = analyzeWithStandardLibrary(
        "cpp-mir-body-ordered-owned-parameter-constructor.gti", source);
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

    expect(
        lang::cppMirFailureConstructorEmptyFieldsEligible(frontend.mir,
                                                          constructor->owner),
        "the constructor should have proved empty fields for body publication");
    expect(lang::cppMirFailureConstructorEmptyStateEligible(frontend.mir,
                                                            constructor->owner),
           "the private empty representation supports generated fields and "
           "an independently verified disarmed source destructor");
    if (declaredDestructor) {
      lang::MirProgram malformed = frontend.mir;
      auto &classes = const_cast<std::vector<lang::MirClassInstance> &>(
          malformed.classInstances());
      for (lang::MirClassInstance &owner : classes) {
        if (owner.id == constructor->owner) {
          owner.requiresActiveDropState = false;
        }
      }
      expect(
          !lang::cppMirFailureConstructorEmptyFieldsEligible(
              malformed, constructor->owner),
          "empty fields with a source destructor require the lifecycle guard");
      expect(!lang::cppMirFailureConstructorEmptyStateEligible(
                 malformed, constructor->owner),
             "private empty representation cannot activate an unguarded "
             "source destructor");
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
            .emitFailureBodyText(
                address, "body-ordered-owned-parameter-constructor-v0", 1)
            .text;
    const std::size_t clone = text.find("clone__gti_mir_failure");
    const std::size_t parameterMove =
        text.find("std::move(__gti_mir_p_", clone);
    expect(clone != std::string::npos && parameterMove != std::string::npos &&
               clone < parameterMove &&
               text.find("mir_rollback_field_v1(") != std::string::npos &&
               text.find("construction rollback deferred") == std::string::npos,
           "the emitted constructor should clone before moving the parameter "
           "and retain exact later-failure rollback");

    const lang::BackendArtifact artifact = gti_test::emitCpp(frontend);
    expect(!artifact.contents.empty() &&
               artifact.contents.find("mir_failure_constructor_tag_v1") !=
                   std::string::npos &&
               artifact.contents.find("mir_empty_state_v1") !=
                   std::string::npos,
           "production emission should build the transformed initializer list "
           "without dereferencing absent native owned-field bindings");

    lang::MirProgram mismatchedReceiver = frontend.mir;
    auto &constructors =
        const_cast<std::vector<lang::MirConstructorInstance> &>(
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
}

void testSourceDestructorOwningFieldCleanup() {
  for (const auto name : {"source_destructor_cleanup_failure.gti",
                          "temporary_constructor_argument_user_drop_gap.gti"}) {
    const auto fixture =
        std::filesystem::path(__FILE__).parent_path() / "fixtures" / name;
    std::ifstream input(fixture);
    expect(input.good(), "source-destructor cleanup fixture must exist");
    const std::string source{std::istreambuf_iterator<char>(input), {}};
    const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
    expect(frontend.canGenerateCode(), "source-destructor fixture must verify");
    if (!frontend.canGenerateCode())
      return;
    const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
    const lang::CppMirBodyEmitter emitter(frontend.mir, map);
    bool checked = false;
    for (const auto &constructor : frontend.mir.constructorInstances()) {
      const auto owned = std::find_if(
          constructor.initializers.begin(), constructor.initializers.end(),
          [&](const auto &initializer) {
            return initializer.ownedParameter &&
                   std::any_of(frontend.mir.classInstances().begin(),
                               frontend.mir.classInstances().end(),
                               [&](const auto &owner) {
                                 return owner.type == initializer.targetType &&
                                        owner.destructor;
                               });
          });
      if (owned == constructor.initializers.end())
        continue;
      checked = true;
      expect(
          !lang::cppMirOwnedParameterFieldBindings(frontend.mir, constructor),
          "fallible parameter cleanup requires body-ordered publication");
      const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Constructor,
                                         .owner = constructor.id};
      const auto text =
          emitter.emitFailureBodyText(address, "source-fields-v0", 1);
      expect(
          text.emitted() &&
              text.text.find(" = std::move(__gti_mir_v_") !=
                  std::string::npos &&
              text.text.find("field initialization publishes in the member") ==
                  std::string::npos &&
              text.text.find("__gti_mir_unique_cleanup_failure<") !=
                  std::string::npos,
          "the owned field must publish in MIR and roll back through its "
          "contained source destructor");
    }
    if (std::string_view(name) ==
        "temporary_constructor_argument_user_drop_gap.gti")
      expect(checked,
             "receiver regression must check its owned field publication");
    const auto artifact = gti_test::emitCpp(frontend);
    expect(artifact.contents.find(
               "if (!sequence.step(__gti_mir_destruct_failure(") !=
               std::string::npos,
           "source cleanup must execute before its aggregate children");
    if (std::string_view(name) == "source_destructor_cleanup_failure.gti") {
      expect(
          artifact.contents.find("Nested::Nested() {") == std::string::npos &&
              artifact.contents.find("Nested::Nested(::gti_internal::backend::"
                                     "mir_failure_constructor_tag_v1") !=
                  std::string::npos,
          "nested construction cannot emit a plain call to a status-only "
          "owning constructor");
    }
  }
}

void testOwningBaseConstructorStatus() {
  for (const auto fixtureName :
       {"inherited_unique_allocation.gti", "generated_base_constructor.gti",
        "generated_base_owner_constructor.gti"}) {
    const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                         "fixtures" / fixtureName;
    std::ifstream input(fixture);
    expect(input.good(), "the inherited-allocation fixture must exist");
    const std::string source{std::istreambuf_iterator<char>(input),
                             std::istreambuf_iterator<char>()};
    const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
    expect(frontend.canGenerateCode(), "the owning-base fixture must verify");
    if (!frontend.canGenerateCode())
      return;
    for (const auto &generated : frontend.hir.constructorInstances()) {
      if (!generated.generatedDefault)
        continue;
      expect(
          generated.declaration == 0 && generated.source == nullptr &&
              generated.initializers.size() == 1,
          "generated construction needs a selected body without synthetic AST");
      for (int mutation = 0; mutation != 7; ++mutation) {
        auto malformed = frontend.hir;
        auto *changed = const_cast<lang::HirConstructorInstance *>(
            malformed.findConstructorInstance(generated.id));
        if (mutation == 0)
          changed->generatedDefault = false;
        else if (mutation == 1)
          changed->initializers.front().constructorTarget = generated.id;
        else if (mutation == 2)
          changed->initializers.front().base = 999999;
        else if (mutation == 3)
          changed->initializers.front().targetType = lang::SemanticType::Int32;
        else if (mutation == 4)
          changed->body.roots.push_back(999999);
        else if (mutation == 5)
          changed->owner = 999999;
        else
          changed->declaration = 999999;
        expect(
            !lang::verifyHirProgramPlans(frontend.semantics, malformed).valid(),
            "generated construction must retain its exact semantic selection");
      }
      bool erasedCall = false;
      auto malformed = frontend.hir;
      for (const auto &function : malformed.functionInstances()) {
        auto *changed = const_cast<lang::HirFunctionInstance *>(
            malformed.findFunctionInstance(function.id));
        for (auto &value : changed->body.values) {
          if (value.constructorTarget == generated.id) {
            value.constructorTarget.reset();
            value.definedFailure.propagation =
                lang::FailurePropagationKind::None;
            erasedCall = true;
          }
        }
      }
      expect(erasedCall &&
                 !lang::verifyHirProgramPlans(frontend.semantics, malformed)
                      .valid(),
             "generated construction cannot erase both target and propagation "
             "into a native default call");
    }
    std::size_t selected = 0;
    for (const auto &constructor : frontend.mir.constructorInstances()) {
      const auto *entry = constructor.body.findBlock(constructor.body.entry);
      if (!entry || entry->instructions.empty() ||
          entry->instructions.front().kind !=
              lang::MirInstructionKind::InitializeBase)
        continue;
      ++selected;
      const auto &stage = entry->instructions.front();
      const auto bindings = lang::cppMirBaseParameterInitializerBindings(
          frontend.mir, constructor);
      expect(bindings && bindings->size() == 1 &&
                 bindings->front().parameters.empty() &&
                 bindings->front().constructor == stage.constructorTarget &&
                 bindings->front().initializer + 1 ==
                     stage.constructorInitializer,
             "zero-argument base initialization must retain its exact native "
             "binding");
      for (const auto standard :
           {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23}) {
        const lang::CppMirBodyEmissionMap map(
            buildRows(frontend, frontend.mir, standard));
        const lang::CppMirBodyEmitter emitter(frontend.mir, map);
        const lang::MirBodyAddress address{
            .kind = lang::MirBodyKind::Constructor, .owner = constructor.id};
        expect(emitter.supportsFailureBodyText(address) &&
                   !emitter.supportsBodyText(address),
               "base initialization must consume its MIR failure edge, not a "
               "terminal plain call");
        const auto text =
            emitter.emitFailureBodyText(address, "owning-base-test-v0", 1);
        expect(
            text.emitted() &&
                text.text.find(" = *__gti_mir_constructor_success;") !=
                    std::string::npos &&
                text.text.find("if (__gti_mir_construct_success_" +
                               std::to_string(stage.id) + ")") !=
                    std::string::npos &&
                text.text.find("auto &__gti_mir_p_" +
                               std::to_string(*stage.destination) + " =") ==
                    std::string::npos &&
                text.text.find("mir_rollback_field_v1(") == std::string::npos,
            "the base must use its status edge without a field alias or field "
            "replacement");
        auto malformed = frontend.mir;
        auto &constructors =
            const_cast<std::vector<lang::MirConstructorInstance> &>(
                malformed.constructorInstances());
        for (auto &candidate : constructors) {
          if (candidate.id == constructor.id)
            candidate.body.blocks[candidate.body.entry - 1]
                .instructions.front()
                .successResultDrop.reset();
        }
        expect(!lang::CppMirBodyEmitter(malformed, map)
                    .supportsFailureBodyText(address),
               "base status emission must reject missing rollback activation");
        for (int mutation = 0; mutation != 3; ++mutation) {
          auto forged = frontend.mir;
          auto *changed = const_cast<lang::MirConstructorInstance *>(
              forged.findConstructorInstance(constructor.id));
          if (mutation == 0)
            changed->initializers.front().base = 999999;
          else if (mutation == 1)
            changed->body.dropObligations[*stage.successResultDrop - 1]
                .dropType.requiresActiveCleanup =
                !changed->body.dropObligations[*stage.successResultDrop - 1]
                     .dropType.requiresActiveCleanup;
          else
            changed->body.blocks[changed->body.entry - 1]
                .terminator.successLifecycle.clear();
          expect(!lang::verifyMirProgram(forged).valid(),
                 "base identity, cleanup requirement and success activation "
                 "are exact");
        }
      }
    }
    expect(selected == 1,
           "the fixture must exercise exactly one owning-base stage");
  }
}

void testBodyOrderedPassivePolicyConstructor() {
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/owned_constructor_policy_forwarding.gti";
  std::ifstream input(fixture);
  expect(input.good(), "the passive policy forwarding fixture must exist");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
  expect(frontend.canGenerateCode(),
         "an owning constructor may move a passive policy after allocation");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  expect(emitter.analyzeProgram().ready(),
         "nested owning construction must preserve body-ordered policy moves");
  const lang::MirConstructorInstance *constructor = nullptr;
  for (const auto &candidate : frontend.mir.constructorInstances()) {
    if (candidate.initializers.size() == 2 &&
        !candidate.initializers.front().ownedParameter &&
        candidate.initializers.back().ownedParameter)
      constructor = &candidate;
  }
  expect(constructor != nullptr, "the fixture must have a late policy move");
  if (constructor == nullptr)
    return;
  expect(!lang::cppMirOwnedParameterFieldBindings(frontend.mir, *constructor),
         "the passive policy must not move before the earlier allocation");
  for (int mutation = 0; mutation != 3; ++mutation) {
    auto malformed = frontend.mir;
    auto *changed = const_cast<lang::MirConstructorInstance *>(
        malformed.findConstructorInstance(constructor->id));
    const auto argument = changed->initializers.back().arguments.front();
    bool applied = false;
    for (auto &block : changed->body.blocks) {
      for (auto &instruction : block.instructions) {
        if (mutation < 2 &&
            instruction.kind == lang::MirInstructionKind::Move &&
            instruction.hirValue == argument) {
          if (mutation == 0)
            instruction.ownership.reset();
          else
            instruction.lifecycle.push_back({});
          applied = true;
        }
        if (mutation == 2 && instruction.fullExpressionEnd != 0) {
          const auto expression = std::find_if(
              changed->body.fullExpressions.begin(),
              changed->body.fullExpressions.end(), [&](const auto &candidate) {
                return candidate.id == instruction.fullExpressionEnd;
              });
          if (expression != changed->body.fullExpressions.end() &&
              expression->constructorInitializer == 2) {
            instruction.fullExpressionEnd = 0;
            applied = true;
          }
        }
      }
    }
    expect(applied, "each passive publication mutation must change its proof");
    const lang::CppMirBodyEmitter malformedEmitter(malformed, map);
    expect(!malformedEmitter.analyzeProgram().ready(),
           "missing ownership/boundary or invented lifecycle must fail closed");
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
      expect(first.initializeInstruction != 0 && first.parameterDrop != 0 &&
                 second.initializeInstruction != 0,
             "a passive lexical class publishes through an explicit Initialize "
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
        constructor.initializers.size() != 2 ||
        constructor.initializers.front().targetType.kind !=
            lang::SemanticType::Class) {
      continue;
    }
    const auto bindings = lang::cppMirConstructedFieldInitializerBindings(
        classFields.mir, constructor);
    expect(bindings.size() == 2,
           "zero-argument class construction may precede a selected empty "
           "owner without losing its exact field publication");
    if (bindings.size() != 2) {
      continue;
    }
    ++defaultFieldConstructors;
    for (int mutation = 0; mutation < 3; ++mutation) {
      lang::MirProgram malformed = classFields.mir;
      auto *changed = const_cast<lang::MirConstructorInstance *>(
          malformed.findConstructorInstance(constructor.id));
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
          if (drop.id == bindings.back().fieldDrop) {
            drop.initiallyActive = true;
          }
        }
      } else {
        for (auto &block : changed->body.blocks) {
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
          "forged constructor identity, premature default-field cleanup, "
          "and observable selected defaults must fail the field proof");
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
                  candidate.destination == slot->id &&
                  candidate.lifecycle.size() == 1 &&
                  !candidate.lifecycle.front().failureCleanup) {
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
          const lang::MirBlock *commitBlock = &candidateBlock;
          auto consumerPosition = std::next(producerPosition);
          if (consumerPosition != candidateBlock.instructions.end() &&
              consumerPosition->kind ==
                  lang::MirInstructionKind::CleanupBeforeAssign) {
            if (consumerPosition->replacementPair != instruction.id ||
                instruction.replacementPair != consumerPosition->id ||
                consumerPosition->destination != instruction.destination ||
                !consumerPosition->lifecycle.empty() ||
                !consumerPosition->replacementCleanup)
              continue;
            if (consumerPosition->definedFailure.propagation ==
                lang::FailurePropagationKind::Destructor) {
              if (std::next(consumerPosition) !=
                      candidateBlock.instructions.end() ||
                  candidateBlock.terminator.kind !=
                      lang::MirTerminatorKind::Invoke ||
                  candidateBlock.terminator.invokeInstruction !=
                      consumerPosition->id)
                continue;
              commitBlock =
                  function.body.findBlock(candidateBlock.terminator.target);
              if (commitBlock == nullptr)
                continue;
              consumerPosition = commitBlock->instructions.begin();
            } else {
              ++consumerPosition;
            }
          }
          if (consumerPosition == commitBlock->instructions.end() ||
              consumerPosition->id != instruction.id) {
            continue;
          }
          const auto endBorrow = std::find_if(
              std::next(consumerPosition), commitBlock->instructions.end(),
              [&](const lang::MirInstruction &candidate) {
                return candidate.kind == lang::MirInstructionKind::EndBorrow &&
                       candidate.loan && *candidate.loan == source->loan;
              });
          const auto drop =
              std::find_if(endBorrow, commitBlock->instructions.end(),
                           [&](const lang::MirInstruction &candidate) {
                             return candidate.id == destroy->id;
                           });
          if (endBorrow != commitBlock->instructions.end() &&
              drop != commitBlock->instructions.end() && endBorrow < drop) {
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
             movedField.consumer->replacementPair != 0 &&
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
      !emitter.supportsNativeContainedBaseConstruction(derivedConstructor->id),
      "the derived constructor now needs its explicit base failure edge, "
      "not the older terminally contained native-base proof");

  const lang::CppMirBodyEmissionText baseText = emitter.emitBodyText(
      {.kind = lang::MirBodyKind::Constructor, .owner = baseConstructor->id},
      "contained-abstract-base-v0", 1);
  const lang::CppMirBodyEmissionText derivedText = emitter.emitFailureBodyText(
      {.kind = lang::MirBodyKind::Constructor, .owner = derivedConstructor->id},
      "contained-derived-v0", 1);
  expect(baseText.emitted(),
         "an abstract base's source constructor must remain directly "
         "spellable from MIR");
  expect(derivedText.emitted() &&
             derivedText.text.find(" = *__gti_mir_constructor_success;") !=
                 std::string::npos,
         "the derived constructor must consume the selected base's MIR status");
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

  auto missingStage = frontend.mir;
  auto *changedConstructor = const_cast<lang::MirConstructorInstance *>(
      missingStage.findConstructorInstance(constructor.id));
  bool detachedStage = false;
  for (auto &block : changedConstructor->body.blocks) {
    for (auto &instruction : block.instructions) {
      if (instruction.kind == lang::MirInstructionKind::Initialize &&
          instruction.constructorInitializer == 1) {
        instruction.constructorInitializer = 0;
        detachedStage = true;
      }
    }
  }
  const auto missingEffects =
      lang::deriveMirDefinedFailureEffects(missingStage);
  expect(detachedStage && !lang::verifyMirProgram(missingStage).valid() &&
             missingEffects.constructors[constructor.id - 1],
         "an aggregate computation without its exact field-write stage cannot "
         "prove a passive array constructor failure-free");

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
  bool checkedReceiverArray = false;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &place : function.body.places) {
      if (place.root == lang::MirPlaceRootKind::This &&
          place.type.kind == lang::SemanticType::Array &&
          place.projections.size() == 1 &&
          place.projections.front().kind == lang::MirProjectionKind::Field) {
        checkedReceiverArray =
            checkedReceiverArray ||
            contains("mir_checked_array_read_v1(__gti_mir_p_" +
                     std::to_string(place.id) + ", UINT64_C(0), &__gti_mir_v_");
      }
    }
  }
  expect(checkedReceiverArray,
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
    const lang::MirPlace *returnedSlot = nullptr;
    for (const auto &block : conditional->body.blocks) {
      if (block.terminator.kind != lang::MirTerminatorKind::Return ||
          !block.terminator.value)
        continue;
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Move ||
            instruction.result != block.terminator.value->value ||
            instruction.operands.size() != 1 ||
            instruction.operands.front().kind != lang::MirOperandKind::Move)
          continue;
        returnedSlot =
            conditional->body.findPlace(instruction.operands.front().place);
      }
    }
    expect(returnedSlot &&
               returnedSlot->root == lang::MirPlaceRootKind::Temporary &&
               returnedSlot->projections.empty() &&
               returnedSlot->type == conditional->returnType,
           "the final return must move the exact conditional owner temporary");
    if (returnedSlot) {
      std::size_t initializations = 0;
      std::size_t transfers = 0;
      std::size_t drops = 0;
      for (const auto &block : conditional->body.blocks) {
        for (const auto &instruction : block.instructions) {
          if (instruction.kind == lang::MirInstructionKind::Initialize &&
              instruction.destination == returnedSlot->id)
            ++initializations;
          for (const auto &event : instruction.lifecycle) {
            const auto *drop =
                conditional->body.findDropObligation(event.source);
            if (!drop || drop->place != returnedSlot->id)
              continue;
            if (event.kind == lang::MirLifecycleEventKind::TransferOut &&
                event.target == 0 && !event.conditional &&
                !event.failureCleanup &&
                drop->kind == lang::MirDropObligationKind::Value &&
                !drop->initiallyActive)
              ++transfers;
            if (event.kind == lang::MirLifecycleEventKind::Drop)
              ++drops;
          }
        }
      }
      expect(initializations == 2 && transfers == 1 && drops == 0,
             "both conditional arms must publish to one slot whose exact "
             "drop obligation transfers once without dropping the owner");
      const std::string slot =
          "__gti_mir_p_" + std::to_string(returnedSlot->id);
      const auto occurrences = [&](const std::string &needle) {
        std::size_t count = 0;
        for (auto offset = text.text.find(needle); offset != std::string::npos;
             offset = text.text.find(needle, offset + needle.size()))
          ++count;
        return count;
      };
      const auto publication =
          text.text.find("std::construct_at(__gti_mir_out_result, std::move(" +
                         slot + ".get()))");
      const auto retirement = text.text.find(slot + ".destroy();");
      expect(text.emitted() && occurrences(slot + ".construct(") == 2 &&
                 publication != std::string::npos &&
                 occurrences(slot + ".destroy();") == 1 &&
                 retirement > publication,
             "emission must construct either arm in that slot, move it into "
             "the result, then retire its moved-from shell exactly once");
    }
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
                       "::gti_failure_record_v2 *__gti_mir_failure_record) "
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
                                                "::gti_failure_record_v2{") !=
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
  auto formatted = std::try_format("value={}", int32_t(7));
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

  // Nominal call operators use the same owned Expected publication schedule
  // as named methods. This proof is independent of the standard wrappers.
  const auto callable =
      analyzeWithStandardLibrary("cpp-mir-callable-expected-result.gti", R"gti(
class Product {
  int value;
public:
  Product(int value) : value(value) {}
  ~Product() {}
};
struct Factory {
  expected<Product, int> operator()(int divisor) {
    int value = 6 / divisor;
    return Product(value);
  }
};
int main() {
  Factory factory{};
  auto result = factory(2);
  return 0;
}
)gti");
  expect(callable.canGenerateCode(),
         "nominal Expected-result invocation has valid frontend/MIR facts");
  if (!callable.canGenerateCode())
    return;
  const lang::MirFunctionInstance *caller = nullptr;
  const lang::MirInstruction *call = nullptr;
  for (const auto &function : callable.mir.functionInstances()) {
    if (function.entryKind == lang::ProgramEntryKind::None)
      continue;
    caller = &function;
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.kind == lang::MirInstructionKind::Call &&
            instruction.callableInvocation && instruction.functionTarget &&
            instruction.info.type.kind == lang::SemanticType::Expected)
          call = &instruction;
  }
  expect(caller && call && call->successResultDrop,
         "nominal invocation activates an exact Expected value obligation");
  if (!caller || !call || !call->successResultDrop)
    return;
  const lang::CppMirBodyEmissionMap callableRows(
      buildRows(callable, callable.mir));
  const lang::MirBodyAddress callerAddress{lang::MirBodyKind::Function,
                                           caller->id};
  expect(lang::CppMirBodyEmitter(callable.mir, callableRows)
             .supportsFailureBodyText(callerAddress),
         "Expected class results publish through ordinary call operators");
  for (int mutation = 0; mutation != 5; ++mutation) {
    auto malformed = callable.mir;
    auto *body = lang::findMirBody(malformed, callerAddress);
    bool changed = false;
    for (auto &block : body->blocks) {
      for (auto &instruction : block.instructions) {
        if (instruction.id == call->id) {
          if (mutation == 0) {
            instruction.functionTarget.reset();
            changed = true;
          } else if (mutation == 1) {
            instruction.successResultDrop.reset();
            changed = true;
          }
        }
        if (instruction.kind != lang::MirInstructionKind::Initialize ||
            instruction.operands.size() != 1 ||
            instruction.operands.front().value != call->result)
          continue;
        if (mutation == 2) {
          instruction.lifecycle.clear();
          changed = true;
        } else if (mutation == 3 && !instruction.lifecycle.empty()) {
          instruction.lifecycle.front().source = 0;
          changed = true;
        } else if (mutation == 4 && !instruction.lifecycle.empty()) {
          instruction.lifecycle.front().conditional = true;
          changed = true;
        }
      }
    }
    expect(changed && !lang::CppMirBodyEmitter(malformed, callableRows)
                           .supportsFailureBodyText(callerAddress),
           "callable Expected publication rejects missing target, activation "
           "and missing or forged binding reparent events");
  }
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
  expect(text.find(".construction_address()") != std::string::npos &&
             text.find(".mark_constructed();") != std::string::npos,
         "inner owners should publish into uninitialized result storage");

  const lang::MirInstruction *stage = nullptr;
  const lang::MirInstruction *outer = nullptr;
  bool readReceiver = false;
  bool mutableReceiver = false;
  bool readArgument = false;
  std::size_t constructorStages = 0;
  for (const lang::MirBlock &block : entry->body.blocks) {
    for (const lang::MirInstruction &instruction : block.instructions) {
      if (instruction.kind != lang::MirInstructionKind::CallInput) {
        continue;
      }
      if (instruction.callInputKind == lang::HirCallInputKind::MoveValue &&
          instruction.preparedParameterDrop && instruction.destination &&
          instruction.lifecycle.size() == 1 &&
          instruction.lifecycle.front().kind ==
              lang::MirLifecycleEventKind::Reparent) {
        for (const auto &candidate : entry->body.blocks)
          for (const auto &consumer : candidate.instructions)
            if (consumer.kind == lang::MirInstructionKind::Construct &&
                consumer.callSite == instruction.callSite) {
              ++constructorStages;
              stage = &instruction;
              const auto slot =
                  "__gti_mir_p_" + std::to_string(*instruction.destination);
              expect(text.find(slot + ".take()") != std::string::npos,
                     "constructor handoff must move and retire each prepared "
                     "representation");
            }
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
  expect(stage != nullptr && constructorStages == 3 && readReceiver &&
             mutableReceiver && readArgument,
         "MIR should retain all three prepared arguments and temporary borrow "
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

void testDiscardedCleanupFreeClassResult() {
  const auto frontend = analyze("discarded-class-result.gti", R"(
struct Packet {
  int value;
  Packet(int value) : value(value) {}
};
Packet make_packet(int divisor) { return Packet(12 / divisor); }
int main() { [[discard]] make_packet(2); return 0; }
)");
  expect(frontend.canGenerateCode(), "discarded nominal results must verify");
  if (!frontend.canGenerateCode())
    return;
  for (const auto &entry : frontend.mir.functionInstances()) {
    if (entry.entryKind != lang::ProgramEntryKind::NoArguments)
      continue;
    lang::MirValueId result = 0;
    lang::SemanticType resultType;
    for (const auto &block : entry.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Call &&
            instruction.functionTarget && instruction.result &&
            instruction.info.type.kind == lang::SemanticType::Class) {
          result = *instruction.result;
          resultType = instruction.info.type;
        }
      }
    }
    expect(result != 0 && entry.body.usesOf(result).empty(),
           "the call must retain an unused nominal SSA result");
    const lang::MirBodyAddress address{lang::MirBodyKind::Function, entry.id};
    const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
    const lang::CppMirBodyEmitter emitter(frontend.mir, map);
    expect(emitter.supportsFailureBodyText(address),
           "unused cleanup-free results must admit transformed emission");
    if (!emitter.supportsFailureBodyText(address))
      return;
    const auto text =
        emitter.emitFailureBodyText(address, "discard-result", 1).text;
    const auto slot = "__gti_mir_v_" + std::to_string(result);
    const auto construct = text.find(slot + ".construction_address()");
    const auto activate = text.find(slot + ".mark_constructed()", construct);
    const auto destroy = text.find(slot + ".destroy()", activate);
    expect(construct != std::string::npos && activate != std::string::npos &&
               destroy != std::string::npos,
           "successful placement must retire the discarded representation");
    for (int mutation = 0; mutation != 2; ++mutation) {
      auto malformed = frontend.mir;
      auto &classes = const_cast<std::vector<lang::MirClassInstance> &>(
          malformed.classInstances());
      bool changed = false;
      for (auto &owner : classes) {
        if (owner.type != resultType)
          continue;
        changed = true;
        if (mutation == 0)
          owner.requiresActiveCleanup = true;
        else
          owner.traits.containsBorrowedState = true;
      }
      const lang::CppMirBodyEmitter rejected(malformed, map);
      expect(changed && !rejected.supportsFailureBodyText(address),
             "discard storage must not absorb semantic cleanup or borrows");
    }
  }
}

void testFreshOwnedCallableArgument() {
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/owning_function_policy_failure.gti";
  std::ifstream input(fixture);
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
  expect(frontend.canGenerateCode(), "fresh owning callable arguments verify");
  if (!frontend.canGenerateCode())
    return;
  const auto emitted = gti_test::emitCpp(frontend);
  expect(emitted.contents.find(
             "fresh closure already occupies its prepared argument slot") !=
             std::string::npos,
         "fresh closure publication must use prepared storage at its creation "
         "step");
  std::size_t stages = 0;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  for (const auto &function : frontend.mir.functionInstances()) {
    if (function.entryKind != lang::ProgramEntryKind::OwnedArguments)
      continue;
    for (const auto &block : function.body.blocks) {
      for (const auto &stage : block.instructions) {
        if (stage.kind != lang::MirInstructionKind::CallInput ||
            stage.info.type.kind != lang::SemanticType::Lambda ||
            stage.lifecycle.size() != 1 ||
            stage.lifecycle.front().kind !=
                lang::MirLifecycleEventKind::Reparent)
          continue;
        ++stages;
        for (int mutation = 0; mutation != 3; ++mutation) {
          auto malformed = frontend.mir;
          auto *body = lang::findMirBody(
              malformed, {lang::MirBodyKind::Function, function.id});
          for (auto &changed : body->findBlock(block.id)->instructions) {
            if (changed.id != stage.id)
              continue;
            if (mutation == 0)
              changed.lifecycle.front().source = 0;
            else if (mutation == 1)
              changed.lifecycle.front().kind =
                  lang::MirLifecycleEventKind::Initialize;
            else
              changed.callInputKind = lang::HirCallInputKind::CopyValue;
          }
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "fresh closure staging cannot invent, duplicate or copy "
                 "ownership");
          expect(!lang::CppMirBodyEmitter(malformed, map)
                      .supportsFailureBodyText(
                          {lang::MirBodyKind::Function, function.id}),
                 "fresh closure representation rejects forged preparation");
        }
      }
    }
  }
  expect(stages != 0,
         "fresh replacement closure must exercise reparented preparation");
}

void testErasedOwnedResultReturn() {
  for (const char *name : {"owning_function_resource_result.gti",
                           "owning_function_string_result.gti"}) {
    const auto fixture =
        std::filesystem::path(__FILE__).parent_path() / "fixtures" / name;
    std::ifstream input(fixture);
    expect(input.good(), "owned callback result fixture must exist");
    const std::string source{std::istreambuf_iterator<char>(input), {}};
    const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
    expect(frontend.canGenerateCode(), "owned callback results must verify");
    if (!frontend.canGenerateCode())
      return;
    const auto lowered =
        gti_test::lowerProgram(frontend, frontend.mir, frontend.mir);
    const auto baseRows =
        lang::buildCppMirBodyEmissionMapRows(lowered, lang::CppStandard::Cpp23);
    expect(!gti_test::emitCpp(frontend).contents.empty(),
           "owned erased callback results must pass production preflight");
    std::size_t returns = 0;
    for (const auto &function : frontend.mir.functionInstances()) {
      for (const auto &block : function.body.blocks) {
        for (const auto &call : block.instructions) {
          if (call.kind != lang::MirInstructionKind::Call ||
              (call.dispatch != lang::CallDispatch::Virtual &&
               !call.lambdaTarget) ||
              !call.result || !call.successResultDrop ||
              call.info.type != function.returnType ||
              call.info.type.kind != lang::SemanticType::Class)
            continue;
          ++returns;
          const lang::MirBodyAddress address{lang::MirBodyKind::Function,
                                             function.id};
          // Stored closures have an unnameable concrete type. Production
          // emits their generic model through the exact declaration overlay.
          auto rows = baseRows;
          const auto *declaration =
              lowered.findFunctionDeclaration(function.declaration);
          expect(declaration != nullptr, "return target declaration exists");
          if (declaration == nullptr)
            continue;
          const auto overlay = lang::cppMirApplyCallableTemplateTypeOverlays(
              rows, lowered, lang::CppStandard::Cpp23, *declaration, function);
          expect(overlay.has_value(), "return target overlay is coherent");
          const lang::CppMirBodyEmissionMap map(std::move(rows));
          const lang::CppMirBodyEmitter emitter(frontend.mir, map);
          expect(emitter.supportsFailureBodyText(address),
                 "owned virtual results need the failure-aware return path");
          if (emitter.supportsFailureBodyText(address)) {
            const auto text =
                emitter.emitFailureBodyText(address, "owned-return", 1).text;
            expect(text.find("__gti_mir_out_result") != std::string::npos,
                   "result storage belongs to the caller of the wrapper");
          }
          for (int mutation = 0; mutation != 3; ++mutation) {
            auto malformed = frontend.mir;
            auto *body = lang::findMirBody(malformed, address);
            for (auto &changed : body->findBlock(block.id)->instructions) {
              if (changed.id != call.id)
                continue;
              if (mutation == 0)
                changed.successResultDrop.reset();
              else if (mutation == 1) {
                if (call.lambdaTarget)
                  changed.lambdaTarget.reset();
                else
                  changed.dispatchOwner = lang::SemanticType::Int32;
              } else
                changed.definedFailure.propagation =
                    lang::FailurePropagationKind::DirectCall;
            }
            expect(!lang::verifyMirProgram(malformed).valid(),
                   "result activation, virtual owner and failure dispatch "
                   "cannot be forged");
          }
        }
      }
    }
    expect(returns != 0,
           "fixture must exercise owned virtual return forwarding");
  }
}

void testConstructedCallbackConditionalArgument() {
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/constructed_callback_argument.gti";
  std::ifstream input(fixture);
  expect(input.good(), "constructed callback argument fixture must exist");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
  expect(frontend.canGenerateCode(), "constructed callback inputs must verify");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  expect(emitter.analyzeProgram().ready(),
         "fresh class arguments followed by conditional arguments must emit");
  std::size_t constructors = 0;
  for (const auto &entry : frontend.mir.functionInstances()) {
    if (entry.entryKind != lang::ProgramEntryKind::OwnedArguments)
      continue;
    for (const auto &block : entry.body.blocks) {
      for (const auto &construct : block.instructions) {
        if (construct.kind != lang::MirInstructionKind::Construct ||
            !construct.constructorTarget || !construct.result ||
            construct.successResultDrop || construct.successResultDestination ||
            construct.operands.size() != 2)
          continue;
        ++constructors;
        expect(block.terminator.kind == lang::MirTerminatorKind::Invoke &&
                   block.terminator.invokeInstruction == construct.id,
               "cleanup-free constructors still need failure routing");
        const lang::MirInstruction *stage = nullptr;
        for (const auto &candidate : entry.body.blocks) {
          for (const auto &instruction : candidate.instructions) {
            if (instruction.kind == lang::MirInstructionKind::CallInput &&
                instruction.operands.size() == 1 &&
                instruction.operands.front().kind ==
                    lang::MirOperandKind::Value &&
                instruction.operands.front().value == *construct.result) {
              stage = &instruction;
              expect(candidate.id == block.terminator.target,
                     "only successful construction can prepare an argument");
            }
          }
        }
        expect(stage && stage->preparedParameterDrop && stage->destination,
               "a fresh argument must retain its exact preparation slot");
        if (!stage || !stage->preparedParameterDrop)
          continue;
        for (int mutation = 0; mutation != 3; ++mutation) {
          auto malformed = frontend.mir;
          auto *body = lang::findMirBody(
              malformed, {lang::MirBodyKind::Function, entry.id});
          auto &term = body->findBlock(block.id)->terminator;
          if (mutation == 0) {
            term.kind = lang::MirTerminatorKind::Goto;
            term.invokeInstruction = 0;
            term.failureRecord = 0;
            term.elseTarget = 0;
          } else if (mutation == 1) {
            std::swap(term.target, term.elseTarget);
          } else {
            body->dropObligations[*stage->preparedParameterDrop - 1]
                .initiallyActive = true;
          }
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "missing failure edge, reversed publication edge, and "
                 "premature lifetime must reject");
        }
      }
    }
  }
  expect(constructors == 3, "fixture must cover three prepared constructors");
}

void testGenericScalarConstructedFieldPublication() {
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/associative_node_constructor.gti";
  std::ifstream input(fixture);
  expect(input.good(), "generic constructed-field fixture must exist");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
  expect(frontend.canGenerateCode(),
         "generic scalar node constructors must verify");
  if (!frontend.canGenerateCode())
    return;
  expect(!gti_test::emitCpp(frontend).contents.empty(),
         "generic scalar move constructors must publish native fields");
  std::size_t nodes = 0;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    if (constructor.initializers.size() != 3 ||
        constructor.initializers.front().targetType.kind !=
            lang::SemanticType::Class)
      continue;
    const lang::MirInstruction *construct = nullptr;
    for (const auto &block : constructor.body.blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.kind == lang::MirInstructionKind::Construct &&
            instruction.operands.size() == 2 &&
            testFieldPublication(constructor.body, instruction))
          construct = &instruction;
    expect(construct != nullptr,
           "node entry must retain both exact scalar moved arguments");
    if (!construct)
      continue;
    const auto *publication =
        testFieldPublication(constructor.body, *construct);
    const auto *stagedValue =
        constructor.body.findValue(construct->operands.front().value);
    const auto *stage =
        stagedValue ? testInstruction(constructor.body, stagedValue->definition)
                    : nullptr;
    expect(stage && stage->kind == lang::MirInstructionKind::CallInput &&
               stage->callInputIndex == 0 && publication &&
               publication->constructorInitializer == 1,
           "generic construction retains ordered input and field identity");
    if (!stage || !publication)
      continue;
    ++nodes;
    for (int mutation = 0; mutation < 4; ++mutation) {
      auto malformed = frontend.mir;
      auto *changed = const_cast<lang::MirConstructorInstance *>(
          malformed.findConstructorInstance(constructor.id));
      for (auto &block : changed->body.blocks) {
        for (auto &instruction : block.instructions) {
          if (mutation == 0 && instruction.id == stage->id)
            instruction.operands.front().type =
                lang::SemanticType(lang::SemanticType::Bool);
          if (mutation == 1 && instruction.id == stage->id)
            instruction.callInputIndex = 1;
          if (mutation == 2 && instruction.id == publication->id)
            instruction.constructorInitializer = 2;
        }
      }
      if (mutation == 3) {
        auto *target = const_cast<lang::MirConstructorInstance *>(
            malformed.findConstructorInstance(*construct->constructorTarget));
        for (auto &instruction : target->body.blocks.front().instructions)
          if (instruction.kind == lang::MirInstructionKind::Lifecycle)
            instruction.fullExpressionEnd = 0;
      }
      expect(!lang::verifyMirProgram(malformed).valid(),
             "forged moved type, argument index, field identity, or target "
             "boundary must reject");
    }
  }
  expect(nodes == 2,
         "both concrete generic node instances must exercise the proof");
}

void testConservativeConstructorAssignmentFusion() {
  const auto frontend = analyzeWithStandardLibrary(
      "cpp-mir-conservative-constructor-assignment.gti", R"gti(
#include <std/memory>
interface Operation { int call(int value); };
class Passive : public Operation {
public:
  Passive() {}
  int call(int value) override { return value; }
};
int main() {
  mut auto owner = std::make_unique<Passive>();
  *owner = Passive();
  return 0;
}
)gti");
  expect(frontend.canGenerateCode(),
         "passive borrowed replacement must verify");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  const auto entry = std::find_if(
      frontend.mir.functionInstances().begin(),
      frontend.mir.functionInstances().end(), [](const auto &function) {
        return function.entryKind == lang::ProgramEntryKind::NoArguments;
      });
  expect(entry != frontend.mir.functionInstances().end(),
         "replacement fixture must contain its entry body");
  if (entry == frontend.mir.functionInstances().end())
    return;
  const lang::MirBodyAddress address{lang::MirBodyKind::Function, entry->id};
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  expect(emitter.supportsFailureBodyText(address),
         "a conservative nonfailing constructor edge must retain adjacent "
         "borrowed replacement without inventing a temporary lifetime");
  std::size_t constructions = 0;
  for (const auto &block : entry->body.blocks) {
    const auto *producer =
        testInstruction(entry->body, block.terminator.invokeInstruction);
    const auto *success = entry->body.findBlock(block.terminator.target);
    if (!producer || producer->kind != lang::MirInstructionKind::Construct ||
        !success || success->instructions.size() < 2 ||
        success->instructions.front().kind !=
            lang::MirInstructionKind::ActivateBorrow)
      continue;
    ++constructions;
    const auto text =
        emitter.emitFailureBodyText(address, "borrowed-replace", 1).text;
    expect(text.find("fuses into binding assignment") != std::string::npos &&
               text.find("if (__gti_mir_construct_success_" +
                         std::to_string(producer->id) + ")") ==
                   std::string::npos,
           "fused construction must not leave a false status branch");
    for (int mutation = 0; mutation != 5; ++mutation) {
      auto malformed = frontend.mir;
      auto *body = lang::findMirBody(malformed, address);
      auto &changed = body->blocks[success->id - 1];
      auto &invoke = body->blocks[block.id - 1].terminator;
      if (mutation == 0)
        changed.instructions.front().loan = 0;
      if (mutation == 1)
        changed.instructions.front().fullExpressionEnd = 1;
      if (mutation == 2)
        invoke.elseTarget = invoke.target;
      if (mutation == 3)
        invoke.successLifecycle.push_back(
            {.kind = lang::MirLifecycleEventKind::Initialize, .target = 1});
      if (mutation == 4)
        std::swap(changed.instructions[0], changed.instructions[1]);
      expect(!lang::CppMirBodyEmitter(malformed, map)
                  .supportsFailureBodyText(address),
             "fusion must reject a forged activation, intervening boundary, "
             "extra edge, lifetime publication or changed evaluation order " +
                 std::to_string(mutation));
    }
  }
  expect(constructions == 1, "fixture must exercise one cross-Invoke fusion");
  expect(!gti_test::emitCpp(frontend).contents.empty(),
         "polymorphic replacement must retain production emission");
}

void testNestedConstructorRepresentationPrecedence() {
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/issue_117_generic_indirect.gti";
  std::ifstream input(fixture);
  expect(input.good(), "nested generic construction fixture must exist");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
  expect(frontend.canGenerateCode(), "nested default constructors must verify");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  std::size_t claimed = 0;
  std::size_t restoredPlacement = 0;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    const auto bindings = lang::cppMirConstructedFieldInitializerBindings(
        frontend.mir, constructor);
    for (const auto &binding : bindings) {
      const auto *producer =
          testInstruction(constructor.body, binding.constructInstruction);
      if (!binding.constructedValue || !producer ||
          producer->kind != lang::MirInstructionKind::Construct ||
          !producer->constructorTarget)
        continue;
      ++claimed;
      expect(!lang::cppMirConstructorRequiresFieldPlacement(frontend.mir,
                                                            constructor) &&
                 emitter.supportsBodyText(
                     {lang::MirBodyKind::Constructor, constructor.id}),
             "exact native initializer publication must not also require "
             "body placement or hide the ordinary constructor definition");
      auto malformed = frontend.mir;
      auto *changed = const_cast<lang::MirConstructorInstance *>(
          malformed.findConstructorInstance(constructor.id));
      for (auto &block : changed->body.blocks)
        for (auto &instruction : block.instructions)
          if (instruction.id == binding.constructInstruction)
            instruction.callSite = instruction.hirValue + 10000;
      expect(
          lang::cppMirConstructedFieldInitializerBindings(malformed, *changed)
              .empty(),
          "a forged call identity must lose native initializer ownership");
      if (lang::cppMirConstructorRequiresFieldPlacement(malformed, *changed)) {
        ++restoredPlacement;
        expect(!lang::CppMirBodyEmitter(malformed, map)
                    .supportsBodyText(
                        {lang::MirBodyKind::Constructor, constructor.id}),
               "unclaimed direct construction must retain status-only "
               "placement, not an ordinary overload using an erased value");
      }
      malformed = frontend.mir;
      changed = const_cast<lang::MirConstructorInstance *>(
          malformed.findConstructorInstance(constructor.id));
      for (auto &block : changed->body.blocks)
        for (auto &instruction : block.instructions)
          if (instruction.id == binding.initializeInstruction)
            instruction.constructorInitializer = 0;
      expect(!lang::verifyMirProgram(malformed).valid(),
             "native publication cannot detach its exact field identity");
    }
  }
  expect(claimed != 0 && restoredPlacement != 0,
         "fixture must exercise competing native and placement proofs");
  expect(!gti_test::emitCpp(frontend).contents.empty(),
         "nested generic constructors must publish consistent definitions");
}

void testOwningConstructedFieldInputTransfers() {
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/associative_owning_node_constructor.gti";
  std::ifstream input(fixture);
  expect(input.good(), "owning nested constructor fixture must exist");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
  expect(frontend.canGenerateCode(), "owning nested constructors must verify");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  expect(emitter.analyzeProgram().ready(),
         "owning field construction must preserve prepared input transfers");
  std::size_t transfers = 0;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    if (constructor.initializers.size() != 3)
      continue;
    for (const auto &block : constructor.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Construct ||
            instruction.lifecycle.empty())
          continue;
        ++transfers;
        expect(instruction.successResultDrop.has_value(),
               "field result activates only on constructor success");
        auto malformed = frontend.mir;
        auto *body = lang::findMirBody(
            malformed, {lang::MirBodyKind::Constructor, constructor.id});
        for (auto &changed : body->findBlock(block.id)->instructions)
          if (changed.id == instruction.id)
            changed.lifecycle.clear();
        expect(!lang::verifyMirProgram(malformed).valid(),
               "constructor cannot omit owning input handoff");
      }
    }
  }
  expect(transfers == 2,
         "one- and two-owner field constructors must be exercised");
}

void testMovedClassArgumentBeforeCheckedArgument() {
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/associative_owned_argument.gti";
  std::ifstream input(fixture);
  expect(input.good(), "owned argument staging fixture must exist");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
  expect(frontend.canGenerateCode(), "owned argument staging must verify");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  expect(emitter.analyzeProgram().ready(),
         "a prepared class move must survive later argument failure edges");
  std::size_t stages = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &stage : block.instructions) {
        if (stage.kind != lang::MirInstructionKind::CallInput ||
            stage.callInputKind != lang::HirCallInputKind::MoveValue ||
            stage.info.type.kind != lang::SemanticType::Class ||
            !stage.preparedParameterDrop || !stage.result || !stage.destination)
          continue;
        const auto uses = function.body.usesOf(*stage.result);
        if (uses.size() != 1 || uses.front().block == block.id)
          continue;
        ++stages;
        const lang::MirBodyAddress address{lang::MirBodyKind::Function,
                                           function.id};
        const auto text =
            emitter.emitFailureBodyText(address, "prepared", 1).text;
        const auto slot = "__gti_mir_p_" + std::to_string(*stage.destination);
        expect(
            text.find(slot + ".construct(std::move(") != std::string::npos &&
                text.find("std::move(" + slot + ".get())") !=
                    std::string::npos &&
                text.find(slot + ".destroy()") != std::string::npos,
            "prepare, call transfer, and retirement must share the exact slot");
        auto malformed = frontend.mir;
        auto *body = lang::findMirBody(malformed, address);
        body->dropObligations[*stage.preparedParameterDrop - 1]
            .initiallyActive = true;
        expect(!lang::verifyMirProgram(malformed).valid(),
               "prepared argument cannot be active before its move initializes "
               "it");
      }
    }
  }
  expect(stages != 0,
         "fixture must move a class across a later checked argument");
}

void testBorrowedFieldCompoundAssignment() {
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/associative_iterator_compound.gti";
  std::ifstream input(fixture);
  expect(input.good(), "borrowed field compound assignment fixture must exist");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
  expect(frontend.canGenerateCode(),
         "iterator compound assignment must verify");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  expect(emitter.analyzeProgram().ready(),
         "checked compound assignment must retain its borrowed field lvalue");
  const auto definition =
      [](const lang::MirBody &body,
         lang::MirValueId valueId) -> const lang::MirInstruction * {
    const auto *value = body.findValue(valueId);
    const auto *block =
        value ? body.findBlock(value->definitionBlock) : nullptr;
    if (block != nullptr)
      for (const auto &instruction : block->instructions)
        if (instruction.id == value->definition)
          return &instruction;
    return nullptr;
  };
  std::size_t assignments = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Assign ||
            instruction.operation != lang::MirOperation::Assign ||
            !instruction.destination || instruction.operands.size() != 1 ||
            instruction.operands.front().kind != lang::MirOperandKind::Value)
          continue;
        const auto *place = function.body.findPlace(*instruction.destination);
        if (!place || place->root != lang::MirPlaceRootKind::Binding ||
            place->projections.size() != 2 ||
            place->projections.front().kind !=
                lang::MirProjectionKind::Dereference ||
            place->projections.back().kind != lang::MirProjectionKind::Field)
          continue;
        const auto *sum =
            definition(function.body, instruction.operands.front().value);
        if (!sum || sum->kind != lang::MirInstructionKind::Compute ||
            sum->operation != lang::MirOperation::Add ||
            sum->operands.size() != 2 ||
            sum->operands.front().kind != lang::MirOperandKind::Value ||
            !sum->result)
          continue;
        ++assignments;
        const auto *read =
            definition(function.body, sum->operands.front().value);
        const auto *sumValue = function.body.findValue(*sum->result);
        const auto *sumBlock =
            sumValue ? function.body.findBlock(sumValue->definitionBlock)
                     : nullptr;
        expect(read && read->kind == lang::MirInstructionKind::Load &&
                   read->operands.size() == 1 &&
                   read->operands.front().kind == lang::MirOperandKind::Copy &&
                   read->operands.front().place == place->id &&
                   read->info.type == place->type &&
                   sum->info.type == place->type,
               "borrowed compound update reads the exact checked field before "
               "computing its sum");
        expect(sum->localFailureSites.size() == 1 &&
                   sum->definedFailure.localOrigins.size() == 1 && sumBlock &&
                   sumBlock->terminator.kind ==
                       lang::MirTerminatorKind::Invoke &&
                   sumBlock->terminator.invokeInstruction == sum->id &&
                   sumBlock->terminator.target == block.id &&
                   sumBlock->terminator.elseTarget != block.id,
               "borrowed field commit occurs only after the checked addition "
               "succeeds");
        auto malformed = frontend.mir;
        auto *body = lang::findMirBody(
            malformed, {lang::MirBodyKind::Function, function.id});
        body->places[place->id - 1].projections.back().field = 0;
        expect(!lang::verifyMirProgram(malformed).valid(),
               "compound assignment cannot forge a borrowed field projection");
      }
    }
  }
  expect(assignments == 1,
         "one projected compound assignment must exercise this route");
}

void testBorrowedTemporaryOperatorReceiver() {
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/borrowed_temporary_operator_receiver.gti";
  std::ifstream input(fixture);
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
  expect(frontend.canGenerateCode(),
         "borrowed temporary operators must verify");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap rows(buildRows(frontend, frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, rows);
  expect(emitter.analyzeProgram().ready(),
         "borrowed operator receivers must alias their exact lifetime slot");
  std::size_t receivers = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::CallInput ||
            instruction.callInputKind !=
                lang::HirCallInputKind::ReadTemporaryBorrow ||
            instruction.callInputRole != lang::MirCallInputRole::Receiver ||
            !instruction.info.traits.containsBorrowedState)
          continue;
        ++receivers;
        const auto *loan = function.body.findLoan(instruction.loan.value_or(0));
        const auto *place =
            loan == nullptr ? nullptr : function.body.findPlace(loan->source);
        const auto dependency = place == nullptr
                                    ? lang::MirTemporaryCarrierDependency{}
                                    : lang::findMirTemporaryCarrierDependency(
                                          function.body, *place);
        expect(dependency && loan->storedDependency == dependency.loan->id &&
                   loan->parent == 0 &&
                   loan->access == lang::AccessMode::ReadOnly,
               "eligible borrowed operators reuse the exact temporary shell "
               "and stored-owner dependency schedule");
        expect(!instruction.destination && !instruction.preparedParameterDrop &&
                   instruction.lifecycle.empty(),
               "receiver identity stage must not copy, move, or own cleanup");
        for (int mutation = 0; mutation != 3; ++mutation) {
          auto malformed = frontend.mir;
          auto *body = lang::findMirBody(
              malformed, {lang::MirBodyKind::Function, function.id});
          for (auto &changedBlock : body->blocks) {
            for (auto &changed : changedBlock.instructions) {
              if (changed.id != instruction.id)
                continue;
              if (mutation == 0)
                changed.callSite = 0;
              else if (mutation == 1)
                changed.callInputRole = lang::MirCallInputRole::Argument;
              else
                changed.callInputKind = lang::HirCallInputKind::MoveValue;
            }
          }
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "temporary receiver schedule must reject forged site, role, "
                 "or ownership-transfer kind");
        }
      }
    }
  }
  expect(receivers == 4, "grouped, conditional, and direct comparisons need "
                         "ordered temporary-borrow receiver stages");
}

void testPassiveNestedConstructorArgument() {
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/passive_nested_constructor_argument.gti";
  std::ifstream input(fixture);
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
  expect(frontend.canGenerateCode(),
         "passive nested constructor argument must verify");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap rows(buildRows(frontend, frontend.mir));
  expect(lang::CppMirBodyEmitter(frontend.mir, rows).analyzeProgram().ready(),
         "source constructors can move prepared passive class arguments into "
         "fields");
  std::size_t tested = 0;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    for (const auto &producerBlock : constructor.body.blocks) {
      for (const auto &construct : producerBlock.instructions) {
        if (construct.kind != lang::MirInstructionKind::Construct ||
            construct.parameterTypes.size() != 2 ||
            construct.parameterTypes.front().kind !=
                lang::SemanticType::Class ||
            !testFieldPublication(constructor.body, construct))
          continue;
        ++tested;
        expect(
            producerBlock.terminator.kind == lang::MirTerminatorKind::Invoke &&
                producerBlock.terminator.invokeInstruction == construct.id &&
                !construct.lifecycle.empty(),
            "prepared passive input handoff precedes success-only publication");
        for (int mutation = 0; mutation != 2; ++mutation) {
          auto malformed = frontend.mir;
          auto *body = lang::findMirBody(
              malformed, {lang::MirBodyKind::Constructor, constructor.id});
          for (auto &block : body->blocks) {
            for (auto &instruction : block.instructions) {
              if (mutation == 0 && instruction.id == construct.id)
                instruction.lifecycle.clear();
              if (mutation == 1 &&
                  instruction.kind == lang::MirInstructionKind::CallInput &&
                  instruction.preparedParameterDrop)
                instruction.callInputIndex = 1;
            }
          }
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "passive prepared arguments require exact input position and "
                 "invocation handoff");
        }
      }
    }
  }
  expect(tested == 1,
         "fixture must exercise one moved passive class constructor argument");
}

void testNestedOwnedFieldAssignment() {
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/nested_owned_field_assignment.gti";
  std::ifstream input(fixture);
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
  expect(frontend.canGenerateCode(),
         "nested owned field replacement must verify");
  if (!frontend.canGenerateCode())
    return;
  const auto originalRows = buildRows(frontend, frontend.mir);
  const lang::CppMirBodyEmissionMap rows(originalRows);
  const lang::CppMirBodyEmitter emitter(frontend.mir, rows);
  expect(emitter.analyzeProgram().ready(),
         "nested owned fields must preserve explicit replacement cleanup");
  std::size_t assignments = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        const auto *destination =
            instruction.destination
                ? function.body.findPlace(*instruction.destination)
                : nullptr;
        if (instruction.kind != lang::MirInstructionKind::Assign ||
            destination == nullptr ||
            destination->root != lang::MirPlaceRootKind::Loan ||
            destination->projections.size() != 2)
          continue;
        ++assignments;
        expect(instruction.replacementPair != 0 &&
                   instruction.lifecycle.size() == 1 &&
                   instruction.lifecycle.front().kind ==
                       lang::MirLifecycleEventKind::Replace,
               "nested replacement retains paired cleanup and exact source "
               "lifetime transfer");
        for (int mutation = 0; mutation != 3; ++mutation) {
          auto malformed = frontend.mir;
          auto *body = lang::findMirBody(
              malformed, {lang::MirBodyKind::Function, function.id});
          auto &changed = body->places[destination->id - 1];
          if (mutation == 0)
            changed.projections.front().field = 0;
          else if (mutation == 1)
            changed.projections.back().kind =
                lang::MirProjectionKind::Dereference;
          else {
            for (const auto &instance : malformed.classInstances()) {
              auto *owner = const_cast<lang::MirClassInstance *>(&instance);
              for (auto &field : owner->declaredFields) {
                if (field.symbol == destination->projections.front().field)
                  field.access = lang::AccessMode::ReadOnly;
              }
            }
          }
          const lang::CppMirBodyEmitter rejected(malformed, rows);
          expect(!rejected.analyzeProgram().ready(),
                 "nested replacement must reject forged field, projection "
                 "kind, or field mutability");
        }
        auto missingRows = originalRows;
        const auto field = destination->projections.front().field;
        std::erase_if(missingRows.symbols, [&](const auto &row) {
          return row.kind == lang::CppMirSymbolRepresentationKind::Field &&
                 row.symbol == field;
        });
        const lang::CppMirBodyEmissionMap incomplete(std::move(missingRows));
        const lang::CppMirBodyEmitter rejected(frontend.mir, incomplete);
        expect(!rejected.analyzeProgram().ready(),
               "every intermediate nominal field requires its exact native "
               "representation row");
        auto wrongEnd = frontend.mir;
        auto *changedBody = lang::findMirBody(
            wrongEnd, {lang::MirBodyKind::Function, function.id});
        const auto *child = changedBody->findLoan(destination->loan);
        expect(
            child != nullptr && child->parent != 0,
            "indexed owner replacement must carry its parent call-result loan");
        if (child != nullptr && child->parent != 0) {
          bool changed = false;
          for (auto &changedBlock : changedBody->blocks) {
            for (auto &ending : changedBlock.instructions) {
              if (ending.kind == lang::MirInstructionKind::EndBorrow &&
                  ending.loan == child->id) {
                ending.loan = child->parent;
                changed = true;
              }
            }
          }
          expect(changed && !lang::CppMirBodyEmitter(wrongEnd, rows)
                                 .analyzeProgram()
                                 .ready(),
                 "nested replacement cannot retire the parent before its child "
                 "loan");
        }
      }
    }
  }
  expect(assignments == 1,
         "fixture must exercise one nested loan-rooted owned replacement");

  const auto freshFixture = std::filesystem::path(__FILE__).parent_path() /
                            "fixtures/constexpr_dependent_branches.gti";
  std::ifstream freshInput(freshFixture);
  const std::string freshSource{std::istreambuf_iterator<char>(freshInput), {}};
  const auto fresh =
      analyzeWithStandardLibrary(freshFixture.string(), freshSource);
  expect(fresh.canGenerateCode(),
         "dependent receiver restoration must retain a concrete MIR schedule");
  if (!fresh.canGenerateCode())
    return;
  const auto freshRows = buildRows(fresh, fresh.mir);
  const lang::CppMirBodyEmissionMap freshMap(freshRows);
  expect(lang::CppMirBodyEmitter(fresh.mir, freshMap).analyzeProgram().ready(),
         "a fresh class value can restore an exact mutable owned field");
  std::size_t restoredFields = 0;
  for (const auto &function : fresh.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        const auto *destination =
            instruction.destination
                ? function.body.findPlace(*instruction.destination)
                : nullptr;
        if (instruction.kind != lang::MirInstructionKind::Assign ||
            destination == nullptr ||
            destination->root != lang::MirPlaceRootKind::This ||
            destination->projections.size() != 1 ||
            destination->type.kind != lang::SemanticType::Class)
          continue;
        ++restoredFields;
        expect(instruction.replacementPair != 0 &&
                   instruction.lifecycle.size() == 1 &&
                   instruction.lifecycle.front().kind ==
                       lang::MirLifecycleEventKind::Replace &&
                   instruction.lifecycle.front().target == 0,
               "receiver restoration keeps paired cleanup and the containing "
               "object's sole field cleanup authority");
        for (int mutation = 0; mutation != 4; ++mutation) {
          auto malformed = fresh.mir;
          auto *changed = const_cast<lang::MirFunctionInstance *>(
              malformed.findFunctionInstance(function.id));
          if (mutation == 0) {
            changed->receiverMutability = lang::ReceiverMutability::ReadOnly;
          } else if (mutation == 1) {
            auto *owner = const_cast<lang::MirClassInstance *>(
                malformed.findClassInstance(*function.owner));
            for (auto &field : owner->declaredFields)
              if (field.symbol == destination->projections.front().field)
                field.access = lang::AccessMode::ReadOnly;
          } else {
            for (auto &changedBlock : changed->body.blocks)
              for (auto &assignment : changedBlock.instructions)
                if (assignment.id == instruction.id) {
                  if (mutation == 2)
                    assignment.replacementPair = 0;
                  else
                    assignment.lifecycle.clear();
                }
          }
          expect(!lang::CppMirBodyEmitter(malformed, freshMap)
                      .analyzeProgram()
                      .ready(),
                 "fresh field replacement rejects immutable access, missing "
                 "cleanup pairing, or missing lifetime publication");
        }
        auto incomplete = freshRows;
        std::erase_if(incomplete.symbols, [&](const auto &row) {
          return row.kind == lang::CppMirSymbolRepresentationKind::Field &&
                 row.symbol == destination->projections.front().field;
        });
        const lang::CppMirBodyEmissionMap missing(std::move(incomplete));
        expect(!lang::CppMirBodyEmitter(fresh.mir, missing)
                    .analyzeProgram()
                    .ready(),
               "fresh field publication requires its exact representation row");
      }
    }
  }
  expect(restoredFields == 1,
         "dependent fixture exercises exactly one fresh owned field restore");
}

void testNestedBorrowedFieldProjection() {
  const auto mutableFixture = std::filesystem::path(__FILE__).parent_path() /
                              "fixtures/nested_borrow_fields_mutable.gti";
  std::ifstream mutableInput(mutableFixture);
  const std::string mutableSource{std::istreambuf_iterator<char>(mutableInput),
                                  {}};
  const auto mutableFrontend =
      analyzeWithStandardLibrary(mutableFixture.string(), mutableSource);
  expect(mutableFrontend.canGenerateCode(),
         "mutable nested field fixture must verify");
  if (mutableFrontend.canGenerateCode()) {
    const lang::CppMirBodyEmissionMap mutableRows(
        buildRows(mutableFrontend, mutableFrontend.mir));
    expect(lang::CppMirBodyEmitter(mutableFrontend.mir, mutableRows)
               .analyzeProgram()
               .ready(),
           "nested construction must consume staged reference inputs and "
           "discarded shells must preserve explicit dependency retirement");
    bool mutated = false;
    auto malformed = mutableFrontend.mir;
    auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
        malformed.functionInstances());
    for (auto &function : functions) {
      for (auto &block : function.body.blocks) {
        for (auto &instruction : block.instructions) {
          if (instruction.kind == lang::MirInstructionKind::Call &&
              instruction.info.traits.containsBorrowedState &&
              instruction.result && instruction.loan &&
              function.body.usesOf(*instruction.result).empty()) {
            instruction.loan.reset();
            mutated = true;
          }
        }
      }
    }
    expect(mutated && !lang::verifyMirProgram(malformed).valid(),
           "discarding a carrier shell cannot erase its owner dependency");
  }
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/nested_borrow_fields.gti";
  std::ifstream input(fixture);
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
  expect(frontend.canGenerateCode(), "nested field loan fixture must verify");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap rows(buildRows(frontend, frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, rows);
  expect(emitter.analyzeProgram().ready(),
         "nested one-owner fields must retain exact constructor and return "
         "publication");
  std::size_t paths = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &place : function.body.places) {
      if (place.root != lang::MirPlaceRootKind::Binding ||
          place.projections.size() != 3 ||
          place.projections[0].kind != lang::MirProjectionKind::Field ||
          place.projections[1].kind != lang::MirProjectionKind::Field ||
          place.projections[2].kind != lang::MirProjectionKind::Dereference)
        continue;
      ++paths;
      for (int mutation = 0; mutation != 3; ++mutation) {
        auto malformed = frontend.mir;
        auto *body = lang::findMirBody(
            malformed, {lang::MirBodyKind::Function, function.id});
        auto &changed = body->places[place.id - 1];
        if (mutation == 0)
          changed.projections[1].field = 0;
        if (mutation == 1)
          changed.projections.back().kind = lang::MirProjectionKind::Field;
        if (mutation == 2)
          changed.type = lang::SemanticType::Bool;
        expect(!lang::verifyMirProgram(malformed).valid(),
               "nested reference projection cannot forge field, dereference, "
               "or referent type");
      }
    }
  }
  expect(paths != 0,
         "fixture must exercise a binding field-field-reference chain");
}

void testPassiveConstructorFieldPublication() {
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/passive_constructor_field.gti";
  std::ifstream input(fixture);
  expect(input.good(), "passive constructor-field fixture must exist");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
  expect(frontend.canGenerateCode(), "passive constructor fields must verify");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  expect(emitter.analyzeProgram().ready(),
         "callback result with a generated passive field must emit");
  std::size_t publications = 0;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    for (const auto &block : constructor.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Initialize ||
            instruction.constructorInitializer == 0 ||
            instruction.info.type.kind != lang::SemanticType::Class ||
            !instruction.lifecycle.empty())
          continue;
        ++publications;
        expect(instruction.lifecycle.empty() && instruction.destination &&
                   instruction.operands.size() == 1,
               "passive fields publish without a fabricated drop obligation");
        for (int mutation = 0; mutation != 3; ++mutation) {
          auto malformed = frontend.mir;
          auto *body = lang::findMirBody(
              malformed, {lang::MirBodyKind::Constructor, constructor.id});
          for (auto &changed : body->findBlock(block.id)->instructions) {
            if (changed.id != instruction.id)
              continue;
            if (mutation == 0)
              changed.constructorInitializer = 0;
            else if (mutation == 1)
              changed.constructorInitializer =
                  constructor.initializers.size() + 1;
            else
              body->places[*changed.destination - 1].projections.front().field =
                  0;
          }
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "missing publication identity and forged initializer/field "
                 "must reject");
        }
      }
    }
  }
  expect(publications == 1, "fixture must exercise exactly one passive field");
}

void testScalarConstructorFieldPublication() {
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/constructor_scalar_side_effects.gti";
  std::ifstream input(fixture);
  expect(input.good(), "scalar initializer side-effect fixture must exist");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
  expect(frontend.canGenerateCode(),
         "side-effecting scalar field initializers must verify");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap rows(buildRows(frontend, frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, rows);
  expect(emitter.analyzeProgram().ready(),
         "scalar field initialization must preserve its exact MIR schedule");
  std::size_t discardedAssignments = 0;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    if (constructor.initializers.size() != 7)
      continue;
    const lang::MirBodyAddress address{lang::MirBodyKind::Constructor,
                                       constructor.id};
    const auto text =
        emitter.emitFailureBodyText(address, "scalar-field-publication", 1)
            .text;
    for (const auto &block : constructor.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Assign &&
            instruction.result &&
            constructor.body.usesOf(*instruction.result).empty()) {
          ++discardedAssignments;
          expect(text.find("std::move(__gti_mir_v_" +
                           std::to_string(*instruction.result) + ")") ==
                     std::string::npos,
                 "discarded assignment metadata must not publish a field");
        }
        if (instruction.kind != lang::MirInstructionKind::Initialize ||
            instruction.constructorInitializer < 2 ||
            instruction.constructorInitializer > 6)
          continue;
        auto malformed = frontend.mir;
        auto *body = lang::findMirBody(malformed, address);
        for (auto &changed : body->findBlock(block.id)->instructions) {
          if (changed.id == instruction.id)
            changed.tombstone = true;
        }
        expect(!lang::verifyMirProgram(malformed).valid(),
               "source initializer metadata cannot replace an explicit "
               "scalar field publication");
      }
    }
  }
  expect(discardedAssignments >= 2,
         "prefix/postfix initializers must exercise discarded assignment "
         "records sharing the published expression's HIR identity");
}

void testPassiveUnexpectedBinding() {
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/expected_scalar_error_binding.gti";
  std::ifstream input(fixture);
  expect(input.good(), "passive error binding fixture must exist");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
  expect(frontend.canGenerateCode(), "passive error bindings must verify");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap rows(buildRows(frontend, frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, rows);
  expect(
      emitter.analyzeProgram().ready(),
      "passive error binding must not construct an inactive success payload");
  std::size_t publications = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    if (function.entryKind == lang::ProgramEntryKind::None)
      continue;
    const lang::MirBodyAddress address{lang::MirBodyKind::Function,
                                       function.id};
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Initialize ||
            instruction.operands.size() != 1 ||
            instruction.operands.front().type.kind !=
                lang::SemanticType::Unexpected)
          continue;
        ++publications;
        for (int mutation = 0; mutation != 3; ++mutation) {
          auto malformed = frontend.mir;
          auto *body = lang::findMirBody(malformed, address);
          for (auto &changed : body->findBlock(block.id)->instructions) {
            if (changed.id != instruction.id)
              continue;
            if (mutation == 0)
              changed.operands.front().type.arguments.front() =
                  lang::SemanticType::Bool;
            else if (mutation == 1)
              changed.operands.push_back(changed.operands.front());
            else
              body->places[*changed.destination - 1].type.arguments.back() =
                  lang::SemanticType::Bool;
          }
          expect(
              !lang::CppMirBodyEmitter(malformed, rows)
                   .supportsFailureBodyText(address),
              "error binding rejects forged payload identity or extra inputs");
        }
      }
    }
  }
  expect(publications == 3,
         "scalar, owning-inactive and void success alternatives are covered");
}

void testExpectedConstructorPayloadPublication() {
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/expected_constructor_payload.gti";
  std::ifstream input(fixture);
  expect(input.good(), "the owning constructor payload fixture must exist");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
  expect(frontend.canGenerateCode(),
         "nested owning constructor payloads must preserve verified MIR");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap rows(buildRows(frontend, frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, rows);
  expect(emitter.analyzeProgram().ready(),
         "an owning constructor result must publish into Expected without "
         "requiring assignment or losing field rollback");
  std::size_t publications = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    if (function.entryKind == lang::ProgramEntryKind::None)
      continue;
    const lang::MirBodyAddress address{lang::MirBodyKind::Function,
                                       function.id};
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Initialize ||
            instruction.info.type.kind != lang::SemanticType::Expected ||
            instruction.operands.size() != 1 ||
            instruction.operands.front().kind != lang::MirOperandKind::Value)
          continue;
        const auto *value =
            function.body.findValue(instruction.operands.front().value);
        const auto *producer =
            value ? testInstruction(function.body, value->definition) : nullptr;
        if (!producer || producer->kind != lang::MirInstructionKind::Construct)
          continue;
        ++publications;
        expect(producer->successResultDrop &&
                   producer->definedFailure.propagation ==
                       lang::FailurePropagationKind::Constructor &&
                   instruction.lifecycle.size() == 2,
               "constructor success must own a payload before transferring "
               "it into the exact Expected binding");
        for (int mutation = 0; mutation != 3; ++mutation) {
          auto malformed = frontend.mir;
          auto *body = lang::findMirBody(malformed, address);
          for (auto &changedBlock : body->blocks) {
            for (auto &changed : changedBlock.instructions) {
              if (changed.id == producer->id && mutation == 0)
                changed.successResultDrop.reset();
              if (changed.id == instruction.id && mutation == 1)
                changed.lifecycle.clear();
              if (changed.id == producer->id && mutation == 2)
                changed.constructorTarget.reset();
            }
          }
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "constructor identity, successful ownership and Expected "
                 "publication cannot be erased");
          expect(!lang::CppMirBodyEmitter(malformed, rows)
                      .supportsFailureBodyText(address),
                 "backend admission must independently reject an incomplete "
                 "constructor payload publication");
        }
      }
    }
  }
  expect(publications == 1,
         "the fixture must contain an exact constructor-to-Expected transfer");
}

void testCleanupFreeConstructorFieldFailure() {
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/constructor_cleanup_free_failure.gti";
  std::ifstream input(fixture);
  expect(input.good(), "the nested constructor failure fixture must exist");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
  expect(frontend.canGenerateCode(),
         "cleanup-free nested construction must retain verified failure MIR");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  for (const auto &body : emitter.analyzeProgram().bodies) {
    for (const auto &issue : body.issues) {
      std::cerr << "constructor fixture: body " << body.body.owner << " i"
                << issue.instruction << ": " << issue.detail << '\n';
    }
  }
  expect(emitter.analyzeProgram().ready(),
         "fallible cleanup-free fields and completed owning fields must "
         "compose through the same constructor status boundary");
  std::size_t checkedFields = 0;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    for (const auto &block : constructor.body.blocks) {
      for (const auto &instruction : block.instructions) {
        const auto *target = instruction.constructorTarget
                                 ? frontend.mir.findConstructorInstance(
                                       *instruction.constructorTarget)
                                 : nullptr;
        const auto *owner =
            target ? frontend.mir.findClassInstance(target->owner) : nullptr;
        if (instruction.kind != lang::MirInstructionKind::Construct ||
            !instruction.result || !owner || owner->requiresActiveCleanup ||
            instruction.definedFailure.propagation !=
                lang::FailurePropagationKind::Constructor)
          continue;
        ++checkedFields;
        expect(!instruction.successResultDrop &&
                   lang::requiresMirFailureControlFlow(
                       instruction, lang::MirFailureControlFlowPosition::None),
               "a cleanup-free constructor needs an edge at every position");
        expect(block.terminator.kind == lang::MirTerminatorKind::Invoke &&
                   block.terminator.invokeInstruction == instruction.id,
               "failure must precede cleanup-free field publication");
        if (block.terminator.kind != lang::MirTerminatorKind::Invoke)
          continue;
        const auto *success =
            constructor.body.findBlock(block.terminator.target);
        expect(success && !success->instructions.empty() &&
                   success->instructions.front().kind ==
                       lang::MirInstructionKind::Initialize &&
                   success->instructions.front().constructorInitializer != 0,
               "the exact field is published only on the success continuation");
        auto malformed = frontend.mir;
        auto *body = lang::findMirBody(
            malformed, {lang::MirBodyKind::Constructor, constructor.id});
        auto &terminator = body->findBlock(block.id)->terminator;
        terminator.kind = lang::MirTerminatorKind::Goto;
        terminator.invokeInstruction = 0;
        terminator.failureRecord = 0;
        terminator.elseTarget = 0;
        expect(!lang::verifyMirProgram(malformed).valid(),
               "removing a cleanup-free constructor failure edge must reject");
      }
    }
  }
  expect(checkedFields == 2,
         "the fixture must cover both levels of cleanup-free construction");
  if (emitter.analyzeProgram().ready()) {
    const auto artifact = gti_test::emitCpp(frontend);
    expect(!artifact.contents.empty(),
           "the complete constructor schedule must reach the native backend");
  }
}

void testNestedCleanupFreeClassResult() {
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/nested_nominal_result.gti";
  std::ifstream input(fixture);
  expect(input.good(), "the nested nominal-result fixture must exist");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
  expect(frontend.canGenerateCode(), "nested class results must verify");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  expect(emitter.analyzeProgram().ready(),
         "nested member, erased callable, and constructor inputs must emit");
  std::size_t nestedResults = 0;
  std::size_t callableResults = 0;
  for (const auto &entry : frontend.mir.functionInstances()) {
    if (entry.entryKind != lang::ProgramEntryKind::OwnedArguments)
      continue;
    const lang::MirBodyAddress address{lang::MirBodyKind::Function, entry.id};
    for (const auto &block : entry.body.blocks) {
      for (const auto &call : block.instructions) {
        if (call.kind != lang::MirInstructionKind::Call ||
            !call.functionTarget || !call.result ||
            call.info.type.kind != lang::SemanticType::Class)
          continue;
        ++nestedResults;
        callableResults += call.callableInvocation.has_value();
        expect(!call.successResultDrop && !call.successResultDestination,
               "cleanup-free results must not need an owning result identity");
        expect(block.terminator.kind == lang::MirTerminatorKind::Invoke &&
                   block.terminator.invokeInstruction == call.id,
               "nested nominal calls must route failure before publication");
        const lang::MirInstruction *stage = nullptr;
        for (const auto &candidateBlock : entry.body.blocks) {
          for (const auto &candidate : candidateBlock.instructions) {
            if (candidate.kind == lang::MirInstructionKind::CallInput &&
                candidate.operands.size() == 1 &&
                candidate.operands.front().kind ==
                    lang::MirOperandKind::Value &&
                candidate.operands.front().value == *call.result) {
              stage = &candidate;
              expect(candidateBlock.id == block.terminator.target,
                     "only the success successor may prepare the result");
            }
          }
        }
        expect(stage != nullptr && stage->preparedParameterDrop &&
                   stage->destination,
               "each nested result must use an exact prepared argument slot");
        if (stage == nullptr || !stage->preparedParameterDrop ||
            !stage->destination)
          continue;
        if (emitter.supportsFailureBodyText(address)) {
          const auto text =
              emitter.emitFailureBodyText(address, "nested", 1).text;
          const auto slot =
              "__gti_mir_p_" + std::to_string(*stage->destination);
          bool constructorConsumer = false;
          for (const auto &candidateBlock : entry.body.blocks) {
            for (const auto &candidate : candidateBlock.instructions) {
              if (candidate.kind == lang::MirInstructionKind::Construct &&
                  candidate.callSite == stage->callSite)
                constructorConsumer = true;
            }
          }
          const bool handoff =
              constructorConsumer
                  ? text.find(slot + ".take()") != std::string::npos
                  : text.find("std::move(" + slot + ".get())") !=
                            std::string::npos &&
                        text.find(slot + ".destroy()") != std::string::npos;
          expect(text.find(slot + ".construction_address()") !=
                         std::string::npos &&
                     text.find(slot + ".mark_constructed()") !=
                         std::string::npos &&
                     handoff,
                 "result placement, successful activation, and handoff must "
                 "agree");
        }
        if (call.callableInvocation) {
          auto malformed = frontend.mir;
          auto *body = lang::findMirBody(malformed, address);
          for (auto &candidate : body->findBlock(block.id)->instructions) {
            if (candidate.id == call.id)
              candidate.callableInvocation =
                  lang::CallableInvocationCapability::Once;
          }
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "result staging must not relax the selected operator's "
                 "receiver capability");
        }
        for (int mutation = 0; mutation != 3; ++mutation) {
          auto malformed = frontend.mir;
          auto *body = lang::findMirBody(malformed, address);
          auto &term = body->findBlock(block.id)->terminator;
          if (mutation == 0) {
            term.kind = lang::MirTerminatorKind::Goto;
            term.invokeInstruction = 0;
            term.failureRecord = 0;
            term.elseTarget = 0;
          } else if (mutation == 1) {
            std::swap(term.target, term.elseTarget);
          } else {
            body->dropObligations[*stage->preparedParameterDrop - 1]
                .initiallyActive = true;
          }
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "missing failure routing, reversed edges, and early "
                 "activation must fail");
        }
      }
    }
  }
  expect(nestedResults == 3 && callableResults == 1,
         "the fixture must cover three producers including one nominal "
         "operator()");
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
  const bool exactPublication =
      constructor->mayRaiseDefinedFailure
          ? count("std::construct_at(" + slot + ".construction_address()") ==
                    1 &&
                count(slot + ".mark_constructed()") == 1
          : count(slot + ".construct()") == 1 &&
                count(slot + ".mark_constructed()") == 0;
  expect(text.find("mir_lifetime_slot<") != std::string::npos &&
             exactPublication && count(slot + ".get().") == 1 &&
             count(slot + ".destroy()") == 1 &&
             text.find(slot + ".destroy()") > text.find(slot + ".get().") &&
             count("std::move(" + slot + ".get())") == 0,
         "the constructor should publish once into its value slot and the "
         "field Load should read it once and retire its cleanup-free "
         "representation slot without inventing a semantic copy or drop");

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
    {"testPreparedConstructorReplacement", testPreparedConstructorReplacement},
    {"testNonFailingOwningDeclarationInitializer",
     testNonFailingOwningDeclarationInitializer},
    {"testCopiedBorrowedArgumentAcrossInvoke",
     testCopiedBorrowedArgumentAcrossInvoke},
    {"testPackParameterCleanup", testPackParameterCleanup},
    {"testForwardListEmplacementPublication",
     testForwardListEmplacementPublication},
    {"testContainerSwapStorageAndMemberResults",
     testContainerSwapStorageAndMemberResults},
    {"testOwningCheckedBodyEmitsWholeCleanupProof",
     testOwningCheckedBodyEmitsWholeCleanupProof},
    {"testRichFieldInitializerSchedule", testRichFieldInitializerSchedule},
    {"testToyEngineConstructorInitializers",
     testToyEngineConstructorInitializers},
    {"testFallibleFieldInitializerPropagation",
     testFallibleFieldInitializerPropagation},
    {"testDependentFieldInitializerSchedule",
     testDependentFieldInitializerSchedule},
    {"testContainedOwningFieldConstructor",
     testContainedOwningFieldConstructor},
    {"testContainedOwningStringFieldConstructor",
     testContainedOwningStringFieldConstructor},
    {"testBodyOrderedOwnedParameterConstructor",
     testBodyOrderedOwnedParameterConstructor},
    {"testSourceDestructorOwningFieldCleanup",
     testSourceDestructorOwningFieldCleanup},
    {"testOwningBaseConstructorStatus", testOwningBaseConstructorStatus},
    {"testPassiveOwnedParameterConstructorFields",
     testPassiveOwnedParameterConstructorFields},
    {"testBodyOrderedPassivePolicyConstructor",
     testBodyOrderedPassivePolicyConstructor},
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
    {"testPassiveRecordComposition", testPassiveRecordComposition},
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
    {"testDiscardedCleanupFreeClassResult",
     testDiscardedCleanupFreeClassResult},
    {"testNestedCleanupFreeClassResult", testNestedCleanupFreeClassResult},
    {"testCleanupFreeConstructorFieldFailure",
     testCleanupFreeConstructorFieldFailure},
    {"testExpectedConstructorPayloadPublication",
     testExpectedConstructorPayloadPublication},
    {"testScalarConstructorFieldPublication",
     testScalarConstructorFieldPublication},
    {"testPassiveUnexpectedBinding", testPassiveUnexpectedBinding},
    {"testPassiveConstructorFieldPublication",
     testPassiveConstructorFieldPublication},
    {"testGenericScalarConstructedFieldPublication",
     testGenericScalarConstructedFieldPublication},
    {"testNestedConstructorRepresentationPrecedence",
     testNestedConstructorRepresentationPrecedence},
    {"testConservativeConstructorAssignmentFusion",
     testConservativeConstructorAssignmentFusion},
    {"testBorrowedFieldCompoundAssignment",
     testBorrowedFieldCompoundAssignment},
    {"testNestedBorrowedFieldProjection", testNestedBorrowedFieldProjection},
    {"testNestedOwnedFieldAssignment", testNestedOwnedFieldAssignment},
    {"testPassiveNestedConstructorArgument",
     testPassiveNestedConstructorArgument},
    {"testBorrowedTemporaryOperatorReceiver",
     testBorrowedTemporaryOperatorReceiver},
    {"testMovedClassArgumentBeforeCheckedArgument",
     testMovedClassArgumentBeforeCheckedArgument},
    {"testOwningConstructedFieldInputTransfers",
     testOwningConstructedFieldInputTransfers},
    {"testConstructedCallbackConditionalArgument",
     testConstructedCallbackConditionalArgument},
    {"testErasedOwnedResultReturn", testErasedOwnedResultReturn},
    {"testFreshOwnedCallableArgument", testFreshOwnedCallableArgument},
    {"testGeneratedDefaultOwningFieldInitializer",
     testGeneratedDefaultOwningFieldInitializer},
};

} // namespace

int main(int argc, char **argv) {
  return runNamedTests(argc, argv, kTests, "lifecycle");
}
