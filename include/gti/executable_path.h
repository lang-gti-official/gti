#pragma once

#include <filesystem>

namespace lang {

std::filesystem::path executablePath(const char *driver);

} // namespace lang
