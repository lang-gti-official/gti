#include "gti/frontend.h"
#include "gti/mir_editor.h"
#include "gti/mir_verifier.h"

#include <algorithm>
#include <filesystem>
#include <iostream>

namespace lang {
struct MirProgramTestAccess {
  static auto &functions(MirProgram &program) { return program.functions; }
};
} // namespace lang

int main() {
  int failures = 0;
  const auto expect = [&](bool condition, const char *message) {
    if (!condition) {
      std::cerr << message << '\n';
      ++failures;
    }
  };
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  const auto analyze = [&](const std::string &source) {
    return lang::Frontend({.stopAfter = lang::FrontendPhase::Semantics})
        .analyze(root / "tests" / "reference-payload-semantic.gti", source,
                 {root / "stdlib" / "prelude.gti"}, {}, {root / "stdlib"});
  };
  const auto acceptedSemantics = [&](const lang::FrontendResult &result) {
    for (const auto &diagnostic : result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    }
    expect(result.syntaxValid && result.semanticValid,
           "reference payload positive passes semantic analysis");
  };
  for (const bool exclusive : {false, true}) {
    const auto result = analyze(
        std::string(exclusive
                        ? "enum Reading { empty, item(mut int& value) };\n"
                        : "enum Reading { empty, item(int& value) };\n") +
        R"gti(
int inspect(mut Reading input) {
  switch (input) {
)gti" +
        (exclusive ? "case Reading::item(mut auto& value): return value;\n"
                   : "case Reading::item(value): return value;\n") +
        R"gti(
  case Reading::empty: return 0;
  }
}
int main() { return 0; }
)gti");
    acceptedSemantics(result);
    bool found = false;
    for (const auto &loan : result.semantics.loans()) {
      if (!loan.referenceSelection)
        continue;
      found = true;
      const auto *entry = result.semantics.findLoan(loan.parent);
      expect(entry && entry->entry &&
                 entry->kind == lang::SemanticLoanKind::StoredValue &&
                 !entry->protectsStorage && !loan.protectsStorage,
             "selection retains checked external authority, not shell storage");
      expect(
          loan.designatedFields.empty() && loan.conditions.empty() &&
              loan.referenceSelection->fields.size() == 1 &&
              loan.referenceSelection->conditions.size() == 1 &&
              loan.referenceSelection->transferredFrom == 0,
          "scalar selection separates external owners from field/tag identity");
      expect(loan.access == (exclusive ? lang::AccessMode::Mutable
                                       : lang::AccessMode::ReadOnly),
             "resolved reference access survives selection");
      if (entry) {
        auto external = entry->place;
        for (const auto field : entry->designatedFields)
          external.projections.push_back(
              {.kind = lang::PlaceProjectionKind::Field, .field = field});
        expect(loan.place == external &&
                   loan.referenceSelection->fields == entry->designatedFields &&
                   loan.referenceSelection->conditions == entry->conditions,
               "selected source is the exact typed parameter reference path");
      }
    }
    if (!found)
      for (const auto &diagnostic : result.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    expect(found,
           "checked entry must produce an exact selected reference loan");
  }
  const auto alias = analyze(R"gti(
enum Reading { empty, item(mut int& value) };
int inspect(mut Reading input) {
  switch (input) {
  case Reading::item(mut auto& value): {
    auto& shared = value;
    return shared;
  }
  case Reading::empty: return 0;
  }
}
int main() { return 0; }
)gti");
  bool foundAlias = false;
  acceptedSemantics(alias);
  for (const auto &loan : alias.semantics.loans())
    if (loan.referenceSelection)
      if (const auto *parent = alias.semantics.findLoan(loan.parent);
          parent && parent->referenceSelection) {
        foundAlias = true;
        const auto &selection = *loan.referenceSelection;
        const auto &original = *parent->referenceSelection;
        expect(selection.source == original.source &&
                   selection.carrier == original.carrier &&
                   selection.fields == original.fields &&
                   selection.conditions == original.conditions &&
                   selection.reference == original.reference &&
                   selection.transferredFrom == original.transferredFrom &&
                   loan.place == parent->place &&
                   loan.access == lang::AccessMode::ReadOnly &&
                   parent->access == lang::AccessMode::Mutable,
               "same-referent aliases retain exact address and narrow access");
      }
  expect(foundAlias,
         "selected reference aliases require a distinct checked child");
  const auto independent = analyze(R"gti(
enum Pair { empty, both(mut int& first, mut int& second) };
int inspect(mut Pair input) {
  switch (input) {
  case Pair::both(mut auto& first, mut auto& second): {
    auto& child = first;
    second = 9;
    return child + second;
  }
  case Pair::empty: return 0;
  }
}
int main() { return 0; }
)gti");
  acceptedSemantics(independent);
  std::vector<lang::SemanticLoanPlace> owners;
  for (const auto &loan : independent.semantics.loans())
    if (loan.referenceSelection && loan.access == lang::AccessMode::Mutable &&
        std::find(owners.begin(), owners.end(), loan.place) == owners.end())
      owners.push_back(loan.place);
  expect(owners.size() == 2 && owners[0].root == owners[1].root &&
             owners[0].projections != owners[1].projections,
         "separate reference fields retain distinct exact external places");
  const auto conflict = analyze(R"gti(
enum Reading { empty, item(mut int& value) };
int inspect(mut Reading input) {
  switch (input) {
  case Reading::item(mut auto& value): {
    auto& shared = value;
    value = 9;
    return shared;
  }
  case Reading::empty: return 0;
  }
}
int main() { return 0; }
)gti");
  expect(std::any_of(conflict.diagnostics.begin(), conflict.diagnostics.end(),
                     [](const auto &diagnostic) {
                       return diagnostic.code == "GTI-S2017";
                     }),
         "an external selected referent remains protected by its live child");
  const auto transfer = analyze(R"gti(
enum Pair { empty, both(mut int& first, mut int& second) };
int consume(mut Pair input) {
  switch (std::move(input)) {
  case Pair::both(first, second): {
    auto& child = first;
    second = 9;
    return child + second;
  }
  case Pair::empty: return 0;
  }
}
int main() { return 0; }
)gti");
  acceptedSemantics(transfer);
  std::vector<lang::SemanticLoanId> transferred;
  for (const auto &loan : transfer.semantics.loans()) {
    if (!loan.referenceSelection ||
        loan.referenceSelection->transferredFrom == 0)
      continue;
    const auto *group =
        transfer.semantics.findLoan(loan.referenceSelection->transferredFrom);
    expect(group && group->entry &&
               group->kind == lang::SemanticLoanKind::StoredValue &&
               !loan.protectsStorage,
           "transfer keeps exact abstract external provenance, not shell life");
    if (loan.parent == 0) {
      transferred.push_back(loan.id);
      expect(loan.access == lang::AccessMode::Mutable,
             "plain consuming pattern transfers mutable field authority");
      const auto *observed = transfer.semantics.findBorrowDependencySet(
          *loan.referenceSelection->source);
      expect(observed && observed->dependencies.size() == 1 &&
                 observed->dependencies.front().parent ==
                     loan.referenceSelection->transferredFrom,
             "extraction retains incoming parents before successor handoff");
    } else {
      const auto *parent = transfer.semantics.findLoan(loan.parent);
      expect(parent && parent->referenceSelection &&
                 parent->referenceSelection->transferredFrom ==
                     loan.referenceSelection->transferredFrom &&
                 loan.access == lang::AccessMode::ReadOnly,
             "a later alias reborrows its live successor, not the old group");
    }
  }
  expect(transferred.size() == 2,
         "consuming two mutable reference fields publishes two successors");
  const auto transferredConflict = analyze(R"gti(
enum Reading { empty, item(mut int& value) };
int consume(mut Reading input) {
  switch (std::move(input)) {
  case Reading::item(value): {
    auto& shared = value;
    value = 9;
    return shared;
  }
  case Reading::empty: return 0;
  }
}
int main() { return 0; }
)gti");
  expect(std::any_of(transferredConflict.diagnostics.begin(),
                     transferredConflict.diagnostics.end(),
                     [](const auto &diagnostic) {
                       return diagnostic.code == "GTI-S2017";
                     }),
         "transferred references still suspend exclusive access for children");
  const auto sourceReuse = analyze(R"gti(
enum Reading { empty, item(mut int& value) };
int consume(mut Reading input) {
  switch (std::move(input)) {
  case Reading::item(value): {
    auto again = std::move(input);
    return value;
  }
  case Reading::empty: return 0;
  }
}
int main() { return 0; }
)gti");
  expect(std::any_of(
             sourceReuse.diagnostics.begin(), sourceReuse.diagnostics.end(),
             [](const auto &diagnostic) {
               return diagnostic.message.find("moved") != std::string::npos;
             }),
         "handoff does not restore availability of the consumed carrier");
  for (const auto &source : {
           R"gti(
enum Reading { empty, value(int& owner) };
[[borrowed_from(owner)]] Reading read(int& owner) {
  return Reading::value(owner);
}
int main() { mut int owner = 3; auto result = read(owner); return 0; }
)gti",
           R"gti(
enum Reading { empty, value(int& owner) };
[[borrowed_from(owner)]] Reading read(int& owner, bool fail) {
  if (fail) return Reading::empty;
  return Reading::value(owner);
}
int main() { mut int owner = 3; auto result = read(owner, true); return 0; }
)gti",
           R"gti(
enum Reading { empty, value(mut int& owner) };
[[borrowed_from(owner)]] Reading read(mut int& owner) {
  return Reading::value(owner);
}
int main() { mut int owner = 3; auto result = read(owner); return 0; }
)gti",
           R"gti(
enum Inner { empty, value(int& owner) };
enum Outer { empty, some(Inner inner) };
[[borrowed_from(owner)]] Outer read(int& owner) {
  return Outer::some(Inner::value(owner));
}
int main() { mut int owner = 3; auto result = read(owner); return 0; }
)gti"})
    acceptedSemantics(analyze(source));
  for (const auto &source : {
           R"gti(
enum Reading { empty, value(int& owner) };
Reading escape() { int local = 3; return Reading::value(local); }
int main() { return 0; }
)gti",
           R"gti(
enum Pair { empty, both(mut int& first, mut int& second) };
int main() { mut int owner = 3; auto result = Pair::both(owner, owner); return 0; }
)gti",
           R"gti(
enum Reading { empty, value(int& owner, int extra) };
int mutate(mut int& owner) { owner = 7; return 1; }
int main() {
  mut int owner = 3;
  auto result = Reading::value(owner, mutate(owner));
  return 0;
}
)gti",
           R"gti(
enum Reading { empty, value(int& owner) };
enum Outer { empty, wrapped(Reading reading, int extra) };
int mutate(mut int& owner) { owner = 7; return 1; }
int main() {
  mut int owner = 3;
  auto result = Outer::wrapped(Reading::value(owner), mutate(owner));
  return 0;
}
)gti"}) {
    const auto result = analyze(source);
    expect(result.syntaxValid && !result.semanticValid,
           "constructor rejects dangling owners, mutable aliases and later "
           "field mutation");
  }
  for (const bool exclusive : {false, true}) {
    const auto copied = analyze(
        std::string("enum Reading { empty, value(") +
        (exclusive ? "mut int&" : "int&") + R"gti( owner) };
int inspect(Reading input) {
  switch (input) {
    case Reading::empty: return 0;
    case Reading::value(value): return value;
  }
}
int main() {
  mut int owner = 3;
  auto first = Reading::value(owner);
  auto second = first;
  return inspect(std::move(first)) + inspect(std::move(second));
}
)gti");
    if (exclusive) {
      expect(copied.syntaxValid && !copied.semanticValid,
             "copying a whole mutable-reference enum cannot duplicate its "
             "exclusive owner authority");
      expect(std::any_of(copied.diagnostics.begin(), copied.diagnostics.end(),
                         [](const auto &diagnostic) {
                           return diagnostic.message.find("copy") !=
                                      std::string::npos ||
                                  diagnostic.message.find("move-only") !=
                                      std::string::npos;
                         }),
             "exclusive enum copying has a source ownership diagnostic");
    } else {
      acceptedSemantics(copied);
    }
  }
  const auto constructed = lang::Frontend().analyze(
      root / "tests" / "reference-payload-constructor.gti", R"gti(
enum Reading { empty, value(int& owner, int sequence) };
[[borrowed_from(owner)]] Reading make(int& owner) {
  return Reading::value(owner, 7);
}
int main() { return 0; }
)gti",
      {root / "stdlib" / "prelude.gti"}, {}, {root / "stdlib"});
  if (!constructed.canGenerateCode())
    for (const auto &diagnostic : constructed.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(constructed.canGenerateCode(),
         "reference constructor reaches independently verified MIR");
  if (constructed.canGenerateCode()) {
    lang::HirFunctionInstanceId owner = 0;
    lang::MirInstructionId constructor = 0;
    lang::MirInstructionId firstStage = 0;
    for (const auto &function : constructed.mir.functionInstances())
      for (const auto &block : function.body.blocks)
        for (const auto &instruction : block.instructions) {
          if (instruction.operation == lang::MirOperation::PayloadConstruct &&
              instruction.operands.size() == 2) {
            owner = function.id;
            constructor = instruction.id;
            const auto *value =
                function.body.findValue(instruction.operands.front().value);
            if (value)
              firstStage = value->definition;
          }
        }
    expect(owner && constructor && firstStage,
           "constructor fixture exposes its exact first field stage");
    for (int mutation = 0; owner && constructor && firstStage && mutation != 6;
         ++mutation) {
      auto malformed = constructed.mir;
      auto &body =
          lang::MirProgramTestAccess::functions(malformed)[owner - 1].body;
      lang::MirInstructionId next = 1;
      for (const auto &block : body.blocks)
        for (const auto &instruction : block.instructions)
          next = std::max(next, instruction.id + 1);
      bool changed = false;
      for (auto &block : body.blocks) {
        for (auto &instruction : block.instructions) {
          if (instruction.id == constructor) {
            if (mutation == 0) {
              instruction.callSite = 0;
              changed = true;
            } else if (mutation == 1) {
              instruction.parameterTypes.front() = lang::SemanticType::Int32;
              changed = true;
            } else if (mutation == 2 && instruction.result) {
              body.values[*instruction.result - 1].borrowDependencies.reset();
              changed = true;
            } else if (mutation == 3 && instruction.loan) {
              body.loans[*instruction.loan - 1].conditions.clear();
              changed = true;
            }
          }
          if (instruction.id == firstStage && mutation == 4) {
            instruction.callInputIndex = 1;
            changed = true;
          }
        }
        if (mutation == 5) {
          const auto stage =
              std::find_if(block.instructions.begin(), block.instructions.end(),
                           [&](const auto &instruction) {
                             return instruction.id == firstStage;
                           });
          if (stage != block.instructions.end() &&
              stage->operands.size() == 1 &&
              stage->operands.front().kind == lang::MirOperandKind::Loan) {
            const auto parent = stage->operands.front().loan;
            block.instructions.insert(
                std::next(stage), {.id = next,
                                   .kind = lang::MirInstructionKind::EndBorrow,
                                   .loan = parent});
            changed = true;
          }
        }
      }
      expect(lang::rebuildMirValueUses(body),
             "constructor mutation retains well-formed SSA use records");
      expect(changed, "each constructor proof mutation changes its target");
      expect(!lang::verifyMirProgram(malformed).valid(),
             "constructor rejects substituted signature/stage/domain/group "
             "and retirement between argument capture and publication");
    }
  }
  for (const bool exclusive : {false, true}) {
    const auto nested = lang::Frontend().analyze(
        root / "tests" / "reference-payload-nested-constructor.gti",
        std::string(exclusive
                        ? "enum Reading { empty, value(mut int& owner) };\n"
                        : "enum Reading { empty, value(int& owner) };\n") +
            R"gti(
enum Outer { empty, wrapped(Reading reading) };
)gti" +
            (exclusive
                 ? "[[borrowed_from(owner)]] Outer make(mut int& owner) {\n"
                 : "[[borrowed_from(owner)]] Outer make(int& owner) {\n") +
            R"gti(
  return Outer::wrapped(Reading::value(owner));
}
int main() { return 0; }
)gti",
        {root / "stdlib" / "prelude.gti"}, {}, {root / "stdlib"});
    if (!nested.canGenerateCode())
      for (const auto &diagnostic : nested.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    expect(nested.canGenerateCode(),
           "nested constructor retains the exact staged producer ancestry");
    if (nested.canGenerateCode()) {
      lang::HirFunctionInstanceId owner = 0;
      lang::MirInstructionId inner = 0, outer = 0;
      for (const auto &function : nested.mir.functionInstances())
        for (const auto &block : function.body.blocks)
          for (const auto &instruction : block.instructions)
            if (instruction.operation == lang::MirOperation::PayloadConstruct &&
                instruction.parameterTypes.size() == 1) {
              owner = function.id;
              if (instruction.parameterTypes.front().kind ==
                  lang::SemanticType::Enum)
                outer = instruction.id;
              else
                inner = instruction.id;
            }
      expect(owner && inner && outer, "nested fixture has two exact producers");
      for (int mutation = 0;
           owner && inner && outer && mutation != (exclusive ? 8 : 5);
           ++mutation) {
        auto malformed = nested.mir;
        auto &body =
            lang::MirProgramTestAccess::functions(malformed)[owner - 1].body;
        lang::MirInstruction *innerProducer = nullptr, *outerProducer = nullptr;
        for (auto &block : body.blocks)
          for (auto &instruction : block.instructions) {
            if (instruction.id == inner)
              innerProducer = &instruction;
            if (instruction.id == outer)
              outerProducer = &instruction;
          }
        bool changed = false;
        if (innerProducer && outerProducer && innerProducer->loan &&
            outerProducer->loan) {
          const auto innerLoan = innerProducer->loan;
          const auto outerValue = outerProducer->hirValue;
          auto &retained = body.loans[*innerProducer->loan - 1];
          auto &published = body.loans[*outerProducer->loan - 1];
          if (mutation == 0) {
            published.parent = retained.id;
            changed = true;
          } else if (mutation == 1) {
            retained.producedBy = outerProducer->hirValue;
            changed = true;
          } else if (mutation == 2) {
            innerProducer->loan = outerProducer->loan;
            changed = true;
          } else if (mutation <= 4) {
            for (auto &block : body.blocks) {
              auto &terminator = block.terminator;
              if (terminator.kind == lang::MirTerminatorKind::Return &&
                  terminator.returnLoan) {
                if (mutation == 3)
                  body.loans[*terminator.returnLoan - 1].conditions.clear();
                else
                  terminator.returnLoan = innerProducer->loan;
                changed = true;
              }
            }
          } else if (mutation == 5) {
            for (auto &block : body.blocks)
              for (auto it = block.instructions.begin();
                   it != block.instructions.end(); ++it)
                if (it->kind == lang::MirInstructionKind::EndBorrow &&
                    it->loan == innerLoan) {
                  block.instructions.erase(it);
                  changed = true;
                  break;
                }
          } else if (mutation == 6) {
            // A second executable use of the supposedly consumed source must
            // invalidate its transfer even when the duplicated stage is unused.
            for (auto &block : body.blocks)
              for (const auto &instruction : block.instructions)
                if (instruction.kind == lang::MirInstructionKind::CallInput &&
                    instruction.callSite == outerValue) {
                  auto duplicate = instruction;
                  duplicate.id = 1;
                  for (const auto &candidateBlock : body.blocks)
                    for (const auto &candidate : candidateBlock.instructions)
                      duplicate.id = std::max(duplicate.id, candidate.id + 1);
                  duplicate.result.reset();
                  block.instructions.push_back(std::move(duplicate));
                  changed = true;
                  break;
                }
          } else {
            for (auto &block : body.blocks)
              for (auto &instruction : block.instructions)
                if (instruction.kind == lang::MirInstructionKind::CallInput &&
                    instruction.callSite == outerValue) {
                  instruction.callInputIndex = 1;
                  changed = true;
                }
          }
        }
        expect(changed, "nested mutation changes its exact proof target");
        expect(lang::rebuildMirValueUses(body),
               "nested mutation retains well-formed SSA use records");
        expect(!lang::verifyMirProgram(malformed).valid(),
               "nested constructor rejects substituted parent/producer/group "
               "and erased or substituted return dependencies");
      }
    }
  }
  for (const auto *source : {
           R"gti(
enum Reading { empty, value(int& owner, int sequence) };
[[borrowed_from(owner)]] Reading make(int& owner) {
  return Reading::value(owner, 7);
}
int main() {
  int owner = 3;
  auto result = make(owner);
  switch (result) {
  case Reading::empty: return 1;
  case Reading::value(value, sequence): return value + sequence - 10;
  }
}
)gti",
           R"gti(
enum Reading { empty, value(int& owner) };
[[borrowed_from(owner)]] Reading make(int& owner, bool fail) {
  if (fail) return Reading::empty;
  return Reading::value(owner);
}
int main() { return 0; }
)gti"}) {
    const auto result = lang::Frontend().analyze(
        root / "tests" / "reference-payload-factory.gti", source,
        {root / "stdlib" / "prelude.gti"}, {}, {root / "stdlib"});
    if (!result.canGenerateCode())
      for (const auto &diagnostic : result.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    expect(
        result.canGenerateCode(),
        "factory calls and empty/borrowed return paths verify independently");
  }
  return failures == 0 ? 0 : 1;
}
