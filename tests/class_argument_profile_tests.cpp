#include "compiler_test_fixture.h"

namespace {
const std::string owned = R"(
class Packet<T, E> {
  T payload;
public:
  Packet(T value) : payload(std::move(value)) {}
  T& value() { return this.payload; }
  int kind() { return 1; }
};
)";
const std::string empty = R"(
class Packet<void, E> {
public:
  Packet() {}
  void value() {}
  int kind() { return 2; }
};
)";
const std::string borrowed = R"(
class Packet<T&, E> {
  T& payload;
public:
  Packet(T& value) : payload(value) {}
  T& value() { return this.payload; }
  int kind() { return 3; }
};
)";

lang::FrontendResult analyze(std::string source) {
  return lang::Frontend().analyze("class-argument-profile.gti",
                                  std::move(source), {standardLibraryPrelude()},
                                  {}, {standardLibraryRoot()});
}

void accepted(std::string source) {
  const auto result = analyze(std::move(source));
  if (!result.canGenerateCode())
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(result.canGenerateCode(), "argument profile is a valid source class");
  if (result.canGenerateCode()) {
    expect(lang::verifyMirProgram(result.mir).valid(),
           "profile preserves verified MIR");
    expect(!gti_test::emitCpp(result).contents.empty(),
           "profile reaches native emission");
  }
}

void rejected(std::string source, std::string_view code,
              std::string_view text) {
  const auto result = analyze(std::move(source));
  expect(!result.canGenerateCode(),
         "invalid profile or borrow has no code generation");
  const auto found =
      std::find_if(result.diagnostics.begin(), result.diagnostics.end(),
                   [&](const auto &diagnostic) {
                     return diagnostic.code == code &&
                            diagnostic.message.find(text) != std::string::npos;
                   });
  if (found == result.diagnostics.end())
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(found != result.diagnostics.end(),
         "profile reports the owning semantic rule");
}
} // namespace

int main() {
  const std::string client = R"(
int inspect<T, E>(Packet<T, E>& packet) { return packet.kind(); }
int main() {
  Packet<int, int> value{42};
  Packet<void, int> empty{};
  int owner = 7;
  Packet<int&, int> reference{owner};
  empty.value();
  return inspect(value) + inspect(empty) + inspect(reference) - 6;
}
)";
  for (const auto &definitions :
       {owned + empty + borrowed, empty + borrowed + owned,
        borrowed + owned + empty})
    accepted(definitions + client);

  rejected(owned + owned, "GTI-S2006", "Duplicate declaration");
  rejected(owned + empty + empty, "GTI-S2006", "Duplicate declaration");
  rejected("class Packet<T&, E> {}; class Packet<T, E&> {};", "GTI-S2006",
           "Duplicate declaration");
  rejected(owned + borrowed + R"(
Packet<int&, int> bad() {
  int local = 42;
  return Packet<int&, int>(local);
}
)",
           "GTI-S2045", "outlives the result");
  rejected(owned + borrowed + R"(
int main() {
  mut int owner = 42;
  Packet<int&, int> reference{owner};
  owner = 7;
  return reference.value();
}
)",
           "GTI-S2017", "borrow");
  return failures == 0 ? 0 : 1;
}
