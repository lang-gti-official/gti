#include "gti/frontend.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>

namespace {
int failures = 0;

void check(const std::string &name, const std::string &source,
           const std::string &code = {}, const std::string &call = {}) {
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  const auto path = root / "raw-access-test.gti";
  lang::FrontendOptions options;
  options.rawPointerAudit = lang::RawPointerAuditMode::Strict;
  const auto result = lang::Frontend(options).analyze(
      path, source, {root / "stdlib/prelude.gti"}, {}, {root / "stdlib"});
  bool passed = result.canGenerateCode();
  if (!code.empty()) {
    const auto start = source.rfind(call);
    passed =
        !result.canGenerateCode() && start != std::string::npos &&
        std::any_of(
            result.diagnostics.begin(), result.diagnostics.end(),
            [&](const auto &diagnostic) {
              return diagnostic.code == code &&
                     diagnostic.phase == lang::DiagnosticPhase::Semantics &&
                     diagnostic.severity == lang::DiagnosticSeverity::Error &&
                     diagnostic.primary.source == path.string() &&
                     diagnostic.primary.start >= start &&
                     diagnostic.primary.end <= start + call.size() &&
                     diagnostic.primary.start < diagnostic.primary.end;
            });
  }
  if (!passed) {
    ++failures;
    std::cerr << "FAIL: " << name << '\n';
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << " ["
                << diagnostic.primary.start << ", " << diagnostic.primary.end
                << "]\n";
  }
}

const std::string helpers = R"(
unsafe int read_address(const int* pointer) {
  unsafe { return *pointer; }
}
unsafe int read_forwarded(const int* pointer) {
  unsafe { return read_address(pointer); }
}
unsafe void write_address(int* pointer) {
  unsafe { *pointer = 9; }
}
unsafe void write_forwarded(int* pointer) {
  unsafe { write_address(pointer); }
}
const int* copy_address(const int* pointer) { return pointer; }
bool same_address(const int* left, const int* right) { return left == right; }
)";
} // namespace

int main() {
  for (const std::string read : {"read_address", "read_forwarded"}) {
    check("expired access through " + read, helpers + R"(
int main() {
  mut const int* pointer = nullptr;
  { int dead = 7; pointer = &dead; }
  unsafe { return )" + read + "(pointer); } }",
          "GTI-S2102", read + "(pointer)");
    check("read conflicts with retained exclusive loan through " + read,
          helpers + R"(
int main() {
  mut int owner = 7;
  const int* pointer = &owner;
  mut int& held = owner;
  unsafe { int observed = )" +
              read + R"((pointer); }
  held = 8;
  return held;
})",
          "GTI-S2102", read + "(pointer)");
    check("read coexists with shared loan through " + read, helpers + R"(
int main() {
  mut int owner = 7;
  const int* pointer = &owner;
  int& held = owner;
  unsafe { int observed = )" + read + R"((pointer); }
  return held;
})");
  }
  for (const std::string write : {"write_address", "write_forwarded"}) {
    check("write conflicts with retained shared loan through " + write,
          helpers + R"(
int main() {
  mut int owner = 7;
  int* pointer = &owner;
  int& held = owner;
  unsafe { )" +
              write + R"((pointer); }
  return held;
})",
          "GTI-S2102", write + "(pointer)");
    // Match ordinary same-block loan endpoints. Entering a nested block still
    // conservatively retains an outer loan in the existing NLL contract.
    check("write after checked loan's last use through " + write, helpers + R"(
int main() { unsafe {
  mut int owner = 7;
  int* pointer = &owner;
  int& held = owner;
  int observed = held;
  )" + write + R"((pointer);
  return owner - observed;
} })");
  }
  check("expired copy and equality helpers remain inert", helpers + R"(
int main() {
  mut const int* pointer = nullptr;
  { int dead = 7; pointer = &dead; }
  const int* copied = copy_address(pointer);
  if (same_address(pointer, copied)) { return 0; }
  return 1;
})");
  check("copy helper preserves expiry for a subsequent access", helpers + R"(
int main() {
  mut const int* pointer = nullptr;
  { int dead = 7; pointer = &dead; }
  const int* copied = copy_address(pointer);
  unsafe { return read_address(copied); }
})",
        "GTI-S2102", "read_address(copied)");
  check("ordinary user aggregate parameter retains raw access requirements",
        R"(
struct Address { mut const int* pointer = nullptr; };
unsafe int read_record(Address address) {
  unsafe { return *address.pointer; }
}
int main() {
  mut Address address{};
  { int dead = 7; address.pointer = &dead; }
  unsafe { return read_record(address); }
})",
        "GTI-S2102", "read_record(address)");
  check("constructor access retains raw argument expiry", R"(
struct Observed {
  mut int value = 0;
  Observed(const int* pointer) { unsafe { this.value = *pointer; } }
};
int main() {
  mut const int* pointer = nullptr;
  { int dead = 7; pointer = &dead; }
  Observed observed{pointer};
  return observed.value;
})",
        "GTI-S2102", "Observed observed{pointer}");
  check("receiver field access retains raw argument expiry", R"(
struct Address {
  mut const int* pointer = nullptr;
  int read() { unsafe { return *this.pointer; } }
};
int main() {
  mut Address address{};
  { int dead = 7; address.pointer = &dead; }
  return address.read();
})",
        "GTI-S2102", "address.read()");
  check("operator receiver retains raw access requirements", R"(
struct Address {
  mut const int* pointer = nullptr;
  int operator+(int other) { unsafe { return *this.pointer + other; } }
};
int main() {
  mut Address address{};
  { int dead = 7; address.pointer = &dead; }
  return address + 0;
})",
        "GTI-S2102", "address + 0");
  check("operator receiver may compare an expired address inertly", R"(
struct Address {
  mut const int* pointer = nullptr;
  int operator+(int other) {
    if (this.pointer != nullptr) { return other; }
    return 0;
  }
};
int main() {
  mut Address address{};
  { int dead = 7; address.pointer = &dead; }
  return address + 0;
})");
  check("call operator retains raw access requirements", R"(
struct Address {
  mut const int* pointer = nullptr;
  int operator()() { unsafe { return *this.pointer; } }
};
int main() {
  mut Address address{};
  { int dead = 7; address.pointer = &dead; }
  return address();
})",
        "GTI-S2102", "address()");
  check("contextual bool operator retains raw access requirements", R"(
struct Address {
  mut const int* pointer = nullptr;
  operator bool() { unsafe { return *this.pointer != 0; } }
};
int main() {
  mut Address address{};
  { int dead = 7; address.pointer = &dead; }
  if (address) { return 0; }
  return 1;
})",
        "GTI-S2102", "if (address)");
  check("inert captured raw address does not conflict with exclusive loan", R"(
int main() {
  mut int owner = 7;
  const int* pointer = &owner;
  auto compare = [pointer]() -> bool { return pointer != nullptr; };
  mut int& held = owner;
  bool nonnull = compare();
  held = 8;
  return nonnull ? held : 0;
})");
  check("captured raw read conflicts with exclusive loan", R"(
int main() {
  mut int owner = 7;
  const int* pointer = &owner;
  auto read = [pointer]() -> int { unsafe { return *pointer; } };
  mut int& held = owner;
  int observed = read();
  held = 8;
  return observed + held;
})",
        "GTI-S2102", "read()");
  check("operator result retains callee-local expiry", R"(
struct Maker {
  const int* operator+(int ignored) {
    int dead = 7;
    return &dead;
  }
};
int main() {
  Maker maker{};
  const int* pointer = maker + 0;
  unsafe { return *pointer; }
})",
        "GTI-S2102", "*pointer");
  check("operator write retains callee-local expiry", R"(
struct Address {
  mut const int* pointer = nullptr;
  void operator+=(int ignored) mut {
    int dead = 7;
    this.pointer = &dead;
  }
};
int main() {
  mut Address address{};
  address += 0;
  unsafe { return *address.pointer; }
})",
        "GTI-S2102", "*address.pointer");
  check("subscript operator preserves raw access requirements", R"(
struct Address {
  mut const int* pointer = nullptr;
  int operator[](int index) { unsafe { return this.pointer[index]; } }
};
int main() {
  mut Address address{};
  { int dead = 7; address.pointer = &dead; }
  return address[0];
})",
        "GTI-S2102", "address[0]");
  check("operator setter preserves live incoming origins", R"(
struct Address {
  mut const int* pointer = nullptr;
  void operator+=(const int* next) mut { this.pointer = next; }
};
int main() {
  mut Address address{};
  int owner = 7;
  address += &owner;
  unsafe { return *address.pointer; }
})");
  const std::string closedDispatch = R"(
interface Observer { bool inspect(const int* pointer); };
class Inert : public Observer {
public:
  bool inspect(const int* pointer) override { return pointer != nullptr; }
};
bool dispatch(Observer& observer, const int* pointer) {
  return observer.inspect(pointer);
}
)";
  const std::string dispatchMain = R"(
int main() {
  mut const int* pointer = nullptr;
  { int dead = 7; pointer = &dead; }
  Inert observer{};
  return dispatch(observer, pointer) ? 0 : 1;
})";
  check("closed inert dispatch family permits expired address transport",
        closedDispatch + dispatchMain);
  check("unselected hostile override prevents an inert family proof",
        closedDispatch + R"(
class Reading : public Observer {
public:
  bool inspect(const int* pointer) override {
    unsafe { return *pointer != 0; }
  }
};
)" + dispatchMain,
        "GTI-S2101", "dispatch(observer, pointer)");
  const std::string genericDispatch = R"(
interface Channel<T> { T inspect(T value); };
class Model<T> : public Channel<T> {
public:
  T inspect(T value) override { return value; }
};
)";
  check("exact local generic receiver permits inert expired transport",
        genericDispatch + R"(
int main() {
  mut const int* pointer = nullptr;
  { int dead = 7; pointer = &dead; }
  Model<const int*> model{};
  Channel<const int*>& base = model;
  return base.inspect(pointer) != nullptr ? 0 : 1;
})");
  check("open generic receiver family remains conservative",
        genericDispatch + R"(
const int* dispatch(Channel<const int*>& observer, const int* pointer) {
  return observer.inspect(pointer);
}
int main() {
  mut const int* pointer = nullptr;
  { int dead = 7; pointer = &dead; }
  Model<const int*> model{};
  return dispatch(model, pointer) != nullptr ? 0 : 1;
})",
        "GTI-S2101", "dispatch(model, pointer)");
  return failures == 0 ? 0 : 1;
}
