#include "gti/driver/native_toolchain.h"
#include "unit_test.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

GTI_UNIT_TEST(native_link_ordered_objects) {
  using namespace lang::driver;
  const NativeLinkRequest request(
      "clang++", std::vector<std::filesystem::path>{"unit-z.o", "unit-a.o"},
      "program", NativeInputs{});
  const auto command = NativeToolchain().command(request);
  const std::vector<std::string> units{"unit-z.o", "unit-a.o"};
  if (std::search(command.begin(), command.end(), units.begin(), units.end()) ==
      command.end()) {
    std::cerr << "Native linking did not preserve code generation unit order\n";
    return 1;
  }
  return 0;
}
