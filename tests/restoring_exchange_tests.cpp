#include "cpp_mir_body_emitter_test_fixture.h"

int main() {
  const auto shapesPath = std::filesystem::path(__FILE__).parent_path() /
                          "fixtures/restoring_exchange_shapes.gti";
  std::ifstream shapesInput(shapesPath);
  const std::string shapesSource{std::istreambuf_iterator<char>(shapesInput),
                                 {}};
  const auto shapes =
      analyzeWithStandardLibrary(shapesPath.string(), shapesSource);
  expect(shapes.canGenerateCode(),
         "structurally eligible owner shapes analyze");
  if (!shapes.canGenerateCode()) {
    for (const auto &diagnostic : shapes.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return 1;
  }
  const lang::CppMirBodyEmissionMap shapeRows(buildRows(shapes, shapes.mir));
  unsigned shapeResults = 0;
  unsigned arrayPublications = 0;
  unsigned arrayMoveBodies = 0;
  for (const auto &function : shapes.mir.functionInstances()) {
    const bool movesArray = std::any_of(
        function.body.blocks.begin(), function.body.blocks.end(),
        [](const auto &block) {
          return std::any_of(
              block.instructions.begin(), block.instructions.end(),
              [](const auto &instruction) {
                return instruction.kind == lang::MirInstructionKind::Move &&
                       instruction.info.type.kind ==
                           lang::SemanticType::Array &&
                       instruction.info.traits.drop == lang::DropKind::Lexical;
              });
        });
    if (movesArray) {
      ++arrayMoveBodies;
      expect(supportsSelectedBodyText(shapes.mir, shapeRows, function.id),
             "owning array moves retain exact argument/return publication, "
             "including caller bodies and non-default-constructible elements");
    }
    for (const auto &block : function.body.blocks) {
      for (const auto &call : block.instructions) {
        if (call.intrinsic != lang::IntrinsicKind::RestoringExchange)
          continue;
        ++shapeResults;
        expect(supportsSelectedBodyText(shapes.mir, shapeRows, function.id),
               "every eligible restoring result has a production MIR consumer");
        for (const auto &initialize : block.instructions) {
          if (initialize.kind != lang::MirInstructionKind::Initialize ||
              initialize.operands.size() != 1 || !call.result ||
              initialize.operands.front().kind != lang::MirOperandKind::Value ||
              initialize.operands.front().value != *call.result)
            continue;
          auto invalid = shapes.mir;
          auto *owner = const_cast<lang::MirFunctionInstance *>(
              invalid.findFunctionInstance(function.id));
          auto *home = owner->body.findBlock(block.id);
          auto publication = std::find_if(
              home->instructions.begin(), home->instructions.end(),
              [&](const auto &item) { return item.id == initialize.id; });
          publication->lifecycle.clear();
          expect(!lang::verifyMirProgram(invalid).valid(),
                 "restoring result cannot lose its binding publication");
          if (call.info.type.kind == lang::SemanticType::Array) {
            ++arrayPublications;
            expect(!supportsSelectedBodyText(invalid, shapeRows, function.id),
                   "array result-slot emission independently requires "
                   "publication");
          }
        }
      }
    }
  }
  expect(shapeResults == 10 && arrayPublications == 3,
         "shape coverage includes active/cleanup-free arrays and owning "
         "alternatives");
  expect(
      arrayMoveBodies >= 4,
      "array move coverage includes the caller and three exchange instances");
  unsigned quietArrayStages = 0;
  for (const auto &constructor : shapes.mir.constructorInstances()) {
    for (const auto &block : constructor.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Initialize ||
            !instruction.constructorInitializer ||
            instruction.info.type.kind != lang::SemanticType::Array ||
            !instruction.lifecycle.empty())
          continue;
        ++quietArrayStages;
        auto invalid = shapes.mir;
        auto *owner = const_cast<lang::MirConstructorInstance *>(
            invalid.findConstructorInstance(constructor.id));
        auto *home = owner->body.findBlock(block.id);
        for (auto &candidate : home->instructions)
          if (candidate.id == instruction.id)
            candidate.constructorInitializer = 0;
        expect(!lang::verifyMirProgram(invalid).valid(),
               "cleanup-free lexical array fields require exact constructor "
               "publication even without an active cleanup token");
      }
    }
  }
  expect(quietArrayStages == 1,
         "unrelated cleanup-free array owner exercises constructor staging");
  const auto resultsPath =
      shapesPath.parent_path() / "restoring_array_results.gti";
  std::ifstream resultsInput(resultsPath);
  const std::string resultsSource{std::istreambuf_iterator<char>(resultsInput),
                                  {}};
  const auto results =
      analyzeWithStandardLibrary(resultsPath.string(), resultsSource);
  expect(results.canGenerateCode(),
         "forwarded and directly consumed array results analyze");
  if (results.canGenerateCode()) {
    const lang::CppMirBodyEmissionMap rows(buildRows(results, results.mir));
    unsigned callPublications = 0;
    unsigned resultTransfers = 0;
    for (const auto &function : results.mir.functionInstances()) {
      for (const auto &block : function.body.blocks) {
        for (const auto &instruction : block.instructions) {
          if (instruction.kind != lang::MirInstructionKind::Call ||
              instruction.info.type.kind != lang::SemanticType::Array ||
              !instruction.functionTarget)
            continue;
          // A failure-capable callee activates its result on the Invoke
          // success edge; a failure-free callee activates it with the call's
          // own Initialize.
          const auto plainActivation = std::find_if(
              instruction.lifecycle.begin(), instruction.lifecycle.end(),
              [](const lang::MirLifecycleEvent &event) {
                return event.kind == lang::MirLifecycleEventKind::Initialize &&
                       event.source == 0;
              });
          const std::optional<lang::MirDropObligationId> resultDrop =
              instruction.successResultDrop ? instruction.successResultDrop
              : plainActivation == instruction.lifecycle.end()
                  ? std::nullopt
                  : std::optional(plainActivation->target);
          if (!resultDrop)
            continue;
          ++callPublications;
          expect(supportsSelectedBodyText(results.mir, rows, function.id),
                 "array call results retain exact production storage through "
                 "their consumers");
          auto invalid = results.mir;
          auto *owner = const_cast<lang::MirFunctionInstance *>(
              invalid.findFunctionInstance(function.id));
          auto *home = owner->body.findBlock(block.id);
          if (instruction.successResultDrop) {
            home->terminator.successLifecycle.clear();
          } else {
            for (auto &candidate : home->instructions)
              if (candidate.id == instruction.id)
                std::erase(candidate.lifecycle, *plainActivation);
          }
          expect(!lang::verifyMirProgram(invalid).valid(),
                 "array call result requires its activation");
          expect(!supportsSelectedBodyText(invalid, rows, function.id),
                 "array result representation independently rejects missing "
                 "activation");
          for (const auto &consumerBlock : function.body.blocks) {
            for (const auto &consumer : consumerBlock.instructions) {
              for (const auto &event : consumer.lifecycle) {
                if (consumer.kind != lang::MirInstructionKind::Call ||
                    event.kind != lang::MirLifecycleEventKind::TransferOut ||
                    event.source != *resultDrop)
                  continue;
                ++resultTransfers;
                auto missing = results.mir;
                auto *missingOwner = const_cast<lang::MirFunctionInstance *>(
                    missing.findFunctionInstance(function.id));
                auto *consumerHome =
                    missingOwner->body.findBlock(consumerBlock.id);
                for (auto &candidate : consumerHome->instructions)
                  if (candidate.id == consumer.id)
                    std::erase(candidate.lifecycle, event);
                expect(!lang::verifyMirProgram(missing).valid(),
                       "direct array argument requires its consumer transfer");
                expect(!supportsSelectedBodyText(missing, rows, function.id),
                       "array result representation rejects untransferred "
                       "consumption");
              }
            }
          }
        }
      }
    }
    expect(callPublications == 4 && resultTransfers == 2,
           "array fixture covers forwarding, discard and both consuming-call "
           "paths");
    expect(!gti_test::emitCppText(results).empty(),
           "array expression results have a complete production consumer");
  }
  const auto failurePath =
      shapesPath.parent_path() / "restoring_array_failure.gti";
  std::ifstream failureInput(failurePath);
  const std::string failureSource{std::istreambuf_iterator<char>(failureInput),
                                  {}};
  const auto arrayFailure =
      analyzeWithStandardLibrary(failurePath.string(), failureSource);
  expect(arrayFailure.canGenerateCode(),
         "pending owning-array returns analyze");
  if (arrayFailure.canGenerateCode()) {
    const lang::CppMirBodyEmissionMap rows(
        buildRows(arrayFailure, arrayFailure.mir));
    unsigned pendingResults = 0;
    for (const auto &function : arrayFailure.mir.functionInstances()) {
      for (const auto &drop : function.body.dropObligations) {
        if (drop.kind != lang::MirDropObligationKind::ReturnResult ||
            drop.dropType.type.kind != lang::SemanticType::Array)
          continue;
        ++pendingResults;
        expect(
            supportsSelectedBodyText(arrayFailure.mir, rows, function.id),
            "pending array result and its rollback have a production consumer");
        auto invalid = arrayFailure.mir;
        auto *owner = const_cast<lang::MirFunctionInstance *>(
            invalid.findFunctionInstance(function.id));
        for (auto &candidate : owner->body.dropObligations)
          if (candidate.id == drop.id)
            candidate.initiallyActive = true;
        expect(!lang::verifyMirProgram(invalid).valid(),
               "unpublished array result cannot be active before construction");
        for (const auto &block : function.body.blocks) {
          for (const auto &instruction : block.instructions) {
            if (instruction.kind != lang::MirInstructionKind::Drop ||
                instruction.lifecycle.size() != 1 ||
                instruction.lifecycle.front().source != drop.id)
              continue;
            auto missingCleanup = arrayFailure.mir;
            auto *missingOwner = const_cast<lang::MirFunctionInstance *>(
                missingCleanup.findFunctionInstance(function.id));
            auto *home = missingOwner->body.findBlock(block.id);
            for (auto &candidate : home->instructions)
              if (candidate.id == instruction.id)
                candidate.lifecycle.clear();
            expect(!lang::verifyMirProgram(missingCleanup).valid(),
                   "pending array return cannot lose its failure cleanup");
          }
        }
      }
    }
    expect(pendingResults == 1,
           "one owning-array return has rollback coverage");
    expect(!gti_test::emitCppText(arrayFailure).empty(),
           "array cleanup fixture enters production emission");
  }
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/restoring_exchange.gti";
  std::ifstream input(path);
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  for (const auto &diagnostic : frontend.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(frontend.canGenerateCode(),
         "passive and unrelated move-only exchange analyze");
  if (!frontend.canGenerateCode())
    return 1;
  expect(lang::verifyMirProgram(frontend.mir).valid(),
         "restoring MIR verifies");

  unsigned seen = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        const bool swap =
            instruction.intrinsic == lang::IntrinsicKind::RestoringSwap;
        if (!swap &&
            instruction.intrinsic != lang::IntrinsicKind::RestoringExchange)
          continue;
        ++seen;
        const auto effect = lang::effects(instruction.intrinsic);
        expect(
            effect.readsPlace && effect.writesPlace && effect.movesValue &&
                !effect.dropsValue && !effect.invokesUserCode &&
                !effect.mayTrap,
            "sealed restoring transfer cannot drop, fail, or invoke user code");
        for (int mutation = 0; mutation != 5; ++mutation) {
          if (mutation == 4 && instruction.lifecycle.empty())
            continue;
          auto invalid = frontend.mir;
          auto *owner = const_cast<lang::MirFunctionInstance *>(
              invalid.findFunctionInstance(function.id));
          auto *targetBlock = owner->body.findBlock(block.id);
          auto target = std::find_if(targetBlock->instructions.begin(),
                                     targetBlock->instructions.end(),
                                     [&](const auto &candidate) {
                                       return candidate.id == instruction.id;
                                     });
          if (mutation == 0)
            target->parameterTypes.front().referenceAccess =
                lang::AccessMode::ReadOnly;
          if (mutation == 1)
            target->operands.back() = target->operands.front();
          if (mutation == 2)
            target->info.type = lang::SemanticType::Unknown;
          if (mutation == 3)
            target->intrinsic = swap ? lang::IntrinsicKind::RestoringExchange
                                     : lang::IntrinsicKind::RestoringSwap;
          if (mutation == 4)
            target->lifecycle.clear();
          expect(!lang::verifyMirProgram(invalid).valid(),
                 "forged access, alias, result or restoration operation is "
                 "rejected");
        }
      }
    }
  }
  expect(seen == 4, "both operations are instantiated for scalar and resource");
  const auto escapedView =
      analyzeWithStandardLibrary("restoring-view-escape.gti", R"(
import std.utility;
import std.string;
std::string_view bad() {
  std::string owner = std::string("local");
  mut std::string_view view = owner.view();
  return std::exchange(view, "literal");
}
int main() { std::println(bad()); return 0; }
)");
  expect(!escapedView.canGenerateCode(),
         "restoring exchange cannot erase a returned text view's owner "
         "dependency");
  const auto swappedView =
      analyzeWithStandardLibrary("restoring-view-swap.gti", R"(
import std.utility;
import std.string;
int main() {
  std::string left = std::string("left");
  mut std::string right = std::string("right");
  mut std::string_view a = left.view();
  mut std::string_view b = right.view();
  std::swap(a, b);
  right.clear();
  std::println(a);
  return 0;
}
)");
  expect(!swappedView.canGenerateCode(),
         "swapping view bindings must not erase or misattribute owner loans");
  for (const auto *denied : {&escapedView, &swappedView}) {
    expect(std::none_of(denied->diagnostics.begin(), denied->diagnostics.end(),
                        [](const auto &d) { return d.code == "GTI-B0001"; }),
           "unsupported dependency exchange is rejected before MIR lowering");
    expect(std::any_of(denied->diagnostics.begin(), denied->diagnostics.end(),
                       [](const auto &d) {
                         return d.code == "GTI-S2045" &&
                                d.message.find(
                                    "exchanged through a mutable reference") !=
                                    std::string::npos &&
                                !d.hints.empty() && d.fixes.empty();
                       }),
           "dependency exchange explains the existing view-storage boundary "
           "without an unsafe fix");
  }
  for (const std::string program :
       {"class Fixed { public: Fixed() {} Fixed(Fixed&& other) = delete; }; "
        "int main() { mut Fixed a{}; mut Fixed b{}; std::swap(a,b); return 0; "
        "}",
        "int take(mut int& value) { return std::move(value); } "
        "int main() { mut int x = 1; return take(x); }"}) {
    const auto denied = analyzeWithStandardLibrary(
        "restoring-eligibility.gti", "import std.utility;\n" + program);
    expect(!denied.canGenerateCode(), "deleted movement and arbitrary "
                                      "reference consumption remain forbidden");
  }
  for (const std::string body : {"mut int x = 1; std::swap(x, x);",
                                 "int x = 1; mut int y = 2; std::swap(x, y);",
                                 "mut int x = 1; int old = "
                                 "std::exchange(x, std::move(x));",
                                 "mut int x = 1; int& borrowed = x; "
                                 "std::exchange(x, 2); return borrowed;",
                                 "mut int x = 1; return std::move(x);"}) {
    // The last case exercises the unchanged baseline, not a negative case.
    const auto probe = analyzeWithStandardLibrary(
        "restoring-borrow.gti",
        "import std.utility;\nint main() { " + body + " return 0; }");
    const bool ordinaryMove = body == "mut int x = 1; return std::move(x);";
    expect(probe.canGenerateCode() == ordinaryMove,
           "self-alias, immutable storage and outstanding loans are rejected");
    expect(std::none_of(probe.diagnostics.begin(), probe.diagnostics.end(),
                        [](const auto &d) { return d.code == "GTI-B0001"; }),
           "invalid source receives semantic diagnostics, never an ICE");
  }
  const auto cpp = gti_test::emitCppText(frontend);
  expect(cpp.find("mir_restoring_exchange_v1(") != std::string::npos &&
             cpp.find("mir_restoring_swap_v1(") != std::string::npos,
         "production C++ consumes the verified restoring instructions");

  const auto indexedPath = path.parent_path() / "restoring_indexed_swap.gti";
  std::ifstream indexedInput(indexedPath);
  const std::string indexedSource{std::istreambuf_iterator<char>(indexedInput),
                                  {}};
  const auto indexed =
      analyzeWithStandardLibrary(indexedPath.string(), indexedSource);
  for (const auto &diagnostic : indexed.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(indexed.canGenerateCode(), "move-only indexed algorithms analyze");
  if (indexed.canGenerateCode()) {
    unsigned indexedSeen = 0;
    unsigned sourceSwaps = 0;
    unsigned checkedSwaps = 0;
    for (const auto &function : indexed.mir.functionInstances()) {
      const auto *hir = indexed.hir.findFunctionInstance(function.id);
      const auto &owners = indexed.hir.classInstances();
      const auto owner = std::find_if(
          owners.begin(), owners.end(), [&](const auto &candidate) {
            return function.owner && candidate.id == *function.owner;
          });
      const bool sourceSwap =
          hir && hir->source &&
          hir->source->name().lexeme == "swap_elements_unchecked" &&
          owner != owners.end() && owner->source &&
          owner->source->name().lexeme == "prefix_slots";
      const bool checkedSwap = hir && hir->source &&
                               hir->source->name().lexeme == "swap_elements" &&
                               owner != owners.end() && owner->source &&
                               owner->source->name().lexeme == "vector";
      unsigned bounds = 0, relocations = 0, uncheckedCalls = 0;
      for (const auto &block : function.body.blocks) {
        for (const auto &instruction : block.instructions) {
          if (sourceSwap || checkedSwap) {
            bounds += instruction.intrinsic ==
                      lang::IntrinsicKind::StorageBoundsCheck;
            const auto *callee = instruction.functionTarget
                                     ? indexed.hir.findFunctionInstance(
                                           *instruction.functionTarget)
                                     : nullptr;
            relocations += callee && callee->source &&
                           callee->source->name().lexeme == "relocate_at";
            uncheckedCalls +=
                callee && callee->source &&
                callee->source->name().lexeme == "swap_elements_unchecked";
          }
          if (instruction.intrinsic !=
              lang::IntrinsicKind::RestoringIndexedSwap)
            continue;
          ++indexedSeen;
          const auto effect = lang::effects(instruction.intrinsic);
          expect(effect.mayTrap && effect.movesValue && effect.writesPlace &&
                     !effect.dropsValue && !effect.invokesUserCode,
                 "indexed swap has a checked boundary but no element cleanup "
                 "or user movement");
          for (int mutation = 0; mutation != 5; ++mutation) {
            auto invalid = indexed.mir;
            auto *owner = const_cast<lang::MirFunctionInstance *>(
                invalid.findFunctionInstance(function.id));
            auto *targetBlock = owner->body.findBlock(block.id);
            auto target = std::find_if(targetBlock->instructions.begin(),
                                       targetBlock->instructions.end(),
                                       [&](const auto &candidate) {
                                         return candidate.id == instruction.id;
                                       });
            if (mutation == 0)
              target->parameterTypes.front().referenceAccess =
                  lang::AccessMode::ReadOnly;
            if (mutation == 1)
              target->localFailureSites.clear();
            if (mutation == 2)
              target->definedFailure.localOrigins.clear();
            if (mutation == 3)
              target->parameterTypes.back() = lang::SemanticType::Int32;
            if (mutation == 4)
              target->operands.front().kind = lang::MirOperandKind::Value;
            expect(!lang::verifyMirProgram(invalid).valid(),
                   "indexed swap rejects forged access, missing bounds "
                   "outcomes and invalid operands");
          }
        }
      }
      if (sourceSwap) {
        ++sourceSwaps;
        expect(bounds == 0 && relocations == 3,
               "unchecked source indexed swap retains all three "
               "vacant-slot relocations: " +
                   std::to_string(bounds) + "/" + std::to_string(relocations));
      }
      if (checkedSwap) {
        ++checkedSwaps;
        expect(bounds == 2 && uncheckedCalls == 1 && relocations == 0,
               "public indexed swap checks each live index once before its "
               "unchecked source operation");
      }
    }
    expect(indexedSeen == 3 && sourceSwaps == 2 && checkedSwaps == 2,
           "array transfers retain three typed intrinsics, while resource and "
           "string vectors swap through source relocation: " +
               std::to_string(indexedSeen) + "/" + std::to_string(sourceSwaps) +
               "/" + std::to_string(checkedSwaps));
    const auto indexedCpp = gti_test::emitCppText(indexed);
    expect(indexedCpp.find("mir_restoring_indexed_swap_v1(") !=
                   std::string::npos &&
               indexedCpp.find(".take()") != std::string::npos,
           "backend retains owning fixed-array argument transport and indexed "
           "transfer");
    const lang::CppMirBodyEmissionMap rows(buildRows(indexed, indexed.mir));
    unsigned arrayArguments = 0;
    for (const auto &function : indexed.mir.functionInstances()) {
      for (const auto &block : function.body.blocks) {
        for (const auto &producer : block.instructions) {
          if (producer.kind != lang::MirInstructionKind::Compute ||
              producer.operation != lang::MirOperation::Aggregate ||
              producer.info.type.kind != lang::SemanticType::Array ||
              producer.lifecycle.empty() || !producer.result)
            continue;
          for (const auto &consumer : block.instructions) {
            if (consumer.kind != lang::MirInstructionKind::Construct ||
                std::none_of(consumer.operands.begin(), consumer.operands.end(),
                             [&](const auto &operand) {
                               return operand.kind ==
                                          lang::MirOperandKind::Value &&
                                      operand.value == *producer.result;
                             }))
              continue;
            ++arrayArguments;
            expect(supportsSelectedBodyText(indexed.mir, rows, function.id),
                   "owning array constructor arguments have a complete backend "
                   "proof");
            for (int mutation = 0; mutation != 2; ++mutation) {
              auto malformed = indexed.mir;
              auto *changed = const_cast<lang::MirFunctionInstance *>(
                  malformed.findFunctionInstance(function.id));
              for (auto &candidate :
                   changed->body.findBlock(block.id)->instructions) {
                if (candidate.id == (mutation == 0 ? producer.id : consumer.id))
                  candidate.lifecycle.clear();
              }
              expect(!lang::verifyMirProgram(malformed).valid(),
                     "array argument cannot lose aggregate publication or "
                     "parameter transfer");
              expect(!supportsSelectedBodyText(malformed, rows, function.id),
                     "backend rejects an array ownership proof loss instead of "
                     "using scalar fallback");
            }
          }
        }
      }
    }
    expect(arrayArguments >= 4,
           "array transport mutations cover stdlib and unrelated constructors");
  }
  for (const std::string body :
       {"mut std::vector<int> v{}; v.push_back(1); int& item = "
        "v[std::size_t(0)]; v.swap_elements(std::size_t(0), std::size_t(0)); "
        "return item;",
        "std::vector<int> v{}; v.swap_elements(std::size_t(0), "
        "std::size_t(0));",
        "mut std::array<int, 2> v{}; v.swap(v);",
        "mut std::vector<Resource> v{}; v.push_back(Resource(1)); Resource "
        "escaped = std::move(v[std::size_t(0)]);"}) {
    const auto denied = analyzeWithStandardLibrary(
        "restoring-indexed-borrow.gti",
        "import std.vector;\nimport std.array;\n"
        "class Resource { public: Resource(int id) {} Resource(Resource& "
        "other) = delete; };\n"
        "int main() { " +
            body + " return 0; }");
    expect(!denied.canGenerateCode(),
           "indexed transfers preserve mutability, exclusivity and initialized "
           "element rules");
    expect(std::none_of(denied.diagnostics.begin(), denied.diagnostics.end(),
                        [](const auto &d) { return d.code == "GTI-B0001"; }),
           "indexed misuse is a source diagnostic, not a backend failure");
  }
  // A restoring swap or exchange of a polymorphic class that another class
  // derives from is rejected for now: a reference to it may name the base
  // part of a derived object, whose dispatch state relocation would replace.
  const std::string hierarchy = R"(import std.utility;
import std.vector;
class Base {
public:
  int value;
  Base(int value) : value(value) {}
  virtual int read() { return this.value; }
};
class Left : public Base {
public:
  Left(int value) : Base(value) {}
  int read() override { return this.value * 100; }
};
interface Shape { int area(); };
class Square : public Shape {
public:
  int side;
  Square(int side) : side(side) {}
  int area() override { return this.side * this.side; }
};
class Plain { public: int value; Plain(int value) : value(value) {} };
class Wider : public Plain { public: Wider(int value) : Plain(value) {} };
class Holder<T> { public: T held; Holder(T held) : held(std::move(held)) {} virtual int tag() { return 1; } };
class Special<T> : public Holder<T> { public: Special(T held) : Holder<T>(std::move(held)) {} int tag() override { return 2; } };
T rotate<T>(mut T& place, T next) requires std::relocatable<T> { return std::exchange(place, std::move(next)); }
)";
  for (const std::string body :
       {// A leaf polymorphic class, a non-polymorphic base and indexed
        // element swaps of a polymorphic base stay valid.
        "mut Square a = Square(1); mut Square b = Square(2); std::swap(a, b); "
        "Square c = rotate(a, Square(3)); return a.area() + c.area();",
        "mut Wider a = Wider(1); mut Wider b = Wider(2); mut Plain& x = a; "
        "mut Plain& y = b; std::swap(x, y); return a.value;",
        "mut std::vector<Base> row{}; row.push_back(Base(1)); "
        "row.push_back(Base(2)); row.swap_elements(std::size_t(0), "
        "std::size_t(1)); return row[std::size_t(0)].read();"}) {
    const auto valid =
        analyzeWithStandardLibrary("restoring-polymorphic-valid.gti",
                                   hierarchy + "int main() { " + body + " }\n");
    expect(valid.canGenerateCode(), "leaf polymorphic, non-polymorphic and "
                                    "indexed restoring transfers stay valid: " +
                                        body);
  }
  for (const std::string body :
       {"mut Left left = Left(3); mut Left other = Left(4); flip(left, other); "
        "return left.read();",
        "mut Left left = Left(3); return replace(left);",
        "mut Base a = Base(1); mut Base b = Base(2); std::swap(a, b); return "
        "a.read();",
        "mut Base whole = Base(3); Base old = rotate<Base>(whole, Base(5)); "
        "return old.read();",
        "mut Holder<int> a = Holder<int>(1); Holder<int> old = "
        "std::exchange(a, Holder<int>(2)); return old.tag();"}) {
    const std::string source =
        hierarchy +
        "void flip(mut Base& first, mut Base& second) { std::swap(first, "
        "second); }\n"
        "int replace(mut Base& view) { Base old = std::exchange(view, "
        "Base(5)); return old.read(); }\n"
        "int main() { " +
        body + " }\n";
    const auto rejected =
        analyzeWithStandardLibrary("restoring-polymorphic-base.gti", source);
    const auto found = std::find_if(
        rejected.diagnostics.begin(), rejected.diagnostics.end(),
        [](const auto &d) {
          return d.code == "GTI-S2018" &&
                 d.message.find("may name the base part of a derived object") !=
                     std::string::npos;
        });
    expect(
        !rejected.canGenerateCode() && found != rejected.diagnostics.end() &&
            found->primary.source.ends_with("restoring-polymorphic-base.gti") &&
            !found->hints.empty(),
        "restoring transfer of a polymorphic base is a source error at the "
        "user's call: " +
            body);
  }
  return failures ? 1 : 0;
}
