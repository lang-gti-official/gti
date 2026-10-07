#include "cpp_backend_test_support.h"
#include "gti/frontend.h"
#include "gti/mir_printer.h"
#include "gti/mir_verifier.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <string_view>

namespace {
int failures = 0;
void expect(bool condition, std::string_view message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}
auto analyze(std::string source) -> lang::FrontendResult {
  const auto stdlib =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  return lang::Frontend().analyze(
      "resource-client.gti",
      "#include <std/resource>\n#include <std/memory>\n" + source,
      {stdlib / "prelude.gti"}, {}, {stdlib});
}
void diagnostics(const lang::FrontendResult &result) {
  for (const auto &diagnostic : result.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  if (std::getenv("GTI_TEST_DUMP_MIR") != nullptr) {
    std::set<std::pair<lang::MirBodyKind, std::size_t>> printed;
    for (const auto &error : lang::verifyMirProgram(result.mir).errors) {
      std::cerr << "MIR body " << static_cast<int>(error.bodyKind) << ':'
                << error.owner << " instruction " << error.instruction << ": "
                << error.message << '\n';
      if (!printed.emplace(error.bodyKind, error.owner).second)
        continue;
      if (error.bodyKind == lang::MirBodyKind::Function) {
        if (const auto *function = result.mir.findFunctionInstance(error.owner))
          std::cerr << lang::MirPrinter().print(function->body);
      } else if (error.bodyKind == lang::MirBodyKind::Constructor) {
        if (const auto *constructor =
                result.mir.findConstructorInstance(error.owner))
          std::cerr << lang::MirPrinter().print(constructor->body);
      }
    }
  }
}
void positive(const std::string &source, std::string_view message) {
  const auto result = analyze(source);
  if (!result.canGenerateCode())
    diagnostics(result);
  expect(result.canGenerateCode() && lang::verifyMirProgram(result.mir).valid(),
         message);
  if (result.canGenerateCode()) {
    try {
      expect(!gti_test::emitCppText(result).empty(),
             "resource client emits through verified MIR");
    } catch (const std::exception &error) {
      std::cerr << error.what() << '\n';
      expect(false, "resource client emits through verified MIR");
    }
  }
}
void negative(const std::string &source, std::string_view code,
              std::string_view message) {
  const auto result = analyze(source);
  const bool matched = std::any_of(
      result.diagnostics.begin(), result.diagnostics.end(),
      [&](const auto &diagnostic) { return diagnostic.code == code; });
  if (result.canGenerateCode() || !matched)
    diagnostics(result);
  expect(!result.canGenerateCode() && matched, message);
  expect(std::none_of(result.diagnostics.begin(), result.diagnostics.end(),
                      [](const auto &diagnostic) {
                        return diagnostic.code == "GTI-B0001";
                      }),
         "resource contract errors are diagnosed before backend admission");
}
} // namespace

int main() {
  positive(R"(
int use(std::resource_ref<std::process_resource> resource) {
  mut std::resource_storage<int, std::process_resource> slots{resource};
  auto allocated = slots.try_allocate(std::size_t(2), std::alignment(std::size_t(64)));
  if (!allocated) { return 1; }
  slots.emplace(std::size_t(0), 42);
  auto grown = slots.try_reserve_prefix(std::size_t(8), std::size_t(1));
  if (!grown) { return 2; }
  if (slots.get(std::size_t(0)) != 42) { return 3; }
  slots.release();
  return 0;
}
int main() {
  std::process_resource resource{};
  unsafe { return use(std::bind_resource(resource)); }
}
)",
           "allocation, growth and cleanup are safe after one unsafe binding");

  positive(R"(
class Fixed {
  int value;
public:
  Fixed(int value) : value(value) {}
  Fixed(Fixed& other) = delete;
  Fixed(Fixed&& other) = delete;
  int read() { return this.value; }
};
int use(std::resource_ref<std::process_resource> resource) {
  mut std::resource_storage<Fixed, std::process_resource> slots{resource};
  auto allocated = slots.try_allocate(std::size_t(1));
  if (!allocated) { return 1; }
  slots.emplace(std::size_t(0), 42);
  auto moved = std::move(slots);
  return moved.get(std::size_t(0)).read() - 42;
}
int main() {
  std::process_resource resource{};
  unsafe { return use(std::bind_resource(resource)); }
}
)",
           "moving the resource owner does not move final-address elements");

  positive(
      R"(
struct Budget {
  unsafe expected<std::raw_allocation,std::alloc_errc> allocate(std::layout request) {
    return unexpected(std::alloc_errc::resource_exhausted);
  }
  unsafe void release(std::raw_allocation allocation) {}
};
int use(std::resource_ref<Budget> resource) {
  mut std::resource_storage<int,Budget> slots{resource};
  auto attempt = slots.try_allocate(std::size_t(1));
  return attempt ? 1 : 0;
}
int main() { Budget budget{}; unsafe { return use(std::bind_resource(budget)); } }
)",
      "unrelated source-defined providers use the same structural protocol");

  negative(R"(
int main() {
  std::process_resource resource{};
  auto bound = std::bind_resource(resource);
  return 0;
}
)",
           "GTI-S2055",
           "initial resource binding requires explicit unsafe authority");
  negative(R"(
int main() {
  std::process_resource resource{};
  auto raw = resource.allocate(std::layout(std::size_t(8), std::size_t(8)));
  return 0;
}
)",
           "GTI-S2055",
           "raw resource operations remain unsafe after protocol introduction");
  negative(R"(
int main() {
  std::process_resource resource{};
  std::resource_ref<std::process_resource> bound{
    gti_internal::resource_binding::trusted, resource};
  return 0;
}
)",
           "GTI-S2058",
           "applications cannot forge the source-defined resource seal");
  negative(R"(
struct Incomplete {};
int main() { Incomplete resource{}; unsafe { auto bound=std::bind_resource(resource); } return 0; }
)",
           "GTI-S2029",
           "binding rejects a provider without exact structural members");
  negative(R"(
struct Wrong {
  unsafe expected<std::raw_allocation,std::alloc_errc> allocate(std::layout request) mut {
    return unexpected(std::alloc_errc::resource_exhausted);
  }
  unsafe void release(std::raw_allocation allocation) {}
};
int main() { mut Wrong resource{}; unsafe { auto bound=std::bind_resource(resource); } return 0; }
)",
           "GTI-S2029",
           "a mutable provider receiver does not satisfy the shared protocol");
  negative(R"(
int use(std::resource_ref<std::process_resource> resource) {
  mut std::resource_storage<int,std::process_resource> slots{resource};
  slots.try_allocate(std::size_t(1)).value();
  slots.emplace(std::size_t(0),42);
  int& element = slots.get(std::size_t(0));
  slots.release();
  return element;
}
int main() { return 0; }
)",
           "GTI-S2017",
           "a live element borrow prevents release of its backing region");
  for (const auto &operation :
       {std::string("slots.clear();"),
        std::string("slots.destroy(std::size_t(0));"),
        std::string("slots.try_reserve_prefix(std::size_t(8), "
                    "std::size_t(1)).value();"),
        std::string("auto moved = std::move(slots);")}) {
    negative(R"(
int use(std::resource_ref<std::process_resource> resource) {
  mut std::resource_storage<int,std::process_resource> slots{resource};
  slots.try_allocate(std::size_t(1)).value();
  slots.emplace(std::size_t(0),42);
  int& element = slots.get(std::size_t(0));
)" + operation + R"(
  return element;
}
int main() { return 0; }
)",
             "GTI-S2017",
             "a live element borrow prevents invalidation: " + operation);
  }
  for (const auto &operation :
       {std::string("values.release();"), std::string("values.clear();"),
        std::string("values.pop_back();"),
        std::string("values.try_reserve(std::size_t(8)).value();"),
        std::string("auto moved = std::move(values);")}) {
    for (const bool iterator : {false, true}) {
      negative(
          R"(
#include <std/resource_vector>
int use(std::resource_ref<std::process_resource> resource) {
  mut std::resource_vector<int,std::process_resource> values{resource};
  values.emplace_back(42);
)" +
              std::string(
                  iterator ? "  auto cursor = values.begin();\n"
                           : "  int& element = values.get(std::size_t(0));\n") +
              operation +
              (iterator ? "\n  return *cursor;\n" : "\n  return element;\n") +
              R"(
}
int main() { return 0; }
)",
          "GTI-S2017",
          "a live vector " + std::string(iterator ? "iterator" : "element") +
              " prevents owner invalidation: " + operation);
    }
  }
  negative(R"(
struct Resource {
  unsafe expected<std::raw_allocation,std::alloc_errc> allocate(std::layout request) {
    return unexpected(std::alloc_errc::resource_exhausted);
  }
  unsafe void release(std::raw_allocation allocation) {}
  void reset() mut {}
};
int main() {
  mut Resource resource{};
  unsafe {
    auto binding = std::bind_resource(resource);
    std::resource_storage<int,Resource> retained{binding};
    resource.reset();
  }
  return 0;
}
)",
           "GTI-S2017",
           "a cleanup-bound lease prevents reset even without a later explicit "
           "read");
  negative(R"(
int use(std::resource_ref<std::process_resource> first,
        std::resource_ref<std::process_resource> second) {
  mut std::resource_storage<int,std::process_resource> left{first};
  std::resource_storage<int,std::process_resource> right{second};
  left = std::move(right);
  return 0;
}
int main() { return 0; }
)",
           "GTI-S2003",
           "assignment cannot silently replace the retained resource binding");
  negative(R"(
std::resource_ref<std::process_resource> escape() {
  std::process_resource local{};
  unsafe { return std::bind_resource(local); }
}
int main() { return 0; }
)",
           "GTI-S2045",
           "a resource binding cannot outlive its checked resource");

  const std::string resettable = R"(
#include <std/resource_vector>
struct Resource {
  unsafe expected<std::raw_allocation,std::alloc_errc> allocate(std::layout request) {
    return unexpected(std::alloc_errc::resource_exhausted);
  }
  unsafe void release(std::raw_allocation allocation) {}
  void reset() mut {}
};
class Fixed {
  int value;
public:
  Fixed(int value) : value(value) {}
  Fixed(Fixed& other) = delete;
  Fixed(Fixed&& other) = delete;
};
)";
  negative(resettable + R"(
int use(std::resource_ref<Resource> resource) {
  resource.get().reset();
  return 0;
}
int main() { return 0; }
)",
           "GTI-S2000",
           "a shared resource binding cannot reach the provider's exclusive "
           "reset");
  negative(resettable + R"(
int main() {
  mut Resource resource{};
  unsafe {
    auto binding = std::bind_resource(resource);
    mut std::resource_vector<int,Resource> values{binding};
    resource.reset();
    values.try_reserve(std::size_t(1)).value();
  }
  return 0;
}
)",
           "GTI-S2017", "a live growable container prevents provider reset");
  negative(resettable + R"(
int main() {
  mut Resource resource{};
  unsafe {
    auto binding = std::bind_resource(resource);
    std::resource_storage<int,Resource> retained{binding};
    auto moved = std::move(resource);
  }
  return 0;
}
)",
           "GTI-S2017", "a live lease prevents moving its provider");
  for (const auto &operation :
       {std::string("values.push_back(7);"),
        std::string("values.emplace_back(7);"),
        std::string("values.try_push_back(7).value();"),
        std::string("mut int& slot = values.get_mut(std::size_t(0));\n"
                    "  slot = 9;")}) {
    negative(R"(
#include <std/resource_vector>
int use(std::resource_ref<std::process_resource> resource) {
  mut std::resource_vector<int,std::process_resource> values{resource};
  values.emplace_back(42);
  int& element = values.get(std::size_t(0));
  )" + operation +
                 R"(
  return element;
}
int main() { return 0; }
)",
             "GTI-S2017",
             "a live vector element prevents growth or mutable access: " +
                 operation);
  }
  negative(R"(
#include <std/resource_vector>
int& leak(std::resource_ref<std::process_resource> resource) {
  mut std::resource_vector<int,std::process_resource> values{resource};
  values.emplace_back(42);
  return values.get(std::size_t(0));
}
int main() { return 0; }
)",
           "GTI-S2017",
           "an element reference cannot escape its local container");
  negative(resettable + R"(
int use(std::resource_ref<Resource> resource) {
  std::resource_vector<Fixed,Resource> values{resource};
  return 0;
}
int main() { return 0; }
)",
           "GTI-S2029",
           "a growable container rejects elements it would need to relocate");
  for (const auto &operation :
       {std::string("left.relocate_to(right, std::size_t(0));"),
        std::string("left.try_reserve_prefix(std::size_t(4), "
                    "std::size_t(0)).value();")}) {
    negative(
        resettable + R"(
int use(std::resource_ref<Resource> resource) {
  mut std::resource_storage<Fixed,Resource> left{resource};
  mut std::resource_storage<Fixed,Resource> right{resource};
  )" + operation +
            R"(
  return 0;
}
int main() { return 0; }
)",
        "GTI-S2029",
        "fixed-address storage rejects relocation of immovable elements: " +
            operation);
  }

  if (failures == 0)
    std::cout << "resource binding and typed lease contracts passed\n";
  return failures == 0 ? 0 : 1;
}
