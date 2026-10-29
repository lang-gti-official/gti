#include "gti/frontend.h"
#include "gti/instance_analysis_cache.h"
#include "gti/semantic_analyzer.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
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
                             bool executableRepresentation = false,
                             lang::TrustedBodyAnalysis trustedBodies =
                                 lang::TrustedBodyAnalysis::Complete) {
  lang::FrontendOptions options;
  options.requireFunctionDefinitions = executableRepresentation;
  options.target = lang::TargetInfo::host();
  options.trustedBodies = trustedBodies;
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
public:
  Registry() = default;
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
import std.thread;
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
import std.thread;
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
import std.thread;
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
        "import std.thread;\n" + result +
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
        "input; }  public: Reader() = default; }; int main() { Reader "
        "reader{value}; return 0; }",
        "struct Reader { int operator()(int& input) { modify(); return input; "
        "}  public: Reader() = default; }; int main() { Reader reader{}; "
        "return reader(value); }",
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
        "struct Cleanup { ~Cleanup() { modify(); }  public: Cleanup() = "
        "default; }; int& current() { Cleanup "
        "cleanup{}; return value; } int main() { return current(); }",
        "struct Cleanup { ~Cleanup() { modify(); }  public: Cleanup() = "
        "default; }; int main() { int& "
        "borrowed = value; [[discard]] Cleanup(); return borrowed; }",
        "struct Cleanup { ~Cleanup() { modify(); }  public: Cleanup() = "
        "default; }; void inspect(Cleanup& "
        "input) {} int main() { int& borrowed = value; inspect(Cleanup()); "
        "return borrowed; }",
        "struct Cleanup { ~Cleanup() { modify(); }  public: Cleanup() = "
        "default; }; int main() { mut Cleanup "
        "destination{}; int& borrowed = value; destination = Cleanup(); return "
        "borrowed; }",
        "struct Cleanup { ~Cleanup() { modify(); }  public: Cleanup() = "
        "default; }; struct Hold { int& "
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
  positive(prefix + "struct Cleanup { ~Cleanup() { modify(); }  public: "
                    "Cleanup() = default; }; int& "
                    "current() { { Cleanup cleanup{}; } return value; } int "
                    "main() { return current(); }",
           "completed earlier cleanup does not conflict with a later returned "
           "borrow");
  positive(prefix + "struct Cleanup { ~Cleanup() { modify(); }  public: "
                    "Cleanup() = default; }; int main() { "
                    "int& borrowed = value; Cleanup held{}; int copied = "
                    "borrowed; return copied; }",
           "constructing an owned local does not prematurely run its deferred "
           "cleanup");
  positive(prefix + "struct Cleanup { ~Cleanup() { modify(); }  public: "
                    "Cleanup() = default; }; struct Hold "
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

void exactWholeObjectVirtualEffects() {
  const std::string readers = R"(
class Base {
public:
  virtual int read() { return 1; }
public:
  Base() = default;
};
class Reader : public Base {
public:
  int read() override { return 2; }
public:
  Reader() = default;
};
class Leaf : public Reader { public: Leaf() = default; };
)";
  for (const std::string storage : {"", "thread_local "}) {
    const bool concurrent = !storage.empty();
    const std::string prefix =
        storage + "mut int guarded = 1; " + storage + "mut int other = 2; ";
    const auto accepted = [&](const std::string &source,
                              std::string_view reason) {
      const auto result = analyze(source, concurrent, true);
      if (!result.canGenerateCode())
        for (const auto &diagnostic : result.diagnostics)
          std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
      expect(result.canGenerateCode(), reason);
    };
    const auto rejected = [&](const std::string &source, bool exactEffects,
                              std::string_view reason) {
      const auto result = analyze(source, concurrent, true);
      const bool conflict =
          std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                      [&](const auto &diagnostic) {
                        return diagnostic.code == "GTI-S2045" &&
                               (!exactEffects ||
                                diagnostic.message.find("overlapping path") !=
                                    std::string::npos);
                      });
      const bool valid =
          !result.semanticValid && conflict && !code(result, "GTI-B0001");
      if (!valid)
        for (const auto &diagnostic : result.diagnostics)
          std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
      expect(valid, reason);
    };
    const std::string declared =
        prefix + readers + storage + "Reader reader{}; ";
    accepted(declared + "int main() { return reader.read() - 2; }",
             "a persistent whole-object receiver may invoke its exact harmless "
             "virtual target without conflicting with its own readonly input");
    accepted(declared + "int main() { int& held = guarded; "
                        "return reader.read() + held; }",
             "known virtual effects preserve an unrelated persistent loan");
    accepted(prefix + readers +
                 "int main() { Reader local{}; "
                 "int& held = guarded; return local.read() + held; }",
             "the physical receiver proof applies equally to owning locals");
    accepted(prefix + readers +
                 "int invoke(Reader value) { int& held = guarded; "
                 "return value.read() + held; } "
                 "int main() { return invoke(Reader()); }",
             "a by-value class formal owns an exact complete receiver rather "
             "than an arbitrary reference to a derived object");
    accepted(
        prefix + readers + storage +
            "Leaf leaf{}; "
            "int main() { int& held = guarded; return leaf.read() + held; }",
        "an inherited final overrider keeps its exact selected declaration");
    accepted(prefix + R"(
interface ReaderContract { int read(); };
class InterfaceReader : public ReaderContract {
public:
  int read() override { return 2; }
public:
  InterfaceReader() = default;
};
)" + storage + R"(
InterfaceReader reader{};
int main() { int& held = guarded; return reader.read() + held; }
)",
             "a direct concrete interface implementation retains its exact "
             "target even when the virtual root has no executable body");
    accepted(declared +
                 "class Holder { int kept = reader.read(); "
                 "public: int value() { return this.kept; }  public: Holder() "
                 "= default; }; "
                 "int main() { Holder held{}; return held.value() - 2; }",
             "field initialization may call an independent complete object's "
             "virtual method without treating constructor this as complete");
    accepted(prefix + R"(
class BaseCall { public: virtual int operator()() { return 1; }  public: BaseCall() = default; };
class ReaderCall : public BaseCall {
public:
  int operator()() override { return 2; }
public:
  ReaderCall() = default;
};
)" + storage + R"(
ReaderCall callable{};
int main() { int& held = guarded; return callable() + held; }
)",
             "an exact virtual operator receiver uses the same physical "
             "object and slot proof as an ordinary member call");

    const std::string effectful = prefix + R"(
void change() { guarded = 9; }
class Base {
public:
  virtual int read() { return 1; }
public:
  Base() = default;
};
class Reader : public Base {
public:
  int read() override { change(); return 2; }
public:
  Reader() = default;
};
)" + storage + "Reader reader{}; ";
    rejected(effectful + "int main() { int& held = guarded; "
                         "return reader.read() + held; }",
             true,
             "an exact readonly virtual target still closes hidden helper "
             "writes and rejects the actual overlapping persistent path");
    rejected(prefix + R"(
void change() { guarded = 9; }
class BaseCall { public: virtual int operator()() { return 1; }  public: BaseCall() = default; };
class ReaderCall : public BaseCall {
public:
  int operator()() override { change(); return 2; }
public:
  ReaderCall() = default;
};
)" + storage + R"(
ReaderCall callable{};
int main() { int& held = guarded; return callable() + held; }
)",
             true,
             "an exact readonly virtual operator still closes its actual "
             "hidden persistent mutation rather than promising effect freedom");
    rejected(effectful + "int main() { Base& view = reader; "
                         "int& held = guarded; return view.read() + held; }",
             false,
             "a base reference must never replace its mutating override with "
             "the harmless statically selected base body");
    rejected(prefix + R"(
class Base { public: virtual int read() { return 1; }  public: Base() = default; };
class Reader : public Base {
public:
  Reader() {
    int& held = guarded;
    int result = this.read();
    if (result == held) {}
  }
  int read() override { return 2; }
};
int main() { Reader reader{}; return 0; }
)",
             false,
             "under-construction this does not establish a complete dynamic "
             "class or erase the unresolved virtual boundary");
    rejected(prefix + R"(
class Reader {
public:
  mut int value = 1;
  virtual int read() { change(); return this.value; }
public:
  Reader() = default;
};
)" + storage + R"(
mut Reader reader{};
void change() { reader.value = 9; }
int main() { return reader.read(); }
)",
             true,
             "a virtual readonly receiver's call-scoped protection rejects a "
             "hidden reentrant write to that same complete object");
  }
}

void packFoldStorageEffects() {
  for (const std::string storage : {"", "thread_local "}) {
    const bool concurrent = !storage.empty();
    const std::string prefix = storage + "mut int32_t guarded = 1; " + storage +
                               "mut int32_t other = 2; ";
    const auto accepted = [&](const std::string &source,
                              std::string_view reason) {
      const auto result = analyze(source, concurrent, true);
      if (!result.canGenerateCode())
        for (const auto &diagnostic : result.diagnostics)
          std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
      expect(result.canGenerateCode(), reason);
    };
    const auto rejected = [&](const std::string &source, bool exactEffects,
                              std::string_view reason) {
      const auto result = analyze(source, concurrent, true);
      const bool conflict =
          std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                      [&](const auto &diagnostic) {
                        return diagnostic.code == "GTI-S2045" &&
                               (!exactEffects ||
                                diagnostic.message.find("overlapping path") !=
                                    std::string::npos);
                      });
      const bool valid =
          !result.semanticValid && conflict && !code(result, "GTI-B0001");
      if (!valid)
        for (const auto &diagnostic : result.diagnostics)
          std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
      expect(valid, reason);
    };
    const std::string localPrefix = R"(
void leaf<T>(mut int32_t& input, T& element) { input += 1; }
void folded<Args...>(Args... args) {
  mut int32_t input = 0;
  (leaf(input, args), ...);
}
)";
    for (const std::string arguments : {"", "1, true"})
      accepted(prefix + localPrefix +
                   "int main() { int32_t& held = guarded; folded(" + arguments +
                   "); return held; }",
               "an exact empty or nonempty user fold preserves an unrelated "
               "persistent loan and authorized formal writes");

    const std::string sharedPrefix = R"(
void leaf<T>(int32_t& input, T& element) { int32_t copy = input; }
void folded<Args...>(Args... args) { (leaf(guarded, args), ...); }
)";
    accepted(prefix + sharedPrefix +
                 "int main() { int32_t& held = guarded; folded(1, true); "
                 "return held; }",
             "direct persistent readonly fixed inputs permit shared reads");
    const std::string mutablePrefix = R"(
void leaf<T>(mut int32_t& input, T& element) { input += 1; other += 1; }
void folded<Args...>(Args... args) { (leaf(guarded, args), ...); }
)";
    accepted(prefix + mutablePrefix +
                 "int main() { folded(1, true); return guarded; }",
             "direct persistent mutable fixed inputs authorize formal writes "
             "without hiding disjoint ambient writes");

    const std::string referencePrefix = R"(
void leaf<T>(int32_t& input, T& element) { int32_t copy = input; }
void folded<Args...>(Args... args) {
  int32_t& input = guarded;
  (leaf(input, args), ...);
}
)";
    for (const std::string arguments : {"", "1, true"})
      accepted(prefix + referencePrefix +
                   "int main() { int32_t& held = guarded; folded(" + arguments +
                   "); return held; }",
               "a real readonly reference-local fixed prefix remains valid "
               "for empty and nonempty expansions");

    const std::string hiddenWrite = R"(
void leaf<T>(int32_t& input, T& element) { guarded = 3; }
void folded<Args...>(Args... args) {
  mut int32_t input = 0;
  (leaf(input, args), ...);
}
)";
    accepted(
        prefix + hiddenWrite +
            "int main() { int32_t& held = guarded; folded(); return held; }",
        "an exact empty fold executes no hidden target effects");
    rejected(prefix + hiddenWrite +
                 "int main() { int32_t& held = guarded; folded(1, true); "
                 "return held; }",
             true,
             "nonempty user fold hidden writes conflict with readonly loans");
    const std::string hiddenRead = R"(
void read(int32_t& input) { int32_t copy = input; }
void leaf<T>(int32_t& input, T& element) { read(guarded); }
void folded<Args...>(Args... args) {
  mut int32_t input = 0;
  (leaf(input, args), ...);
}
)";
    rejected(prefix + hiddenRead +
                 "int main() { mut int32_t& held = guarded; folded(1); "
                 "return held; }",
             true,
             "nonempty user fold hidden reads conflict with exclusive loans");

    const std::string sharedHiddenWrite = R"(
void leaf<T>(int32_t& input, T& element) { guarded = 3; }
void folded<Args...>(Args... args) { (leaf(guarded, args), ...); }
)";
    accepted(
        prefix + sharedHiddenWrite +
            "int main() { int32_t& held = guarded; folded(); return held; }",
        "zero concrete invocations create no fixed-input borrow");
    rejected(prefix + sharedHiddenWrite +
                 "int main() { folded(1, true); return 0; }",
             true,
             "a readonly direct persistent fixed input protects against its "
             "own target's hidden write without a separate named loan");
    const std::string mutableHiddenRead = R"(
void read(int32_t& input) { int32_t copy = input; }
void leaf<T>(mut int32_t& input, T& element) { read(guarded); input += 1; }
void folded<Args...>(Args... args) { (leaf(guarded, args), ...); }
)";
    rejected(prefix + mutableHiddenRead + "int main() { folded(1); return 0; }",
             true,
             "an exclusive direct persistent fixed input protects against "
             "its own target's hidden read");
    const std::string referenceHiddenWrite = R"(
void leaf<T>(int32_t& input, T& element) { guarded = 3; }
void folded<Args...>(Args... args) {
  int32_t& input = guarded;
  (leaf(input, args), ...);
}
)";
    accepted(prefix + referenceHiddenWrite +
                 "int main() { folded(); return 0; }",
             "an empty expansion retains actual reference-local setup but "
             "does not invent a target invocation");
    rejected(prefix + referenceHiddenWrite +
                 "int main() { folded(1); return 0; }",
             true,
             "a fixed reference-local retains its exact persistent origin "
             "through the concrete target's hidden write");
    rejected(prefix + R"(
void leaf<T>(int32_t& input, T& element);
void folded<Args...>(Args... args) {
  mut int32_t input = 0;
  (leaf(input, args), ...);
}
int main() { int32_t& held = guarded; folded(1); return held; }
)",
             false,
             "a nonempty fold with a declaration-only GTI target remains "
             "conservatively unknown rather than effect-free");
  }

  const std::string dynamicPrefix = R"(
thread_local mut int32_t guarded = 1;
int32_t initialize() { guarded += 1; return 2; }
thread_local mut int32_t input = initialize();
void leaf<T>(mut int32_t& fixed, T& element) { fixed += 1; }
void folded<Args...>(Args... args) { (leaf(input, args), ...); }
)";
  const auto empty =
      analyze(dynamicPrefix + "int main() { int32_t& held = guarded; "
                              "folded(); return held; }",
              true, true);
  if (!empty.canGenerateCode())
    for (const auto &diagnostic : empty.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(empty.canGenerateCode(),
         "an exact empty fold does not initialize its direct dynamic TLS "
         "fixed prefix while an unrelated loan is live");
  const auto nonempty =
      analyze(dynamicPrefix + "int main() { int32_t& held = guarded; "
                              "folded(1); return held; }",
              true, true);
  expect(!nonempty.semanticValid && code(nonempty, "GTI-S2045") &&
             !code(nonempty, "GTI-B0001"),
         "a nonempty fold retains dynamic fixed-prefix TLS initialization "
         "effects and rejects its hidden conflicting write");

  for (const bool exclusive : {false, true}) {
    const std::string orderedPrefix =
        R"(
thread_local mut int32_t first = 1;
thread_local mut int32_t other = 2;
)" +
        std::string(exclusive
                        ? "int32_t initialize() { return first; } "
                        : "int32_t initialize() { first = 3; return 4; } ") +
        "thread_local mut int32_t later = initialize(); "
        "void leaf<T>(" +
        (exclusive ? "mut " : "") +
        "int32_t& input, int32_t& second, T& element) {} ";
    const std::string direct = "void folded<Args...>(Args... args) { "
                               "(leaf(first, later, args), ...); } ";
    const auto emptyOrdered =
        analyze(orderedPrefix + direct + "int main() { folded(); return 0; }",
                true, true);
    if (!emptyOrdered.canGenerateCode())
      for (const auto &diagnostic : emptyOrdered.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    expect(emptyOrdered.canGenerateCode(),
           "an empty fold does not initialize any later fixed TLS input");
    const auto conflict =
        analyze(orderedPrefix + direct + "int main() { folded(1); return 0; }",
                true, true);
    const bool orderedConflict = std::any_of(
        conflict.diagnostics.begin(), conflict.diagnostics.end(),
        [](const auto &diagnostic) {
          return diagnostic.code == "GTI-S2045" &&
                 std::any_of(diagnostic.related.begin(),
                             diagnostic.related.end(), [](const auto &related) {
                               return related.message.find(
                                          "Earlier reference or borrowed-state "
                                          "inputs are already bound") !=
                                      std::string::npos;
                             });
        });
    if (!orderedConflict || code(conflict, "GTI-B0001"))
      for (const auto &diagnostic : conflict.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    expect(!conflict.semanticValid && orderedConflict &&
               !code(conflict, "GTI-B0001"),
           "a later TLS fixed input's initializer cannot violate an earlier "
           "shared or exclusive fixed input before the fold invocation");

    const auto warmed = analyze(orderedPrefix + R"(
void folded<Args...>(Args... args) {
  int32_t& ready = later;
  (leaf(first, ready, args), ...);
}
int main() { folded(1); return 0; }
)",
                                true, true);
    if (!warmed.canGenerateCode())
      for (const auto &diagnostic : warmed.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    expect(warmed.canGenerateCode(),
           "an actual reference-local setup initializes the later slot before "
           "the earlier fold input is bound");
  }
  const auto disjointInitialization = analyze(R"(
thread_local mut int32_t first = 1;
thread_local mut int32_t other = 2;
int32_t initialize() { other += 1; return 3; }
thread_local mut int32_t later = initialize();
void leaf<T>(mut int32_t& input, int32_t& second, T& element) { input += 1; }
void folded<Args...>(Args... args) { (leaf(first, later, args), ...); }
int main() { folded(1); return 0; }
)",
                                              true, true);
  if (!disjointInitialization.canGenerateCode())
    for (const auto &diagnostic : disjointInitialization.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(disjointInitialization.canGenerateCode(),
         "ordered fold input protection preserves a later TLS initializer "
         "whose hidden write is disjoint from the earlier input");
  const auto unavailable = analyze(R"(
class Owner {
public:
  Owner() {}
  Owner(Owner&) = delete;
  Owner(Owner&&) = default;
};
void leaf<T>(Owner& input, T& element) {}
void folded<Args...>(Args... args) {
  mut Owner input{};
  Owner moved = std::move(input);
  (leaf(input, args), ...);
}
int main() { folded(); return 0; }
)",
                                   true, true);
  expect(!unavailable.semanticValid && code(unavailable, "GTI-S2018") &&
             !code(unavailable, "GTI-B0001"),
         "exact-empty runtime-effect suppression preserves ordinary moved "
         "binding availability checks");
}

void enumDesignatorEffects() {
  for (const std::string storage : {"", "thread_local "}) {
    const bool concurrent = !storage.empty();
    const std::string prefix = storage + "mut int32_t guarded = 1; " + storage +
                               "mut int32_t other = 2; " + R"(
class Cleanup {
public:
  Cleanup() {}
  ~Cleanup() { guarded = 3; }
};
enum Payload { owned(Cleanup value), empty };
)";
    const auto accepted = [&](const std::string &source,
                              std::string_view reason) {
      const auto result = analyze(source, concurrent, true);
      if (!result.canGenerateCode())
        for (const auto &diagnostic : result.diagnostics)
          std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
      expect(result.canGenerateCode(), reason);
    };
    accepted(prefix + R"(
thread_local mut Payload input = Payload::empty;
int32_t inspect(Payload& value, int32_t index) {
  switch(value) {
  case Payload::owned(auto& payload):
    int32_t values[1] = {1};
    return values[index];
  case Payload::empty: return 0;
  }
}
int main() {
  int32_t& held = guarded;
  int32_t observed = inspect(input, 0);
  return held + observed;
}
)",
             "case designators do not create phantom enum cleanup when a "
             "borrowed switch body can propagate a failure");
    accepted(prefix + R"(
int main() {
  Payload owned = Payload::owned(Cleanup());
  int32_t& held = guarded;
  return held;
}
)",
             "a real payload construction transfers into an owner whose "
             "scope cleanup follows the later reference binding's endpoint");
    accepted(prefix + R"(
int main() {
  Payload first = Payload::empty;
  Payload second = (Payload::empty);
  return 0;
}
)",
             "actual unit-alternative values and groupings still construct "
             "ordinary owning enum values");
    const auto invalidUnitCall = analyze(prefix + R"(
int main() { Payload value = Payload::empty(); return 0; }
)",
                                         concurrent, true);
    expect(!invalidUnitCall.semanticValid &&
               code(invalidUnitCall, "GTI-S2067") &&
               !code(invalidUnitCall, "GTI-B0001"),
           "a unit alternative remains a value rather than an accepted "
           "payload-constructor call");
    const auto scopeConflict = analyze(prefix + R"(
int main() {
  int32_t& held = guarded;
  Payload owned = Payload::owned(Cleanup());
  return held;
}
)",
                                       concurrent, true);
    const bool realScopeCleanup = std::any_of(
        scopeConflict.diagnostics.begin(), scopeConflict.diagnostics.end(),
        [](const auto &diagnostic) {
          return diagnostic.code == "GTI-S2045" &&
                 std::any_of(diagnostic.related.begin(),
                             diagnostic.related.end(), [](const auto &related) {
                               return related.message.find(
                                          "cleanup runs at scope exit") !=
                                      std::string::npos;
                             });
        });
    expect(!scopeConflict.semanticValid && realScopeCleanup &&
               !code(scopeConflict, "GTI-B0001"),
           "a later owning local retains its real destructor conflict when "
           "scope cleanup precedes the earlier reference binding's endpoint");
    const std::string temporary = R"(
void inspect(Payload& value) {}
int main() {
  int32_t& held = )";
    accepted(prefix + temporary + "other; " + R"(
  inspect(Payload::owned(Cleanup()));
  return held;
}
)",
             "a real enum temporary's destructor preserves disjoint loans");
    const auto rejected = analyze(prefix + temporary + "guarded; " + R"(
  inspect(Payload::owned(Cleanup()));
  return held;
}
)",
                                  concurrent, true);
    const bool cleanupConflict = std::any_of(
        rejected.diagnostics.begin(), rejected.diagnostics.end(),
        [](const auto &diagnostic) {
          return diagnostic.code == "GTI-S2045" &&
                 std::any_of(
                     diagnostic.related.begin(), diagnostic.related.end(),
                     [](const auto &related) {
                       return related.message.find(
                                  "Remaining temporaries are cleaned") !=
                              std::string::npos;
                     });
        });
    expect(!rejected.semanticValid && cleanupConflict &&
               !code(rejected, "GTI-B0001"),
           "a genuinely constructed enum temporary retains its mutating "
           "destructor's conflict at the full-expression cleanup boundary");
  }
}

void packFoldCallerFacts() {
  for (const bool terminal : {false, true}) {
    const std::string source =
        std::string(terminal ? "[[noreturn, track_caller]] "
                             : "[[track_caller]] ") +
        "void leaf<T>(T& element) { " + (terminal ? "while (true) {}" : "") +
        R"( }
void folded<Args...>(Args... args) { (leaf(args), ...); }
int main() { folded(1); return 0; }
)";
    const auto result = analyze(source, true, true);
    if (!result.canGenerateCode())
      for (const auto &diagnostic : result.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    expect(result.canGenerateCode(),
           "a concrete fold preserves ordinary selected caller and terminal "
           "contracts through verified HIR and MIR");
    std::size_t selected = 0;
    for (const auto &function : result.hir.functionInstances())
      for (const auto &value : function.body.values)
        if (value.kind == lang::HirValueKind::Call && value.functionTarget) {
          const auto *target =
              result.hir.findFunctionInstance(*value.functionTarget);
          if (!target || !target->source ||
              target->source->name().lexeme != "leaf")
            continue;
          ++selected;
          expect(value.neverReturns == terminal && value.tracksCaller &&
                     value.callerLocation && value.callerLocationProof &&
                     lang::exprAs<lang::Call>(value.source),
                 "each expanded call retains its exact declaration flags "
                 "and ordinary source-call caller-location proof");
        }
    expect(selected == 1,
           "the one-element fold exposes exactly one ordinary selected call");
  }
}

void concreteEffectObligationReplay() {
  for (const std::string storage : {"", "thread_local "}) {
    const bool concurrent = !storage.empty();
    const std::string prefix = storage + "mut int32_t guarded = 1; " + storage +
                               "mut int32_t other = 2; " + storage +
                               "mut int32_t third = 3; ";
    const auto accepted = [&](const std::string &source,
                              std::string_view reason) {
      const auto result = analyze(source, concurrent, true);
      if (!result.canGenerateCode())
        for (const auto &diagnostic : result.diagnostics)
          std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
      expect(result.canGenerateCode(), reason);
    };
    const std::string leaf =
        "void leaf<T>(int32_t& fixed, T& element) { guarded = 9; } ";
    const std::string selected = R"(
void selected<Args...>(Args... args) {
  int32_t& fixed = guarded;
  (leaf(fixed, args), ...);
}
)";
    for (const bool reverseDeclarations : {false, true}) {
      const std::string fold =
          reverseDeclarations ? selected + leaf : leaf + selected;
      accepted(prefix + fold + "int main() { return 0; }",
               "an uninstantiated symbolic fold retains a dependent effect "
               "obligation without claiming an executed nonempty call");
      accepted(prefix + fold + "int main() { selected(); return 0; }",
               "a selected zero expansion replays the dependent obligation "
               "without executing a hidden conflicting write in either "
               "declaration order");
      const auto nonempty =
          analyze(prefix + fold + "int main() { selected(1); return 0; }",
                  concurrent, true);
      expect(!nonempty.semanticValid && code(nonempty, "GTI-S2045") &&
                 !code(nonempty, "GTI-B0001"),
             "an actual nonempty fold's internal persistent input obligation "
             "is checked before HIR in either declaration order");
    }
    const std::string localLambda = R"(
void selected<uint64_t Mode>() {
  if constexpr (Mode < 2) {
    int32_t& held = guarded;
    auto hidden = []() -> void {
      if constexpr (Mode == 0) { other = 9; }
      else { guarded = 9; }
    };
    hidden();
    int32_t copy = held;
  }
}
)";
    accepted(prefix + localLambda + "int main() { selected<0>(); return 0; }",
             "an exact concrete local lambda preserves a disjoint hidden "
             "storage effect without an outer caller loan");
    std::string emptyLambda = localLambda;
    emptyLambda.replace(emptyLambda.find("other = 9;"),
                        std::string_view("other = 9;").size(), "");
    accepted(prefix + emptyLambda + "int main() { selected<0>(); return 0; }",
             "an analyzed empty local lambda owns an exact empty summary "
             "rather than an unresolved or inherited source ID");
    for (const bool reverse : {false, true}) {
      const std::string calls = reverse ? "selected<1>(); selected<0>(); "
                                        : "selected<0>(); selected<1>(); ";
      const auto rejected = analyze(prefix + localLambda + "int main() { " +
                                        calls + "return 0; }",
                                    concurrent, true);
      const bool exactConflict =
          std::any_of(rejected.diagnostics.begin(), rejected.diagnostics.end(),
                      [](const auto &diagnostic) {
                        return diagnostic.code == "GTI-S2045" &&
                               diagnostic.message.find("overlapping path") !=
                                   std::string::npos;
                      });
      if (!exactConflict || code(rejected, "GTI-B0001"))
        for (const auto &diagnostic : rejected.diagnostics)
          std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
      expect(!rejected.semanticValid && exactConflict &&
                 !code(rejected, "GTI-B0001"),
             "the same lambda syntax's distinct concrete effects cannot be "
             "lost or cross-contaminated by either instance query order");
      const auto emptyAndWrite = analyze(
          prefix + emptyLambda + "int main() { " + calls + "return 0; }",
          concurrent, true);
      const auto realConflicts = std::count_if(
          emptyAndWrite.diagnostics.begin(), emptyAndWrite.diagnostics.end(),
          [](const auto &diagnostic) {
            return diagnostic.code == "GTI-S2045" &&
                   diagnostic.message.find("overlapping path") !=
                       std::string::npos;
          });
      if (realConflicts != 1 || code(emptyAndWrite, "GTI-B0001"))
        for (const auto &diagnostic : emptyAndWrite.diagnostics)
          std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
      expect(!emptyAndWrite.semanticValid && realConflicts == 1 &&
                 !code(emptyAndWrite, "GTI-B0001"),
             "empty and writing instances of the same lambda syntax retain "
             "exactly the one real conflicting effect in either query order");
      const std::string disjoint = R"(
void selected<uint64_t Mode>() {
  if constexpr (Mode < 2) {
    int32_t& held = guarded;
    auto hidden = []() -> void {
      if constexpr (Mode == 0) { other = 9; }
      else { third = 9; }
    };
    hidden();
    int32_t copy = held;
  }
}
)";
      accepted(prefix + disjoint + "int main() { " + calls + "return 0; }",
               "query-qualified local effect owners preserve distinct "
               "disjoint lambda specializations in both orders");
    }
  }
}

void invalidEffectQueryDiagnostics() {
  for (const auto bodies : {lang::TrustedBodyAnalysis::Complete,
                            lang::TrustedBodyAnalysis::Reachable})
    for (const bool borrowed : {false, true}) {
      for (const bool reverse : {false, true}) {
        const std::string good =
            "[[discard]] read().transform([](int value) -> void {}); ";
        const std::string bad = "[[discard]] read().transform_error(mapper); ";
        const std::string source =
            R"(
thread_local mut int guarded = 1;
expected<int, int> read() { return unexpected(7); }
void mapper(int value) {}
int main() {
)" + std::string(borrowed ? "int& held = guarded; " : "") +
            (reverse ? good + bad : bad + good) +
            (borrowed ? "return held; }" : "return 0; }");
        const auto result = analyze(source, true, true, bodies);
        const auto *entry =
            result.sourceGraph.findUnit(result.sourceGraph.entryUnit());
        const auto member = source.rfind("transform_error");
        const auto atMember = [&](std::string_view message) {
          return std::count_if(
                     result.diagnostics.begin(), result.diagnostics.end(),
                     [&](const auto &diagnostic) {
                       return diagnostic.code == "GTI-S2079" &&
                              diagnostic.message == message && entry &&
                              diagnostic.primary.source ==
                                  entry->path.string() &&
                              diagnostic.primary.start == member &&
                              diagnostic.primary.end == member + 15 &&
                              std::any_of(diagnostic.related.begin(),
                                          diagnostic.related.end(),
                                          [](const auto &related) {
                                            return related.message ==
                                                   "Reported inside this "
                                                   "library instance.";
                                          });
                     }) == 1;
        };
        expect(
            !result.canGenerateCode() && !result.semanticValid &&
                result.diagnostics.size() == 2 &&
                atMember("Static assertion failed. A 'transform_error' mapper "
                         "must return an error value, not 'void'.") &&
                atMember("Static assertion failed. An 'expected' error type "
                         "cannot be 'void'.") &&
                !code(result, "GTI-S2045") && !code(result, "GTI-B0001") &&
                result.mir.functionInstances().empty(),
            "failed effect queries preserve canonical library diagnostic "
            "origins and independent body/result-type assertions, with or "
            "without a retained TLS borrow or a later independent valid query");
      }
    }

  const std::string assertion =
      "static_assert(sizeof(T) == uint64_t(4), \"exact instance body\"); ";
  const std::vector<std::pair<std::string, std::string>> instances = {
      {"void selected<T>() { " + assertion + "}", "selected"},
      {"class Selected<T> { public: Selected() { " + assertion + "} };",
       "construct"},
      {"class Selected<T> { public: Selected() = default; ~Selected() { " +
           assertion + "} };",
       "destroy"},
      {"class Selected<T> { " + assertion +
           "public: Selected() = default; int field = 1; };",
       "fields"},
  };
  for (const auto &[declaration, kind] : instances) {
    const auto use = [&](std::string_view type, std::string_view name) {
      return kind == "selected" ? "selected<" + std::string(type) + ">(); "
                                : "Selected<" + std::string(type) + "> " +
                                      std::string(name) + "{}; ";
    };
    const std::string prefix =
        "thread_local mut int guarded = 1; " + declaration;
    const std::string good = use("int32_t", "good");
    const std::string bad = use("int64_t", "bad");
    const auto allowed = analyze(prefix + "int main() { int& held = guarded; " +
                                     good + "return held; }",
                                 true, true);
    if (!allowed.canGenerateCode())
      for (const auto &diagnostic : allowed.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    expect(allowed.canGenerateCode(),
           "a valid concrete provider outcome preserves its exact owner/type "
           "key and an unrelated live TLS borrow");
    for (const bool reverse : {false, true}) {
      const auto result =
          analyze(prefix + "int main() { int& held = guarded; " +
                      (reverse ? bad + good : good + bad) + "return held; }",
                  true, true);
      const auto assertions = std::count_if(
          result.diagnostics.begin(), result.diagnostics.end(),
          [](const auto &diagnostic) {
            return diagnostic.code == "GTI-S2079" &&
                   diagnostic.message.find("exact instance body") !=
                       std::string::npos;
          });
      expect(!result.semanticValid && !result.canGenerateCode() &&
                 assertions == 1 && !code(result, "GTI-S2045") &&
                 !code(result, "GTI-B0001") &&
                 result.mir.functionInstances().empty() &&
                 result.mir.constructorInstances().empty() &&
                 result.mir.destructorInstances().empty(),
             "failed function/constructor/destructor/field effect queries "
             "cannot emit MIR or contaminate another concrete specialization "
             "in either query order");
    }
  }

  const std::string keyed = R"(
thread_local mut int guarded = 1;
class Keyed<T, uint64_t OwnerValue> {
public:
  void select<U, uint64_t MethodValue>() {
    static_assert(sizeof(T) != uint64_t(8) || OwnerValue != uint64_t(7) ||
                  sizeof(U) != uint64_t(4) || MethodValue != uint64_t(9),
                  "exact owner and method instance");
  }
public:
  Keyed() = default;
};
)";
  const std::string owners =
      "Keyed<int32_t, 7> first{}; Keyed<int64_t, 6> second{}; "
      "Keyed<int64_t, 7> third{}; ";
  const std::string safeKeys =
      "first.select<int32_t, 9>(); second.select<int32_t, 9>(); "
      "third.select<int64_t, 9>(); third.select<int32_t, 8>(); ";
  const std::string badKey = "third.select<int32_t, 9>(); ";
  for (const bool reverse : {false, true}) {
    const auto result =
        analyze(keyed + "int main() { " + owners + "int& held = guarded; " +
                    (reverse ? badKey + safeKeys : safeKeys + badKey) +
                    "return held; }",
                true, true);
    const auto exactFailures = std::count_if(
        result.diagnostics.begin(), result.diagnostics.end(),
        [](const auto &diagnostic) {
          return diagnostic.code == "GTI-S2079" &&
                 diagnostic.message.find("exact owner and method instance") !=
                     std::string::npos;
        });
    expect(!result.canGenerateCode() && exactFailures == 1 &&
               !code(result, "GTI-S2045") && !code(result, "GTI-B0001") &&
               result.mir.functionInstances().empty(),
           "deferred invalid diagnostics use the complete owner and function "
           "type/value key without cross-specialization contamination");
  }

  const auto open = analyze(R"(
thread_local mut int guarded = 1;
interface Callback { void call(); };
void selected<uint64_t Mode>(Callback& callback) {
  int& held = guarded;
  callback.call();
  int copy = held;
}
void dispatch(Callback& callback) { selected<0>(callback); }
int main() { return 0; }
)",
                            true, true);
  expect(!open.semanticValid && code(open, "GTI-S2045") &&
             !code(open, "GTI-B0001"),
         "a genuinely open concrete dispatch remains an unknown storage "
         "effect rather than a deferred invalid-body diagnostic");
}

void stagedEffectQueryDiagnostics() {
  for (const bool constructor : {false, true}) {
    const std::string selected = constructor ? R"(
class Selected<T, uint64_t Width> {
public:
  int value;
  constexpr Selected(int input) : value(input) {
    static_assert(sizeof(T) == Width, "staged exact body");
  }
};
constexpr int read<T, uint64_t Width>() {
  Selected<T, Width> value{42};
  return value.value;
}
)"
                                             : R"(
constexpr int read<T, uint64_t Width>() {
  static_assert(sizeof(T) == Width, "staged exact body");
  return 42;
}
)";
    const std::string prefix =
        "thread_local mut int guarded = 1; " + selected + R"(
void check<T, uint64_t Width>() {
  static_assert(read<T, Width>() == 42, "staged exact value");
}
)";
    for (const bool bad : {false, true}) {
      const std::string type = bad ? "int64_t" : "int32_t";
      const std::string runtime =
          constructor ? "int runtime() { Selected<" + type +
                            ", 4> value{1}; return value.value; } "
                      : "int runtime() { return read<" + type + ", 4>(); } ";
      const std::string main =
          "int main() { int& held = guarded; check<int32_t, 4>(); "
          "return runtime() + held; } ";
      for (const bool reverse : {false, true}) {
        const auto result = analyze(
            prefix + (reverse ? main + runtime : runtime + main), true, true);
        if (!bad) {
          if (!result.canGenerateCode())
            for (const auto &diagnostic : result.diagnostics)
              std::cerr << diagnostic.code << ": " << diagnostic.message
                        << '\n';
          expect(result.canGenerateCode(),
                 "ordinary staged function and constructor providers preserve "
                 "valid exact type/value requests in either query order");
          continue;
        }
        const auto errors = std::count_if(
            result.diagnostics.begin(), result.diagnostics.end(),
            [](const auto &diagnostic) {
              return diagnostic.code == "GTI-S2079" &&
                     diagnostic.message ==
                         "Static assertion failed. staged exact body";
            });
        expect(!result.canGenerateCode() && !result.semanticValid &&
                   errors == 1 && !code(result, "GTI-S2045") &&
                   !code(result, "GTI-B0001") &&
                   result.mir.functionInstances().empty(),
               "an invalid runtime effect query cannot contaminate a "
               "different valid staged key or permit MIR in either demand "
               "order");
      }
    }
  }
}

void concreteQueryDiagnosticOwnership() {
  // A failed required constant is never a runtime effect root. Its existing
  // discovery/constant diagnostics must not acquire another effect budget
  // error, nor suppress an independent executable hidden-storage conflict.
  const std::string constant = R"(
constexpr uint64_t depth<uint64_t N>() {
  if constexpr (N == 0) { return 0; }
  else { return depth<N - 1>() + 1; }
}
constexpr uint64_t invalid = depth<200>();
)";
  const auto rejected =
      analyze(constant + "int main() { return int32_t(invalid); }", true, true);
  expect(!rejected.canGenerateCode() && rejected.diagnostics.size() == 2 &&
             code(rejected, "GTI-S2105") && code(rejected, "GTI-S2057") &&
             std::none_of(rejected.diagnostics.begin(),
                          rejected.diagnostics.end(),
                          [](const auto &d) {
                            return d.message.find("initializer-effect") !=
                                   std::string::npos;
                          }) &&
             !code(rejected, "GTI-B0001"),
         "a failed required constant retains only its canonical discovery "
         "and evaluation errors");
  const auto independent = analyze(constant + R"(
thread_local mut int guarded = 1;
void modify() { guarded = 2; }
int main() { int& held = guarded; modify(); return held; }
)",
                                   true, true);
  expect(!independent.canGenerateCode() && code(independent, "GTI-S2045") &&
             code(independent, "GTI-S2057") && !code(independent, "GTI-B0001"),
         "discarding a failed constant's nonexecuting effect root preserves "
         "independent executable storage obligations");

  for (const auto bodies : {lang::TrustedBodyAnalysis::Complete,
                            lang::TrustedBodyAnalysis::Reachable}) {
    const auto reflection = analyze(R"(
import std.meta;
class Node<T> {
  static_assert(std::meta::type<Node<T>>().field_count() == uint64_t(2),
                "outer reflection assertion");
  T value;
public:
  Node(T value) : value(value) {}
};
int main() { Node<int32_t> value{1}; return 0; }
)",
                                    true, true, bodies);
    const auto errors =
        std::count_if(reflection.diagnostics.begin(),
                      reflection.diagnostics.end(), [](const auto &d) {
                        return d.severity == lang::DiagnosticSeverity::Error;
                      });
    expect(!reflection.canGenerateCode() && errors == 1 &&
               code(reflection, "GTI-S2079") &&
               !code(reflection, "GTI-S2057") &&
               !code(reflection, "GTI-B0001") &&
               reflection.mir.functionInstances().empty(),
           "a recursive same-key reflection query supplies facts while its "
           "outer provider reports the failed assertion exactly once");
  }

  // The effect budget is an uncertified exact query, not an empty-effects
  // proof. Direct provider calls do not run recursive HIR discovery, so this
  // exercises the retained failure when that ordinary body would be valid.
  const std::string chain = R"(
void chain<uint64_t N>() {
  if constexpr (N != 0) { chain<N - 1>(); }
}
int main() { chain<65>(); return 0; }
)";
  lang::FrontendOptions options;
  options.stopAfter = lang::FrontendPhase::Semantics;
  const auto parsed =
      lang::Frontend(options).analyze("query-budget.gti", chain);
  lang::SemanticVisitor semantic(lang::TargetInfo::host(), &parsed.sourceGraph,
                                 false, nullptr, &parsed.sources);
  expect(semantic.check(parsed.program),
         "effect-query budget exhaustion defers diagnostic ownership to "
         "ordinary concrete discovery");
  lang::FunctionId function = 0;
  for (const auto &statement : parsed.program.declarations())
    if (const auto *declaration =
            lang::stmtAs<lang::FunctionDecl>(statement.get());
        declaration && declaration->name().lexeme == "chain")
      if (const auto *info = semantic.model().findFunction(*declaration))
        function = info->id;
  expect(function != 0, "the budget fixture resolves its exact generic owner");
  if (function == 0)
    return;
  for (const bool reverse : {false, true}) {
    const auto check = [&](std::uint64_t value) {
      return semantic.analyzeFunctionInstance(
          function, {}, {}, {}, {lang::CompileTimeValue::uint64(value)});
    };
    const auto first = check(reverse ? 1 : 0);
    const auto second = check(reverse ? 0 : 1);
    const auto &good = reverse ? second : first;
    const auto &bad = reverse ? first : second;
    expect(good.valid() && !bad.valid() &&
               std::any_of(bad.diagnostics.begin(), bad.diagnostics.end(),
                           [](const auto &d) {
                             return d.code == "GTI-S2105" &&
                                    d.message.find(
                                        "initializer-effect-depth") !=
                                        std::string::npos;
                           }),
           "a retained effect-budget failure gates its exact value key on "
           "fresh/repeated provider requests without poisoning other keys");
  }
}

void lateConcreteQueryContexts() {
  const auto library =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const auto trustedPath =
      std::filesystem::weakly_canonical(library / "std/functional.gti");
  const std::string declarations = R"(
struct Relay<T> {
  void invoke<F>(F& callback, T value) { callback(value); }
  void invoke<F>(F& callback) { callback(); }
public:
  Relay() = default;
};
F retain<F>(F& callback) { return callback; }
void escape<F>(F& callback) { auto held = retain(callback); held(); }
)";
  const std::string calls = R"(
import std.functional;
import std.panic;
thread_local mut int guarded = 1;
void selected<F>(F& callback) { Relay<int> relay{}; relay.invoke(callback, 4); }
void empty<F>(F& callback) { Relay<int> relay{}; relay.invoke(callback); }
expected<int, int> compute(bool fail) {
  if (fail) { return unexpected(7); }
  return 42;
}
expected<void, int> perform(bool fail) {
  if (fail) { return unexpected(7); }
  return;
}
expected<void, int> next() { return; }
void finish() {}
[[noreturn, track_caller]] void terminal(int value) { std::panic("terminal"); }
int main() {
  int& held = guarded;
  auto one = [](int value) -> void {};
  auto zero = []() -> void {};
  selected(one);
  empty(zero);
  auto first = compute(false).transform(one);
  auto second = perform(false).transform(zero);
  int snapshot = held;
  auto third = perform(false).transform(finish).and_then(next);
  auto fourth = compute(true).transform(terminal);
  return snapshot;
}
)";
  const std::string invalid = R"(
import std.functional;
thread_local mut int guarded = 1;
int main() {
  int& held = guarded;
  auto callback = []() -> void {};
  escape(callback);
  return held;
}
)";
  const std::string failingBody = R"(
import std.functional;
thread_local mut int guarded = 1;
int main() {
  int& held = guarded;
  auto callback = []() -> void {};
  selected(callback);
  return held;
}
void selected<F>(F& callback) { Relay<int> relay{}; relay.invoke(callback); }
)";
  const std::string unresolved = R"(
thread_local mut int guarded = 1;
expected<void, int> perform() { return; }
void finish() {}
int main() {
  int& held = guarded;
  auto result = perform().transform(finish);
  return held;
}
)";
  std::ifstream input(trustedPath);
  const std::string original((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>{});
  expect(!original.empty(),
         "the trusted relay fixture preserves its original library context");
  const std::unordered_map<std::string, std::string> trusted = {
      {trustedPath.string(), original + "\n" + declarations}};
  for (const auto bodies : {lang::TrustedBodyAnalysis::Complete,
                            lang::TrustedBodyAnalysis::Reachable}) {
    lang::FrontendOptions options;
    options.requireFunctionDefinitions = true;
    options.trustedBodies = bodies;
    options.cache = std::make_shared<lang::FrontendCache>();
    options.instanceCache = std::make_shared<lang::InstanceAnalysisCache>();
    options.preambleCache = std::make_shared<lang::SemanticPreambleCache>();
    const auto load = [&](const std::string &source, bool cached = false) {
      auto selected = options;
      if (!cached) {
        selected.instanceCache.reset();
        selected.preambleCache.reset();
      }
      return lang::Frontend(selected).analyze("late-query-context.gti", source,
                                              {library / "prelude.gti"},
                                              trusted, {library});
    };
    for (const bool cached : {false, true}) {
      const auto first = load(calls, cached);
      const auto repeated = load(calls + "\n// later snapshot\n", cached);
      if (!first.canGenerateCode() || !repeated.canGenerateCode())
        for (const auto *result : {&first, &repeated})
          for (const auto &d : result->diagnostics)
            std::cerr << d.code << ": " << d.message << '\n';
      expect(first.canGenerateCode() && repeated.canGenerateCode(),
             "concrete effect queries retain pending trusted callable "
             "contracts for void/zero-argument/noreturn mapping and unrelated "
             "user relays in complete/reachable/cached/preamble contexts");
      const auto escaped = load(invalid, cached);
      expect(!escaped.canGenerateCode() && code(escaped, "GTI-S2046") &&
                 !code(escaped, "GTI-B0001") &&
                 escaped.mir.functionInstances().empty(),
             "late callable contract finalization does not make a genuinely "
             "escaping callable confined");
      const auto later = load(failingBody, cached);
      expect(later.canGenerateCode(),
             "an invalid query or failed pending batch does not poison a "
             "later independent request using the same caches");
      const auto open = load(unresolved, cached);
      expect(!open.canGenerateCode() && code(open, "GTI-S2045") &&
                 !code(open, "GTI-S2046") && !code(open, "GTI-B0001"),
             "a signature-erased function item remains an unknown ambient "
             "target under a live TLS loan after its callable contract is "
             "correctly finalized");
    }
  }
}

void reflectedQueryEffects() {
  const std::string prefix = R"(
import std.meta;
struct Reading { int first = 1; int second = 2;  public: Reading() = default; };
struct Empty { public: Empty() = default; };
thread_local mut int guarded = 1;
thread_local mut int other = 2;
class Types<Elements...> { public: Types() = default; };
)";
  for (const bool reflected : {false, true}) {
    for (const std::string mutation : {"", "other = 9;", "guarded = 9;"}) {
      const std::string selected =
          "void inspect<T>() { constexpr for (auto item : " +
          std::string(reflected ? "std::meta::fields_of<T>()"
                                : "Types<T, int64_t>()") +
          ") { " + mutation + " } } ";
      const auto result = analyze(
          prefix + selected + "int main() { int& held = guarded; inspect<" +
              (reflected ? "Reading" : "int32_t") + ">(); return held; }",
          true, true);
      if (mutation == "guarded = 9;") {
        expect(!result.semanticValid && code(result, "GTI-S2045") &&
                   !code(result, "GTI-B0001"),
               "concrete ordinary/reflected constexpr-for bodies retain "
               "hidden overlapping persistent writes");
      } else {
        if (!result.canGenerateCode())
          for (const auto &diagnostic : result.diagnostics)
            std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
        expect(result.canGenerateCode(),
               "ordinary/reflected constexpr-for effect queries preserve "
               "exact registered sequence types and disjoint storage");
      }
    }
  }
  positive(prefix + R"(
void inspect<T>() {
  constexpr for (auto field : std::meta::fields_of<T>()) { guarded = 9; }
}
int main() { int& held = guarded; inspect<Empty>(); return held; }
)",
           "an exact empty reflected sequence executes no hidden write");
  const auto invalid = analyze(R"(
thread_local mut int guarded = 1;
void inspect<T>(T& sequence) { constexpr for (auto item : sequence) {} }
int main() { int sequence = 0; int& held = guarded; inspect(sequence); return held; }
)",
                               true, true);
  expect(!invalid.canGenerateCode() && code(invalid, "GTI-S2107") &&
             !code(invalid, "GTI-S2045") && !code(invalid, "GTI-B0001"),
         "an actually ineligible concrete sequence retains its ordinary "
         "source diagnostic rather than an empty effect proof");

  positive(prefix + R"(
constexpr int count<T>() {
  mut int result = 0;
  constexpr for (auto field : std::meta::fields_of<T>()) { result += 1; }
  return result;
}
struct FieldCount<T> {
  mut int value = 0;
  constexpr FieldCount() {
    constexpr for (auto field : std::meta::fields_of<T>()) { this.value += 1; }
  }
};
constexpr int constructed<T>() { FieldCount<T> result{}; return result.value; }
static_assert(count<Reading>() == 2 && count<Empty>() == 0);
static_assert(constructed<Reading>() == 2 && constructed<Empty>() == 0);
int main() { return count<Reading>() - 2; }
)",
           "staged function/constructor snapshots retain trusted computed "
           "reflection type identities for nonempty and empty sequences");
}

void reflectionRegistrySnapshots() {
  const auto library =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const auto metaPath =
      std::filesystem::weakly_canonical(library / "std/meta.gti");
  const std::string text = R"(
import std.meta;
struct Reading { int first = 1; int second = 2;  public: Reading() = default; };
void inspect<T>() { constexpr for (auto field : std::meta::fields_of<T>()) {} }
int main() { inspect<Reading>(); return 0; }
)";
  lang::FrontendWorkMetrics metrics;
  lang::FrontendOptions options;
  options.workMetrics = &metrics;
  options.cache = std::make_shared<lang::FrontendCache>();
  options.preambleCache = std::make_shared<lang::SemanticPreambleCache>();
  options.trustedBodies = lang::TrustedBodyAnalysis::Reachable;
  const auto load =
      [&](const std::string &source,
          const std::unordered_map<std::string, std::string> &overrides = {}) {
        metrics = {};
        return lang::Frontend(options).analyze(
            "reflection-registry.gti", source, {library / "prelude.gti"},
            overrides, {library});
      };
  const auto first = load(text);
  const auto cached = load(text + "\n// a later editor snapshot\n");
  expect(first.canGenerateCode() && cached.canGenerateCode() &&
             metrics.semanticPreambleAdoptions == 1,
         "a trusted preamble snapshot carries computed-type declaration "
         "bindings into a later reflected generic query");

  std::ifstream input(metaPath);
  std::string altered((std::istreambuf_iterator<char>(input)),
                      std::istreambuf_iterator<char>{});
  bool complete = !altered.empty();
  for (const std::string_view marker :
       {"@compiler_type(\"meta_list\")", "@compiler_type(\"meta_field\")"}) {
    const auto position = altered.find(marker);
    complete &= position != std::string::npos;
    if (position != std::string::npos)
      altered.erase(position, marker.size());
  }
  expect(complete,
         "registry reset fixture reads the trusted binding declarations");
  if (!complete)
    return;
  // Keep every public spelling and declaration order, but remove two trusted
  // identity bindings. Neither a preamble nor a reused visitor may invent
  // them from an earlier request or from the unchanged public class names.
  const auto unbound = load(text, {{metaPath.string(), std::move(altered)}});
  expect(!unbound.canGenerateCode() && code(unbound, "GTI-S2107") &&
             !code(unbound, "GTI-B0001"),
         "changed trusted declarations invalidate the preamble's registry "
         "rather than reusing old computed-type identities");

  lang::SourceGraph graph = first.sourceGraph;
  lang::SourceManager sources = first.sources;
  lang::SemanticVisitor semantic(lang::TargetInfo::host(), &graph, false,
                                 nullptr, &sources);
  expect(semantic.check(first.program),
         "first registry request checks successfully");
  expect(lang::HirLowerer()
             .lower(first.program, semantic.model(), semantic)
             .valid(),
         "the first request establishes its trusted registry");
  graph = unbound.sourceGraph;
  sources = unbound.sources;
  const bool checked = semantic.check(unbound.program);
  const auto rejected =
      checked ? lang::HirLowerer().lower(unbound.program, semantic.model(),
                                         semantic)
              : lang::HirLoweringResult{};
  expect(!checked || !rejected.valid(),
         "resetting one semantic visitor clears registry bindings absent from "
         "its next catalog");
  graph = first.sourceGraph;
  sources = first.sources;
  expect(semantic.check(first.program) &&
             lang::HirLowerer()
                 .lower(first.program, semantic.model(), semantic)
                 .valid(),
         "a subsequent visitor request rebuilds only its current trusted "
         "declaration registry");
}

void emptyPackDestructorProvider() {
  const auto source = analyze(R"(
class Empty<Args...> { public: ~Empty() {}  public: Empty() = default; };
int main() { Empty<> value{}; return 0; }
)");
  lang::SemanticVisitor semantic(lang::TargetInfo::host(), &source.sourceGraph,
                                 false, nullptr, &source.sources);
  expect(semantic.check(source.program),
         "the empty-pack destructor provider fixture has valid primary "
         "semantic facts");
  struct Provider final : lang::SemanticInstanceProvider {
    lang::SemanticVisitor &delegate;
    std::size_t emptyDestructors = 0;
    bool reject = false;
    explicit Provider(lang::SemanticVisitor &delegate) : delegate(delegate) {}
    lang::SemanticTypeTraits
    traitsFor(const lang::SemanticType &type) const override {
      return delegate.traitsFor(type);
    }
    bool
    requiresActiveCleanupFor(const lang::SemanticType &type) const override {
      return delegate.requiresActiveCleanupFor(type);
    }
    lang::SemanticInstanceAnalysis analyzeFunctionInstance(
        lang::FunctionId id, const std::vector<lang::SemanticType> &a,
        const std::vector<lang::CompileTimeValue> &b,
        const std::vector<lang::SemanticType> &c,
        const std::vector<lang::CompileTimeValue> &d) override {
      return delegate.analyzeFunctionInstance(id, a, b, c, d);
    }
    lang::SemanticInstanceAnalysis analyzeConstructorInstance(
        lang::ConstructorId id, const std::vector<lang::SemanticType> &a,
        const std::vector<lang::CompileTimeValue> &b,
        const std::vector<lang::SemanticType> &c,
        const std::vector<lang::CompileTimeValue> &d) override {
      return delegate.analyzeConstructorInstance(id, a, b, c, d);
    }
    lang::SemanticInstanceAnalysis analyzeDestructorInstance(
        lang::ClassId id, const std::vector<lang::SemanticType> &a,
        const std::vector<lang::CompileTimeValue> &b) override {
      auto result = delegate.analyzeDestructorInstance(id, a, b);
      if (a.empty() && b.empty()) {
        ++emptyDestructors;
        // Public-provider fault injection: a selected invalid outcome must
        // not disappear merely because its concrete argument pack is empty.
        if (reject)
          result.diagnostics.push_back(lang::makeDiagnostic(
              "GTI-S2079", lang::DiagnosticPhase::Semantics,
              {"empty-provider.gti", 0, 1, 1},
              "Rejected concrete destructor provider outcome."));
      }
      return result;
    }
    lang::SemanticInstanceAnalysis analyzeClassFieldInitializers(
        lang::ClassId id, const std::vector<lang::SemanticType> &a,
        const std::vector<lang::CompileTimeValue> &b) override {
      return delegate.analyzeClassFieldInitializers(id, a, b);
    }
  } provider(semantic);
  const auto accepted =
      lang::HirLowerer().lower(source.program, semantic.model(), provider);
  expect(accepted.valid() && provider.emptyDestructors == 1,
         "an empty concrete class pack still requires one ordinary destructor "
         "provider analysis");
  provider.reject = true;
  provider.emptyDestructors = 0;
  const auto rejected =
      lang::HirLowerer().lower(source.program, semantic.model(), provider);
  expect(!rejected.valid() && provider.emptyDestructors == 1 &&
             std::any_of(rejected.diagnostics.begin(),
                         rejected.diagnostics.end(),
                         [](const auto &diagnostic) {
                           return diagnostic.code == "GTI-S2079";
                         }),
         "an ordinary invalid empty-pack destructor outcome gates executable "
         "HIR admission instead of bypassing its provider");
}

void packFoldUnsafeBoundary() {
  const std::string unsafeTarget = "unsafe void leaf<T>(T& element) {} ";
  for (const std::string arguments : {"", "1"}) {
    const auto outside =
        analyze(unsafeTarget +
                    "void folded<Args...>(Args... args) { (leaf(args), ...); } "
                    "int main() { folded(" +
                    arguments + "); return 0; }",
                true, true);
    expect(!outside.semanticValid && code(outside, "GTI-S2055") &&
               !code(outside, "GTI-B0001"),
           "an unsafe selected fold callable requires ordinary lexical "
           "unsafe admission, not inferred backend permission");
    const auto inside = analyze(unsafeTarget +
                                    "void folded<Args...>(Args... args) { "
                                    "unsafe { (leaf(args), ...); } } "
                                    "int main() { folded(" +
                                    arguments + "); return 0; }",
                                true, true);
    if (!inside.canGenerateCode())
      for (const auto &diagnostic : inside.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    expect(inside.canGenerateCode(),
           "an explicitly admitted fold retains the exact source unsafe "
           "operation on its ordinary per-element call");
  }
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
       {"struct Read { Read(int& input, int copied) {}  public: Read() = "
        "default; }; int main() { Read "
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
struct Cleanup { ~Cleanup() { modify(); }  public: Cleanup() = default; };
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
        "struct State { mut int value = 1;  public: State() = default; }; int "
        "main() { thread_local mut "
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
      "import std.utility;\nthread_local mut int value = 1; int main() "
      "{ int previous = std::exchange(value, 2); return previous + value; }",
      "the exact restoring exchange contract can update a TLS value "
      "without exposing an unavailable persistent slot");
}

void invalidDeclarations() {
  for (const std::string source :
       {"class Bad { thread_local mut int field = 0;  public: Bad() = default; "
        "}; int main() { return 0; "
        "}",
        "thread_local constexpr int value = 0; int main() { return value; }",
        "int generic<T>(T input) { thread_local mut int value = 0; return "
        "value; } int main() { return generic(1) + generic(true); }",
        "class Bad<T> { public: static thread_local mut int value = 0;  "
        "public: Bad() = default; }; int "
        "main() { return Bad<int>::value + Bad<bool>::value; }",
        "int use(int input) { thread_local int value = input; return value; } "
        "int main() { return use(1); }",
        "class Bad { int value = 1; public: int use() { thread_local int copy "
        "= this.value; return copy; }  public: Bad() = default; }; int main() "
        "{ return 0; }"}) {
    const auto result = analyze(source);
    expect(!result.semanticValid && code(result, "GTI-S2124") &&
               !code(result, "GTI-B0001"),
           "invalid TLS placement and automatic initializer dependencies are "
           "source diagnostics");
  }
  const std::string source =
      "class Bad { thread_local mut int field = 0;  public: Bad() = default; "
      "}; int main() { return 0; }";
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
struct Cleanup { ~Cleanup() { modify(); }  public: Cleanup() = default; };
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
import std.panic;
import std.string;
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

void persistentClassAliasBoundaries() {
  for (const std::string storage : {"", "thread_local "}) {
    const std::string prefix = R"(
class Meter {
public:
  mut int count;
  Meter(int value) : count(value) {}
  int read() { return this.count; }
  void set() mut { this.count = 9; }
};
)" + storage + R"(mut Meter meter{1};
void direct() { meter.count = 9; }
void forwarded() { direct(); }
void method() { meter.set(); }
void reference() { mut Meter& alias = meter; alias.count = 9; }
void scalar_reference() { mut int& alias = meter.count; alias = 9; }
void assign(mut int& value) { value = 9; }
void reborrow() { mut Meter& alias = meter; assign(alias.count); }
int initialize() { direct(); return 0; }
class Trigger { public: int value = initialize(); Trigger() {} };
void initializer() { Trigger value{}; }
)";
    for (const std::string mutation :
         {"direct", "forwarded", "method", "reference", "scalar_reference",
          "reborrow", "initializer"}) {
      const auto result = analyze(
          prefix +
              "int inspect(Meter& borrowed) { int before = "
              "borrowed.read(); " +
              mutation +
              "(); return before + borrowed.read(); } int main() { return "
              "inspect(meter); }",
          !storage.empty(), true);
      expect(!result.semanticValid && code(result, "GTI-S2045") &&
                 !code(result, "GTI-B0001"),
             "safe persistent class references reject hidden alias mutation "
             "through all optimizer-control paths");
    }
    positive(prefix +
                 "int inspect(int snapshot) { direct(); return snapshot; } "
                 "int main() { return inspect(meter.read()); }",
             "copying a scalar before the call ends its storage borrow before "
             "a later hidden mutation",
             !storage.empty());
  }
}

void requirementProbeIsolation() {
  const std::string prefix = R"(
thread_local mut int value = 1;
thread_local mut int other = 2;
void modify() { value = 2; }
struct Cleanup { ~Cleanup() { modify(); }  public: Cleanup() = default; };
bool inspect(Cleanup& input) { return true; }
bool with_flag(Cleanup& input, bool flag) { return flag; }
)";
  const auto queryOnly =
      analyze(prefix + "int main() { int& borrowed = value; bool checked = "
                       "requires { { inspect(Cleanup()) } -> bool; }; "
                       "return checked ? borrowed : 0; }",
              true, true);
  if (!queryOnly.canGenerateCode())
    for (const auto &diagnostic : queryOnly.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(queryOnly.canGenerateCode(),
         "an unevaluated requirement does not publish speculative temporary "
         "cleanup into its caller's persistent effects");

  for (const bool nested : {true, false}) {
    const std::string selected =
        prefix + R"(
int selected<uint64_t Mode>() {
  if constexpr (Mode == 0) {
    int first = 0;
    int second = 1;
    int third = 2;
    bool checked = requires { { inspect(Cleanup()) } -> bool; };
    return checked ? first + second + third : 0;
  } else {
)" +
        (nested
             ? "bool seen = with_flag(Cleanup(), requires { "
               "{ inspect(Cleanup()) } -> bool; }); return seen ? 1 : 0; "
             : "bool checked = requires { { inspect(Cleanup()) } -> bool; }; "
               "bool seen = inspect(Cleanup()); "
               "return checked && seen ? 1 : 0; ") +
        "} }";
    for (const bool reverse : {false, true}) {
      const std::string calls =
          reverse ? "int first = selected<1>(); int second = selected<0>(); "
                  : "int first = selected<0>(); int second = selected<1>(); ";
      const auto source = [&](std::string_view owner) {
        return selected + "int main() { int& borrowed = " + std::string(owner) +
               "; " + calls + "return borrowed + first + second; }";
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
             nested ? "a requires probe inside a live caller expression "
                      "preserves that expression's real temporary cleanup"
                    : "an unevaluated requirement cannot suppress later real "
                      "cleanup in either concrete query order");
      const auto allowed = analyze(source("other"), true, true);
      if (!allowed.canGenerateCode())
        for (const auto &diagnostic : allowed.diagnostics)
          std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
      expect(allowed.canGenerateCode(),
             "requirement probes preserve disjoint-owner effects in both "
             "concrete query orders without importing caller cleanup frames");
    }
  }
}

void macroResultStorageEffects() {
  for (const std::string storage : {"", "thread_local "}) {
    const bool concurrent = !storage.empty();
    const std::string prefix = storage + "mut int guarded = 1; " + storage +
                               "mut int other = 2; " + storage +
                               "mut int third = 3; ";
    const auto accepted = [&](const std::string &source,
                              std::string_view message) {
      const auto result = analyze(source, concurrent, true);
      if (!result.canGenerateCode())
        for (const auto &diagnostic : result.diagnostics)
          std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
      expect(result.canGenerateCode(), message);
    };
    const auto rejected = [&](const std::string &source,
                              std::string_view message) {
      const auto result = analyze(source, concurrent, true);
      const bool conflict =
          std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                      [](const auto &diagnostic) {
                        return diagnostic.code == "GTI-S2045" &&
                               diagnostic.message.find("overlapping path") !=
                                   std::string::npos;
                      });
      if (!conflict || code(result, "GTI-B0001"))
        for (const auto &diagnostic : result.diagnostics)
          std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
      expect(!result.semanticValid && conflict && !code(result, "GTI-B0001"),
             message);
    };
    const std::string owner = R"(
struct Cleanup {
  Cleanup() {}
  Cleanup(Cleanup& other) = delete;
  Cleanup(Cleanup&& other) = default;
  ~Cleanup() { guarded = 9; }
};
#macro Cleanup owned() { return Cleanup(); }
#macro Cleanup nested_owned() { return owned(); }
)";
    accepted(prefix + owner + R"(
int main() {
  int& held = guarded;
  Cleanup saved = nested_owned();
  int snapshot = held;
  return snapshot;
}
)",
             "owning macro results transfer through nested result regions "
             "without a phantom cleanup under the caller's persistent loan");
    rejected(prefix + owner + R"(
int main() {
  int& held = guarded;
  [[discard]] nested_owned();
  return held;
}
)",
             "discarding a real owning macro result executes its conflicting "
             "cleanup before the persistent loan ends");
    accepted(prefix + owner + R"(
int main() {
  int& held = other;
  [[discard]] nested_owned();
  return held;
}
)",
             "actual owning macro cleanup remains legal for a disjoint "
             "persistent owner");
    accepted("import std.source_location;\n" + prefix + R"(
#macro ignore<F>(F action) {}
int main() {
  int& held = guarded;
  std::string_view written = std::expression_text(++guarded);
  ignore([]() -> void { guarded = 9; });
  return held + int(written.size());
}
)",
             "source-text and ignored fresh-callable probes never execute "
             "their operand's conflicting persistent write");

    const std::string callable = R"(
#macro int invoke<F>(F action) { return action(); }
#macro int relay<F>(F action) { return invoke(action); }
int selected<uint64_t Mode>() {
  int& held = guarded;
  int result = relay([]() -> int {
    if constexpr (Mode == 0) { other = 9; }
    else if constexpr (Mode == 1) { guarded = 9; }
    else { third = 9; }
    return 1;
  });
  return held + result;
}
)";
    for (const bool reverse : {false, true}) {
      const std::string conflicting =
          reverse ? "int first = selected<1>(); int second = selected<0>(); "
                  : "int first = selected<0>(); int second = selected<1>(); ";
      rejected(prefix + callable + "int main() { " + conflicting +
                   "return first + second; }",
               "fresh lambdas forced through nested result macros retain "
               "their exact hidden conflicting effect in both query orders");
      const std::string disjoint =
          reverse ? "int first = selected<2>(); int second = selected<0>(); "
                  : "int first = selected<0>(); int second = selected<2>(); ";
      accepted(prefix + callable + "int main() { " + disjoint +
                   "return first + second; }",
               "concrete fresh-callable macro effects remain disjoint and "
               "query-qualified in either specialization order");
    }
  }

  const auto discardable = analyze(R"(
[[discardable]] #macro T status<T>(T expression) { return expression; }
#macro legacy(int expression) { int saved = expression; }
#macro stop() { return; }
void caller<T>(T value) { status(value); }
int main() { caller(3); status(4); legacy(5); stop(); return 0; }
)",
                                   true, true);
  if (!discardable.canGenerateCode())
    for (const auto &diagnostic : discardable.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(discardable.canGenerateCode(),
         "ordinary macro statement analysis preserves discardable source "
         "contracts for symbolic and concrete calls and legacy void forms");
  const std::string requiredSource = "#macro int required() { return 1; } "
                                     "int main() { required(); return 0; }";
  const auto required = analyze(requiredSource, true, true);
  const auto call = requiredSource.rfind("required();");
  const auto ignored = std::find_if(
      required.diagnostics.begin(), required.diagnostics.end(),
      [](const auto &diagnostic) { return diagnostic.code == "GTI-S2009"; });
  expect(!required.semanticValid && ignored != required.diagnostics.end() &&
             ignored->primary.start ==
                 call + std::string_view("required(").size() &&
             ignored->primary.end == ignored->primary.start + 1 &&
             ignored->fixes.size() == 1 &&
             ignored->fixes.front().span.start == call &&
             ignored->fixes.front().replacement == "[[discard]] " &&
             !code(required, "GTI-B0001"),
         "non-discardable macro results retain the ordinary exact call "
         "diagnostic and explicit-discard fix without bypassing cleanup");
}

void initializerLocalDependencies() {
  const std::string helpers = R"(
#macro int local_value(int expression) {
  mut int saved = expression;
  saved += 1;
  return saved;
}
#macro int nested_value(int expression) {
  int saved = expression;
  return local_value(saved);
}
#macro int local_callback<F>(F action) {
  int saved = action();
  return saved;
}
)";
  const auto accepted = [&](const std::string &source,
                            std::string_view message) {
    const auto result = analyze(source, true, true);
    if (!result.canGenerateCode())
      for (const auto &diagnostic : result.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    expect(result.canGenerateCode(), message);
  };
  accepted(helpers + R"(
thread_local int first = local_value(3);
thread_local int second = nested_value(4);
thread_local int third = local_callback([]() -> int { return 7; });
int main() { return first + second + third; }
)",
           "namespace TLS initialization permits its own lexical values and "
           "fresh-callable results through copied nested-probe contexts");
  accepted(helpers + R"(
int main() {
  thread_local int first = nested_value(3);
  thread_local auto second = local_callback([]() -> int { return 7; });
  return first + second;
}
)",
           "typed and inferred local TLS initializers use their own exact "
           "declaration region rather than the surrounding function");
  accepted(helpers + R"(
#macro int nested_storage() {
  int local = 3;
  thread_local int nested = local_value(4);
  return local + nested;
}
thread_local int outer = nested_storage();
int main() { return outer; }
)",
           "nested independent TLS initialization restores the outer "
           "initializer's exact lexical declaration authority");

  for (const std::string type : {"int", "auto"}) {
    const std::string source =
        helpers + "int main() { int caller = 4; thread_local " + type +
        " state = nested_value(caller); return state; }";
    const auto result = analyze(source, true, true);
    const auto use = source.rfind("caller)");
    const auto declaration = source.find("caller =");
    const auto diagnostic = std::find_if(
        result.diagnostics.begin(), result.diagnostics.end(),
        [&](const auto &item) {
          return item.code == "GTI-S2124" && item.primary.start == use &&
                 item.primary.end == use + std::string_view("caller").size() &&
                 item.primary.source.ends_with("thread-local-semantics.gti") &&
                 std::any_of(item.related.begin(), item.related.end(),
                             [&](const auto &related) {
                               return related.span.start == declaration &&
                                      related.span.source ==
                                          item.primary.source;
                             });
        });
    expect(!result.semanticValid && diagnostic != result.diagnostics.end() &&
               !code(result, "GTI-B0001"),
           "substituted enclosing automatic values remain forbidden TLS "
           "dependencies with exact use and declaration spans");

    const auto nested = analyze(helpers + R"(
#macro int nested_storage() {
  int local = 3;
  thread_local )" + type + R"( nested = nested_value(local);
  return nested;
}
thread_local int outer = nested_storage();
int main() { return outer; }
)",
                                true, true);
    expect(!nested.semanticValid && code(nested, "GTI-S2124") &&
               !code(nested, "GTI-B0001"),
           "a nested TLS initializer cannot capture another initializer's "
           "lexical local in either typed or inferred storage");
  }
  const auto captured = analyze(helpers + R"(
int main() {
  int caller = 4;
  auto action = [caller]() -> int { return caller; };
  thread_local int state = local_value(action());
  return state;
}
)",
                                true, true);
  expect(!captured.semanticValid && code(captured, "GTI-S2124") &&
             !code(captured, "GTI-B0001"),
         "an enclosing captured closure remains surrounding automatic "
         "storage when substituted into an independent TLS initializer");
  const auto parameter = analyze(helpers + R"(
int read(int caller) {
  thread_local int state = nested_value(caller);
  return state;
}
int main() { return read(4); }
)",
                                 true, true);
  expect(!parameter.semanticValid && code(parameter, "GTI-S2124") &&
             !code(parameter, "GTI-B0001"),
         "an enclosing function parameter cannot acquire initializer-local "
         "ownership through a macro's copied expression context");
}

void deferredProbeIsolation() {
  const std::string prefix = R"(
thread_local mut int value = 1;
thread_local mut int other = 2;
void modify() { value = 2; }
struct Cleanup { ~Cleanup() { modify(); }  public: Cleanup() = default; };
bool inspect(Cleanup& input) { return true; }
void observe(bool value) {}
#macro ignore(bool value) {}
#macro force(bool value) { observe(value); }
)";
  // An unused expression parameter is checked, but neither its temporary nor
  // its destructor executes. Probe effects must not enter the caller's graph.
  const auto ignored =
      analyze(prefix + "int main() { int& borrowed = value; "
                       "ignore(inspect(Cleanup())); return borrowed; }",
              true, true);
  if (!ignored.canGenerateCode())
    for (const auto &diagnostic : ignored.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(ignored.canGenerateCode(),
         "an ignored deferred expression does not eagerly execute a "
         "temporary's conflicting TLS cleanup");

  for (const bool forced : {true, false}) {
    // The second form makes a real temporary after an ignored probe of the
    // same shape. A discarded probe must not consume or suppress real cleanup.
    const std::string selected =
        prefix + R"(
int selected<uint64_t Mode>() {
  if constexpr (Mode == 0) {
    int first = 0;
    int second = 1;
    int third = 2;
    ignore(inspect(Cleanup()));
    return first + second + third;
  } else {
)" +
        (forced ? "force(inspect(Cleanup())); return 1; "
                : "ignore(inspect(Cleanup())); bool seen = "
                  "inspect(Cleanup()); return seen ? 1 : 0; ") +
        "} }";
    for (const bool reverse : {false, true}) {
      const std::string calls =
          reverse ? "int first = selected<1>(); int second = selected<0>(); "
                  : "int first = selected<0>(); int second = selected<1>(); ";
      const auto source = [&](std::string_view owner) {
        return selected + "int main() { int& borrowed = " + std::string(owner) +
               "; " + calls + "return borrowed + first + second; }";
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
             forced ? "actually forced deferred temporary cleanup conflicts "
                      "with the retained TLS origin in either query order"
                    : "real cleanup after an ignored probe is not suppressed "
                      "in either concrete query order");
      const auto allowed = analyze(source("other"), true, true);
      if (!allowed.canGenerateCode())
        for (const auto &diagnostic : allowed.diagnostics)
          std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
      expect(allowed.canGenerateCode(),
             "deferred probe isolation preserves disjoint-owner "
             "execution through both concrete query orders");
    }
  }
}
} // namespace

int main() {
  storageAndMetadata();
  lifetimeAndEffects();
  exactWholeObjectVirtualEffects();
  persistentClassAliasBoundaries();
  packFoldStorageEffects();
  enumDesignatorEffects();
  packFoldCallerFacts();
  concreteEffectObligationReplay();
  invalidEffectQueryDiagnostics();
  stagedEffectQueryDiagnostics();
  concreteQueryDiagnosticOwnership();
  lateConcreteQueryContexts();
  reflectedQueryEffects();
  reflectionRegistrySnapshots();
  emptyPackDestructorProvider();
  packFoldUnsafeBoundary();
  orderedInputProtection();
  fullExpressionCleanup();
  invalidDeclarations();
  instanceCleanupCache();
  instanceTextRelayEffects();
  requirementProbeIsolation();
  macroResultStorageEffects();
  initializerLocalDependencies();
  deferredProbeIsolation();
  return failures ? 1 : 0;
}
