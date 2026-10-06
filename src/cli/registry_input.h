#pragma once
#include <string>
#include <string_view>
namespace lang::cli {
// Explicit stdin is bounded and single-token; terminal entry disables echo.
[[nodiscard]] std::string readRegistryToken(bool tokenStdin);
[[nodiscard]] std::string readRegistryUsername();
[[nodiscard]] std::string readRegistrySecret(std::string_view prompt,
                                             bool allowEmpty = false);
} // namespace lang::cli
