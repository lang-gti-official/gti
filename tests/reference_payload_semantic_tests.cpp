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
  const auto onlyAdmissionDiagnostics = [&](const lang::FrontendResult
                                                &result) {
    bool expected = !result.diagnostics.empty();
    for (const auto &diagnostic : result.diagnostics) {
      const bool gate =
          diagnostic.code == "GTI-S2067" &&
          (diagnostic.message ==
               "Payload enums do not yet store reference fields." ||
           diagnostic.message ==
               "Payload enum fields are values and do not take 'mut'." ||
           diagnostic.message.find(
               "is not a supported owning or passive enum payload.") !=
               std::string::npos);
      if (!gate) {
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
        expected = false;
      }
    }
    expect(
        expected,
        "producer positives must have only the closed admission diagnostics");
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
    // Deliberately observe semantic producer evidence while the independent
    // public admission gate remains closed. This is not an accepted program
    // or a test-only bypass of HIR/MIR verification.
    expect(result.syntaxValid && !result.semanticValid &&
               !result.canGenerateCode(),
           "reference payloads remain source-rejected during proof staging");
    onlyAdmissionDiagnostics(result);
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
  onlyAdmissionDiagnostics(alias);
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
  onlyAdmissionDiagnostics(independent);
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
  return failures == 0 ? 0 : 1;
}
