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
    const auto copied =
        analyze(std::string("enum Reading { empty, value(") +
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
  const auto nestedConsumption = analyze(R"gti(
enum Reading { empty, value(mut int& owner) };
enum Outer { empty, wrapped(Reading reading) };
int update(mut int& owner) {
  mut auto result = Outer::wrapped(Reading::value(owner));
  switch (std::move(result)) {
    case Outer::empty: return 1;
    case Outer::wrapped(inner): {
      switch (std::move(inner)) {
        case Reading::empty: return 2;
        case Reading::value(value): { value = 19; return 0; }
      }
    }
  }
}
int main() { mut int owner = 3; return update(owner); }
)gti");
  expect(nestedConsumption.syntaxValid && !nestedConsumption.semanticValid,
         "consuming a nested borrowed-value payload remains a diagnosed "
         "structural boundary");
  expect(std::any_of(nestedConsumption.diagnostics.begin(),
                     nestedConsumption.diagnostics.end(),
                     [](const auto &diagnostic) {
                       return diagnostic.code == "GTI-S2067";
                     }) &&
             std::none_of(nestedConsumption.diagnostics.begin(),
                          nestedConsumption.diagnostics.end(),
                          [](const auto &diagnostic) {
                            return diagnostic.code == "GTI-B0001";
                          }),
         "nested borrowed-value handoff rejects in semantics, not as an ICE");
  const std::string observedPrefix = R"gti(
#include <std/string>
enum Observed { ready(int& value), failed(std::string error) };
[[borrowed_from(owner)]] Observed produce(int& owner, bool fail) {
  if (fail) return Observed::failed(std::string("bad"));
  return Observed::ready(owner);
}
int inspect(Observed& result, Observed& other) {
  switch (result) {
    case Observed::ready(value): return value;
    case Observed::failed(auto& error): return int(error.size());
  }
}
)gti";
  for (const bool exclusive : {false, true}) {
    const auto observed = lang::Frontend().analyze(
        root / "tests" / "reference-payload-owned-field.gti",
        observedPrefix + R"gti(
int main() {
  int owner = 3;
  mut auto result = produce(owner, true);
  if (inspect(result, result) != 3) return 1;
  switch (result) {
    case Observed::ready(value): return value;
)gti" +
            (exclusive
                 ? "case Observed::failed(mut auto& error): { "
                   "error.append(\"!\"); return int(error.size()) - 4; }\n"
                 : "case Observed::failed(auto& error): "
                   "return int(error.size()) - 3;\n") +
            "  }\n}\n",
        {root / "stdlib" / "prelude.gti"}, {}, {root / "stdlib"});
    expect(observed.canGenerateCode(),
           "owned payload storage is independent of external reference owners");
    if (!observed.canGenerateCode())
      for (const auto &diagnostic : observed.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    if (!observed.canGenerateCode())
      continue;
    auto forged = observed.mir;
    bool changed = false;
    for (auto &function : lang::MirProgramTestAccess::functions(forged)) {
      const auto external = std::find_if(
          function.body.loans.begin(), function.body.loans.end(),
          [](const auto &loan) {
            return loan.kind == lang::MirLoanKind::Stored && !loan.entry;
          });
      if (external == function.body.loans.end())
        continue;
      for (auto &loan : function.body.loans) {
        if (loan.kind != lang::MirLoanKind::Local || loan.referenceSelection)
          continue;
        const bool ownedField = std::any_of(
            function.body.blocks.begin(), function.body.blocks.end(),
            [&](const auto &block) {
              return std::any_of(
                  block.instructions.begin(), block.instructions.end(),
                  [&](const auto &i) {
                    return i.hirValue == loan.producedBy &&
                           i.operation == lang::MirOperation::PayloadExtract &&
                           i.payloadExtractionKind ==
                               lang::PayloadExtractionKind::BorrowValue;
                  });
            });
        if (!ownedField)
          continue;
        expect(loan.parent == 0,
               "owned local enum field borrows have no external-group parent");
        loan.parent = external->id;
        changed = true;
        break;
      }
      if (changed)
        break;
    }
    expect(
        changed && !lang::verifyMirProgram(forged).valid(),
        "an external dependency cannot authorize physical enum field storage");
    auto omitted = observed.mir;
    bool omittedDependency = false;
    for (auto &function : lang::MirProgramTestAccess::functions(omitted)) {
      for (auto &loan : function.body.loans) {
        if (loan.kind != lang::MirLoanKind::Local || loan.referenceSelection ||
            loan.parent == 0 || loan.storedDependency == 0)
          continue;
        loan.storedDependency = 0;
        omittedDependency = true;
        break;
      }
      if (omittedDependency)
        break;
    }
    expect(
        omittedDependency && !lang::verifyMirProgram(omitted).valid(),
        "physical child reborrows retain their parent's external dependency");
    auto missingSelection = observed.mir;
    bool removedSelection = false;
    for (auto &function :
         lang::MirProgramTestAccess::functions(missingSelection)) {
      for (auto &loan : function.body.loans) {
        const auto *parent = function.body.findLoan(loan.parent);
        if (!loan.referenceSelection || !parent || !parent->entry)
          continue;
        loan.referenceSelection.reset();
        removedSelection = true;
        break;
      }
      if (removedSelection)
        break;
    }
    expect(removedSelection &&
               !lang::verifyMirProgram(missingSelection).valid(),
           "unguarded scalar projection cannot omit its exact selection proof");
    for (int mutation = 0; mutation != 4; ++mutation) {
      auto detached = observed.mir;
      bool altered = false;
      for (auto &function : lang::MirProgramTestAccess::functions(detached)) {
        for (auto &block : function.body.blocks) {
          for (auto &instruction : block.instructions) {
            if (instruction.operation != lang::MirOperation::PayloadExtract ||
                instruction.payloadExtractionKind !=
                    lang::PayloadExtractionKind::BorrowValue)
              continue;
            if (mutation == 0)
              instruction.payloadExtractionKind =
                  lang::PayloadExtractionKind::CopyValue;
            else if (mutation == 1)
              instruction.enumVariant = 0;
            else if (mutation == 2)
              instruction.operands.front().place = 0;
            else {
              const auto *carrier =
                  function.body.findPlace(instruction.operands.front().place);
              if (!carrier || function.parameterBindings.size() != 2)
                continue;
              const auto other = std::find_if(
                  function.body.places.begin(), function.body.places.end(),
                  [&](const auto &place) {
                    return place.root == lang::MirPlaceRootKind::Binding &&
                           place.binding == function.parameterBindings[1] &&
                           place.projections.empty();
                  });
              if (other == function.body.places.end() || !other->key ||
                  !carrier->key)
                continue;
              auto replacement = *carrier;
              replacement.id = function.body.places.size() + 1;
              replacement.binding = other->binding;
              replacement.key = other->key;
              replacement.key->projections = carrier->key->projections;
              function.body.places.push_back(std::move(replacement));
              instruction.operands.front().place =
                  function.body.places.back().id;
            }
            altered = true;
            break;
          }
          if (altered)
            break;
        }
        if (altered)
          break;
      }
      expect(
          altered && !lang::verifyMirProgram(detached).valid(),
          "owned field dependency requires exact borrow mode, tag and carrier");
    }
  }
  for (const std::string body :
       {"result = produce(owner, true); return int(error.size());",
        "auto moved = std::move(result); return int(error.size());"}) {
    const auto invalidated = analyze(observedPrefix + R"gti(
int main() {
  int owner = 3;
  mut auto result = produce(owner, true);
  switch (result) {
    case Observed::ready(value): return value;
    case Observed::failed(auto& error): {
)gti" + body + "\n    }\n  }\n}\n");
    expect(
        !invalidated.semanticValid &&
            std::none_of(invalidated.diagnostics.begin(),
                         invalidated.diagnostics.end(),
                         [](const auto &d) { return d.code == "GTI-B0001"; }),
        "live owned payload references prevent shell replacement and move");
  }
  const auto referenceEntry = lang::Frontend().analyze(
      root / "tests" / "reference-payload-formal.gti", R"gti(
#include <std/string>
enum Reading { ready(int& value), failed(std::string error) };
bool has_value(Reading& result) {
  switch (result) {
    case Reading::ready: return true;
    case Reading::failed: return false;
  }
}
int main() { return 0; }
)gti",
      {root / "stdlib" / "prelude.gti"}, {}, {root / "stdlib"});
  expect(referenceEntry.canGenerateCode(),
         "enum reference formal has a checked abstract external entry");
  if (!referenceEntry.canGenerateCode())
    for (const auto &diagnostic : referenceEntry.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  if (referenceEntry.canGenerateCode()) {
    for (int mutation = 0; mutation != 5; ++mutation) {
      auto malformed = referenceEntry.mir;
      bool changed = false;
      for (auto &function : lang::MirProgramTestAccess::functions(malformed)) {
        for (auto &loan : function.body.loans) {
          const auto *source = function.body.findPlace(loan.source);
          if (!loan.entry || loan.kind != lang::MirLoanKind::Stored ||
              !source || source->type.kind != lang::SemanticType::Reference ||
              source->type.arguments.size() != 1 ||
              source->type.arguments.front().kind != lang::SemanticType::Enum)
            continue;
          if (mutation == 0)
            loan.conditions.clear();
          else if (mutation == 1)
            function.body.places[loan.source - 1].binding = 0;
          else if (mutation == 2)
            loan.carriers.push_back(source->binding);
          else if (mutation == 3)
            loan.parent = loan.id;
          else
            function.body.places[loan.source - 1].type.arguments.front() =
                lang::SemanticType::Int32;
          changed = true;
          break;
        }
        if (changed)
          break;
      }
      expect(changed, "enum formal mutation finds its abstract entry");
      expect(!lang::verifyMirProgram(malformed).valid(),
             "abstract enum entry rejects missing guard, formal, wrong "
             "carrier, parent and referent type");
    }
  }
  const std::string classCarrierPrefix = R"gti(
class carrier {
  int32_t& source;
public:
  carrier(int32_t& value) : source(value) {}
};
enum holder { one(carrier value), none };
)gti";
  const auto classCarrier = lang::Frontend().analyze(
      root / "tests" / "reference-class-payload.gti",
      classCarrierPrefix + R"gti(
int main() {
  mut int32_t backing = 3;
  carrier value = carrier(backing);
  holder held = holder::one(value);
  return 0;
}
)gti",
      {root / "stdlib" / "prelude.gti"}, {}, {root / "stdlib"});
  if (!classCarrier.canGenerateCode())
    for (const auto &diagnostic : classCarrier.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(
      classCarrier.canGenerateCode(),
      "passive borrowed class payload preserves origin and prepared lifetime");
  const auto danglingClassCarrier = analyze(classCarrierPrefix + R"gti(
holder dangling() {
  mut int32_t backing = 3;
  carrier value = carrier(backing);
  return holder::one(value);
}
int main() { return 0; }
)gti");
  expect(danglingClassCarrier.syntaxValid &&
             !danglingClassCarrier.semanticValid,
         "borrowed class payload cannot escape its local referent");
  const auto assignedClassCarrier = analyze(classCarrierPrefix + R"gti(
int main() {
  int32_t backing = 3;
  carrier value = carrier(backing);
  mut holder first = holder::one(value);
  holder second = holder::one(value);
  first = second;
  return 0;
}
)gti");
  expect(assignedClassCarrier.syntaxValid &&
             !assignedClassCarrier.semanticValid,
         "native passive copy transport does not grant source enum assignment");
  if (classCarrier.canGenerateCode())
    for (const auto &enumeration : classCarrier.mir.payloadEnums())
      if (!enumeration.variants.empty())
        expect(enumeration.traits.copyable &&
                   !enumeration.traits.copyAssignable,
               "borrowed class enum preserves construction-only copy traits");
  if (classCarrier.canGenerateCode()) {
    for (int mutation = 0; mutation != 6; ++mutation) {
      auto malformed = classCarrier.mir;
      bool changed = false;
      for (auto &function : lang::MirProgramTestAccess::functions(malformed)) {
        auto &body = function.body;
        for (auto &block : body.blocks)
          for (auto &instruction : block.instructions) {
            if (mutation < 3 &&
                instruction.operation == lang::MirOperation::PayloadConstruct &&
                instruction.operands.size() == 1 &&
                !instruction.lifecycle.empty()) {
              auto &event = instruction.lifecycle.front();
              if (mutation == 0)
                event.source = 0;
              else if (mutation == 1)
                instruction.lifecycle.push_back(event);
              else
                instruction.lifecycle.clear();
              changed = true;
            }
            if ((mutation == 3 || mutation == 4) &&
                instruction.kind == lang::MirInstructionKind::Construct &&
                instruction.loan && instruction.constructorTarget) {
              auto &loan = body.loans[*instruction.loan - 1];
              if (mutation == 3)
                loan.source = 0;
              else
                loan.producedBy = 0;
              changed = true;
            }
            if (mutation == 5 &&
                instruction.kind == lang::MirInstructionKind::CallInput &&
                instruction.preparedParameterDrop) {
              const auto replacement = std::find_if(
                  body.dropObligations.begin(), body.dropObligations.end(),
                  [&](const auto &drop) {
                    return drop.id != *instruction.preparedParameterDrop &&
                           drop.dropType.type == instruction.info.type;
                  });
              if (replacement != body.dropObligations.end()) {
                instruction.preparedParameterDrop = replacement->id;
                changed = true;
              }
            }
          }
      }
      expect(
          changed && !lang::verifyMirProgram(malformed).valid(),
          "borrowed class payload rejects absent/duplicate lifetime transfer "
          "and forged constructor origin/publication");
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
  {
    const std::string wrapper = R"gti(
#include <std/panic>
enum Cell { empty, value(int& owner) };
class View {
  mut Cell cell;
public:
  View(int& owner) : cell(Cell::value(owner)) {}
  View() : cell(Cell::empty) {}
  int& read() {
    switch(this.cell) {
    case Cell::value(auto& owner): return owner;
    case Cell::empty: std::panic("empty");
    }
  }
  int& take() && {
    switch(this.cell) {
    case Cell::value(auto& owner): return owner;
    case Cell::empty: std::panic("empty");
    }
  }
};
)gti";
    acceptedSemantics(analyze(wrapper + R"gti(
int main() {
  int owner = 42;
  mut auto view = View(owner);
  int& retained = std::move(view).take();
  return retained - 42;
}
)gti"));
    for (const auto *suffix :
         {"View escape() { int local = 42; return View(local); } int main() { "
          "return 0; }",
          "int& escape() { int local = 42; mut auto view = View(local); return "
          "std::move(view).take(); } int main() { return 0; }",
          "int main() { mut int owner = 42; auto view = View(owner); owner = "
          "7; return view.read(); }"}) {
      const auto result = analyze(wrapper + suffix);
      expect(result.syntaxValid && !result.semanticValid,
             "borrowed class cannot escape a local or invalidate a live "
             "external owner");
    }
    const auto owned = analyze(R"gti(
class Owner {
  mut int value;
public:
  Owner() : value(42) {}
  int& take() && { return this.value; }
};
int main() { return 0; }
)gti");
    expect(
        owned.syntaxValid && !owned.semanticValid,
        "consuming reference extraction cannot retain receiver-owned storage");
  }
  {
    const std::string generic = R"gti(
#include <std/panic>
enum BorrowedCell<T> { empty, value(T& owner) };
class GenericView<T> {
  mut BorrowedCell<T> cell;
public:
  GenericView(T& owner) : cell(BorrowedCell<T>::value(owner)) {}
  T& read() {
    switch (this.cell) {
    case BorrowedCell<T>::value(auto& owner): return owner;
    case BorrowedCell<T>::empty: std::panic("empty");
    }
  }
};
)gti";
    acceptedSemantics(analyze(generic + R"gti(
int main() {
  int owner = 42;
  auto view = GenericView<int>(owner);
  GenericView<int> copied = view;
  return copied.read() + view.read() - 84;
}
)gti"));
    for (const auto *suffix :
         {"GenericView<int> escape() { int local = 1; return "
          "GenericView<int>(local); } int main() { return 0; }",
          "int& escape() { int local = 1; auto view = GenericView<int>(local); "
          "return view.read(); } int main() { return 0; }",
          "int main() { mut int owner = 1; auto view = "
          "GenericView<int>(owner); GenericView<int> copied = view; owner = 2; "
          "return copied.read(); }"}) {
      const auto result = analyze(generic + suffix);
      expect(result.syntaxValid && !result.semanticValid,
             "concrete generic borrow proofs reject local escape and live-copy "
             "mutation");
    }
    const auto nondependent = analyze(generic + R"gti(
class Invalid<T> {
  int& bad() { int local = 7; return local; }
public:
  Invalid() = default;
};
int main() { return 0; }
)gti");
    expect(nondependent.syntaxValid && !nondependent.semanticValid,
           "deferred conditional selection does not hide unrelated "
           "local-reference errors");
    const auto ownedErrorCopy = analyze(R"gti(
#include <std/string>
enum CopyState<T, E> { ready(T& owner), failed(E error) };
class CopyView<T, E> {
  mut CopyState<T, E> state;
public:
  CopyView(T& owner) : state(CopyState<T, E>::ready(owner)) {}
};
int main() {
  int owner = 42;
  auto view = CopyView<int, std::string>(owner);
  CopyView<int, std::string> copied = view;
  return 0;
}
)gti");
    expect(ownedErrorCopy.syntaxValid && !ownedErrorCopy.semanticValid,
           "shared-reference wrapper is not copyable when another payload is "
           "move-only");
    auto invalidSelection = generic;
    const auto selectedReturn = invalidSelection.find("return owner;");
    expect(selectedReturn != std::string::npos,
           "generic selected return fixture exists");
    if (selectedReturn != std::string::npos) {
      invalidSelection.replace(selectedReturn,
                               std::string("return owner;").size(),
                               "T local = owner; return local;");
      const auto concrete = lang::Frontend().analyze(
          root / "tests" / "reference-payload-generic-negative.gti",
          invalidSelection + R"gti(
int main() { int owner = 42; auto view = GenericView<int>(owner); return view.read(); }
)gti",
          {root / "stdlib" / "prelude.gti"}, {}, {root / "stdlib"});
      expect(
          concrete.syntaxValid && !concrete.canGenerateCode() &&
              std::any_of(concrete.diagnostics.begin(),
                          concrete.diagnostics.end(),
                          [](const auto &diagnostic) {
                            return diagnostic.code.starts_with("GTI-S");
                          }),
          "concrete instance must recheck and reject a dangling deferred arm");
    }
  }
  // A loop body is analyzed once. A selection inside it may use the complete
  // dependency set of a carrier the loop cannot write; a writable carrier's
  // set is not carried across iterations, and the diagnostic says so.
  const std::string loopCarriers = R"gti(
enum Slot { item(int& value), empty };
Slot borrow_slot(int& owner, bool present) {
  if (!present) { return Slot::empty; }
  return Slot::item(owner);
}
enum Edit { item(mut int& value), empty };
Edit edit_slot(mut int& owner) { return Edit::item(owner); }
int sum_parameter(Slot& slot, int count) {
  mut int total = 0;
  mut int index = 0;
  while (index < count) {
    index += 1;
    switch (slot) {
    case Slot::item(auto& value): total += value; break;
    case Slot::empty: continue;
    }
  }
  return total;
}
)gti";
  acceptedSemantics(analyze(loopCarriers + R"gti(
int main() {
  int owner = 3;
  auto slot = borrow_slot(owner, true);
  mut int total = 0;
  for (mut int index = 0; index < 2; index += 1) {
    switch (slot) {
    case Slot::item(auto& value): total += value; break;
    case Slot::empty: return 1;
    }
  }
  do {
    switch (slot) {
    case Slot::item(auto& value): total += value; break;
    case Slot::empty: return 2;
    }
  } while (false);
  return total + sum_parameter(slot, 2);
}
)gti"));
  const auto loopDiagnostic = [&](const std::string &body,
                                  const std::string &selected,
                                  const std::string &message,
                                  const std::string &hint) {
    const std::string source = loopCarriers + body;
    const auto result = analyze(source);
    const auto site = source.find(selected, loopCarriers.size());
    const auto found = std::find_if(
        result.diagnostics.begin(), result.diagnostics.end(),
        [&](const auto &diagnostic) {
          return diagnostic.code == "GTI-S2067" &&
                 diagnostic.message.find(message) != std::string::npos;
        });
    return result.syntaxValid && !result.semanticValid &&
           site != std::string::npos && found != result.diagnostics.end() &&
           found->primary.start == site + selected.find("value") &&
           found->primary.end == found->primary.start + 5 &&
           (hint.empty()
                ? found->hints.empty()
                : found->hints.size() == 1 &&
                      found->hints.front().find(hint) != std::string::npos) &&
           std::none_of(result.diagnostics.begin(), result.diagnostics.end(),
                        [](const auto &diagnostic) {
                          return diagnostic.message.find("consuming handoff") !=
                                 std::string::npos;
                        });
  };
  expect(loopDiagnostic(R"gti(
int main() {
  int owner = 3;
  mut auto slot = borrow_slot(owner, true);
  mut int total = 0;
  for (mut int index = 0; index < 2; index += 1) {
    switch (slot) {
    case Slot::item(auto& value): total += value; break;
    case Slot::empty: return 1;
    }
  }
  return total;
}
)gti",
                        "auto& value): total",
                        "requires the subject's complete checked external",
                        "such as one declared without 'mut'"),
         "a writable loop carrier's selection names the loop boundary");
  expect(loopDiagnostic(R"gti(
int main() {
  mut int owner = 3;
  mut auto slot = edit_slot(owner);
  for (mut int index = 0; index < 2; index += 1) {
    switch (slot) {
    case Edit::item(mut auto& value): value += 1; break;
    case Edit::empty: return 1;
    }
  }
  return 0;
}
)gti",
                        "mut auto& value): value",
                        "requires the subject's complete checked external",
                        "select the mutable reference before the loop"),
         "a mutable loop selection is told to select before the loop");
  expect(loopDiagnostic(R"gti(
int main() {
  mut int owner = 3;
  auto slot = edit_slot(owner);
  switch (slot) {
  case Edit::item(mut auto& value): value += 1; return 0;
  case Edit::empty: return 1;
  }
}
)gti",
                        "mut auto& value): value",
                        "requires a mutable enum receiver", ""),
         "an exclusive stored reborrow needs mutable enum storage");
  const auto moved = analyze(loopCarriers + R"gti(
void take(Slot slot) {}
int main() {
  int owner = 3;
  auto slot = borrow_slot(owner, true);
  for (mut int index = 0; index < 2; index += 1) {
    switch (slot) {
    case Slot::item(auto& value): if (value != 3) { return 1; } break;
    case Slot::empty: return 2;
    }
    take(std::move(slot));
  }
  return 0;
}
)gti");
  expect(moved.syntaxValid && !moved.semanticValid &&
             std::any_of(moved.diagnostics.begin(), moved.diagnostics.end(),
                         [](const auto &diagnostic) {
                           return diagnostic.code == "GTI-S2018";
                         }) &&
             std::none_of(moved.diagnostics.begin(), moved.diagnostics.end(),
                          [](const auto &diagnostic) {
                            return diagnostic.code == "GTI-S2067";
                          }),
         "a carrier moved inside the loop fails value flow at the backedge");
  return failures == 0 ? 0 : 1;
}
