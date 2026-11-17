#pragma once

// Snapshot outputs of a gti-test case (`//@ snapshot: mir | llvm-ir |
// diagnostics | format`): selection and normalisation of verified MIR,
// FileCheck-style pattern matching, and comparison with (or blessing of) the
// sibling expected file. See tests/ui/README.md.

#include "test_file.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace lang {
class LoweredProgram;
}

namespace gti_test {

// The verified MIR bodies of the named functions (`mir-function:`), or of
// every function defined in `testSources` when `names` is empty, printed by
// the compiler's MirPrinter and normalised: program-wide numeric identities
// (declarations, symbols, classes, enums and the like) are renumbered by
// first appearance, and fields at their default value (0, -, []) are
// omitted, so a snapshot neither depends on how much standard library was
// analysed before the test nor churns when the printer gains a field.
// `error` is set when a named function has no body.
[[nodiscard]] std::string selectMir(const lang::LoweredProgram &program,
                                    const std::vector<std::string> &names,
                                    const std::set<std::string> &testSources,
                                    std::string &error);

// LLVM IR as the compiler emitted it, with each `define` of a function from
// the test (`!gti.source` naming a span of one of `sources`, path -> text)
// suffixed by `; <name>` so patterns can find it by its source name, and the
// test directory prefix removed from paths.
[[nodiscard]] std::string
annotateLlvmIr(const std::string &ir,
               const std::map<std::string, std::string> &sources,
               const std::string &directory);

// Renumbering and default-field elision of MIR text (exposed for tests).
[[nodiscard]] std::string normalizeMir(const std::string &text);

// Matches `// CHECK:` patterns against `output`. Returns one problem line
// per failure; empty when every pattern holds.
[[nodiscard]] std::vector<std::string>
matchChecks(const std::vector<CheckPattern> &checks, const std::string &output);

// Compares `actual` with the snapshot's expected file, or writes it when
// blessing. Returns the failure text (empty on a match or a bless) and sets
// `blessed` when the file was written.
[[nodiscard]] std::string compareSnapshot(const TestFile &test,
                                          Snapshot snapshot,
                                          const std::string &actual, bool bless,
                                          bool &blessed);

// A line diff of expected and actual text, capped at `limit` lines.
[[nodiscard]] std::string lineDiff(const std::string &expected,
                                   const std::string &actual,
                                   std::size_t limit = 40);

// Expected files larger than this fail tidy: narrow the snapshot.
inline constexpr std::size_t snapshotLineLimit = 400;
inline constexpr std::size_t snapshotByteLimit = 48 * 1024;

} // namespace gti_test
