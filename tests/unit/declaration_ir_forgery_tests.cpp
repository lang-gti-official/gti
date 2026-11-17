// MIR and HIR verifier checks over forged programs from the former
// compiler_declarations_tests.cpp: hidden scope-end drops of moved loop
// locals, special-member and constructor pack element roles and bounds,
// constructor pack bindings, prepared borrowed-temporary handoff, and a
// split method's prototype substituted for its bound body. The forged
// programs have no source-level observable. (Moved unchanged; their GTI
// sources also became tests/ui cases.)
#include "../lowered_program_test_support.h"
#include "../temporary_directory.h"
#include "gti/frontend.h"
#include "gti/lowered_program_builder.h"
#include "gti/mir_printer.h"
#include "gti/mir_verifier.h"
#include "unit_test.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

std::filesystem::path standardLibraryPrelude() {
  return std::filesystem::path(__FILE__)
             .parent_path()
             .parent_path()
             .parent_path() /
         "stdlib/prelude.gti";
}

std::filesystem::path standardLibraryRoot() {
  return standardLibraryPrelude().parent_path();
}

std::filesystem::path fixturePath(std::string_view name) {
  return std::filesystem::path(__FILE__).parent_path().parent_path() /
         "fixtures" / name;
}

bool hasMirVerificationError(const lang::MirVerificationResult &result,
                             std::string_view text) {
  return std::any_of(result.errors.begin(), result.errors.end(),
                     [&](const auto &error) {
                       return error.message.find(text) != std::string::npos;
                     });
}

void testMovedLoopLocalHiddenScopeEnd() {
  const lang::FrontendResult lexicalLocals = lang::Frontend().analyze(
      "moved-loop-local-lexical.gti", R"(
class Tracker {
  int32_t value;

public:
  Tracker(int32_t initial) : value(initial) {}
  Tracker(Tracker&& other) = default;
  ~Tracker() {}

  int32_t read() { return this.value; }
};

int32_t consume(Tracker taken) { return taken.read(); }

int main() {
  mut int32_t total = 0;
  mut int32_t index = 0;
  while (index < 4) {
    mut Tracker element{index + 1};
    if (index % 2 == 0) {
      total = total + consume(std::move(element));
    }
    index++;
  }
  return total;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(lexicalLocals.canGenerateCode() && lexicalLocals.mirValid &&
             lexicalLocals.mir.valid(),
         "a conditionally moved lexically dropped loop-local should verify "
         "through its scope-end drop across the loop backedge");

  const lang::HirFunctionInstance *lexicalMain = nullptr;
  for (const lang::HirFunctionInstance &function :
       lexicalLocals.hir.functionInstances()) {
    if (function.source != nullptr &&
        function.source->name().lexeme == "main") {
      lexicalMain = &function;
      break;
    }
  }
  lang::MirProgram hiddenScopeEnd = lexicalLocals.mir;
  std::size_t hiddenDrops = 0;
  if (lexicalMain != nullptr) {
    auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
        hiddenScopeEnd.functionInstances());
    for (lang::MirFunctionInstance &function : functions) {
      if (function.id != lexicalMain->id) {
        continue;
      }
      for (lang::MirBlock &block : function.body.blocks) {
        for (lang::MirInstruction &instruction : block.instructions) {
          if (instruction.kind != lang::MirInstructionKind::Drop ||
              !instruction.destination) {
            continue;
          }
          const lang::MirPlace *place =
              function.body.findPlace(*instruction.destination);
          if (place == nullptr ||
              place->root != lang::MirPlaceRootKind::Binding ||
              !place->projections.empty() ||
              place->traits.drop != lang::DropKind::Lexical || !place->key) {
            continue;
          }
          instruction.ownership =
              lang::OwnershipEvent{.kind = lang::OwnershipEventKind::Read,
                                   .place = *place->key,
                                   .reachable = false};
          ++hiddenDrops;
        }
      }
    }
  }
  const lang::MirVerificationResult hiddenScopeEndResult =
      lang::verifyMirProgram(hiddenScopeEnd);
  expect(hiddenDrops != 0 && !hiddenScopeEndResult.valid() &&
             hasMirVerificationError(
                 hiddenScopeEndResult,
                 "initialization targets a place whose lifetime is already "
                 "active"),
         "a lexically dropped loop-local whose scope-end drop is hidden from "
         "the ownership flow model must still fail re-initialization");
}

void testOutOfClassPrototypePublication() {
  const auto frontend = lang::Frontend().analyze("split-methods.gti", R"(
namespace demo {
interface Factory { static int make(int n = 7); };
class Counter {
  mut int count = 0;
public:
  Counter() {}
  int read();
  void add(int amount) mut;
};
int Factory::make(int value) { return value; }
int Counter::read() { return this.count; }
void Counter::add(int value) mut { this.count += value; }
}
int main() {
  mut demo::Counter count{};
  count.add(demo::Factory::make());
  return count.read() - 7;
}
)",
                                                 {standardLibraryPrelude()}, {},
                                                 {standardLibraryRoot()});
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
  expect(frontend.canGenerateCode(),
         "separate static and receiver method bodies retain canonical identity "
         "and defaults");
  if (frontend.canGenerateCode()) {
    lang::HirProgram forged = frontend.hir;
    auto &instances = const_cast<std::vector<lang::HirFunctionInstance> &>(
        forged.functionInstances());
    bool changed = false;
    for (auto &instance : instances) {
      const auto *info = frontend.semantics.findFunction(instance.declaration);
      if (info != nullptr && info->definition != nullptr) {
        instance.source = info->declaration;
        changed = true;
        break;
      }
    }
    const auto rejected = lang::LoweredProgramBuilder().build(
        frontend.program, frontend.semantics, forged, frontend.mir,
        frontend.mir, {}, lang::TargetInfo::host());
    expect(
        changed && !rejected.valid(),
        "publication must reject replacing the bound body with its prototype");
  }
}

void testPackSpecialMemberForgeries() {
  const auto fixture = fixturePath("pack_special_member_construction.gti");
  const auto result = lang::Frontend().analyze(fixture, std::nullopt,
                                               {standardLibraryPrelude()}, {},
                                               {standardLibraryRoot()});
  expect(result.canGenerateCode() && lang::verifyMirProgram(result.mir).valid(),
         "exact generated special members must consume concrete pack elements "
         "without pretending to invoke an ordinary constructor");
  if (!result.canGenerateCode()) {
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.message << '\n';
    return;
  }
  std::size_t copies = 0;
  std::size_t moves = 0;
  for (const auto &function : result.mir.functionInstances()) {
    const auto *hir = result.hir.findFunctionInstance(function.id);
    if (hir == nullptr || hir->source == nullptr ||
        (hir->source->name().lexeme != "construct" &&
         hir->source->name().lexeme != "copies"))
      continue;
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.operands.size() != 1 ||
            (instruction.kind != lang::MirInstructionKind::Move &&
             instruction.kind != lang::MirInstructionKind::Load))
          continue;
        const auto *place =
            function.body.findPlace(instruction.operands.front().place);
        if (place == nullptr || place->projections.size() != 1 ||
            place->projections.front().kind !=
                lang::MirProjectionKind::PackElement)
          continue;
        const bool moved = instruction.kind == lang::MirInstructionKind::Move;
        copies += !moved;
        moves += moved;
        expect(instruction.callSite == 0 && !instruction.constructorTarget &&
                   instruction.info.type == place->type &&
                   instruction.operands.front().kind ==
                       (moved ? lang::MirOperandKind::Move
                              : lang::MirOperandKind::Copy),
               "generated pack construction is an exact existing value "
               "copy/move, not a staged ordinary call");
        if (!moved)
          continue;
        expect(instruction.lifecycle.size() == 1 &&
                   instruction.lifecycle.front().kind ==
                       lang::MirLifecycleEventKind::Move &&
                   instruction.lifecycle.front().source != 0 &&
                   instruction.lifecycle.front().target != 0,
               "owning special-member pack movement must transfer the exact "
               "entry element into its returned value lifetime");
        auto missingTransfer = function.body;
        missingTransfer.blocks[block.id - 1]
            .instructions[&instruction - block.instructions.data()]
            .lifecycle.clear();
        expect(!lang::verifyMirBody(missingTransfer, function.id).valid(),
               "removing the owning pack-to-value transfer must be rejected");
        auto wrongSource = function.body;
        auto &forged =
            wrongSource.blocks[block.id - 1]
                .instructions[&instruction - block.instructions.data()];
        forged.operands.front().kind = lang::MirOperandKind::Copy;
        expect(!lang::verifyMirBody(wrongSource, function.id).valid(),
               "a pack move cannot be forged into a copy of a move-only owner");
      }
    }
  }
  expect(copies == 2 && moves == 1,
         "copyable packs remain reusable while an owning pack moves once");
}

void testConstructorPackForgeries() {
  const auto borrowedFixture =
      fixturePath("forward_list_emplacement_borrowed.gti");
  const auto borrowed = lang::Frontend().analyze(borrowedFixture, std::nullopt,
                                                 {standardLibraryPrelude()}, {},
                                                 {standardLibraryRoot()});
  expect(borrowed.canGenerateCode() &&
             lang::verifyMirProgram(borrowed.mir).valid(),
         "forwarding an owned pack element to a read-only constructor "
         "parameter must retain the selected signature in MIR");
  std::size_t borrowedPackInputs = 0;
  for (const auto &constructor : borrowed.mir.constructorInstances()) {
    for (const auto &block : constructor.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::CallInput ||
            instruction.callInputKind != lang::HirCallInputKind::ReadBorrow ||
            instruction.operands.size() != 1)
          continue;
        const auto &operand = instruction.operands.front();
        const auto *place = constructor.body.findPlace(operand.place);
        if (place == nullptr || place->projections.size() != 1 ||
            place->projections.front().kind !=
                lang::MirProjectionKind::PackElement)
          continue;
        ++borrowedPackInputs;
        expect(operand.kind == lang::MirOperandKind::BorrowRead &&
                   operand.type.kind == lang::SemanticType::Reference &&
                   operand.type.arguments.size() == 1 &&
                   operand.type.arguments.front() == place->type &&
                   instruction.info.type == operand.type &&
                   !instruction.preparedParameterDrop,
               "a borrowed pack element must not become an owned parameter");
        for (const auto &candidateBlock : constructor.body.blocks) {
          for (const auto &candidate : candidateBlock.instructions) {
            expect(candidate.kind != lang::MirInstructionKind::Move ||
                       std::none_of(candidate.operands.begin(),
                                    candidate.operands.end(),
                                    [&](const auto &input) {
                                      return input.place == place->id;
                                    }),
                   "read-only pack forwarding must not move its referent");
          }
        }
        for (int mutation = 0; mutation != 2; ++mutation) {
          auto malformed = borrowed.mir;
          auto *changed = const_cast<lang::MirConstructorInstance *>(
              malformed.findConstructorInstance(constructor.id));
          if (mutation == 0) {
            for (auto &changedBlock : changed->body.blocks)
              for (auto &stage : changedBlock.instructions)
                if (stage.id == instruction.id)
                  stage.callInputKind = lang::HirCallInputKind::MoveValue;
          } else {
            for (auto &changedPlace : changed->body.places)
              if (changedPlace.id == place->id)
                changedPlace.projections.front().constantIndex = 999;
          }
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "pack-reference roles and element bounds must verify "
                 "independently of lowering");
        }
      }
    }
  }
  expect(borrowedPackInputs == 1,
         "the borrowed emplacement fixture must exercise pack projection");
  const lang::FrontendResult inPlace = lang::Frontend().analyze(
      gti_test::temporaryDirectory() / "gti-constructor-pack-in-place.gti",
      R"(
import std.memory;
enum class InPlaceTag { value };
class Immovable {
public:
  int value;
  Immovable(int initial) : value(initial) {}
  Immovable(Immovable& other) = delete;
  Immovable(Immovable&& other) = delete;
};
struct Node<T> {
  T value;
  Node<Args...>(InPlaceTag tag, Args... args) : value(T(args...)) {}
};
int main() {
  auto node = std::make_unique<Node<Immovable>>(InPlaceTag::value, 42);
  auto number = std::make_unique<Node<int>>(InPlaceTag::value, 42);
  auto flag = std::make_unique<Node<bool>>(InPlaceTag::value, true);
  auto character = std::make_unique<Node<char>>(InPlaceTag::value, 'x');
  auto zero = std::make_unique<Node<int>>(InPlaceTag::value);
  return node->value.value - 42;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!inPlace.canGenerateCode()) {
    for (const auto &diagnostic : inPlace.diagnostics) {
      std::cerr << diagnostic.message << '\n';
    }
    std::cerr << lang::MirPrinter().print(inPlace.mir);
  }
  expect(inPlace.canGenerateCode() &&
             lang::verifyMirProgram(inPlace.mir).valid(),
         "constructor packs should construct an immovable field in place");
  std::size_t packBindings = 0;
  std::size_t emptyPacks = 0;
  for (const auto &constructor : inPlace.mir.constructorInstances()) {
    if (constructor.parameterBindings.size() != 2)
      continue;
    const auto pack = std::find_if(
        constructor.body.places.begin(), constructor.body.places.end(),
        [&](const auto &place) {
          return place.root == lang::MirPlaceRootKind::Binding &&
                 place.binding == constructor.parameterBindings.back() &&
                 place.projections.empty() &&
                 place.type.kind == lang::SemanticType::TypePack;
        });
    if (pack == constructor.body.places.end())
      continue;
    ++packBindings;
    for (int mutation = 0; mutation != 2; ++mutation) {
      auto malformed = inPlace.mir;
      auto *changed = const_cast<lang::MirConstructorInstance *>(
          malformed.findConstructorInstance(constructor.id));
      if (mutation == 0) {
        changed->parameterBindings.back() = changed->parameterBindings.front();
      } else {
        for (auto &place : changed->body.places) {
          if (place.id == pack->id)
            place.type.concretePack = false;
        }
      }
      expect(!lang::verifyMirProgram(malformed).valid(),
             "constructor packs reject duplicate bindings and symbolic packs");
    }
    if (pack->type.arguments.empty()) {
      ++emptyPacks;
      auto malformed = inPlace.mir;
      auto *changed = const_cast<lang::MirConstructorInstance *>(
          malformed.findConstructorInstance(constructor.id));
      std::swap(changed->parameterBindings.front(),
                changed->parameterBindings.back());
      expect(!lang::verifyMirProgram(malformed).valid(),
             "an empty pack cannot precede a fixed constructor binding even "
             "when flattened parameter types are unchanged");
    }
  }
  expect(packBindings == 5 && emptyPacks == 1,
         "all concrete node constructor packs need exact binding metadata");
  const std::string source = R"(
import std.string;
import std.memory;

class Capture {
public:
  Capture<T>(T value) {}
public:
  Capture() = default;
};

void discard_constructor_pack<Args...>(Args... args) {}
class PackedCapture {
public:
  PackedCapture<Args...>(int fixed, Args... args) {
    discard_constructor_pack(args...);
  }
public:
  PackedCapture() = default;
};

class Tagged<Stored> {
  Stored stored;

public:
  Tagged<Tag>(Stored value, Tag tag) : stored(value) {}
  Stored read() { return this.stored; }
};

int main() {
  Capture integer = Capture(1);
  Capture boolean = Capture(true);
  PackedCapture empty_pack = PackedCapture(0);
  PackedCapture two_pack = PackedCapture(0, true, 2);
  auto allocated_pack = std::make_unique<PackedCapture>(0, 'a', false);
  Tagged<int> tagged = Tagged<int>(4, true);
  mut std::string source = std::string("hello");
  mut std::string copy = std::string(source.begin(), source.end());
  source.push_back('!');
  if (copy == "hello" and source == "hello!" and tagged.read() == 4) {
    return 0;
  }
  return 1;
}
)";

  const std::filesystem::path entry = gti_test::temporaryDirectory() /
                                      "gti-constructor-local-generics/main.gti";
  const lang::FrontendResult valid = lang::Frontend().analyze(
      entry, source, {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!valid.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : valid.diagnostics) {
      std::cerr << "Unexpected constructor-local generic diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(valid.canGenerateCode() && valid.mir.valid() &&
             lang::verifyMirProgram(valid.mir).valid(),
         "constructor-local type arguments should infer exactly, satisfy "
         "declared requirements, and lower through verified MIR");
  if (!valid.canGenerateCode())
    return;
  bool checkedBorrowedPreparation = false;
  for (const auto &function : valid.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &input : block.instructions) {
        if (input.kind != lang::MirInstructionKind::CallInput ||
            input.callInputKind !=
                lang::HirCallInputKind::MoveBorrowedTemporary)
          continue;
        checkedBorrowedPreparation = true;
        expect(input.preparedParameterDrop && input.lifecycle.size() == 1 &&
                   input.lifecycle.front().kind ==
                       lang::MirLifecycleEventKind::Reparent,
               "borrowed iterator temporaries retain prepared ownership until "
               "construction");
        if (!input.preparedParameterDrop)
          continue;
        for (int mutation = 0; mutation != 3; ++mutation) {
          auto malformed = valid.mir;
          auto *body = lang::findMirBody(
              malformed, {lang::MirBodyKind::Function, function.id});
          for (auto &changedBlock : body->blocks) {
            for (auto &changed : changedBlock.instructions) {
              if (changed.id == input.id) {
                if (mutation == 0)
                  changed.lifecycle.clear();
                if (mutation == 1)
                  changed.preparedParameterDrop.reset();
              }
              if (mutation == 2 &&
                  changed.kind == lang::MirInstructionKind::Construct)
                std::erase_if(changed.lifecycle, [&](const auto &event) {
                  return event.kind ==
                             lang::MirLifecycleEventKind::TransferOut &&
                         event.source == *input.preparedParameterDrop;
                });
            }
          }
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "borrowed preparation rejects lost reparent, obligation or "
                 "invocation handoff");
        }
      }
    }
  }
  expect(checkedBorrowedPreparation,
         "constructor iterator exercises prepared borrowed temporary");
}

} // namespace

GTI_UNIT_TEST(moved_loop_local_hidden_scope_end) {
  testMovedLoopLocalHiddenScopeEnd();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(out_of_class_prototype_publication) {
  testOutOfClassPrototypePublication();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(pack_special_member_forgeries) {
  testPackSpecialMemberForgeries();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(constructor_pack_forgeries) {
  testConstructorPackForgeries();
  return failures == 0 ? 0 : 1;
}
