// Manifest diagnostic positions: toml++ reports columns in Unicode scalars,
// while GTI spans and edits are byte offsets, so a column after two-, three-
// and four-byte characters must land on the right byte. A project transcript
// shows only the rendered column, not the offset an edit uses.
#include "../../src/driver/manifest_source.h"
#include "unit_test.h"

#include <iostream>
#include <string_view>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

} // namespace

GTI_UNIT_TEST(manifest_source_offsets) {
  using lang::driver::detail::manifestSourceOffset;
  // "é" is two bytes, "€" three and "😀" four; "x" is column 12.
  constexpr std::string_view source = "[package]\nname = \"é€😀x\"\n";
  expect(manifestSourceOffset(source, 2, 9) == source.find("é"),
         "an ASCII prefix counts one byte per scalar");
  expect(manifestSourceOffset(source, 2, 12) == source.find('x'),
         "multi-byte scalars advance by their encoded length");
  expect(manifestSourceOffset(source, 2, 99) == source.size() - 1,
         "a column past the line stops at its end");
  expect(manifestSourceOffset(source, 9, 1) == source.size(),
         "a line past the source stops at its end");
  expect(manifestSourceOffset(source, 0, 3) == 0 &&
             manifestSourceOffset(source, 2, 0) == 0,
         "an unknown position is the start of the source");
  return failures == 0 ? 0 : 1;
}
