#include "compiler_test_fixture.h"

namespace {
lang::FrontendResult analyze(std::string source) {
  return lang::Frontend().analyze("semantic-call-regression.gti",
                                  std::move(source), {standardLibraryPrelude()},
                                  {}, {standardLibraryRoot()});
}
void accepted(std::string source) {
  const auto result = analyze(std::move(source));
  if (!result.canGenerateCode())
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(result.canGenerateCode(),
         "valid generic call composition reaches MIR");
  if (result.canGenerateCode())
    expect(lang::verifyMirProgram(result.mir).valid(),
           "call composition verifies");
}
void rejected(std::string source, std::string_view code,
              std::string_view message, std::string_view anchor,
              std::string_view hint = {}) {
  const auto result = analyze(source);
  std::vector<const lang::Diagnostic *> errors;
  for (const auto &diagnostic : result.diagnostics)
    if (diagnostic.severity == lang::DiagnosticSeverity::Error)
      errors.push_back(&diagnostic);
  if (errors.size() != 1 || errors.front()->code != code)
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(errors.size() == 1, "one causal error without follow-on diagnostics");
  if (errors.size() != 1)
    return;
  const auto &diagnostic = *errors.front();
  expect(diagnostic.code == code &&
             diagnostic.message.find(message) != std::string::npos,
         "stable specific diagnostic code and cause");
  expect(diagnostic.primary.source.ends_with("semantic-call-regression.gti") &&
             diagnostic.primary.start == source.rfind(anchor) &&
             diagnostic.primary.end > diagnostic.primary.start,
         "diagnostic identifies the caller expression");
  if (!hint.empty())
    expect(std::any_of(diagnostic.hints.begin(), diagnostic.hints.end(),
                       [&](const auto &text) {
                         return text.find(hint) != std::string::npos;
                       }),
           "diagnostic explains the actionable failure");
}
} // namespace
int main() {
  accepted(R"GTI(concept steps<I> = requires(mut I& p) { { ++p } -> void; };
struct Counter { mut int at = 0; public: Counter() = default; void operator++() mut { this.at++; } };
void step<steps I>(mut I& it) { ++it; }
I skip<steps I>(mut I first) {
  step(first);
  return std::move(first);
}
int main() {
  Counter c{};
  Counter d = skip(c);
  return d.at == 1 ? 0 : 1;
}
)GTI");
  accepted(R"GTI(import std.functional;
class table<K, H> {
  H hasher;
public:
  table(H h) requires std::read_callable<H, K, uint64_t> : hasher(std::move(h)) {}
  uint64_t slot(K key, uint64_t n) requires std::read_callable<H, K, uint64_t> {
    return this.hasher(key) % n;
  }
};
struct id_hash {
  id_hash() = default;
  uint64_t operator()(int& v) { return uint64_t(v); }
};
int main() {
  table<int, id_hash> t{id_hash()};
  return t.slot(9, uint64_t(4)) == uint64_t(1) ? 0 : 1;
}
)GTI");
  accepted(
      R"GTI(concept maps<F, A, R> = requires(F& f, A& a) { { f(a) } -> R; };
class table<K, H> {
  H hasher;
public:
  table(H h) requires maps<H, K, uint64_t> : hasher(std::move(h)) {}
  uint64_t slot(K key, uint64_t n) requires maps<H, K, uint64_t> {
    return this.hasher(key) % n;
  }
};
struct id_hash {
  id_hash() = default;
  uint64_t operator()(int& v) { return uint64_t(v); }
};
int main() {
  table<int, id_hash> t{id_hash()};
  return t.slot(9, uint64_t(4)) == uint64_t(1) ? 0 : 1;
}
)GTI");
  accepted(R"GTI(import std.functional;
class table<K, H> {
  H hasher;
public:
  table(H h) requires std::read_callable<H, K, uint64_t> : hasher(std::move(h)) {}
  uint64_t slot(K key, uint64_t n) requires std::read_callable<H, K, uint64_t> {
    uint64_t hash = this.hasher(key); return hash % n;
  }
};
struct id_hash {
  id_hash() = default;
  uint64_t operator()(int& v) { return uint64_t(v); }
};
int main() {
  table<int, id_hash> t{id_hash()};
  return t.slot(9, uint64_t(4)) == uint64_t(1) ? 0 : 1;
}
)GTI");
  accepted(R"GTI(class Parcel {
public:
  int value;
  Parcel(int v) : value(v) {}
  Parcel(Parcel&) = delete;
  Parcel(Parcel&&) = default;
};
void consume<Args...>(int tag, Args... args) {}
void consume(char tag) {}
int main() { Parcel p{42}; consume(1, std::move(p)); return 0; }
)GTI");
  accepted(R"GTI(constexpr int source() { return 7; }
consteval int check(int value) { return value; }
int main() { return check(source()) - 7; }
)GTI");
  accepted(
      R"GTI(concept stepped<T> = requires(T& value) { { value + value } -> T; };
void sink<stepped Args...>(Args... args) {}
void forward<stepped Args...>(Args... args) { sink(args...); }
int main() { forward(1, 2); return 0; }
)GTI");
  rejected(R"GTI(class Parcel {
public:
  int value;
  Parcel(int v) : value(v) {}
  Parcel(Parcel&) = delete;
  Parcel(Parcel&&) = default;
};
void consume<Args...>(int tag, Args... args) {}
void consume(char tag) {}
int main() { Parcel p{42}; consume(1, p); return 0; }
)GTI",
           "GTI-S2018", "deleted copy constructor", "p);", "std::move");
  rejected(R"GTI(import std.string;
import std.format;

int main() {
  mut std::string s = "hello";
  std::println("{}", s);
  return 0;
}
)GTI",
           "GTI-S2018", "deleted copy constructor", "s);", "std::move");
  rejected(R"GTI(int dynamic_value() { return 7; }
consteval int check(int value) { return value; }
int main() { return check(dynamic_value()); }
)GTI",
           "GTI-S2057", "Consteval invocation", "); }",
           "Argument 1 to immediate function 'check'");
  rejected(R"GTI(consteval int check(int value) { return value; }
int run(int value) { return check(value); }
int main() { return run(1); }
)GTI",
           "GTI-S2057", "Consteval invocation", "); }\nint main",
           "Argument 1 to immediate function 'check'");
  return failures == 0 ? 0 : 1;
}
