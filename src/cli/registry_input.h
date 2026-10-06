#pragma once
#include <string>
namespace lang::cli {
// Explicit stdin is bounded and single-token; terminal entry disables echo.
[[nodiscard]] std::string readRegistryToken(bool tokenStdin);
} // namespace lang::cli
