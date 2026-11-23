// CI consumers supply required context and honor configured timeout scaling.
#include "unit_test.h"
#include <cstdlib>
#include <filesystem>
#include <string>

GTI_UNIT_TEST(ci_consumers) {
  const auto script =
      std::filesystem::path(__FILE__).parent_path() / "ci_consumer_checks.py";
  std::string quoted = "'";
  for (char c : script.string()) {
    quoted += c == '\'' ? "'\\''" : std::string(1, c);
  }
  quoted += "'";
  return std::system(("python3 " + quoted).c_str()) == 0 ? 0 : 1;
}
