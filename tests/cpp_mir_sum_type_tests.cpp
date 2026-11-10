// C++ representation of sum types: native unions, tagged payload_variant
// storage, the C++ type schedule for embedded payload enums, staged
// move-only arguments and element-subject switch text. The backend-neutral
// semantic and MIR facts live in sum_type_tests.cpp.

#include "gti/frontend.h"
#include "gti/optimizer.h"

#include "cpp_backend_test_support.h"

#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

lang::FrontendResult analyze(std::string_view name, std::string source) {
  return lang::Frontend().analyze(std::string(name), std::move(source));
}

lang::FrontendResult analyzeWithPrelude(std::string_view name,
                                        std::string source) {
  const std::filesystem::path library =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  return lang::Frontend().analyze(std::string(name), std::move(source),
                                  {library / "prelude.gti"}, {}, {library});
}

std::string validSource() {
  return R"(
enum class BitKind : uint8_t { integer, real };

union Bits {
  mut uint32_t integer;
  mut float real;
  mut BitKind kind;
};

enum class Message {
  quit,
  move(int32_t x, int32_t y),
  code(int32_t value),
};

int32_t inspect(Message message) {
  switch (message) {
  case Message::quit:
    return 1;
  case Message::move(x, y):
    return x + y;
  case Message::code(value):
    return value;
  }
}

int main() {
  mut Bits bits{uint32_t(0)};
  unsafe {
    bits.real = 42.0;
  }
  Message message = Message::move(20, 22);
  return inspect(message) - 42;
}
)";
}

void testNativeUnionAndVariantStorage() {
  const lang::FrontendResult result = analyze("sum-types.gti", validSource());
  expect(result.canGenerateCode(), "sum-type fixture analyzes");
  if (!result.canGenerateCode())
    return;
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(result.hir, lang::OptimizationLevel::O1);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(result, result.mir, result.mir, optimizations);
  expect(
      artifact.contents.find("union Bits {") != std::string::npos &&
          // Native alternatives carry the source ordinals unshifted; the
          // backend-private empty shell is the final alternative.
          artifact.contents.find(
              "::gti_internal::backend::payload_variant<__gti_variant_0, "
              "__gti_variant_1, __gti_variant_2, std::monostate> "
              "__gti_value;") != std::string::npos &&
          artifact.contents.find(".__gti_value.index()") != std::string::npos &&
          artifact.contents.find("::gti_internal::backend::payload_get<1>(") !=
              std::string::npos &&
          artifact.contents.find("std::is_union_v<Bits>") != std::string::npos,
      "the C++ backend should use native unions and tagged variant storage");
}

void testEmbeddedPayloadEnumSchedule() {
  const lang::FrontendResult aggregateContainment =
      analyzeWithPrelude("owning-payload-aggregate-containment.gti", R"(
class resource {
public:
  resource() {}
  ~resource() {}
};
enum class holder { one(resource value), none };
class wrapper {
public:
  holder value = holder::one(resource());
public:
  wrapper() = default;
};
struct default_wrapper {
  holder value = holder::one(resource());
public:
  default_wrapper() = default;
};
class generic_wrapper<T> {
  T value;
public:
  generic_wrapper(T value) : value(std::move(value)) {}
};
int main() {
  wrapper first = wrapper();
  default_wrapper second = default_wrapper();
  generic_wrapper<holder> third = generic_wrapper<holder>(holder::none);
  return 0;
}
)");
  expect(aggregateContainment.canGenerateCode(),
         "aggregate containment fixture analyzes");
  if (!aggregateContainment.canGenerateCode())
    return;
  const lang::BackendArtifact containmentArtifact =
      gti_test::emitCpp(aggregateContainment);
  const std::size_t enumDefinition =
      containmentArtifact.contents.find("struct holder {");
  const std::size_t wrapperDefinition =
      containmentArtifact.contents.find("class wrapper {");
  expect(enumDefinition != std::string::npos &&
             wrapperDefinition != std::string::npos &&
             enumDefinition < wrapperDefinition &&
             containmentArtifact.contents.find(
                 ", std::monostate> __gti_value;") != std::string::npos,
         "the C++ type schedule should define an embedded payload enum before "
         "its owning class and retain the private disengaged shell");
}

void testStagedMoveOnlyArguments() {
  const auto program =
      analyzeWithPrelude("cleanup-free-move-only-payload.gti", R"(
struct token {
  int32_t id;
  token(int32_t raw) : id(raw) {}
  token(token& other) = delete;
  token(token&& other) = default;
};
enum class choice {
  none,
  some(token item),
};
token pick(choice value) {
  switch (std::move(value)) {
  case choice::some(item):
    return item;
  case choice::none:
    return token(0);
  }
}
int32_t tag(choice value) {
  switch (value) {
  case choice::some:
    return 1;
  case choice::none:
    return 0;
  }
}
int main() {
  choice named = choice::some(token(2));
  switch (named) {
  case choice::some:
    break;
  case choice::none:
    return 1;
  }
  return pick(std::move(named)).id + tag(choice::none) - 2;
}
)");
  expect(program.canGenerateCode(), "move-only payload fixture analyzes");
  if (!program.canGenerateCode())
    return;
  const std::string text = gti_test::emitCpp(program).contents;
  const std::size_t observer =
      text.find("_tag(::__gti_program::choice __gti_mir_arg_0) {");
  const std::size_t observerEnd =
      observer == std::string::npos ? observer : text.find("\n  }\n", observer);
  const std::string observerBody =
      observerEnd == std::string::npos
          ? std::string{}
          : text.substr(observer, observerEnd - observer);
  expect(text.find("_pick(std::move(__gti_mir_v_") != std::string::npos &&
             text.find("_tag(std::move(__gti_mir_v_") != std::string::npos &&
             observerBody.find("switch (__gti_mir_p_") != std::string::npos &&
             observerBody.find("switch (__gti_mir_v_") == std::string::npos,
         "by-value arguments move their staged move-only locals, and an "
         "observing switch reads its parameter in place");
}

void testElementSubjectSwitchText() {
  const auto program = analyzeWithPrelude("fixed-array-element-subject.gti", R"(
struct token {
  mut int32_t id;
  token(int32_t raw) : id(raw) {}
};
enum class choice {
  none,
  some(token item),
  pair(int32_t left, int32_t right),
};
struct record {
  choice state;
  record(choice value) : state(value) {}
};
class shelf {
  choice[2] items;

public:
  shelf() : items({choice::none, choice::some(token(3))}) {}
  int32_t at(int32_t index) {
    switch (this.items[index]) {
    case choice::some(auto& item):
      return item.id;
    default:
      return 0;
    }
  }
};
choice[2] table = {choice::none, choice::some(token(5))};
int32_t dynamic(int32_t index) {
  choice[3] items = {choice::none, choice::some(token(4)), choice::pair(5, 6)};
  switch (items[index]) {
  case choice::some(auto& item):
    return item.id;
  case choice::pair(left, auto& right):
    return left * 10 + right;
  case choice::none:
    return 0;
  }
}
int32_t constant() {
  choice[2] items = {choice::none, choice::some(token(4))};
  switch (items[1]) {
  case choice::some(auto& item):
    return item.id;
  default:
    return 0;
  }
}
int32_t nested(int32_t row, int32_t column) {
  choice[2][2] grid = {{choice::none, choice::some(token(7))},
                       {choice::pair(1, 2), choice::none}};
  switch (grid[row][column]) {
  case choice::some(auto& item):
    return item.id;
  default:
    return 0;
  }
}
int32_t global(int32_t index) {
  switch (table[index]) {
  case choice::some(auto& item):
    return item.id;
  default:
    return 0;
  }
}
int32_t field() {
  record[2] records = {record(choice::none), record(choice::some(token(8)))};
  switch (records[1].state) {
  case choice::some(auto& item):
    return item.id;
  default:
    return 0;
  }
}
int32_t update(int32_t index) {
  mut choice[2] items = {choice::none, choice::some(token(9))};
  switch (items[index]) {
  case choice::some(mut auto& item):
    item.id = item.id + 1;
    break;
  default:
    break;
  }
  switch (items[index]) {
  case choice::some(auto& item):
    return item.id;
  default:
    return 0;
  }
}
int main() {
  shelf books{};
  return dynamic(1) + dynamic(2) + constant() + nested(0, 1) + global(1) +
         field() + update(1) + books.at(1) - 97;
}
)");
  expect(program.canGenerateCode(), "element subject fixture analyzes");
  if (!program.canGenerateCode())
    return;
  std::string emitted;
  try {
    emitted = gti_test::emitCpp(program).contents;
  } catch (const std::exception &error) {
    std::cerr << "element subject emission: " << error.what() << '\n';
  }
  expect(emitted.find("mir_checked_array_bound_v1(") != std::string::npos &&
             emitted.find("].__gti_value.index()") != std::string::npos &&
             emitted.find("].__gti_value).__gti_field_") != std::string::npos &&
             emitted.find("].state.__gti_value.index()") != std::string::npos,
         "the C++ backend checks the bound, then switches on and reads "
         "payloads from the element itself");
}

} // namespace

int main() {
  testNativeUnionAndVariantStorage();
  testEmbeddedPayloadEnumSchedule();
  testStagedMoveOnlyArguments();
  testElementSubjectSwitchText();
  if (failures != 0) {
    std::cerr << failures << " test(s) failed\n";
    return 1;
  }
  std::cout << "sum-type C++ representation tests passed\n";
  return 0;
}
