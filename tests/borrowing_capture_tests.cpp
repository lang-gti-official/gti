#include "compiler_test_fixture.h"

int main() {
  const auto positive = [](const std::string &name, const std::string &source) {
    auto result = analyzeTrustedPreludeFixture(name + ".gti", source);
    if (!result.canGenerateCode())
      for (const auto &diagnostic : result.diagnostics)
        std::cerr << name << ": " << diagnostic.code << ": "
                  << diagnostic.message << '\n';
    expect(result.canGenerateCode(), name);
    return result;
  };
  const auto negative = [](const std::string &name, const std::string &source,
                           const std::string &code) {
    const auto result = analyzeTrustedPreludeFixture(name + ".gti", source);
    expect(!result.canGenerateCode() &&
               hasDiagnosticCode(result.diagnostics, code) &&
               !hasDiagnosticCode(result.diagnostics, "GTI-B0001"),
           name + " is rejected by semantics, not the backend");
  };
  positive("shared-capture", R"(
int main() {
  int value = 7;
  auto read = [&value]() -> int { return value; };
  return read() - 7;
}
)");
  positive("exclusive-capture", R"(
int main() {
  mut int value = 7;
  mut auto increment = [mut &value]() -> int { value += 1; return value; };
  int first = increment();
  int second = increment();
  return first + second - 17;
}
)");
  positive("disjoint-captures", R"(
struct State { mut int left = 2; mut int right = 3; };
int main() {
  mut State state{};
  mut auto& left = state.left;
  mut auto& right = state.right;
  mut auto update = [mut &left, mut &right]() -> int {
    left += 1;
    right += 2;
    return left + right;
  };
  return update() - 8;
}
)");
  positive("confined-generic-capture", R"(
int invoke<F>(mut F& callback) { return callback(); }
int main() {
  mut int value = 3;
  mut auto update = [mut &value]() -> int { value += 2; return value; };
  return invoke(update) - 5;
}
)");
  negative("shared-capture-mutation", R"(
int main() {
  mut int value = 0;
  auto callback = [&value]() -> int { value += 1; return value; };
  return callback();
}
)", "GTI-S2002");
  negative("exclusive-capture-readonly-source", R"(
int main() {
  int value = 0;
  mut auto callback = [mut &value]() -> int { return value; };
  return callback();
}
)", "GTI-S2017");
  negative("exclusive-capture-readonly-call", R"(
int main() {
  mut int value = 0;
  auto callback = [mut &value]() -> int { value += 1; return value; };
  return callback();
}
)", "GTI-S2046");
  negative("capture-owner-conflict", R"(
int main() {
  mut int value = 0;
  auto callback = [&value]() -> int { return value; };
  value = 4;
  return callback();
}
)", "GTI-S2017");
  negative("exclusive-capture-alias", R"(
int main() {
  mut int value = 0;
  mut auto& first = value;
  mut auto& second = value;
  mut auto callback = [mut &first, mut &second]() -> int { return first + second; };
  return callback();
}
)", "GTI-S2017");
  negative("exclusive-capture-copy", R"(
int main() {
  mut int value = 0;
  mut auto callback = [mut &value]() -> int { value += 1; return value; };
  mut auto copy = callback;
  return copy();
}
)", "GTI-S2003");
  return failures == 0 ? 0 : 1;
}
