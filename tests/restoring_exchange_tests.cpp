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
  for (const auto &function : shapes.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &call : block.instructions) {
        if (call.intrinsic != lang::IntrinsicKind::RestoringExchange)
          continue;
        ++shapeResults;
        expect(lang::CppMirBodyEmitter(shapes.mir, shapeRows)
                   .supportsFailureBodyText(
                       {lang::MirBodyKind::Function, function.id}),
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
            expect(!lang::CppMirBodyEmitter(invalid, shapeRows)
                        .supportsFailureBodyText(
                            {lang::MirBodyKind::Function, function.id}),
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
              !instruction.functionTarget || !instruction.successResultDrop)
            continue;
          ++callPublications;
          expect(lang::CppMirBodyEmitter(results.mir, rows)
                     .supportsFailureBodyText(
                         {lang::MirBodyKind::Function, function.id}),
                 "array call results retain exact production storage through "
                 "their consumers");
          auto invalid = results.mir;
          auto *owner = const_cast<lang::MirFunctionInstance *>(
              invalid.findFunctionInstance(function.id));
          auto *home = owner->body.findBlock(block.id);
          home->terminator.successLifecycle.clear();
          expect(!lang::verifyMirProgram(invalid).valid(),
                 "array call result requires success-edge activation");
          expect(!lang::CppMirBodyEmitter(invalid, rows)
                      .supportsFailureBodyText(
                          {lang::MirBodyKind::Function, function.id}),
                 "array result representation independently rejects missing "
                 "activation");
          for (const auto &consumerBlock : function.body.blocks) {
            for (const auto &consumer : consumerBlock.instructions) {
              for (const auto &event : consumer.lifecycle) {
                if (consumer.kind != lang::MirInstructionKind::Call ||
                    event.kind != lang::MirLifecycleEventKind::TransferOut ||
                    event.source != *instruction.successResultDrop)
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
                expect(!lang::CppMirBodyEmitter(missing, rows)
                            .supportsFailureBodyText(
                                {lang::MirBodyKind::Function, function.id}),
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
            lang::CppMirBodyEmitter(arrayFailure.mir, rows)
                .supportsFailureBodyText(
                    {lang::MirBodyKind::Function, function.id}),
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
#include <std/utility>
#include <std/string>
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
#include <std/utility>
#include <std/string>
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
  for (const std::string &program :
       {"class Fixed { public: Fixed() {} Fixed(Fixed&& other) = delete; }; "
        "int main() { mut Fixed a{}; mut Fixed b{}; std::swap(a,b); return 0; "
        "}",
        "int take(mut int& value) { return std::move(value); } "
        "int main() { mut int x = 1; return take(x); }"}) {
    const auto denied = analyzeWithStandardLibrary(
        "restoring-eligibility.gti", "#include <std/utility>\n" + program);
    expect(!denied.canGenerateCode(), "deleted movement and arbitrary "
                                      "reference consumption remain forbidden");
  }
  for (const std::string &body : {"mut int x = 1; std::swap(x, x);",
                                  "int x = 1; mut int y = 2; std::swap(x, y);",
                                  "mut int x = 1; int old = "
                                  "std::exchange(x, std::move(x));",
                                  "mut int x = 1; int& borrowed = x; "
                                  "std::exchange(x, 2); return borrowed;",
                                  "mut int x = 1; return std::move(x);"}) {
    // The last case exercises the unchanged baseline, not a negative case.
    const auto probe = analyzeWithStandardLibrary(
        "restoring-borrow.gti",
        "#include <std/utility>\nint main() { " + body + " return 0; }");
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
    for (const auto &function : indexed.mir.functionInstances()) {
      for (const auto &block : function.body.blocks) {
        for (const auto &instruction : block.instructions) {
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
    }
    expect(indexedSeen >= 4, "indexed transfers instantiate across arrays and "
                             "vectors of unrelated resources and strings");
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
            expect(lang::CppMirBodyEmitter(indexed.mir, rows)
                       .supportsFailureBodyText(
                           {lang::MirBodyKind::Function, function.id}),
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
              expect(!lang::CppMirBodyEmitter(malformed, rows)
                          .supportsFailureBodyText(
                              {lang::MirBodyKind::Function, function.id}),
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
  for (const std::string &body :
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
        "#include <std/vector>\n#include <std/array>\n"
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
  return failures ? 1 : 0;
}
