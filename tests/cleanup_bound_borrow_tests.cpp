#include "gti/frontend.h"
#include "gti/mir_printer.h"
#include "gti/mir_verifier.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>

namespace {
int failures = 0;
void expect(bool condition, const std::string &name) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << name << '\n';
  }
}
const std::string prefix = R"gti(
#include <std/utility>
mut int observed = 0;
struct Resource {
  mut int value = 7;
  void reset() mut { this.value = 9; }
};
class Guard {
  Resource& resource;
public:
  Guard(Resource& resource) : resource(resource) {}
  ~Guard() { observed += this.resource.value; }
};
class Box<T> {
  T item;
public:
  Box(T item) : item(std::move(item)) {}
};
)gti";
lang::FrontendResult analyze(const std::string &source) {
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  return lang::Frontend().analyze(
      root / "cleanup-bound-test.gti", prefix + source,
      {root / "stdlib/prelude.gti"}, {}, {root / "stdlib"});
}
void show(const lang::FrontendResult &result) {
  for (const auto &diagnostic : result.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  if (std::getenv("GTI_TEST_DUMP_MIR") != nullptr) {
    for (const auto &error : lang::verifyMirProgram(result.mir).errors) {
      if (error.bodyKind == lang::MirBodyKind::Function) {
        if (const auto *function = result.mir.findFunctionInstance(error.owner))
          std::cerr << lang::MirPrinter().print(function->body);
      } else if (error.bodyKind == lang::MirBodyKind::Constructor) {
        if (const auto *constructor =
                result.mir.findConstructorInstance(error.owner))
          std::cerr << lang::MirPrinter().print(constructor->body);
      }
    }
  }
}
void positive(const std::string &name, const std::string &source) {
  const auto result = analyze(source);
  if (!result.canGenerateCode())
    show(result);
  expect(result.canGenerateCode(), name);
  if (result.canGenerateCode())
    expect(lang::verifyMirProgram(result.mir).valid(), name + " verifies");
}
void negative(const std::string &name, const std::string &source,
              const std::string &expectedCode = "GTI-S2017") {
  const auto result = analyze(source);
  if (std::getenv("GTI_TEST_DUMP_DIAGNOSTICS") != nullptr) {
    std::cerr << name << '\n';
    show(result);
  }
  const bool semanticError = std::any_of(
      result.diagnostics.begin(), result.diagnostics.end(), [&](const auto &d) {
        return d.severity == lang::DiagnosticSeverity::Error &&
               d.code == expectedCode;
      });
  if (result.canGenerateCode() || !semanticError)
    show(result);
  expect(!result.canGenerateCode() && semanticError, name);
}
} // namespace

int main() {
  positive("shared independent guards and whole movement", R"gti(
int main() {
  mut Resource owner{};
  { Guard first{owner}; Guard second{owner}; auto moved = std::move(first); }
  owner.reset(); return observed - 14;
})gti");
  positive("guard factory", R"gti(
Guard acquire(Resource& owner) { return Guard(owner); }
int main() { Resource owner{}; auto guard = acquire(owner); return 0; }
)gti");
  positive("consumed guard parameter", R"gti(
void consume(Guard guard) {}
int main() { mut Resource owner{}; consume(Guard(owner)); owner.reset(); return 0; }
)gti");
  positive("nested generic owning guard", R"gti(
int main() { mut Resource owner{}; { Box<Guard> box{Guard(owner)}; }
  owner.reset(); return observed - 7; }
)gti");
  positive("independent dependency set", R"gti(
class Both {
  Resource& first; Resource& second;
public:
  Both(Resource& first, Resource& second) : first(first), second(second) {}
  ~Both() { observed += this.first.value + this.second.value; }
};
int main() { Resource a{}; Resource b{}; Both both{a, b}; return 0; }
)gti");
  positive("cleanup-bound buffering retains an adapter shell and its owner",
           R"gti(
#include <std/io>
#include <std/string>
int main() {
  std::byte_buffer bytes = {uint8_t(97)};
  auto view = bytes.as_span();
  mut std::io::memory_reader source{view};
  mut std::io::buffered_reader<std::io::memory_reader> input{source, std::size_t(2)};
  mut std::string line{};
  auto result = std::io::getline(input, line);
  return result ? 0 : 1;
}
)gti");
  positive("resource is available after the complete cleanup dependency scope",
           R"gti(
class View {
  Resource& resource;
public:
  View(Resource& resource) : resource(resource) {}
  int read() { return this.resource.value; }
};
class Adapter {
  View& view;
public:
  Adapter(View& view) : view(view) {}
  ~Adapter() { observed += this.view.read(); }
};
int main() {
  mut Resource resource{};
  { View view{resource}; Adapter adapter{view}; }
  resource.reset();
  return observed - 7;
}
)gti");
  negative("cleanup transitively retains the borrowed carrier's owner", R"gti(
class View {
  Resource& resource;
public:
  View(Resource& resource) : resource(resource) {}
  int read() { return this.resource.value; }
};
class Adapter {
  View& view;
public:
  Adapter(View& view) : view(view) {}
  ~Adapter() { observed += this.view.read(); }
};
int main() {
  mut Resource resource{};
  View view{resource};
  Adapter adapter{view};
  resource.reset();
  return 0;
}
)gti");
  negative("last use cannot release cleanup dependency", R"gti(
int main() { mut Resource owner{}; Guard guard{owner}; owner.reset(); return 0; }
)gti");
  negative("movement cannot release cleanup dependency", R"gti(
int main() { mut Resource owner{}; Guard guard{owner}; auto moved = std::move(guard);
  owner.reset(); return 0; }
)gti");
  negative("nested guard cannot release cleanup dependency", R"gti(
int main() { mut Resource owner{}; Box<Guard> box{Guard(owner)};
  owner.reset(); return 0; }
)gti");
  negative("local resource cannot escape", R"gti(
Guard bad() { Resource owner{}; return Guard(owner); }
int main() { auto guard = bad(); return 0; }
)gti",
           "GTI-S2045");
  negative("cleanup cannot upgrade a shared reference", R"gti(
class Bad {
  Resource& resource;
public:
  Bad(Resource& resource) : resource(resource) {}
  ~Bad() { this.resource.reset(); }
};
int main() { Resource owner{}; Bad guard{owner}; return 0; }
)gti",
           "GTI-S2000");
  return failures == 0 ? 0 : 1;
}
