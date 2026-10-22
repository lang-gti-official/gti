#include "gti/frontend.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {
int failures = 0;
void expect(bool condition, std::string_view message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

lang::FrontendResult analyze(std::string source, bool concurrent = true) {
  lang::FrontendOptions options;
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
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
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
        "struct Cleanup { ~Cleanup() { modify(); } }; int& current() { Cleanup "
        "cleanup{}; return value; } int main() { return current(); }",
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

void invalidDeclarations() {
  for (const std::string source :
       {"class Bad { thread_local mut int field = 0; }; int main() { return 0; "
        "}",
        "thread_local constexpr int value = 0; int main() { return value; }",
        "int generic<T>(T input) { thread_local mut int value = 0; return "
        "value; } int main() { return generic(1); }",
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
}
} // namespace

int main() {
  storageAndMetadata();
  lifetimeAndEffects();
  invalidDeclarations();
  return failures ? 1 : 0;
}
