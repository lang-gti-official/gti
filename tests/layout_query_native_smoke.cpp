#include "gti/frontend.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace {

const lang::FunctionDecl *findMain(const lang::Program &program) {
  for (const lang::StmtPtr &declaration : program.declarations()) {
    const auto *function =
        dynamic_cast<const lang::FunctionDecl *>(declaration.get());
    if (function != nullptr && function->name().lexeme == "main") {
      return function;
    }
  }
  return nullptr;
}

std::unordered_map<std::string, std::uint64_t>
queryValues(const lang::FrontendResult &result) {
  std::unordered_map<std::string, std::uint64_t> values;
  const lang::FunctionDecl *main = findMain(result.program);
  if (main == nullptr || main->body() == nullptr) {
    return values;
  }
  for (const lang::StmtPtr &statement : main->body()->statements()) {
    const auto *variable =
        dynamic_cast<const lang::VariableDecl *>(statement.get());
    const auto *query = variable == nullptr
                            ? nullptr
                            : dynamic_cast<const lang::LayoutQuery *>(
                                  variable->initializer().get());
    if (query == nullptr ||
        result.semantics.typeOf(*query) != lang::SemanticType::UInt64) {
      continue;
    }
    const std::optional<lang::ConstantValue> constant =
        result.semantics.findConstant(*query);
    const auto *integer = constant == std::nullopt
                              ? nullptr
                              : std::get_if<lang::ConstantInteger>(&*constant);
    if (integer != nullptr && !integer->negative &&
        integer->domain ==
            lang::CheckedIntegerDomain{.width = 64, .signedValue = false}) {
      values.emplace(variable->name().lexeme, integer->magnitude);
    }
  }
  return values;
}

} // namespace

int main() {
  const lang::TargetInfo host = lang::TargetInfo::host();
  if (!host.supported()) {
    return 1;
  }

  const lang::FrontendResult result =
      lang::Frontend().analyze("layout-query-native-smoke.gti", R"(
using Word = uint16_t;
using Matrix = int16_t[3][4];

int main() {
  uint64_t bool_size = sizeof(bool);
  uint64_t bool_alignment = alignof(bool);
  uint64_t char_size = sizeof(char);
  uint64_t char_alignment = alignof(char);
  uint64_t i8_size = sizeof(int8_t);
  uint64_t i8_alignment = alignof(int8_t);
  uint64_t i16_size = sizeof(int16_t);
  uint64_t i16_alignment = alignof(int16_t);
  uint64_t i32_size = sizeof(int32_t);
  uint64_t i32_alignment = alignof(int32_t);
  uint64_t i64_size = sizeof(int64_t);
  uint64_t i64_alignment = alignof(int64_t);
  uint64_t u8_size = sizeof(uint8_t);
  uint64_t u8_alignment = alignof(uint8_t);
  uint64_t u16_size = sizeof(uint16_t);
  uint64_t u16_alignment = alignof(uint16_t);
  uint64_t u32_size = sizeof(uint32_t);
  uint64_t u32_alignment = alignof(uint32_t);
  uint64_t u64_size = sizeof(uint64_t);
  uint64_t u64_alignment = alignof(uint64_t);
  uint64_t float_size = sizeof(float);
  uint64_t float_alignment = alignof(float);
  uint64_t pointer_size = sizeof(void*);
  uint64_t pointer_alignment = alignof(const int32_t*);
  uint64_t alias_size = sizeof(Word);
  uint64_t matrix_size = sizeof(Matrix);
  uint64_t matrix_alignment = alignof(Matrix);
  return 0;
}
)");
  if (!result.canGenerateCode() || !result.diagnostics.empty()) {
    return 2;
  }

  const std::unordered_map<std::string, std::uint64_t> actual =
      queryValues(result);
  const auto matches = [&](std::string_view name, std::size_t value) {
    const auto found = actual.find(std::string(name));
    return found != actual.end() && found->second == value;
  };

  using Matrix = std::int16_t[3][4];
  if (actual.size() != 27 || !matches("bool_size", sizeof(bool)) ||
      !matches("bool_alignment", alignof(bool)) ||
      !matches("char_size", sizeof(std::uint8_t)) ||
      !matches("char_alignment", alignof(std::uint8_t)) ||
      !matches("i8_size", sizeof(std::int8_t)) ||
      !matches("i8_alignment", alignof(std::int8_t)) ||
      !matches("i16_size", sizeof(std::int16_t)) ||
      !matches("i16_alignment", alignof(std::int16_t)) ||
      !matches("i32_size", sizeof(std::int32_t)) ||
      !matches("i32_alignment", alignof(std::int32_t)) ||
      !matches("i64_size", sizeof(std::int64_t)) ||
      !matches("i64_alignment", alignof(std::int64_t)) ||
      !matches("u8_size", sizeof(std::uint8_t)) ||
      !matches("u8_alignment", alignof(std::uint8_t)) ||
      !matches("u16_size", sizeof(std::uint16_t)) ||
      !matches("u16_alignment", alignof(std::uint16_t)) ||
      !matches("u32_size", sizeof(std::uint32_t)) ||
      !matches("u32_alignment", alignof(std::uint32_t)) ||
      !matches("u64_size", sizeof(std::uint64_t)) ||
      !matches("u64_alignment", alignof(std::uint64_t)) ||
      !matches("float_size", sizeof(float)) ||
      !matches("float_alignment", alignof(float)) ||
      !matches("pointer_size", sizeof(void *)) ||
      !matches("pointer_alignment", alignof(void *)) ||
      !matches("alias_size", sizeof(std::uint16_t)) ||
      !matches("matrix_size", sizeof(Matrix)) ||
      !matches("matrix_alignment", alignof(Matrix))) {
    return 3;
  }
  return 0;
}
