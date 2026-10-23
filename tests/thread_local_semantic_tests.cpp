#include "gti/frontend.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
int failures = 0;
void expect(bool condition, std::string_view message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

lang::FrontendResult analyze(std::string source, bool concurrent = true,
                             bool executableRepresentation = false) {
  lang::FrontendOptions options;
  options.requireFunctionDefinitions = executableRepresentation;
  options.target = lang::TargetInfo::host();
  if (concurrent) {
    options.target.executionProfile = lang::ExecutionProfile::Concurrent;
    options.target.runtimeCapabilities.threads = true;
  }
  const auto library =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  return lang::Frontend(options).analyze(
      "thread-local-semantics.gti", std::move(source),
      {library / "prelude.gti"}, {}, {library});
}

bool code(const lang::FrontendResult &result, std::string_view expected) {
  return std::any_of(
      result.diagnostics.begin(), result.diagnostics.end(),
      [&](const auto &diagnostic) { return diagnostic.code == expected; });
}

void positive(std::string source, std::string_view message,
              bool concurrent = true) {
  const auto result = analyze(std::move(source), concurrent);
  if (!result.semanticValid)
    for (const auto &diagnostic : result.diagnostics) {
      std::cerr << diagnostic.code << " at " << diagnostic.primary.start << ": "
                << diagnostic.message << '\n';
      for (const auto &related : diagnostic.related)
        std::cerr << "  at " << related.span.start << ": " << related.message
                  << '\n';
    }
  expect(result.semanticValid, message);
}

void storageAndMetadata() {
  const auto result = analyze(R"(
thread_local mut int counter = 0;
class Registry {
public:
  static thread_local mut int value = 1;
};
int next() {
  thread_local mut int local = 3;
  local += 1;
  return local;
}
int main() { counter += 1; Registry::value += 1; return next(); }
)");
  expect(
      result.semanticValid,
      "TLS mutable namespace, static and block storage is valid concurrently");
  expect(result.semantics.programInitializationPlan().steps.empty(),
         "TLS declarations never enter the eager process initialization plan");
  const auto &plan = result.semantics.threadLocalInitializationPlan();
  expect(plan.steps.size() == 3,
         "three distinct TLS storage identities are retained");
  for (const auto &step : plan.steps) {
    const auto *symbol = result.semantics.database().findSymbol(step.symbol);
    expect(step.threadLocal && symbol && symbol->threadLocal,
           "every TLS plan step retains an exact thread-local declaration "
           "identity");
    expect(step.ensureFailure.empty(),
           "literal primitive TLS needs no dynamic initialization guard");
  }
  positive(R"(
class Resource {
public:
  Resource() {}
  Resource(Resource&) = delete;
  Resource(Resource&&) = delete;
  ~Resource() {}
};
thread_local Resource resource{};
int main() { return 0; }
)",
           "an unrelated noncopyable cleanup-owning TLS class is accepted");
  positive("int main() { thread_local mut auto count = 0; count += 1; return "
           "count; }",
           "inferred block-local TLS retains ordinary local type inference");
  positive("thread_local mut int count = 1; int main() { return count; }",
           "TLS also works in the single-threaded profile", false);
}

void lifetimeAndEffects() {
  for (const std::string source :
       {"thread_local mut int value = 0; void modify() { value = 2; } int "
        "main() { int& borrowed = value; modify(); return borrowed; }",
        "mut int value = 0; void modify() { value = 2; } int main() { mut int& "
        "borrowed = value; modify(); return borrowed; }",
        "thread_local mut int value = 0; void modify() { value = 2; } int "
        "use(int& input) { modify(); return input; } int main() { return "
        "use(value); }",
        "thread_local mut int value = 0; int main() { int& borrowed = value; "
        "modify(); return borrowed; } void modify() { value = 2; }"}) {
    const auto result = analyze(source, source.starts_with("thread_local"));
    expect(!result.semanticValid && code(result, "GTI-S2045") &&
               !code(result, "GTI-B0001"),
           "transitive storage mutation conflicts with retained or call-scoped "
           "loans independently of declaration order");
  }
  positive("thread_local mut int first = 1; thread_local mut int second = 2; "
           "void modify() { second += 1; } int main() { int& borrowed = first; "
           "modify(); return borrowed; }",
           "a live TLS borrow does not prevent unrelated TLS storage mutation");
  for (const std::string storage : {"", "thread_local "}) {
    const std::string prefix =
        storage + "mut int value = 1; " + storage + "mut int other = 2; ";
    const auto captured = analyze(
        prefix + "void modify() { value = 3; } int main() { int& input = "
                 "value; auto callback = [&input]() -> int { modify(); return "
                 "input; }; return callback(); }",
        !storage.empty());
    expect(!captured.semanticValid && code(captured, "GTI-S2045") &&
               !code(captured, "GTI-B0001"),
           "captured loans retain exact process/TLS origins through hidden "
           "same-thread callback effects");
    positive(prefix + "void modify() { other = 3; } int main() { int& input = "
                      "value; auto callback = [&input]() -> int { modify(); "
                      "return input; }; return callback(); }",
             "captured loans do not alias disjoint persistent callback "
             "effects",
             !storage.empty());
  }
  positive(
      "thread_local int value = 1; int read() { return value; } int main() { "
      "int& borrowed = value; int copied = read(); return borrowed + copied; }",
      "shared TLS borrows permit independent shared observations");
  positive(
      "thread_local mut int value = 1; void modify() { value = 2; } int main() "
      "{ int& borrowed = value; int copied = borrowed; modify(); return "
      "copied; }",
      "a proved shared last-use endpoint precedes a hidden storage mutation");
  positive("int& current() { thread_local int value = 1; return value; } int "
           "main() { int& borrowed = current(); return borrowed; }",
           "a local TLS declaration outlives its declaring function");
  const auto shadowedReturn =
      analyze("int& current() { thread_local int value = 1; { int value = 2; "
              "return value; } } int main() { return current(); }");
  expect(!shadowedReturn.semanticValid && code(shadowedReturn, "GTI-S2017"),
         "prospective TLS return summaries never authorize a shadowing "
         "automatic local");
  positive(R"(
#include <std/thread>
thread_local int source = 7;
struct Read {
  int& target;
  Read(int& value) : target(value) {}
  void operator()() { int copied = this.target; }
};
void joined(int& input) {
  auto result = std::parallel_invoke(Read(input));
}
int main() { joined(source); return 0; }
)",
           "a joined scoped worker may borrow TLS while the owner thread and "
           "loan remain alive");
  positive(R"(
#include <std/thread>
struct Read {
  int& target;
  Read(int& value) : target(value) {}
  void operator()() { int copied = this.target; }
};
int main() {
  thread_local mut int source = 7;
  auto result = std::parallel_invoke(Read(source));
  return 0;
}
)",
           "a function-local TLS borrow retains a joined scoped dependency");
  const auto escapingTask = analyze(R"(
#include <std/thread>
thread_local int source = 7;
int main() {
  int& input = source;
  auto task = [&input]() -> void { int copied = input; };
  auto started = std::thread::start(std::move(task));
  return 0;
}
)");
  expect(!escapingTask.semanticValid && code(escapingTask, "GTI-S2084"),
         "owned thread tasks cannot retain checked TLS borrows past the owner "
         "lifetime");
  const std::vector<std::pair<std::string, std::string>> workerResults = {
      {"int& worker() { thread_local int state = 1; return state; } ",
       "GTI-S2084"},
      {"struct View { int& item; View(int& source) : item(source) {} }; "
       "thread_local int state = 1; View worker() { return View(state); } ",
       "GTI-S2045"}};
  for (const auto &[result, diagnostic] : workerResults) {
    const auto scopedResult = analyze(
        "#include <std/thread>\n" + result +
        "int main() { auto joined = std::parallel_invoke(worker); return 0; }");
    expect(!scopedResult.semanticValid && code(scopedResult, diagnostic),
           "the scoped void-task boundary or existing borrowed-carrier return "
           "proof rejects worker-owned TLS result publication before drain");
  }

  const std::string prefix = R"(
thread_local mut int value = 1;
void modify() { value = 2; }
)";
  for (const std::string source :
       {"class Reader { public: Reader(int& input) { modify(); int copied = "
        "input; } }; int main() { Reader reader{value}; return 0; }",
        "struct Reader { int operator()(int& input) { modify(); return input; "
        "} }; int main() { Reader reader{}; return reader(value); }",
        "struct View { int& item; View(int& source) : item(source) {} }; int "
        "read(View view) { modify(); return view.item; } int main() { return "
        "read(View(value)); }",
        "void update(mut int& input) { input = 3; } void forwarded() { "
        "update(value); } int main() { int& borrowed = value; forwarded(); "
        "return borrowed; }",
        "int initialize() { modify(); return 3; } int read(int copied = "
        "initialize()) { return copied; } int main() { int& borrowed = value; "
        "int copied = read(); return borrowed + copied; }",
        "int initialize() { modify(); return 3; } int read(int& input, int "
        "copied = initialize()) { return input + copied; } int main() { return "
        "read(value); }",
        "struct Cleanup { ~Cleanup() { modify(); } }; int& current() { Cleanup "
        "cleanup{}; return value; } int main() { return current(); }",
        "struct Cleanup { ~Cleanup() { modify(); } }; int main() { int& "
        "borrowed = value; [[discard]] Cleanup(); return borrowed; }",
        "struct Cleanup { ~Cleanup() { modify(); } }; void inspect(Cleanup& "
        "input) {} int main() { int& borrowed = value; inspect(Cleanup()); "
        "return borrowed; }",
        "struct Cleanup { ~Cleanup() { modify(); } }; int main() { mut Cleanup "
        "destination{}; int& borrowed = value; destination = Cleanup(); return "
        "borrowed; }",
        "struct Cleanup { ~Cleanup() { modify(); } }; struct Hold { int& "
        "input; Hold(int& source) : input(source) {} ~Hold() { int copied = "
        "this.input; } }; int main() { Hold first{value}; Cleanup second{}; "
        "return 0; }"}) {
    const auto result = analyze(prefix + source);
    if (result.semanticValid || !code(result, "GTI-S2045"))
      for (const auto &diagnostic : result.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    expect(!result.semanticValid && code(result, "GTI-S2045"),
           "constructors, operators, aggregate relays, forwarding and cleanup "
           "preserve persistent-loan protection");
  }
  positive(prefix + "struct Cleanup { ~Cleanup() { modify(); } }; int& "
                    "current() { { Cleanup cleanup{}; } return value; } int "
                    "main() { return current(); }",
           "completed earlier cleanup does not conflict with a later returned "
           "borrow");
  positive(prefix + "struct Cleanup { ~Cleanup() { modify(); } }; int main() { "
                    "int& borrowed = value; Cleanup held{}; int copied = "
                    "borrowed; return copied; }",
           "constructing an owned local does not prematurely run its deferred "
           "cleanup");
  positive(prefix + "struct Cleanup { ~Cleanup() { modify(); } }; struct Hold "
                    "{ int& input; Hold(int& source) : input(source) {} "
                    "~Hold() { int copied = this.input; } }; int main() { "
                    "Cleanup first{}; Hold second{value}; return 0; }",
           "reverse cleanup retires an earlier dependency before an unrelated "
           "destructor mutation");
  for (const std::string source :
       {"thread_local mut int other = initialize(); int main() { int& borrowed "
        "= value; int copied = other; return borrowed + copied; }",
        "int read() { thread_local mut int other = initialize(); return other; "
        "} int main() { int& borrowed = value; int copied = read(); return "
        "borrowed + copied; }"}) {
    const auto result =
        analyze(prefix + "int initialize() { modify(); return 4; } " + source);
    expect(!result.semanticValid && code(result, "GTI-S2045"),
           "TLS first-use initializer effects cannot hide a conflicting "
           "storage access");
  }
  positive(
      prefix + "int initialize() { modify(); return 4; } thread_local mut int "
               "other = initialize(); int main() { int warmed = other; int& "
               "ready = other; int& borrowed = value; int copied = other; "
               "return warmed + ready + borrowed + copied; }",
      "a checked loan proves that the same TLS slot is already initialized");
}

void orderedInputProtection() {
  const std::string prefix =
      "thread_local mut int value = 1; int modify() { value = 2; return 3; } ";
  const std::string source =
      prefix + "int read(int& input, int copied) { return input + copied; } "
               "int main() { return read(value, modify()); }";
  const auto result = analyze(source);
  const auto site =
      source.rfind("modify()") + std::string_view("modify(").size();
  expect(!result.semanticValid &&
             std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                         [&](const auto &diagnostic) {
                           return diagnostic.code == "GTI-S2045" &&
                                  diagnostic.primary.start == site &&
                                  diagnostic.primary.end == site + 1;
                         }),
         "a stabilized reference input is protected at the exact later "
         "mutating argument span");
  for (const std::string body :
       {"struct Read { Read(int& input, int copied) {} }; int main() { Read "
        "read{value, modify()}; return 0; }",
        "struct View { int& item; View(int& source) : item(source) {} }; int "
        "read(View view, int copied) { return view.item + copied; } int "
        "main() { return read(View(value), modify()); }",
        "int read(mut int& input, int copied) { return input + copied; } int "
        "main() { return read(value, value); }"}) {
    const auto candidate = analyze(prefix + body);
    expect(!candidate.semanticValid && code(candidate, "GTI-S2045"),
           "stabilized constructor, borrowed-value and exclusive inputs obey "
           "the same ordered protection rule");
  }
  positive(prefix +
               "int read(int copied, int& input) { return input + copied; } "
               "int main() { return read(modify(), value); }",
           "a mutation completed before reference binding remains legal");
  positive(prefix +
               "int read(int& input, int copied) { return input + copied; } "
               "int main() { return read(value, value); }",
           "stabilized shared inputs permit later shared reads");
}

void fullExpressionCleanup() {
  const std::string prefix = R"(
thread_local mut int value = 1;
void modify() { value = 2; }
struct Cleanup { ~Cleanup() { modify(); } };
bool inspect(Cleanup& input) { return true; }
int& current(Cleanup& input) { return value; }
)";
  for (const std::string body :
       {"int main() { int& borrowed = value; if (inspect(Cleanup())) {} "
        "return borrowed; }",
        "int main() { int& borrowed = value; while (inspect(Cleanup())) { "
        "break; } return borrowed; }",
        "int main() { int& borrowed = value; do {} while "
        "(inspect(Cleanup())); return borrowed; }",
        "int main() { int& borrowed = value; for (; inspect(Cleanup());) { "
        "break; } return borrowed; }",
        "int main() { int& borrowed = value; for (mut int i = 0; i < 1; "
        "inspect(Cleanup())) { i += 1; } return borrowed; }",
        "int main() { int& borrowed = value; bool copied = "
        "inspect(Cleanup()); return borrowed; }",
        "struct Holder { int& item; Holder() : item(current(Cleanup())) {} }; "
        "int main() { Holder held{}; return held.item; }"}) {
    const auto result = analyze(prefix + body);
    if (result.semanticValid || !code(result, "GTI-S2045")) {
      std::cerr << "Full-expression source: " << body << '\n';
      for (const auto &diagnostic : result.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    }
    expect(!result.semanticValid && code(result, "GTI-S2045"),
           "one full-expression cleanup rule protects conditions, loop "
           "steps, initializers, returned borrows and constructor fields");
  }
  for (const std::string body :
       {"int main() { int& borrowed = current(Cleanup()); return borrowed; }",
        "int& get() { return current(Cleanup()); } int main() { return "
        "get(); }"}) {
    const auto result = analyze(prefix + body);
    expect(!result.semanticValid && code(result, "GTI-S2017"),
           "an escaping borrow cannot borrow a temporary input even when its "
           "declared result names persistent storage");
  }
  positive(prefix + "struct Wrapper { Cleanup item; Wrapper(Cleanup source) : "
                    "item(std::move(source)) {} }; int main() { int& "
                    "borrowed = value; Wrapper held{Cleanup()}; int copied = "
                    "borrowed; return copied; }",
           "transferring a temporary into an owner does not execute its "
           "eventual destructor at the initializer boundary");
  positive(
      prefix + "struct Wrapper<T> { Cleanup item; T marker; "
               "Wrapper(Cleanup source, T value) : "
               "item(std::move(source)), marker(value) {} }; int main() "
               "{ int& borrowed = value; Wrapper<int> held{Cleanup(), 1}; "
               "int copied = borrowed; return copied; }",
      "concrete generic effect closure preserves conditional cleanup "
      "rather than flattening moved-owner cleanup into an immediate write");
  positive("thread_local mut int value = 1; void modify() { value = 2; } "
           "class Cleanup { public: Cleanup() {} Cleanup(Cleanup&) = delete; "
           "Cleanup(Cleanup&&) = default; ~Cleanup() { modify(); } }; Cleanup "
           "make() { Cleanup local{}; return local; } int main() { int& "
           "borrowed = value; Cleanup held = make(); int copied = borrowed; "
           "return copied; }",
           "an owning factory transfers its local cleanup obligation rather "
           "than treating an implicit return move as a destructor call");
  const auto helperCleanup = analyze(
      prefix + "void helper() { Cleanup local{}; } int main() { int& borrowed "
               "= value; helper(); return borrowed; }");
  expect(!helperCleanup.semanticValid && code(helperCleanup, "GTI-S2045"),
         "a callee's actual destructor effects are preserved even when only "
         "its caller holds the persistent loan");
  for (const std::string constructor :
       {"struct Wrapper { Cleanup item; Wrapper(Cleanup source, int divisor) "
        ": item(std::move(source)) { int result = 1 / divisor; } }; ",
        "struct Wrapper<T> { Cleanup item; Wrapper(Cleanup source, T divisor) "
        ": item(std::move(source)) { int result = 1 / divisor; } }; "}) {
    const bool generic = constructor.find("<T>") != std::string::npos;
    const auto result =
        analyze(prefix + constructor + "int main() { int& borrowed = value; " +
                (generic ? "Wrapper<int>" : "Wrapper") +
                " held{Cleanup(), 1}; return borrowed; }");
    expect(!result.semanticValid && code(result, "GTI-S2045"),
           "potential constructor failure retains cleanup of an initialized "
           "field even after its input parameter moved");
  }
  for (const std::string source :
       {"thread_local mut int value = 1; int main() { int taken = "
        "std::move(value); return taken; }",
        "int main() { thread_local mut int value = 1; int taken = "
        "std::move(value); return taken; }",
        "struct State { mut int value = 1; }; int main() { thread_local mut "
        "State state{}; int taken = std::move(state.value); return taken; }",
        "class Resource { public: Resource() {} Resource(Resource&) = "
        "delete; Resource(Resource&&) = default; ~Resource() {} }; Resource "
        "take() { thread_local mut Resource saved{}; return saved; } int "
        "main() { return 0; }"}) {
    const auto result = analyze(source);
    expect(!result.semanticValid && code(result, "GTI-S2018"),
           "destructive transfer cannot invalidate persistent TLS availability "
           "across calls");
  }
  positive(
      "#include <std/utility>\nthread_local mut int value = 1; int main() "
      "{ int previous = std::exchange(value, 2); return previous + value; }",
      "the exact restoring exchange contract can update a TLS value "
      "without exposing an unavailable persistent slot");
}

void invalidDeclarations() {
  for (const std::string source :
       {"class Bad { thread_local mut int field = 0; }; int main() { return 0; "
        "}",
        "thread_local constexpr int value = 0; int main() { return value; }",
        "int generic<T>(T input) { thread_local mut int value = 0; return "
        "value; } int main() { return generic(1) + generic(true); }",
        "class Bad<T> { public: static thread_local mut int value = 0; }; int "
        "main() { return Bad<int>::value + Bad<bool>::value; }",
        "int use(int input) { thread_local int value = input; return value; } "
        "int main() { return use(1); }",
        "class Bad { int value = 1; public: int use() { thread_local int copy "
        "= this.value; return copy; } }; int main() { return 0; }"}) {
    const auto result = analyze(source);
    expect(!result.semanticValid && code(result, "GTI-S2124") &&
               !code(result, "GTI-B0001"),
           "invalid TLS placement and automatic initializer dependencies are "
           "source diagnostics");
  }
  const std::string source =
      "class Bad { thread_local mut int field = 0; }; int main() { return 0; }";
  const auto invalid = analyze(source);
  const auto diagnostic =
      std::find_if(invalid.diagnostics.begin(), invalid.diagnostics.end(),
                   [](const auto &item) { return item.code == "GTI-S2124"; });
  expect(diagnostic != invalid.diagnostics.end() &&
             diagnostic->primary.start == source.find("thread_local") &&
             diagnostic->primary.end == source.find("thread_local") + 12 &&
             !diagnostic->related.empty() && diagnostic->fixes.empty(),
         "TLS contract errors retain the exact keyword, related declaration "
         "and no speculative rewrite");
  for (const std::string borrowed :
       {"int source = 1; thread_local int& retained = source; int main() { "
        "return 0; }",
        "struct View { int& item; View(int& value) : item(value) {} }; int "
        "source = 1; thread_local View retained{source}; int main() { return "
        "0; }"}) {
    const auto result = analyze(borrowed);
    expect(!result.semanticValid && code(result, "GTI-S2045"),
           "checked borrowed state cannot be retained in TLS through thread "
           "shutdown");
  }
  const std::string library =
      "int initialize() { return 1; } thread_local int state = initialize(); "
      "int read() { return state; }";
  const auto executableLibrary = analyze(library, true, true);
  expect(executableLibrary.semanticValid,
         "a library callable can use dynamic TLS under the explicit runtime "
         "attachment contract");
  expect(analyze(library).semanticValid,
         "declaration/tooling analysis does not require an artifact's hosted "
         "entry");
  expect(analyze("thread_local int state = 1; int read() { return state; }",
                 true, true)
             .semanticValid,
         "passive TLS requires no managed drain in a library artifact");
  const auto constexprStorage = analyze(
      "constexpr int read() { thread_local int state = 1; return state; } "
      "static_assert(read() == 1); int main() { return 0; }");
  expect(!constexprStorage.semanticValid && code(constexprStorage, "GTI-S2057"),
         "the legacy constexpr fallback cannot reinterpret thread-local "
         "storage as an automatic constant variable");
  const auto constexprGlobal = analyze(
      "thread_local int state = 4; constexpr int read() { return state; } "
      "static_assert(read() == 4); int main() { return 0; }");
  expect(!constexprGlobal.semanticValid && code(constexprGlobal, "GTI-S2057"),
         "a constant initializer does not turn per-thread storage into a "
         "constant-context binding");
  positive("constexpr int read() { if constexpr (false) { thread_local int "
           "state = 1; return state; } return 2; } static_assert(read() == 2); "
           "int main() { return 0; }",
           "an unexecuted constexpr branch does not execute runtime TLS");
}

void instanceCleanupCache() {
  // The first selected arm has more root expressions than the second. Reusing
  // its full-expression index in the next instance used to omit the latter's
  // temporary destructor from the source effect graph and reach invalid MIR.
  const std::string prefix = R"(
thread_local mut int value = 1;
thread_local mut int other = 2;
void modify() { value = 2; }
struct Cleanup { ~Cleanup() { modify(); } };
bool inspect(Cleanup& input) { return true; }
int selected<uint64_t Mode>() {
  if constexpr (Mode == 0) {
    int first = 0;
    int second = 1;
    int third = 2;
    int fourth = 3;
    int fifth = 4;
    return first + second + third + fourth + fifth;
  } else {
    bool seen = inspect(Cleanup());
    return seen ? 1 : 0;
  }
}
)";
  for (const bool reverse : {false, true}) {
    const std::string calls =
        reverse ? "int first = selected<1>(); int second = selected<0>(); "
                : "int first = selected<0>(); int second = selected<1>(); ";
    const auto rejected =
        analyze(prefix + "int main() { int& borrowed = value; " + calls +
                    "return borrowed + first + second; }",
                true, true);
    expect(!rejected.semanticValid && code(rejected, "GTI-S2045") &&
               !code(rejected, "GTI-B0001"),
           "each concrete constexpr-selected body retains exact temporary "
           "cleanup effects independently of query order");
    positive(prefix + "int main() { int& borrowed = other; " + calls +
                 "return borrowed + first + second; }",
             "isolated concrete cleanup facts do not conflict with disjoint "
             "TLS loans in either query order");
  }
}
void instanceTextRelayEffects() {
  // Match the payload-field and composed reference-accessor shape exercised by
  // view_field_payloads.gti. The copied view must keep the TLS string's origin,
  // not just its local carrier or the reference accessor's result shell.
  const std::string prefix = R"(
#include <std/panic>
#include <std/string>
thread_local mut std::string value{"owned text"};
thread_local mut std::string other{"other text"};
void modify() { value.append("!"); }
enum Slot<T> { full(T value), empty };
class Holder<T> {
public:
  Slot<T> slot;
  Holder(T value) : slot(Slot<T>::full(std::move(value))) {}
  T& get() {
    switch (this.slot) {
    case Slot<T>::full(auto& value): return value;
    case Slot<T>::empty: std::panic("empty holder");
    }
  }
  T& again() { return this.get(); }
};
int selected<uint64_t Mode>() {
  if constexpr (Mode == 0) {
    int first = 0;
    int second = 1;
    int third = 2;
    return first + second + third;
  } else {
    modify();
    return 1;
  }
}
)";
  for (const bool reverse : {false, true}) {
    const std::string calls =
        reverse ? "int first = selected<1>(); int second = selected<0>(); "
                : "int first = selected<0>(); int second = selected<1>(); ";
    const auto source = [&](std::string_view owner) {
      return prefix +
             "int main() { Holder<std::string_view> held = "
             "Holder<std::string_view>(" +
             std::string(owner) +
             ".view()); std::string_view borrowed = held.again(); " + calls +
             "return borrowed.empty() ? first : second; }";
    };
    const std::string rejectedSource = source("value");
    const auto rejected = analyze(rejectedSource, true, true);
    const auto mutation = rejectedSource.find("selected<1>()");
    const bool exactConflict = std::any_of(
        rejected.diagnostics.begin(), rejected.diagnostics.end(),
        [&](const auto &diagnostic) {
          return diagnostic.code == "GTI-S2045" &&
                 diagnostic.message.find("overlapping path") !=
                     std::string::npos &&
                 diagnostic.primary.start >= mutation &&
                 diagnostic.primary.end <=
                     mutation + std::string_view("selected<1>()").size();
        });
    expect(!rejected.semanticValid && exactConflict &&
               !code(rejected, "GTI-B0001"),
           "payload/accessor text relays retain their TLS owner against "
           "hidden concrete mutation in either query order");
    const auto allowed = analyze(source("other"), true, true);
    if (!allowed.canGenerateCode())
      for (const auto &diagnostic : allowed.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    expect(allowed.canGenerateCode(),
           "the same payload/accessor relay permits disjoint TLS mutation "
           "through both concrete query orders");
  }
}
} // namespace

int main() {
  storageAndMetadata();
  lifetimeAndEffects();
  orderedInputProtection();
  fullExpressionCleanup();
  invalidDeclarations();
  instanceCleanupCache();
  instanceTextRelayEffects();
  return failures ? 1 : 0;
}
