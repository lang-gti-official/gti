#include "gti/frontend.h"

#include <algorithm>
#include <filesystem>
#include <iostream>

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
)gti"}) {
    const auto result = analyze(source);
    expect(result.syntaxValid && !result.semanticValid,
           "constructor rejects dangling owners, mutable aliases and later "
           "field mutation");
  }
  return failures == 0 ? 0 : 1;
}
