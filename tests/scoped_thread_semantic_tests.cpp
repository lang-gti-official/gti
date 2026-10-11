#include "gti/frontend.h"
#include "gti/semantic_analyzer.h"

#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
int failures = 0;
void expect(bool value, std::string_view message) {
  if (!value) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}
} // namespace

int main() {
  const std::string source = R"(
class PairJob {
  int& input;
  mut int& output;
public:
  PairJob(int& input, mut int& output) : input(input), output(output) {}
  void operator()() mut { this.output = this.input; }
};
class NestedJob {
  mut PairJob pair;
public:
  NestedJob(int& input, mut int& output) : pair(PairJob(input, output)) {}
  void operator()() mut { this.pair(); }
};
class ReadJob<T> {
  T& value;
public:
  ReadJob(T& value) : value(value) {}
  void operator()() {}
};
class WriteJob<T> {
  mut T& value;
public:
  WriteJob(mut T& value) : value(value) {}
  void operator()() mut {}
};
[[no_share]] struct ExclusiveOnly { mut int value = 0; };
[[no_transfer]] struct SharedOnly { int value = 0; };
[[no_transfer, no_share]] struct Affine { int value = 0; };
[[no_transfer]] class LocalJob {
  int& value;
public:
  LocalJob(int& value) : value(value) {}
  void operator()() {}
};
class CleanupJob {
  int& value;
public:
  CleanupJob(int& value) : value(value) {}
  void operator()() {}
  ~CleanupJob() {}
};
[[unsafe_transfer]] class AuditedRead<T> {
  T& value;
public:
  AuditedRead(T& value) : value(value) {}
  void operator()() {}
  ~AuditedRead() {}
};
struct OwnedJob { void operator()() {} };
struct OnceJob { void operator()() && {} };
unsafe void unsafe_task() {}
void safe_task() {}
struct WrongResult { int operator()() { return 0; } };
struct NeedsArgument { void operator()(int value) {} };
struct RawJob { mut int* address = nullptr; void operator()() {} };
int main() { return 0; }
)";
  lang::FrontendOptions options;
  options.stopAfter = lang::FrontendPhase::Parsing;
  auto parsed =
      lang::Frontend(options).analyze("scoped-task-shapes.gti", source);
  auto &program = parsed.program;
  for (const auto &diagnostic : parsed.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(parsed.sourceValid && parsed.syntaxValid,
         "scoped shape fixture parses");
  lang::SemanticVisitor semantics(lang::TargetInfo::host(), &parsed.sourceGraph,
                                  true, nullptr, &parsed.sources);
  const bool checked = semantics.check(program);
  for (const auto &diagnostic : semantics.errors())
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(checked, "scoped shape fixture has valid ordinary local semantics");
  if (!checked)
    return 1;

  const auto type = [&](std::string_view name,
                        std::vector<lang::SemanticType> arguments = {}) {
    for (const auto &declaration : program.declarations()) {
      const auto *record =
          dynamic_cast<const lang::ClassDecl *>(declaration.get());
      if (record != nullptr && record->name().lexeme == name) {
        const auto *info = semantics.model().findClassType(*record);
        if (info != nullptr)
          return lang::SemanticType::classType(info->id, std::move(arguments));
      }
    }
    expect(false, "fixture class has a semantic identity");
    return lang::SemanticType(lang::SemanticType::Unknown);
  };
  const auto shape = [&](const lang::SemanticType &task) {
    return semantics.scopedThreadTaskShapeFor(task);
  };
  const auto pairType = type("PairJob");
  const auto pair = shape(pairType);
  expect(pair && pair->dependencies.size() == 2 &&
             pair->dependencies[0].fields.size() == 1 &&
             pair->dependencies[1].fields.size() == 1 &&
             pair->dependencies[0].fields != pair->dependencies[1].fields &&
             pair->dependencies[0].reference.referenceAccess ==
                 lang::AccessMode::ReadOnly &&
             pair->dependencies[1].reference.referenceAccess ==
                 lang::AccessMode::Mutable &&
             pair->entry.capability ==
                 lang::CallableInvocationCapability::Mutable,
         "two checked field paths retain independent access and exact entry");
  const auto nested = shape(type("NestedJob"));
  expect(nested && nested->dependencies.size() == 2 &&
             nested->dependencies[0].fields.size() == 2 &&
             nested->dependencies[1].fields.size() == 2 &&
             nested->dependencies[0].fields[0] ==
                 nested->dependencies[1].fields[0] &&
             nested->dependencies[0].fields[1] !=
                 nested->dependencies[1].fields[1],
         "nested user-defined carriers preserve full field identity");
  expect(!semantics.traitsFor(pairType).transferCapable &&
             !semantics.traitsFor(pairType).shareCapable,
         "scoped inspection never grants ordinary transfer or sharing");

  for (const auto name : {"OwnedJob", "OnceJob"}) {
    const auto task = shape(type(name));
    expect(task && task->dependencies.empty(),
           "owned tasks need no borrowed paths");
  }
  const auto once = shape(type("OnceJob"));
  expect(once &&
             once->entry.capability == lang::CallableInvocationCapability::Once,
         "consuming entry identity is retained for later cleanup planning");

  const auto exclusive = type("ExclusiveOnly"), shared = type("SharedOnly"),
             affine = type("Affine");
  expect(shape(type("WriteJob", {exclusive})).has_value() &&
             !shape(type("ReadJob", {exclusive})),
         "exclusive borrowing requires transfer, shared borrowing requires "
         "sharing");
  expect(shape(type("ReadJob", {shared})).has_value() &&
             !shape(type("WriteJob", {shared})),
         "shared-only referents cannot be lent for exclusive worker access");
  expect(!shape(type("ReadJob", {affine})) &&
             !shape(type("WriteJob", {affine})),
         "thread-affine referents remain excluded through either borrow mode");
  expect(shape(type("AuditedRead", {shared})).has_value() &&
             !shape(type("AuditedRead", {exclusive})),
         "an audited cleanup policy cannot erase checked referent sharing");
  for (const auto name :
       {"LocalJob", "CleanupJob", "WrongResult", "NeedsArgument", "RawJob"})
    expect(!shape(type(name)), "affinity, unaudited cleanup, raw state and "
                               "entry contracts remain checked");
  for (const auto &declaration : program.declarations()) {
    const auto *function =
        dynamic_cast<const lang::FunctionDecl *>(declaration.get());
    if (function == nullptr || (function->name().lexeme != "unsafe_task" &&
                                function->name().lexeme != "safe_task"))
      continue;
    const auto *info = semantics.model().findFunction(*function);
    expect(info != nullptr, "free task has a resolved function identity");
    if (info != nullptr) {
      const auto task = lang::SemanticType::functionItemType(
          info->id, info->returnType, info->parameterTypes);
      expect(shape(task).has_value() ==
                 (function->name().lexeme == "safe_task"),
             "scoped function items preserve the unsafe entry obligation");
    }
  }
  return failures == 0 ? 0 : 1;
}
