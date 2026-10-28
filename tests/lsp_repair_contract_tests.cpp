// Exercise the production protocol adapter without a test-only server mode.
#define main gti_lsp_server_main
#include "../src/lsp/main.cpp"
#undef main

#include <stdexcept>

namespace {
void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

LspFixIt edit(std::size_t start, std::size_t end, std::string replacement,
              std::string id = "repair") {
  return {.fix = {.span = {"/test.gti", start, end, 1},
                  .replacement = std::move(replacement),
                  .message = "Repair expression",
                  .applicability = lang::RepairApplicability::MachineApplicable,
                  .repairId = std::move(id)},
          .uri = "file:///test.gti",
          .source = "abcdef",
          .sourceKnown = true};
}
} // namespace

int main() {
  require(lang::FixIt{}.applicability ==
              lang::RepairApplicability::RequiresReview,
          "unannotated repairs must require review");
  LspDiagnostic diagnostic;
  diagnostic.fixes = {edit(0, 1, "A"), edit(4, 5, "E"),
                      edit(1, 2, "B", "alternative"), edit(2, 3, "C", "")};
  auto groups = repairGroups(diagnostic);
  require(groups.size() == 3 && groups[0].size() == 2,
          "atomic group was split or alternatives combined");
  require(validRepairGroup(groups[0]), "valid atomic group rejected");
  CodeActionCandidate candidate{.diagnostic = diagnostic,
                                .fixes = groups[0],
                                .version = 7,
                                .preferred = true};
  auto action =
      codeActionJson(candidate, true, false, true, true, false, false);
  const auto *changes =
      member(member(&action, "edit"), "documentChanges")->getAsArray();
  require(changes && changes->size() == 1,
          "atomic group must be one document edit");
  require(member(&changes->front(), "edits")->getAsArray()->size() == 2,
          "atomic group lost an edit");
  require(integerMember(member(&changes->front(), "textDocument"), "version") ==
              7,
          "edit lost version precondition");

  auto bad = groups[0];
  bad[1].fix.span = bad[0].fix.span;
  require(!validRepairGroup(bad), "overlap accepted");
  bad = {edit(1, 1, "x"), edit(1, 1, "y")};
  require(!validRepairGroup(bad), "ambiguous coincident insertions accepted");
  bad = {edit(0, 2, "x"), edit(2, 2, "y")};
  require(!validRepairGroup(bad), "touching insertion accepted");
  require(validRepairGroup({edit(0, 1, "x"), edit(1, 2, "y")}),
          "adjacent replacements rejected");
  for (auto span : {lang::SourceSpan{"", 4, 3}, lang::SourceSpan{"", 0, 99}}) {
    bad = groups[0];
    bad[1].fix.span = span;
    require(!validRepairGroup(bad),
            "invalid member did not reject whole group");
  }
  bad = groups[0];
  bad[1].sourceKnown = false;
  require(!validRepairGroup(bad), "unknown source accepted");
  bad = groups[0];
  bad[1].uri = "file:///other.gti";
  require(!validRepairGroup(bad), "cross-document group partially accepted");
  bad = groups[0];
  bad[1].source += "changed";
  require(!validRepairGroup(bad), "inconsistent source identity accepted");
  bad = groups[0];
  bad[1].fix.applicability = lang::RepairApplicability::Incomplete;
  require(!validRepairGroup(bad), "incomplete group accepted");
  bad[1].fix.applicability = lang::RepairApplicability::RequiresReview;
  require(!validRepairGroup(bad), "mixed applicability accepted");
  bad[0].fix.applicability = lang::RepairApplicability::RequiresReview;
  require(validRepairGroup(bad),
          "complete explicit-review alternative rejected");
  require(!validRepairGroup({edit(1, 2, "b")}), "no-op repair accepted");
  bad = {edit(1, 1, "x")};
  bad[0].source = "\xc3\xa9";
  require(!validRepairGroup(bad), "UTF-8 scalar split accepted");
  bad[0].source = "\r\n";
  require(!validRepairGroup(bad), "CRLF split accepted");
  diagnostic.fixes = {edit(99, 99, "x")};
  require(!hasAvailableRepair(diagnostic), "invalid repair advertised");
  diagnostic.fixes = {edit(0, 1, "A")};
  diagnostic.repairSnapshot = "file:///test.gti:7";
  diagnostic.diagnostic.code = "GTI-test";
  auto published = diagnosticJson(diagnostic, false, true, true, false, false);
  JsonValue requested = JsonArray{std::move(published)};
  require(diagnosticRequested(diagnostic, &requested, true, true),
          "current snapshot rejected");
  diagnostic.repairSnapshot = "file:///test.gti:8";
  require(!diagnosticRequested(diagnostic, &requested, true, true),
          "identical diagnostic from stale snapshot accepted");
  std::cout
      << "repair grouping, projection, ranges and snapshot identity passed\n";
}
