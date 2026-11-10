#include "compiler_test_fixture.h"
#include "cpp_backend_test_support.h"

// Frozen C++ backend representation of std::inline_polymorphic's slot. The
// backend-neutral slot contract lives in polymorphic_storage_compiler_tests.

namespace {

constexpr std::string_view source = R"(
import std.polymorphic;
import std.string;

interface meter {
  int read();
  void bump(int amount) mut;
};

class gauge : public meter {
  mut int value;
public:
  gauge(int value) : value(value) {}
  int read() override { return this.value; }
  void bump(int amount) mut override { this.value = this.value + amount; }
};

class labelled : public meter {
  std::string label;
public:
  labelled(std::string label) : label(std::move(label)) {}
  int read() override { return int(this.label.size()); }
  void bump(int amount) mut override {}
};

int observe(std::inline_polymorphic<meter, 32, 8>& value) {
  return value.get().read();
}

int main() {
  mut std::inline_polymorphic<meter, 32, 8> value{};
  value.replace(gauge(3));
  value.get().bump(1);
  mut std::inline_polymorphic<meter, 32, 8> moved = std::move(value);
  int inline_read = observe(moved);
  moved.replace(labelled(std::string("abc")));
  int boxed_read = observe(moved);
  moved.reset();
  return inline_read + boxed_read - 7;
}
)";

void testPolymorphicStorageCppRepresentation() {
  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "polymorphic-storage-contract.gti", std::string(source),
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(frontend.canGenerateCode(),
         "the installed polymorphic surface should pass semantic analysis");
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << diagnostic.message << '\n';
    }
    return;
  }

  const lang::BackendArtifact artifact = gti_test::emitCpp(frontend);
  const std::size_t begin = artifact.contents.find(
      "template <typename Interface, std::size_t Bytes, std::size_t Align>\n"
      "class polymorphic_storage {");
  const std::size_t end = artifact.contents.find(
      "inline bool polymorphic_storage_has_value(", begin);
  const std::string representation =
      begin == std::string::npos || end == std::string::npos
          ? std::string{}
          : artifact.contents.substr(begin, end - begin);
  const std::size_t tableBegin =
      artifact.contents.find("polymorphic_storage_operations_for{");
  const std::string table = tableBegin == std::string::npos
                                ? std::string{}
                                : artifact.contents.substr(tableBegin, 600);
  expect(!representation.empty() &&
             representation.find("alignas(Align) std::byte bytes_[Bytes]") !=
                 std::string::npos &&
             representation.find("operations_->relocate(") !=
                 std::string::npos &&
             representation.find("memcpy") == std::string::npos &&
             representation.find("operator new") == std::string::npos &&
             table.find("mir_object_relocate_v1(") != std::string::npos &&
             artifact.contents.find(
                 "::gti_internal::backend::polymorphic_storage_construct(") !=
                 std::string::npos &&
             artifact.contents.find(
                 "::gti_internal::backend::polymorphic_storage_read_mut(") !=
                 std::string::npos,
         "the C++ slot relocates through each payload's generated move and "
         "never byte-copies or allocates a polymorphic payload");
}

} // namespace

int main() {
  testPolymorphicStorageCppRepresentation();
  if (failures != 0) {
    std::cerr << failures
              << " C++ polymorphic storage representation test(s) failed\n";
    return 1;
  }
  std::cout << "C++ polymorphic storage representation tests passed\n";
  return 0;
}
