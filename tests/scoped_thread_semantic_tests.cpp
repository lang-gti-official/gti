#include "cpp_backend_test_support.h"
#include "gti/frontend.h"
#include "gti/mir_printer.h"
#include "gti/mir_verifier.h"
#include "gti/semantic_analyzer.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
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

lang::FrontendResult
invocation(std::string body, bool concurrent = true,
           lang::FrontendPhase phase = lang::FrontendPhase::Hir,
           bool privateDeclaration = true) {
  const auto standardLibrary =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const auto threadPath = standardLibrary / "std/thread.gti";
  std::ifstream input(threadPath);
  std::ostringstream contents;
  contents << input.rdbuf();
  // Exercise the private declaration from its actual trusted source identity.
  // Direct private calls supplement tests of the production public wrapper.
  std::ostringstream fixture;
  fixture << R"(
namespace scoped_fixture {
class Write {
  mut int& target;
public:
  Write(mut int& target) : target(target) {}
  void operator()() mut { this.target = 9; }
};
class Read {
  int& target;
public:
  Read(int& target) : target(target) {}
  void operator()() {}
};
struct Data { mut int left = 0; mut int right = 0; };
class Pair {
  int& input;
  mut int& output;
public:
  Pair(int& input, mut int& output) : input(input), output(output) {}
  void operator()() mut { this.output = this.input; }
};
class Nested {
  mut Pair task;
public:
  Nested(int& input, mut int& output) : task(Pair(input, output)) {}
  void operator()() mut { this.task(); }
};
struct Once { void operator()() && {} };
[[no_transfer, no_share]] struct Affine { int value = 0; };
class AffineRead {
  Affine& value;
public:
  AffineRead(Affine& value) : value(value) {}
  void operator()() {}
};
)";
  if (privateDeclaration)
    fixture << R"(
int forward_one<Task>(Task task) {
  return gti_internal::thread_scope(std::move(task));
}
int forward_group<Tasks...>(Tasks... tasks) {
  return gti_internal::thread_scope(tasks...);
}
int relay_group<Tasks...>(Tasks... tasks) {
  return forward_group(tasks...);
}
)";
  fixture << R"(
int scope_probe(mut int& first, mut int& second, mut Data& data,
                Affine& affine) {
)" << body << "\n}\n}\n";
  if (privateDeclaration)
    contents << fixture.str();
  lang::FrontendOptions options;
  options.stopAfter = phase;
  options.target.executionProfile =
      concurrent ? lang::ExecutionProfile::Concurrent
                 : lang::ExecutionProfile::SingleThreaded;
  options.target.runtimeCapabilities.threads = true;
  return lang::Frontend(options).analyze(
      "scoped-invocation.gti",
      "#include <std/thread>\n" +
          (privateDeclaration ? std::string{} : fixture.str()) +
          "int main() { return 0; }",
      {standardLibrary / "prelude.gti"},
      {{threadPath.string(), contents.str()}}, {standardLibrary});
}

void checkComposedAuthorities(const lang::FrontendResult &result) {
  if (!result.canGenerateCode())
    return;
  bool checked = false;
  for (std::size_t index = 0; index < result.mir.functionInstances().size();
       ++index) {
    const auto &body = result.mir.functionInstances()[index].body;
    for (const auto &loan : body.loans) {
      if (loan.kind != lang::MirLoanKind::Stored || loan.entry ||
          loan.escapes || loan.parent == 0 ||
          loan.additionalDependencies.size() != 1)
        continue;
      const auto *source =
          body.findPlace(loan.additionalDependencies[0].source);
      const auto authority = std::find_if(
          body.loans.begin(), body.loans.end(), [&](const auto &candidate) {
            const auto *origin = body.findPlace(candidate.source);
            return candidate.entry &&
                   candidate.kind == lang::MirLoanKind::Parameter &&
                   source != nullptr && origin != nullptr &&
                   source->root == lang::MirPlaceRootKind::Binding &&
                   origin->root == lang::MirPlaceRootKind::Binding &&
                   origin->binding == source->binding &&
                   candidate.id != loan.parent;
          });
      if (authority == body.loans.end())
        continue;
      checked = true;
      expect(lang::mirLoanRetainsDependency(body, loan.id, authority->id),
             "general lifetime queries retain the secondary input authority");
      auto collapsed = result.mir;
      auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
          collapsed.functionInstances());
      functions[index]
          .body.loans[loan.id - 1]
          .additionalDependencies[0]
          .source = loan.source;
      expect(!lang::verifyMirProgram(collapsed).valid(),
             "MIR rejects collapsing independent composed-reference origins");

      auto early = body;
      bool changed = false;
      for (auto &block : early.blocks) {
        for (auto &instruction : block.instructions) {
          if (instruction.kind == lang::MirInstructionKind::EndBorrow &&
              instruction.loan == loan.id) {
            instruction.loan = authority->id;
            changed = true;
            break;
          }
        }
        if (changed)
          break;
      }
      const auto verification = lang::verifyMirBody(early);
      expect(changed && !verification.valid() &&
                 std::any_of(
                     verification.errors.begin(), verification.errors.end(),
                     [](const auto &error) {
                       return error.message.find("before its child reborrow") !=
                              std::string::npos;
                     }),
             "MIR retains the secondary input until its composed child ends");
    }
  }
  expect(checked, "ordinary carrier exercises independent reference entries");
}

void checkScopeMir(const lang::FrontendResult &result) {
  if (!result.mirValid)
    return;
  bool found = false;
  for (std::size_t bodyIndex = 0;
       bodyIndex < result.mir.functionInstances().size(); ++bodyIndex) {
    const auto &body = result.mir.functionInstances()[bodyIndex].body;
    for (std::size_t blockIndex = 0; blockIndex < body.blocks.size();
         ++blockIndex) {
      const auto &block = body.blocks[blockIndex];
      for (std::size_t index = 0; index < block.instructions.size(); ++index) {
        const auto &scope = block.instructions[index];
        if (scope.intrinsic != lang::IntrinsicKind::ThreadScope)
          continue;
        found = true;
        expect(scope.definedFailure.propagation ==
                   lang::FailurePropagationKind::TaskJoin,
               "scope retains the worker failure propagation boundary");
        const auto mutate = [&](auto change) {
          auto malformed = result.mir;
          auto &functions =
              const_cast<std::vector<lang::MirFunctionInstance> &>(
                  malformed.functionInstances());
          change(
              functions[bodyIndex].body.blocks[blockIndex].instructions[index]);
          return lang::verifyMirProgram(malformed).valid();
        };
        expect(!mutate([](auto &call) {
          call.definedFailure.propagation = lang::FailurePropagationKind::None;
        }),
               "MIR rejects a scope without join failure containment");
        if (!scope.scopedThreadTasks.empty()) {
          expect(!mutate([](auto &call) { call.scopedThreadTasks.pop_back(); }),
                 "MIR rejects a missing scoped task record");
          expect(!mutate(
                     [](auto &call) { call.scopedThreadTasks[0].adapter = 0; }),
                 "MIR rejects a scoped task without its exact adapter");
          if (scope.scopedThreadTasks[0].dependency != 0)
            expect(!mutate([](auto &call) {
              call.scopedThreadTasks[0].dependency = 0;
            }),
                   "MIR rejects erased borrowed task origins");
          if (scope.scopedThreadTasks.size() > 1 &&
              scope.scopedThreadTasks[0].dependency != 0 &&
              scope.scopedThreadTasks[1].dependency != 0 &&
              body.findLoan(scope.scopedThreadTasks[0].dependency)->entry &&
              body.findLoan(scope.scopedThreadTasks[1].dependency)->entry &&
              scope.scopedThreadTasks[0].dependency !=
                  scope.scopedThreadTasks[1].dependency)
            expect(!mutate([](auto &call) {
              call.scopedThreadTasks[1].dependency =
                  call.scopedThreadTasks[0].dependency;
            }),
                   "MIR rejects rebinding a task to another pack element loan");
        }
      }
    }
  }
  expect(found, "fixture contains its executable MIR parallel scope");
}

void checkInvocations() {
  for (const auto &body :
       {"return forward_one(Pair(first, second));",
        "return forward_one(Nested(first, second));",
        "return forward_group(Write(first), Write(second));",
        "return forward_group(Read(first), Read(first));",
        "return forward_group(Write(data.left), Write(data.right));",
        "return forward_group(Pair(first, second), Once(), Read(data.left));",
        "return relay_group(Write(first), Write(second));",
        "return relay_group(Read(first), Read(first));",
        "return forward_group();"}) {
    const auto result = invocation(body, true, lang::FrontendPhase::Mir);
    if (!result.canGenerateCode())
      std::cerr << "Forwarding: " << body << '\n';
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    if (!result.hirValid)
      for (const auto &error :
           lang::verifyHirProgramPlans(result.semantics, result.hir).errors)
        std::cerr << "HIR: " << error << '\n';
    if (!result.mirValid)
      for (const auto &function : result.mir.functionInstances())
        if (std::any_of(function.body.places.begin(),
                        function.body.places.end(), [](const auto &place) {
                          return place.type.kind ==
                                 lang::SemanticType::TypePack;
                        }))
          std::cerr << lang::MirPrinter{}.print(function.body);
    expect(result.canGenerateCode(),
           "generic source forwarding preserves borrowed task origins");
    checkScopeMir(result);
  }
  for (const auto &body : {"return forward_group(Write(first), Write(first));",
                           "return relay_group(Read(first), Write(first));",
                           "return forward_one(AffineRead(affine));"}) {
    const auto result = invocation(body);
    if (result.canGenerateCode())
      std::cerr << "Accepted invalid forwarding: " << body << '\n';
    const bool semanticError = std::any_of(
        result.diagnostics.begin(), result.diagnostics.end(),
        [](const auto &diagnostic) {
          return diagnostic.phase == lang::DiagnosticPhase::Semantics;
        });
    expect(!result.canGenerateCode() && semanticError,
           "generic forwarding cannot hide aliasing or thread affinity");
  }
  for (const auto &body :
       {"mut auto task = Pair(first, second); task(); return 0;",
        "mut auto task = Nested(first, second); task(); return 0;",
        "{ mut auto task = Pair(first, second); task(); } "
        "first = 1; second = 2; return 0;"}) {
    const auto result = invocation(body, true, lang::FrontendPhase::Mir, false);
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    expect(
        result.canGenerateCode(),
        "ordinary composed entry-reference carriers retain verified MIR loans");
    checkComposedAuthorities(result);
  }
  const auto sharedLocals =
      invocation("auto left = Read(first); auto right = Read(first); "
                 "left(); right(); return 0;",
                 true, lang::FrontendPhase::Mir, false);
  for (const auto &diagnostic : sharedLocals.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(sharedLocals.canGenerateCode(),
         "ordinary read-only carriers may coexist through a mutable input");
  const auto sharedRead = invocation(
      "auto child = Read(first); int copy = first; child(); return copy;", true,
      lang::FrontendPhase::Mir, false);
  for (const auto &diagnostic : sharedRead.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(
      sharedRead.canGenerateCode(),
      "shared children suspend exclusive access, not ordinary reference reads");
  const auto exclusiveRead = invocation(
      "mut auto child = Write(first); int copy = first; child(); return copy;",
      true, lang::FrontendPhase::Mir, false);
  expect(!exclusiveRead.semanticValid,
         "an exclusive child continues to prohibit overlapping parent reads");
  const auto sharedMutation =
      invocation("auto left = Read(first); first = 3; left(); return 0;", true,
                 lang::FrontendPhase::Mir, false);
  expect(!sharedMutation.semanticValid,
         "shared child carriers do not authorize parent mutation");
  for (const auto &body :
       {"return gti_internal::thread_scope(Write(first), Write(second));",
        "return gti_internal::thread_scope(Read(first), Read(first));",
        "return gti_internal::thread_scope(Write(data.left), "
        "Write(data.right));",
        "return gti_internal::thread_scope(Pair(first, second));",
        "return gti_internal::thread_scope(Nested(first, second));",
        "auto task = Pair(first, second); return "
        "gti_internal::thread_scope(std::move(task));",
        "return gti_internal::thread_scope(Once());",
        "return gti_internal::thread_scope();"}) {
    const auto result = invocation(body, true, lang::FrontendPhase::Mir);
    if (!result.diagnostics.empty())
      std::cerr << "Invocation: " << body << '\n';
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << " at "
                << diagnostic.primary.start << '\n';
    if (!result.mirValid)
      for (const auto &function : result.mir.functionInstances())
        if (std::any_of(function.body.blocks.begin(),
                        function.body.blocks.end(), [](const auto &block) {
                          return std::any_of(
                              block.instructions.begin(),
                              block.instructions.end(),
                              [](const auto &instruction) {
                                return instruction.intrinsic ==
                                       lang::IntrinsicKind::ThreadScope;
                              });
                        }))
          std::cerr << lang::MirPrinter{}.print(function.body);
    expect(result.sourceValid && result.syntaxValid && result.semanticValid,
           "scoped invocation resolves actual consumed task origins");
    expect(
        result.hirValid &&
            lang::verifyHirProgramPlans(result.semantics, result.hir).valid(),
        "scoped invocation preserves an exact verified HIR task group");
    expect(result.mirValid && lang::verifyMirProgram(result.mir).valid(),
           "scoped invocation preserves verified MIR task loans and join-all");
    checkScopeMir(result);
    const lang::ResolvedCallInfo *selected = nullptr;
    for (const auto &declaration : result.program.declarations()) {
      const auto *space =
          dynamic_cast<const lang::NamespaceDecl *>(declaration.get());
      if (space == nullptr || space->name().lexeme != "scoped_fixture")
        continue;
      for (const auto &member : space->declarations()) {
        const auto *function =
            dynamic_cast<const lang::FunctionDecl *>(member.get());
        if (function == nullptr || function->name().lexeme != "scope_probe" ||
            !function->body())
          continue;
        const auto *returned = dynamic_cast<const lang::ReturnStmt *>(
            function->body()->statements().back().get());
        const auto *call =
            returned == nullptr
                ? nullptr
                : dynamic_cast<const lang::Call *>(returned->value().get());
        if (call != nullptr)
          selected = result.semantics.findCall(*call);
      }
    }
    if (selected != nullptr) {
      const auto &resolved = *selected;
      expect(resolved.intrinsic == lang::IntrinsicKind::ThreadScope,
             "resolved operation is the exact scoped intrinsic");
      expect(resolved.parameterTypes.size() ==
                 resolved.scopedThreadTasks.size(),
             "every concrete operand retains one exact task record");
      for (const auto &task : resolved.scopedThreadTasks) {
        expect(task.dependencies.size() == task.shape.dependencies.size(),
               "every borrowed path has its actual origin");
        for (std::size_t index = 0; index < task.dependencies.size(); ++index) {
          const auto &dependency = task.dependencies[index];
          expect(
              dependency.place.root != 0 &&
                  dependency.designatedFields ==
                      task.shape.dependencies[index].fields &&
                  dependency.access ==
                      task.shape.dependencies[index].reference.referenceAccess,
              "origin, field identity and selected access survive resolution");
        }
      }
    }
    expect(selected != nullptr,
           "fixture reaches the identity-bound scope operation");
    for (std::size_t index = 0; index < result.hir.threadTaskAdapters().size();
         ++index) {
      if (!result.hir.threadTaskAdapters()[index].scoped)
        continue;
      auto malformed = result.hir;
      auto &adapters = const_cast<std::vector<lang::HirThreadTaskAdapter> &>(
          malformed.threadTaskAdapters());
      ++adapters[index].scopeArgument;
      expect(!lang::verifyHirProgramPlans(result.semantics, malformed).valid(),
             "HIR rejects a scoped adapter rebound to another operand");
      if (!adapters[index].scopeDependencies.empty()) {
        adapters[index].scopeArgument =
            result.hir.threadTaskAdapters()[index].scopeArgument;
        adapters[index].scopeDependencies.front().place.domain = {};
        expect(
            !lang::verifyHirProgramPlans(result.semantics, malformed).valid(),
            "HIR rejects an origin from outside its body domain");
      }
      break;
    }
  }
  for (const auto &body :
       {"return gti_internal::thread_scope(Write(first), Write(first));",
        "return gti_internal::thread_scope(Read(first), Write(first));",
        "return gti_internal::thread_scope(AffineRead(affine));",
        "auto task = Read(first); return gti_internal::thread_scope(task);"}) {
    const auto result = invocation(body);
    bool scopedError = false;
    for (const auto &diagnostic : result.diagnostics)
      scopedError |= diagnostic.code == "GTI-S2084" &&
                     diagnostic.phase == lang::DiagnosticPhase::Semantics;
    expect(!result.semanticValid && scopedError,
           "overlap, affinity and unconsumed inputs fail in semantics");
  }
  const auto single = invocation("return gti_internal::thread_scope();", false);
  expect(!single.semanticValid,
         "scoped execution requires the concurrent profile");
  for (const auto &body :
       {"auto task = Pair(first, second); second = 3; return "
        "gti_internal::thread_scope(std::move(task));",
        "auto task = Pair(first, second); first = 3; return "
        "gti_internal::thread_scope(std::move(task));",
        "auto child = Write(second); auto task = Pair(first, second); child(); "
        "return gti_internal::thread_scope(std::move(task));"}) {
    const auto result = invocation(body);
    bool loanError = false;
    for (const auto &diagnostic : result.diagnostics)
      loanError |= diagnostic.code == "GTI-S2017";
    expect(!result.semanticValid && loanError,
           "composing entry references does not permit parent mutation or "
           "erase live children");
  }
}

void checkProductionLowering() {
  const auto fixtures =
      std::filesystem::path(__FILE__).parent_path() / "fixtures";
  lang::FrontendOptions options;
  options.stopAfter = lang::FrontendPhase::Mir;
  options.target.executionProfile = lang::ExecutionProfile::Concurrent;
  options.target.runtimeCapabilities.threads = true;
  for (const auto name :
       {"scoped_parallel_threads.gti", "scoped_parallel_cleanup.gti",
        "scoped_parallel_failure.gti"}) {
    const auto path = fixtures / name;
    std::ifstream input(path);
    std::ostringstream source;
    source << input.rdbuf();
    const auto standardLibrary =
        fixtures.parent_path().parent_path() / "stdlib";
    const auto result = lang::Frontend(options).analyze(
        path, source.str(), {standardLibrary / "prelude.gti"}, {},
        {standardLibrary});
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << name << ": " << diagnostic.code << ": " << diagnostic.message
                << '\n';
    expect(result.canGenerateCode(),
           "public parallel wrapper has verified source ownership");
    if (!result.canGenerateCode()) {
      for (const auto &error : lang::verifyMirProgram(result.mir).errors) {
        std::cerr << error.message << '\n';
        if (const auto *function = result.mir.findFunctionInstance(error.owner))
          std::cerr << lang::MirPrinter{}.print(function->body);
      }
      continue;
    }
    bool checkedPackCleanup = false;
    for (const auto &function : result.mir.functionInstances()) {
      for (const auto &loan : function.body.loans) {
        const auto *source = function.body.findPlace(loan.source);
        if (!loan.entry || !loan.lifecycleBound || source == nullptr ||
            source->projections.size() != 1 ||
            source->projections.front().kind !=
                lang::MirProjectionKind::PackElement)
          continue;
        checkedPackCleanup = true;
        auto malformed = result.mir;
        auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
            malformed.functionInstances());
        functions[function.id - 1].body.loans[loan.id - 1].lifecycleBound =
            false;
        expect(!lang::verifyMirProgram(malformed).valid(),
               "owning pack entries cannot lose destructor-bound retention");
      }
    }
    if (std::string_view(name) == "scoped_parallel_cleanup.gti")
      expect(checkedPackCleanup,
             "production cleanup fixture checks exact per-element entry loans");
    try {
      const auto artifact =
          gti_test::emitCpp(result, lang::CppStandard::Cpp23, options.target);
      expect(artifact.contents.find("managed_thread_scope<") !=
                 std::string::npos,
             "production scope roots exact generated adapters");
    } catch (const std::exception &error) {
      std::cerr << name << ": " << error.what() << '\n';
      for (const auto &function : result.mir.functionInstances())
        if (std::any_of(function.body.blocks.begin(),
                        function.body.blocks.end(), [](const auto &block) {
                          return std::any_of(
                              block.instructions.begin(),
                              block.instructions.end(),
                              [](const auto &instruction) {
                                return instruction.intrinsic ==
                                       lang::IntrinsicKind::ThreadScope;
                              });
                        }))
          std::cerr << lang::MirPrinter{}.print(function.body);
      expect(false,
             "public scope is executable through the sealed backend frontier");
    }
  }
}
} // namespace

int main() {
  checkProductionLowering();
  checkInvocations();
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
