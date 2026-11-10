// Frozen C++-backend representation checks moved out of the ownership
// compiler suite (compiler_ownership_tests.cpp). Each case asserts generated
// C++ text: type and member spellings, special-member syntax, failure-ABI
// helpers, preamble helpers, mangled names, or C++ standard selection. None
// of that defines GTI semantics; the language behaviour these programs
// exercise is asserted on MIR (and LLVM native emission) in the ownership
// suite.

#include "compiler_test_fixture.h"
#include "cpp_backend_test_support.h"
#include "private_stdlib_fixture.h"

namespace {

bool hasMirIntrinsicKind(const lang::FrontendResult &frontend,
                         lang::IntrinsicKind kind) {
  for (const auto &function : frontend.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.intrinsic == kind)
          return true;
  return false;
}

lang::FrontendResult analyzeWithMemory(std::string_view name,
                                       std::string source) {
  source.insert(0, "import std.memory;\n");
  return lang::Frontend().analyze(std::filesystem::temp_directory_path() /
                                      std::string(name),
                                  std::move(source), {standardLibraryPrelude()},
                                  {}, {standardLibraryRoot()});
}

lang::FrontendResult analyzeSourceStorageFixture(std::string_view name,
                                                 std::string source) {
  source.insert(0, R"(
std::storage<T> fixture_storage<T>(uint64_t count) requires std::storage_element<T> {
  std::allocator allocator{};
  auto allocated = allocator.try_storage<T>(count);
  return std::move(allocated).expect("test storage allocation");
}
)");
  return analyzeWithMemory(name, std::move(source));
}

void testExplicitValueMovesCppEmission() {
  const lang::FrontendResult valid = lang::Frontend().analyze(
      "explicit-value-moves.gti", R"(
struct Record {
  int value = 4;
public:
  Record() = default;
};

int forward_int(int value) { return std::move(value); }
T transfer<T>(T value) { return std::move(value); }

int main() {
  int source = 1;
  int moved = std::move(source);
  int forwarded = forward_int(std::move(moved));

  mut int reusable = 2;
  int first = std::move(reusable);
  reusable = 3;
  int generic = transfer(std::move(first));

  Record record = Record();
  Record moved_record = std::move(record);
  return reusable + forwarded + generic + moved_record.value;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(valid.canGenerateCode(), "explicit-move fixture should analyze");
  if (!valid.canGenerateCode())
    return;
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(valid.hir, lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(valid, valid.mir, valid.mir, optimizations);
  expect(
      artifact.contents.find("const std::int32_t value") == std::string::npos &&
          artifact.contents.find("const std::int32_t source") ==
              std::string::npos &&
          artifact.contents.find("const Record record") == std::string::npos &&
          artifact.contents.find(
              "// GTI verified-MIR body: scalar-cfg-failure-v1 "
              "function-instance") != std::string::npos &&
          artifact.contents.find("__gti_mir_v_1 = std::move(__gti_mir_p_1);") !=
              std::string::npos &&
          artifact.contents.find("__gti_entry__gti_mir_failure(") !=
              std::string::npos,
      "the C++ backend should keep explicitly consumed immutable bindings "
      "physically movable");
}

void testNonNullReferencesCppEmission() {
  lang::FrontendResult frontend =
      lang::Frontend().analyze("references.gti", nonNullReferenceSource());
  expect(frontend.canGenerateCode(),
         "non-null read-only and mutable references should validate");
  if (!frontend.canGenerateCode())
    return;
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(artifact.contents.find(
             "const ::__gti_program::Counter & __gti_mir_arg_0") !=
                 std::string::npos &&
             artifact.contents.find(
                 "::__gti_program::Counter & __gti_mir_arg_0") !=
                 std::string::npos &&
             artifact.contents.find("std::int32_t *__gti_mir_p_") !=
                 std::string::npos,
         "references should preserve const access in MIR signatures and use "
         "pointer carriers for retained local loans");
}

void testGlobalBorrowReturnsCppEmission() {
  const lang::FrontendResult valid =
      lang::Frontend().analyze("global-borrow-returns.gti", R"(
class LayerStack {
public:
  void Update() mut {}
public:
  LayerStack() = default;
};

class Application {
public:
  static mut Application& Get() {
    unsafe {
      return *Application::app;
    }
  }

  mut LayerStack& GetLayerStack() mut { return this.layers; }

private:
  mut LayerStack layers = LayerStack();
  static mut Application* app = nullptr;
public:
  Application() = default;
};

mut int global_value = 1;
mut int& global_value_ref() { return global_value; }

void update_application() {
  Application::Get().GetLayerStack().Update();
  Application::Get().GetLayerStack().Update();
}

void update_application_in_scopes() {
  {
    mut Application& first = Application::Get();
    first.GetLayerStack().Update();
  }
  mut Application& second = Application::Get();
  second.GetLayerStack().Update();
}

int main() {
  mut int& value = global_value_ref();
  value += 1;
  return value - 2;
}
)");
  expect(valid.canGenerateCode(), "global borrow fixture should analyze");
  if (!valid.canGenerateCode())
    return;
  const lang::ClassDecl *application =
      findTopLevelClass(valid.program, "Application");
  const lang::FunctionDecl *get = nullptr;
  if (application != nullptr) {
    for (const lang::StmtPtr &member : application->members()) {
      const auto *function =
          dynamic_cast<const lang::FunctionDecl *>(member.get());
      if (function != nullptr && function->name().lexeme == "Get") {
        get = function;
        break;
      }
    }
  }
  const lang::FunctionInfo *getInfo =
      get == nullptr ? nullptr : valid.semantics.findFunction(*get);
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(valid.hir, lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(valid, valid.mir, valid.mir, optimizations);
  const std::string emittedGet =
      getInfo == nullptr ? std::string{}
                         : "static ::__gti_program::Application &__gti_fn_" +
                               std::to_string(getInfo->id) + "_Get()";
  const std::string emittedFailureGet = getInfo == nullptr
                                            ? std::string{}
                                            : "static bool __gti_fn_" +
                                                  std::to_string(getInfo->id) +
                                                  "_Get__gti_mir_failure(";
  // Reading the static pointer cannot fail, so the accessor has only its
  // plain form, which returns the loaned referent.
  const std::size_t getBody =
      getInfo == nullptr
          ? std::string::npos
          : artifact.contents.find("Application::__gti_fn_" +
                                   std::to_string(getInfo->id) + "_Get() {");
  const std::size_t getReturn =
      getBody == std::string::npos
          ? std::string::npos
          : artifact.contents.find("return *__gti_mir_loan_1;", getBody);
  expect(!emittedGet.empty() && !emittedFailureGet.empty() &&
             artifact.contents.find(emittedGet) != std::string::npos &&
             artifact.contents.find(emittedFailureGet) == std::string::npos &&
             artifact.contents.find("::__gti_program::Application::app;") !=
                 std::string::npos &&
             getReturn != std::string::npos &&
             getReturn < artifact.contents.find("\n  }", getBody),
         "the C++ backend should emit the static mutable-reference accessor "
         "without exposing unsafe syntax at its call sites");
}

void testPrefixStorageCapabilityCppEmission() {
  const auto valid =
      gti_test::analyzePrivateMemoryFixture("source-prefix-owner.gti", R"(
concept prefix_fixture_element<T> = std::storage_element<T> && std::relocatable<T>;
class Prefixed<prefix_fixture_element T> {
  mut std::detail::prefix_owner<T> data;
public:
  Prefixed(uint64_t capacity) : data(std::detail::prefix_owner<T>(capacity)) {}
  void push(T value) mut { this.data.append_value(std::move(value)); }
  void pop() mut { this.data.pop(); }
  T& at(uint64_t index) { return this.data.read(index); }
  mut T& at_mut(uint64_t index) mut { return this.data.read_mut(index); }
  uint64_t length() { return this.data.length(); }
  void take_from(mut Prefixed<T>& source) mut { source.data.relocate_to(this.data); }
};
int main() {
  mut Prefixed<int> values{uint64_t(4)};
  values.push(7);
  values.push(9);
  if (values.length() != uint64_t(2) or values.at(uint64_t(0)) != 7) return 1;
  mut int& slot = values.at_mut(uint64_t(1));
  slot = 11;
  values.pop();
  mut Prefixed<int> moved{uint64_t(4)};
  moved.take_from(values);
  return moved.length() == uint64_t(1) && moved.at(uint64_t(0)) == 7 ? 0 : 2;
}
)");
  expect(valid.canGenerateCode(), "ordinary source prefix owner must pass");
  if (valid.canGenerateCode()) {
    const auto artifact = gti_test::emitCppText(valid);
    expect(concreteBackendSymbols(artifact, "__gti_type_", "Prefixed").size() ==
                   1 &&
               artifact.find("construct_representation_at(") !=
                   std::string::npos &&
               artifact.find("mir_object_relocate_v1(") != std::string::npos &&
               artifact.find("class prefix_storage") == std::string::npos &&
               artifact.find("mir_prefix_") == std::string::npos,
           "source prefix policy emits ordinary classes and general "
           "lifetime/borrow operations");
  }
}

void testReceiverTiedReferenceReturnsCppEmission() {
  lang::FrontendResult frontend = gti_test::analyzeTrustedPrivateMemoryFixture(
      "receiver-tied-references.gti", R"(
class Box<T> {
  T value;

public:
  Box(T initial) : value(initial) {}

  T& get() {
    return this.value;
  }
};

class Buffer<std::storage_element T> {
  mut std::detail::prefix_owner<T> data;

public:
  Buffer(uint64_t capacity)
      : data(std::detail::prefix_owner<T>(capacity)) {}

  void push(T value) mut {
    this.data.append_value(std::move(value));
  }

  T& at(uint64_t index) {
    return this.data.read(index);
  }
};

int main() {
  Box<int> box = Box<int>(7);
  int& boxed = box.get();
  mut Buffer<int> buffer = Buffer<int>(uint64_t(1));
  buffer.push(9);
  int& stored = buffer.at(uint64_t(0));
  return boxed + stored - 16;
}
)");
  expect(frontend.canGenerateCode(),
         "methods should return read-only references borrowed from 'this'");
  if (!frontend.canGenerateCode())
    return;
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  // Box<int>::get() cannot fail: its plain form returns the exact loaned
  // referent. The checked prefix read keeps its failure-form loan output.
  const std::size_t plainGet = artifact.contents.find("_get() const {");
  expect(
      plainGet != std::string::npos &&
          artifact.contents.find("return *__gti_mir_loan_1;", plainGet) <
              artifact.contents.find("\n  }", plainGet) &&
          concreteBackendSymbols(artifact.contents, "__gti_member_", "get")
                  .size() == 1 &&
          artifact.contents.find("const std::int32_t *__gti_mir_loan_1{};") !=
              std::string::npos &&
          artifact.contents.find("const std::int32_t *__gti_mir_loan_2{};") !=
              std::string::npos &&
          artifact.contents.find("__gti_mir_loan_2 = __gti_mir_loan_1;") !=
              std::string::npos &&
          artifact.contents.find("*__gti_mir_out_result = __gti_mir_loan_2;") !=
              std::string::npos &&
          hasMirIntrinsicKind(frontend, lang::IntrinsicKind::RawBorrow) &&
          artifact.contents.find("mir_prefix_read_v1") == std::string::npos,
      "receiver-tied borrows should lower through exact const loan carriers "
      "and publish the returned loan without copying its referent");

  const lang::FrontendResult nested =
      gti_test::analyzeTrustedPrivateMemoryFixture(
          "nested-receiver-reference.gti", R"(
class ElementBuffer {
  mut std::detail::prefix_owner<int> data;

public:
  ElementBuffer()
      : data(std::detail::prefix_owner<int>(uint64_t(1))) {}

  void set(int value) mut {
    this.data.append_value(std::move(value));
  }

  int& at() {
    return this.data.read(uint64_t(0));
  }
};

class BufferFacade {
  mut ElementBuffer buffer;

public:
  BufferFacade() : buffer(ElementBuffer()) {}

  void set(int value) mut {
    this.buffer.set(value);
  }

  int& at() {
    return this.buffer.at();
  }
};

int main() {
  mut BufferFacade facade = BufferFacade();
  facade.set(42);
  return facade.at() - 42;
}
)");
  const lang::MirLoan *callResultLoan = nullptr;
  const lang::MirLoan *nestedReturnLoan = nullptr;
  for (const lang::MirFunctionInstance &function :
       nested.mir.functionInstances()) {
    for (const lang::MirLoan &loan : function.body.loans) {
      if (loan.kind != lang::MirLoanKind::Return || !loan.escapes) {
        continue;
      }
      for (const lang::MirBlock &block : function.body.blocks) {
        for (const lang::MirInstruction &instruction : block.instructions) {
          if (instruction.kind != lang::MirInstructionKind::Borrow ||
              instruction.loan != loan.id || instruction.operands.size() != 1 ||
              instruction.operands.front().kind != lang::MirOperandKind::Loan) {
            continue;
          }
          const lang::MirLoan *candidate =
              function.body.findLoan(instruction.operands.front().loan);
          if (candidate != nullptr &&
              candidate->kind == lang::MirLoanKind::CallResult) {
            callResultLoan = candidate;
            nestedReturnLoan = &loan;
          }
        }
      }
    }
  }
  expect(nested.canGenerateCode() && callResultLoan != nullptr &&
             nestedReturnLoan != nullptr,
         "nested receiver fixture should transfer a call-result loan");
  if (callResultLoan != nullptr && nestedReturnLoan != nullptr) {
    const lang::OptimizationResult nestedOptimizations =
        lang::OptimizationPipeline().run(nested.hir,
                                         lang::OptimizationLevel::O0);
    const lang::BackendArtifact nestedArtifact =
        gti_test::emitCpp(nested, nested.mir, nested.mir, nestedOptimizations);
    const std::string transferText =
        "__gti_mir_loan_" + std::to_string(nestedReturnLoan->id) +
        " = __gti_mir_loan_" + std::to_string(callResultLoan->id) + ";";
    expect(nestedArtifact.contents.find(transferText) != std::string::npos,
           "the C++ backend should emit the verified call-result pointer "
           "transfer instead of re-borrowing the wrapper object");
  }
}

void testArgumentTiedReferenceReturnsCppEmission() {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("argument-tied-reference-returns.gti", R"(
class Box {
  mut int32_t value;

public:
  Box(int32_t initial) : value(initial) {}
  int32_t& observe() { return this.value; }
  int32_t read() { return this.value; }
};

int32_t& pick(int32_t& candidate) { return candidate; }

int32_t& forward(int32_t& candidate) { return pick(candidate); }

int32_t retain_argument_return() {
  mut int32_t local = 7;
  int32_t& chosen = pick(local);
  return chosen;
}

int32_t retain_forwarded_return() {
  mut int32_t local = 11;
  int32_t& chosen = forward(local);
  return chosen;
}

int32_t retain_call_result_argument() {
  mut Box box = Box(13);
  int32_t& chosen = pick(box.observe());
  return chosen;
}

int main() {
  return retain_argument_return() + retain_forwarded_return() +
         retain_call_result_argument() - 31;
}
)");
  expect(frontend.canGenerateCode(),
         "argument-tied reference fixture should analyze");
  if (!frontend.canGenerateCode())
    return;
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  // The regression the runtime test catches: without an instruction that
  // names it, the emitted callee published a default-initialized pointer.
  expect(
      !artifact.contents.empty() &&
          artifact.contents.find("auto *__gti_mir_loan_1 = __gti_mir_p_1;") !=
              std::string::npos &&
          artifact.contents.find("__gti_mir_loan_2 = __gti_mir_loan_1;") !=
              std::string::npos,
      "the C++ backend should alias the reference parameter's carrier and "
      "publish it through the returned loan");
}

void testStoredReferenceGroundworkCppEmission() {
  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "stored-references.gti", R"(
struct SingleSentinel { public: SingleSentinel() = default; };

class BorrowingIterator<T> {
  T& value;
  mut uint64_t position = 0;

public:
  BorrowingIterator(T& source) : value(source) {}

  T& operator*() { return value; }
  void operator++() mut { this.position++; }
  bool operator!=(BorrowingIterator<T>& other) {
    return this.position != other.position;
  }
  bool operator!=(SingleSentinel& sentinel) { return this.position == 0; }
};

class SingleRange<T> {
  mut T value;

public:
  SingleRange(T initial) : value(initial) {}

  BorrowingIterator<T> begin() {
    return BorrowingIterator<T>(value);
  }

  SingleSentinel end() { return SingleSentinel(); }

  void replace(T next) mut { this.value = next; }
};

class WrappedRange<T> {
  SingleRange<T>& internal;

public:
  WrappedRange(SingleRange<T>& source) : internal(source) {}

  BorrowingIterator<T> begin() {
    return internal.begin();
  }
};

int main() {
  mut SingleRange<int> range = SingleRange<int>(7);
  mut BorrowingIterator<int> iterator = range.begin();
  int& value = *iterator;
  ++iterator;
  mut BorrowingIterator<int> moved = std::move(iterator);
  ++moved;
  mut BorrowingIterator<int> copied = moved;
  ++copied;
  if (!(moved != copied)) { return 1; }
  mut int total = 0;
  for (int& item : range) { total += item; }
  mut SingleRange<int> wrapped_range = SingleRange<int>(7);
  WrappedRange<int> wrapped = WrappedRange<int>(wrapped_range);
  mut BorrowingIterator<int> wrapped_iterator = wrapped.begin();
  int& wrapped_value = *wrapped_iterator;
  return value + total + wrapped_value - 21;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(frontend.canGenerateCode(), "stored-reference fixture should analyze");
  if (!frontend.canGenerateCode())
    return;
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const std::unordered_set<std::string> iteratorTypes = concreteBackendSymbols(
      artifact.contents, "__gti_type_", "BorrowingIterator");
  const bool exactIteratorSpecialMembers =
      !iteratorTypes.empty() &&
      std::all_of(iteratorTypes.begin(), iteratorTypes.end(),
                  [&](const std::string &name) {
                    return artifact.contents.find(
                               "const std::int32_t & value") !=
                               std::string::npos &&
                           artifact.contents.find(name + "(const " + name +
                                                  " &other) = default") !=
                               std::string::npos &&
                           artifact.contents.find(name + "(" + name +
                                                  " &&) = default") !=
                               std::string::npos &&
                           artifact.contents.find("operator=(const " + name +
                                                  " &other) = delete") !=
                               std::string::npos;
                  });
  expect(exactIteratorSpecialMembers,
         "the C++ backend should preserve read-only storage and frontend "
         "special-member decisions on each concrete iterator application");
}

void testUniqueOwnershipAndAllocationCppEmission() {
  lang::FrontendResult frontend = analyzeWithMemory("unique-ownership.gti", R"(
struct Widget {
public:
  mut int value = 0;

  Widget(int initial) : value(initial) {}
  int read() { return this.value; }
  void increment() mut { this.value += 1; }
public:
  Widget() = default;
};

struct Holder {
  std::unique_ptr<Widget> widget = std::unique_ptr<Widget>();
public:
  Holder() = default;
};

int inspect(Widget& widget) { return widget.read(); }

std::unique_ptr<Widget> create(int value) {
  std::unique_ptr<Widget> widget = std::make_unique<Widget>(value);
  return std::move(widget);
}

int main() {
  mut std::unique_ptr<Widget> widget = create(4);
  expected<std::unique_ptr<Widget>, int> opened = create(6);
  Holder holder = Holder();
  std::unique_ptr<Widget> owners[1] = {std::make_unique<Widget>(9)};
  widget->increment();
  if (widget and widget != nullptr and inspect(*widget) == 5 and
      opened.has_value()) {
    return 0;
  }
  std::unique_ptr<Widget>& owner_reference = widget;
  return 1;
}
)");
  expect(frontend.canGenerateCode(), "unique allocation, checked access, and "
                                     "explicit transfer should validate");
  if (!frontend.canGenerateCode())
    return;
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(artifact.contents.find("class unique_ptr") != std::string::npos &&
             artifact.contents.find("std::unique_ptr<T> owner;") !=
                 std::string::npos &&
             artifact.contents.find("std::unique_ptr<T> owner = nullptr") ==
                 std::string::npos &&
             artifact.contents.find(
                 "__gti_std::unique_ptr<::__gti_program::Widget>") !=
                 std::string::npos,
         "the nominal unique_ptr wrapper should initialize its empty owner "
         "through one selected schedule, not a duplicate native default");
  expect(
      artifact.contents.find(
          "::gti_internal::backend::make_unique<::__gti_program::Widget>(") ==
              std::string::npos &&
          artifact.contents.find(
              "mir_object_allocation_v1<::__gti_program::Widget>") !=
              std::string::npos &&
          artifact.contents.find("__gti_std::__gti_fn_") != std::string::npos &&
          // The migrated caller emits from verified MIR and selects the
          // concrete transformed factory sibling. Its out-result type carries
          // the otherwise non-deducible owner argument.
          artifact.contents.find("_make_unique__gti_mir_failure(") !=
              std::string::npos &&
          artifact.contents.find("GTI_FAILURE_CODE_EMPTY_OWNER_ACCESS_V1") !=
              std::string::npos &&
          artifact.contents.find(
              "::gti_internal::backend::owner_access((*this).owner)") ==
              std::string::npos &&
          artifact.contents.find("unique_owner_is_null") != std::string::npos &&
          artifact.contents.find("unique_owner_has_value") == std::string::npos,
      "the public factory should remain ordinary GTI while its concrete "
      "verified-MIR sibling lowers only narrow owner capabilities");
  expect(
      // The factory's verified-MIR sibling publishes the owner by move
      // into the caller's receiving local.
      artifact.contents.find(
          "construct_representation_at(__gti_mir_out_result, "
          "std::move(__gti_mir_p_2.get()))") != std::string::npos &&
          artifact.contents.find(
              "unique_ptr(const unique_ptr &other) = delete") !=
              std::string::npos &&
          artifact.contents.find("unique_ptr(unique_ptr &&other) noexcept : "
                                 "owner(std::move(other.owner)) {\n      }") !=
              std::string::npos &&
          artifact.contents.find("unique_ptr() noexcept {}") !=
              std::string::npos &&
          artifact.contents.find("~unique_ptr() noexcept {}") !=
              std::string::npos &&
          artifact.contents.find(
              "const __gti_std::unique_ptr<Widget> widget =") ==
              std::string::npos,
      "nominal unique owners should remain move-only and physically movable");
}

void testUniqueOwnerNullStateCppEmission() {
  const lang::FrontendResult nullState =
      analyzeWithMemory("unique-owner-null-state.gti", R"(
struct Widget {
public:
  Widget() {}
};

class Nullable {
  mut bool empty = false;

public:
  Nullable(nullptr_t value) : empty(true) {}
  void operator=(nullptr_t value) mut { this.empty = true; }
  bool is_empty() { return this.empty; }
public:
  Nullable() = default;
};

void consume(std::unique_ptr<Widget> value) {}

int main() {
  mut Nullable nullable = nullptr;
  nullable = nullptr;
  if (!nullable.is_empty()) { return 1; }

  mut std::unique_ptr<Widget> owner = nullptr;
  owner.reset();
  owner = nullptr;
  owner = std::make_unique<Widget>();
  owner = nullptr;
  return 0;
}
)");
  expect(nullState.canGenerateCode(), "null-state fixture should analyze");
  if (!nullState.canGenerateCode())
    return;
  const lang::OptimizationResult nullOptimizations =
      lang::OptimizationPipeline().run(nullState.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact nullArtifact = gti_test::emitCpp(
      nullState, nullState.mir, nullState.mir, nullOptimizations);
  expect(
      nullArtifact.contents.find(
          "explicit unique_ptr(const std::nullptr_t empty);") !=
              std::string::npos &&
          nullArtifact.contents.find(
              "unique_ptr<::__gti_program::Widget>::unique_ptr(std::nullptr_t "
              "__gti_mir_arg_0) : owner(::gti_internal::backend::"
              "mir_empty_state_v1<std::remove_cv_t<decltype(owner)>>()) {") !=
              std::string::npos &&
          // Resetting to null cannot fail for this owner, so the selected
          // assignment and its reset call keep their exact plain identities.
          nullArtifact.contents.find(
              "unique_ptr<::__gti_program::Widget>::operator=(std::nullptr_t "
              "__gti_mir_arg_0)") != std::string::npos &&
          containsMangledFunction(nullArtifact.contents, "::", "reset", "();"),
      "the C++ backend should retain source-defined null construction and "
      "assignment identities without a public-type special case");
}

void testUniqueOwnerSwapCppEmission() {
  const lang::FrontendResult frontend =
      analyzeWithMemory("unique-owner-swap.gti", R"(
struct Value {
  int32_t number;
  Value(int32_t initial) : number(initial) {}
  int32_t read() { return this.number; }
};

int main() {
  mut std::unique_ptr<Value> first = std::make_unique<Value>(1);
  mut std::unique_ptr<Value> second = std::make_unique<Value>(2);
  first.swap(second);
  return first->read() == 2 and second->read() == 1 ? 0 : 1;
}
)");
  expect(frontend.canGenerateCode(),
         "unique-owner swap fixture should analyze");
  if (!frontend.canGenerateCode())
    return;
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir,
                        lang::OptimizationPipeline().run(
                            frontend.hir, lang::OptimizationLevel::O0));
  expect(artifact.contents.find(".swap(") != std::string::npos,
         "the backend should lower unique-owner swap to native unique_ptr "
         "swap without replacement cleanup");
}

void testPolymorphicUniqueOwnerUpcastCppEmission() {
  const lang::FrontendResult frontend =
      analyzeWithMemory("polymorphic-unique-owner-upcast.gti", R"(
[[transfer, share]] interface Layer {
  uint64_t id();
  void update() mut;
};

class DemoLayer : public Layer {
  mut uint64_t updates = 0;

public:
  DemoLayer() {}
  uint64_t id() override { return 7; }
  void update() mut override { this.updates++; }
};

class OtherLayer : public Layer {
  mut uint64_t updates = 0;

public:
  OtherLayer() {}
  uint64_t id() override { return 8; }
  void update() mut override { this.updates++; }
};

int main() {
  mut std::unique_ptr<Layer> layer =
      std::upcast_unique<Layer, DemoLayer>(
          std::make_unique<DemoLayer>());
  mut std::unique_ptr<Layer> other =
      std::upcast_unique<Layer, OtherLayer>(
          std::make_unique<OtherLayer>());
  layer->update();
  other->update();
  return layer->id() == 7 and other->id() == 8 ? 0 : 1;
}
)");
  expect(frontend.canGenerateCode(),
         "unique-owner upcast fixture should analyze");
  if (!frontend.canGenerateCode())
    return;
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir,
                        lang::OptimizationPipeline().run(
                            frontend.hir, lang::OptimizationLevel::O0));
  const std::unordered_set<std::string> upcastMembers =
      concreteBackendSymbols(artifact.contents, "__gti_member_", "upcast");
  const auto hasSelectedNativeOwnerSpecialization =
      [&](std::string_view owner) {
        const std::string prefix =
            "template <> bool "
            "__gti_program::__gti_std::unique_ptr<::__gti_program::" +
            std::string(owner) + ">::";
        const std::size_t declaration = artifact.contents.find(prefix);
        if (declaration == std::string::npos) {
          return false;
        }
        const auto member =
            std::find_if(upcastMembers.begin(), upcastMembers.end(),
                         [&](const std::string &candidate) {
                           return artifact.contents.find(
                                      prefix + candidate + "__gti_mir_failure(",
                                      declaration) == declaration;
                         });
        if (member == upcastMembers.end()) {
          return false;
        }
        const std::string specialization =
            prefix + *member + "__gti_mir_failure(";
        return artifact.contents.find(specialization,
                                      declaration + specialization.size()) !=
               std::string::npos;
      };
  expect(artifact.contents.find("unique_owner_upcast<") != std::string::npos &&
             artifact.contents.find("virtual ~Layer() noexcept = default;") !=
                 std::string::npos &&
             upcastMembers.size() == 2 &&
             hasSelectedNativeOwnerSpecialization("DemoLayer") &&
             hasSelectedNativeOwnerSpecialization("OtherLayer"),
         "the backend should lower the private owner conversion and preserve "
         "polymorphic destruction while declaring a selected method on its "
         "native generic owner before use");
}

void testSourceDefinedStorageCppEmission() {
  lang::FrontendResult frontend =
      analyzeSourceStorageFixture("source-storage.gti", R"(
concept buffer_element<T> =
    std::storage_element<T> && std::movable<T> && std::relocatable<T>;
class Buffer<buffer_element T> {
  mut std::storage<T> data;
  mut uint64_t count = 0;
  mut uint64_t reserved = 0;

public:
  Buffer(uint64_t capacity)
      : data(fixture_storage<T>(capacity)), reserved(capacity) {}

  uint64_t capacity() { return this.reserved; }

  void push(T value) mut {
    this.data.construct(this.count, std::move(value));
    this.count++;
  }

  T& at(uint64_t index) { return this.data.get(index); }

  void grow(uint64_t capacity) mut {
    mut std::storage<T> replacement = fixture_storage<T>(capacity);
    this.data.relocate_to(replacement, this.count);
    this.data = std::move(replacement);
    this.reserved = capacity;
  }

  void pop() mut {
    this.count--;
    this.data.destroy(this.count);
  }
};

int main() {
  mut Buffer<int> values = Buffer<int>(uint64_t(2));
  mut Buffer<char> characters = Buffer<char>(uint64_t(1));
  values.push(7);
  values.push(9);
  characters.push('G');
  values.grow(uint64_t(4));
  if (values.capacity() == 4 and values.at(uint64_t(0)) == 7 and
      values.at(uint64_t(1)) == 9 and characters.at(uint64_t(0)) == 'G') {
    values.pop();
    return 0;
  }
  return 1;
}
)");
  expect(frontend.canGenerateCode(), "source storage fixture should analyze");
  if (!frontend.canGenerateCode())
    return;
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const auto buffers =
      concreteBackendSymbols(artifact.contents, "__gti_type_", "Buffer");
  expect(buffers.size() == 2,
         "source storage emits both ordinary Buffer instances: " +
             std::to_string(buffers.size()));
  const auto storageInstances = std::count_if(
      frontend.hir.classInstances().begin(),
      frontend.hir.classInstances().end(), [](const auto &instance) {
        return instance.source && instance.source->name().lexeme == "storage";
      });
  // Adding source iterator aliases makes storage's definition eligible for
  // ordinary template representation. Concrete GTI instances remain typed and
  // verified; digest-named C++ classes are not the language's layout contract.
  expect(
      storageInstances >= 2 &&
          (concreteBackendSymbols(artifact.contents, "__gti_type_", "storage")
                   .size() >= 2 ||
           (artifact.contents.find("class storage {") != std::string::npos &&
            artifact.contents.find("::__gti_std::storage<std::int32_t>") !=
                std::string::npos &&
            artifact.contents.find("::__gti_std::storage<char8_t>") !=
                std::string::npos)),
      "source storage emits an ordinary class/template with both typed "
      "storage instances");
  expect(artifact.contents.find("construct_representation_at(") !=
             std::string::npos,
         "source storage emits general object construction");
  expect(artifact.contents.find("::gti_internal::backend::storage<") ==
                 std::string::npos &&
             artifact.contents.find("mir_storage_") == std::string::npos &&
             artifact.contents.find("mir_allocate_storage_v1") ==
                 std::string::npos,
         "source storage never uses the retired sparse backend");
}

void testRecoverableSourceStorageCppEmission() {
  const lang::FrontendResult recoverableStorage =
      analyzeWithMemory("recoverable-source-storage.gti", R"(
int main() {
  std::allocator allocator{};
  auto allocated = allocator.try_storage<int>(uint64_t(2), std::alignment(uint64_t(64)));
  if (!allocated) { return 3; }
  mut auto values = std::move(allocated).value();
  if (values.capacity() != uint64_t(2) || values.initialized(uint64_t(1))) {
    return 1;
  }
  values.construct(uint64_t(1), 42);
  if (!values.initialized(uint64_t(1)) || values.initialized(uint64_t(0))) {
    return 2;
  }
  return values.get(uint64_t(1)) - 42;
}
)");
  expect(recoverableStorage.canGenerateCode(),
         "recoverable storage fixture should analyze");
  if (!recoverableStorage.canGenerateCode())
    return;
  const lang::OptimizationResult recoverableOptimizations =
      lang::OptimizationPipeline().run(recoverableStorage.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact recoverableArtifact =
      gti_test::emitCpp(recoverableStorage, recoverableStorage.mir,
                        recoverableStorage.mir, recoverableOptimizations);
  expect(recoverableArtifact.contents.find("construct_representation_at(") !=
                 std::string::npos &&
             recoverableArtifact.contents.find("storage_try_allocate(") ==
                 std::string::npos &&
             recoverableArtifact.contents.find("storage_initialized(") ==
                 std::string::npos,
         "recoverable allocation and sparse-state observers retain ordinary "
         "source identities and typed results through HIR/MIR");
}

void testCheckedConstructorStorageCppEmission() {
  const lang::FrontendResult checkedConstructorStorage =
      lang::Frontend().analyze("checked-constructor-storage.gti", R"(
import std.memory;

class checked_value {
  mut int32_t stored;

public:
  checked_value(int32_t value) : stored(value) {
    this.stored = value + int32_t(0);
  }

  checked_value(checked_value& other) = delete;
  checked_value(checked_value&& other) = default;
};

int main() {
  std::allocator allocator = std::allocator();
  mut auto allocated =
      allocator.try_storage<checked_value>(std::size_t(1));
  if (!allocated) { return 1; }
  mut std::storage<checked_value> values =
      std::move(allocated).value();
  values.construct(std::size_t(0), checked_value(int32_t(7)));
  return 0;
}
)",
                               {standardLibraryPrelude()}, {},
                               {standardLibraryRoot()});
  expect(checkedConstructorStorage.canGenerateCode(),
         "checked constructor storage fixture should analyze");
  if (!checkedConstructorStorage.canGenerateCode())
    return;
  const lang::BackendArtifact checkedConstructorArtifact = gti_test::emitCpp(
      checkedConstructorStorage, checkedConstructorStorage.mir,
      checkedConstructorStorage.mir,
      lang::OptimizationPipeline().run(checkedConstructorStorage.hir,
                                       lang::OptimizationLevel::O0));
  const lang::MirInstruction *checkedConstruction = nullptr;
  const lang::MirConstructorInstance *checkedTarget = nullptr;
  for (const lang::MirFunctionInstance &function :
       checkedConstructorStorage.mir.functionInstances()) {
    const lang::HirFunctionInstance *hirFunction =
        checkedConstructorStorage.hir.findFunctionInstance(function.id);
    if (hirFunction == nullptr || hirFunction->source == nullptr ||
        hirFunction->source->name().lexeme != "main") {
      continue;
    }
    for (const lang::MirBlock &block : function.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        const lang::MirConstructorInstance *target =
            instruction.constructorTarget
                ? checkedConstructorStorage.mir.findConstructorInstance(
                      *instruction.constructorTarget)
                : nullptr;
        const lang::HirConstructorInstance *hirTarget =
            target == nullptr
                ? nullptr
                : checkedConstructorStorage.hir.findConstructorInstance(
                      target->id);
        if (instruction.kind == lang::MirInstructionKind::Construct &&
            instruction.constructorKind == lang::ConstructorKind::Ordinary &&
            instruction.definedFailure.propagation ==
                lang::FailurePropagationKind::Constructor &&
            hirTarget != nullptr && hirTarget->source != nullptr &&
            hirTarget->source->name().lexeme == "checked_value") {
          checkedConstruction = &instruction;
          checkedTarget = target;
        }
      }
    }
  }
  const std::string checkedSuccess =
      checkedConstruction == nullptr
          ? ""
          : "__gti_mir_construct_success_" +
                std::to_string(checkedConstruction->id);
  const std::string checkedTag =
      checkedTarget == nullptr ? ""
                               : "mir_failure_constructor_tag_v1<" +
                                     std::to_string(checkedTarget->id) + ">{}";
  expect(
      checkedConstruction != nullptr && checkedTarget != nullptr &&
          checkedConstructorArtifact.contents.find(
              "bool " + checkedSuccess + " = false;") != std::string::npos &&
          checkedConstructorArtifact.contents.find(checkedTag) !=
              std::string::npos &&
          checkedConstructorArtifact.contents.find(", &" + checkedSuccess) !=
              std::string::npos &&
          checkedConstructorArtifact.contents.find("if (" + checkedSuccess +
                                                   ")") != std::string::npos,
      "a may-raise constructor staged into a by-value storage call must use "
      "the verified failure ABI and publish its prepared-parameter slot only "
      "on the Invoke success edge");
}

void testFixedArrayPackForwardingCppEmission() {
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/storage_fixed_array_emplacement.gti";
  std::ifstream input(fixture);
  const std::string source((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
  expect(!source.empty(), "fixed-array forwarding regression must load");
  const auto frontend =
      analyzeWithMemory("fixed-array-pack-forwarding.gti", source);
  expect(frontend.canGenerateCode(), "fixed-array forwarding should analyze");
  if (!frontend.canGenerateCode())
    return;
  const auto artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir,
                        lang::OptimizationPipeline().run(
                            frontend.hir, lang::OptimizationLevel::O0));
  expect(artifact.contents.find("class ArrayHolder {") != std::string::npos &&
             artifact.contents.find("construct_representation_at(") !=
                 std::string::npos &&
             artifact.contents.find("forward_pack_argument(args)...") ==
                 std::string::npos,
         "array forwarding is emitted from concrete verified stages");
}

void testVariadicStorageConstructionAndVectorCppEmission() {
  const lang::FrontendResult frontend =
      analyzeSourceStorageFixture("variadic-storage-construction.gti", R"(
import std.memory;

class DefaultValue {
  int32_t stored = 17;

public:
  int32_t value() {
    return this.stored;
  }
public:
  DefaultValue() = default;
};

class Pair {
  int32_t first;
  bool active;

public:
  Pair(int32_t left, bool enabled) : first(left), active(enabled) {}

  int32_t value() {
    if (this.active) {
      return this.first;
    }
    return 0;
  }
};

class MoveOnlyValue {
  std::unique_ptr<Pair> value;

public:
  MoveOnlyValue(int32_t left, bool active)
      : value(std::make_unique<Pair>(left, active)) {}

  int32_t read() {
    return this.value->value();
  }
};

class EmplacingBuffer<std::storage_element T> {
  mut std::storage<T> data;
  mut uint64_t count = 0;

public:
  EmplacingBuffer(uint64_t capacity)
      : data(fixture_storage<T>(capacity)) {}

  mut T& emplace<Args...>(Args... args) mut {
    uint64_t index = this.count;
    this.data.emplace(index, args...);
    this.count++;
    return this.data.get_mut(index);
  }
};

int main() {
  mut EmplacingBuffer<Pair> pairs =
      EmplacingBuffer<Pair>(uint64_t(2));
  Pair& first = pairs.emplace(7, true);
  int32_t observed = first.value();
  [[discard]] pairs.emplace(9, false);

  mut EmplacingBuffer<MoveOnlyValue> owned =
      EmplacingBuffer<MoveOnlyValue>(uint64_t(1));
  MoveOnlyValue pending = MoveOnlyValue(23, true);
  MoveOnlyValue& placed = owned.emplace(std::move(pending));
  int32_t moved_value = placed.read();

  mut std::storage<DefaultValue> defaults =
      fixture_storage<DefaultValue>(uint64_t(1));
  defaults.emplace(uint64_t(0));
  int32_t default_value =
      defaults.get(uint64_t(0)).value();

  mut std::storage<int32_t> numbers =
      fixture_storage<int32_t>(uint64_t(2));
  numbers.emplace(uint64_t(0));
  numbers.emplace(uint64_t(1), 5);
  if (observed == 7 and moved_value == 23 and default_value == 17 and
      numbers.get(uint64_t(0)) == 0 and
      numbers.get(uint64_t(1)) == 5) {
    return 0;
  }
  return 1;
}
)");
  expect(frontend.canGenerateCode(), "variadic storage fixture should analyze");
  if (frontend.canGenerateCode()) {
    const lang::OptimizationResult optimizations =
        lang::OptimizationPipeline().run(frontend.hir,
                                         lang::OptimizationLevel::O0);
    const lang::BackendArtifact artifact =
        gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
    expect(
        concreteBackendSymbols(artifact.contents, "__gti_type_",
                               "EmplacingBuffer")
                    .size() == 2 &&
            artifact.contents.find("class Pair {") != std::string::npos &&
            concreteBackendSymbols(artifact.contents, "__gti_member_",
                                   "emplace")
                    .size() >= 2 &&
            artifact.contents.find("_emplace__gti_mir_failure(") !=
                std::string::npos &&
            artifact.contents.find("construct_representation_at(") !=
                std::string::npos &&
            artifact.contents.find("mir_storage_construct_v1") ==
                std::string::npos &&
            artifact.contents.find("::gti_internal::backend::storage<") ==
                std::string::npos &&
            artifact.contents.find("forward_pack_argument(args)...") ==
                std::string::npos,
        "the backend should pass each staged concrete pack input directly to "
        "storage construction without a temporary element or AST forwarding");
  }
  const lang::FrontendResult vectorFrontend = lang::Frontend().analyze(
      "standard-vector-emplace.gti", R"(
import std.vector;
import std.memory;

class Reading {
  int32_t left;
  int32_t right;

public:
  Reading(int32_t first, int32_t second) : left(first), right(second) {}

  int32_t total() {
    return this.left + this.right;
  }
};

int main() {
  mut std::vector<Reading> readings = std::vector<Reading>();
  readings.reserve(std::size_t(1));
  Reading& first = readings.emplace_back(2, 3);
  int32_t observed = first.total();
  [[discard]] readings.emplace_back(5, 7);

  mut int32_t total = 0;
  for (Reading& reading : readings) {
    total += reading.total();
  }

  mut std::vector<int32_t> zeros =
      std::vector<int32_t>(std::size_t(2));
  zeros[std::size_t(1)] = 11;

  mut std::vector<std::unique_ptr<Reading>> owned =
      std::vector<std::unique_ptr<Reading>>();
  owned.push_back(std::make_unique<Reading>(1, 1));
  owned.insert(std::size_t(0), std::make_unique<Reading>(4, 5));
  owned.erase(std::size_t(0));
  if (observed == 5 and total == 17 and readings.size() == 2 and
      readings.capacity() == 2 and zeros[std::size_t(0)] == 0 and
      zeros[std::size_t(1)] == 11 and owned.size() == 1 and
      owned[std::size_t(0)]->total() == 2) {
    return 0;
  }
  return 1;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(vectorFrontend.canGenerateCode(), "vector fixture should analyze");
  if (vectorFrontend.canGenerateCode()) {
    const lang::BackendArtifact vectorArtifact = gti_test::emitCpp(
        vectorFrontend, vectorFrontend.mir, vectorFrontend.mir,
        lang::OptimizationPipeline().run(vectorFrontend.hir,
                                         lang::OptimizationLevel::O0));
    expect(vectorArtifact.contents.find("mir_object_relocate_v1(") !=
                   std::string::npos &&
               vectorArtifact.contents.find("mir_object_destroy_v1(") !=
                   std::string::npos &&
               vectorArtifact.contents.find("mir_prefix_") == std::string::npos,
           "the backend lowers source shifts through general exact-address "
           "lifetimes");
  }
}

void testAggregateOwnershipTraitsCppEmission() {
  lang::FrontendResult frontend = gti_test::analyzeTrustedPrivateMemoryFixture(
      "aggregate-ownership.gti", R"(
class Buffer<std::storage_element T> {
  mut std::detail::prefix_owner<T> data;
  uint64_t reserved;

public:
  Buffer(uint64_t capacity)
      : data(std::detail::prefix_owner<T>(capacity)), reserved(capacity) {}

  uint64_t capacity() {
    return this.reserved;
  }
};

class NestedBuffer {
  Buffer<int> buffer;

public:
  NestedBuffer(uint64_t capacity) : buffer(Buffer<int>(capacity)) {}

  uint64_t capacity() {
    return this.buffer.capacity();
  }
};

struct CopyableValue {
  int value = 1;
public:
  CopyableValue() = default;
};

Buffer<int> transfer(Buffer<int> value) {
  return std::move(value);
}

NestedBuffer transfer_nested(NestedBuffer value) {
  return std::move(value);
}

uint64_t inspect(Buffer<int>& value) {
  return value.capacity();
}

CopyableValue copy_value(CopyableValue value) {
  return value;
}

int main() {
  Buffer<int> buffer = Buffer<int>(uint64_t(2));
  Buffer<int> moved = transfer(std::move(buffer));
  NestedBuffer nested = NestedBuffer(uint64_t(3));
  NestedBuffer moved_nested = transfer_nested(std::move(nested));
  CopyableValue value = CopyableValue();
  CopyableValue copied = copy_value(value);
  return int(inspect(moved) + moved_nested.capacity()) - 5 + copied.value - 1;
}
)");
  expect(frontend.canGenerateCode(), "aggregate fixture should analyze");
  if (!frontend.canGenerateCode())
    return;
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const std::unordered_set<std::string> bufferTypes =
      concreteBackendSymbols(artifact.contents, "__gti_type_", "Buffer");
  const std::string concreteBuffer =
      bufferTypes.size() == 1 ? *bufferTypes.begin() : std::string();
  const std::size_t emittedProgram =
      artifact.contents.find("namespace __gti_program {");
  std::vector<lang::HirConstructorInstanceId> ownerConstructors;
  for (const auto &constructor : frontend.hir.constructorInstances())
    if (constructor.source != nullptr &&
        (constructor.source->name().lexeme == "Buffer" ||
         constructor.source->name().lexeme == "NestedBuffer"))
      ownerConstructors.push_back(constructor.id);
  expect(ownerConstructors.size() == 2 &&
             std::all_of(ownerConstructors.begin(), ownerConstructors.end(),
                         [&](auto id) {
                           return artifact.contents.find(
                                      "scalar-cfg-constructor-failure-v1 "
                                      "constructor-instance " +
                                      std::to_string(id) + "\n") !=
                                  std::string::npos;
                         }),
         "both concrete owner constructors retain their exact MIR body "
         "identity independently of imported library constructor numbering");
  expect(
      !concreteBuffer.empty() && emittedProgram != std::string::npos &&
          // The failure-free transfers stage the moved parameter in their
          // returned value's slot and move it out after its source drop.
          containsMangledFunction(artifact.contents, "", "transfer", "(") &&
          containsMangledFunction(artifact.contents, "", "transfer_nested",
                                  "(") &&
          artifact.contents.find(
              "__gti_mir_v_1.construct(std::move(__gti_mir_p_1.get()));") !=
              std::string::npos &&
          artifact.contents.find("return std::move(__gti_mir_v_1.get());") !=
              std::string::npos &&
          artifact.contents.find("mir_prefix_allocate_v1") ==
              std::string::npos &&
          artifact.contents.find("__gti_entry__gti_mir_failure(") !=
              std::string::npos &&
          artifact.contents.find("return std::move(value)", emittedProgram) ==
              std::string::npos &&
          artifact.contents.find(concreteBuffer + "(const " + concreteBuffer +
                                 " &other) = delete;") != std::string::npos &&
          artifact.contents.find(concreteBuffer + "(" + concreteBuffer +
                                 " &&) = default;") != std::string::npos &&
          artifact.contents.find(concreteBuffer + " &operator=(const " +
                                 concreteBuffer + " &other) = delete;") !=
              std::string::npos &&
          artifact.contents.find(concreteBuffer + " &operator=(" +
                                 concreteBuffer + " &&) = default;") !=
              std::string::npos,
      "the backend should keep immutable move-only aggregates transferable");
}

void testDefaultedDefaultLifecycleCppEmission() {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("defaulted-default-lifecycle.gti", R"(
class ImplicitStructural {
  int value = 7;
public:
  ImplicitStructural() = default;
};

class ExplicitStructural {
  int value = 7;

public:
  ExplicitStructural() = default;
  ~ExplicitStructural() = default;
  int read() { return this.value; }
};

class NestedStructural {
  ExplicitStructural value = ExplicitStructural();

public:
  NestedStructural() = default;
  ~NestedStructural() = default;
  int read() { return this.value.read(); }
};

int main() {
  ExplicitStructural direct = ExplicitStructural();
  NestedStructural nested = NestedStructural();
  return direct.read() + nested.read() - 14;
}
)");
  expect(frontend.canGenerateCode(),
         "defaulted lifecycle fixture should analyze");
  if (!frontend.canGenerateCode())
    return;
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir,
                        lang::OptimizationPipeline().run(
                            frontend.hir, lang::OptimizationLevel::O0));
  expect(
      artifact.contents.find("ExplicitStructural();") != std::string::npos &&
          artifact.contents.find("~ExplicitStructural() noexcept = default;") !=
              std::string::npos &&
          artifact.contents.find("ExplicitStructural::ExplicitStructural(") !=
              std::string::npos &&
          artifact.contents.find("std::int32_t value;") != std::string::npos &&
          artifact.contents.find("std::int32_t value =") == std::string::npos &&
          artifact.contents.find("__gti_lifecycle_active_") ==
              std::string::npos,
      "the backend should emit the selected generated constructor while "
      "keeping structural destruction and no duplicate default or active-drop "
      "adapter");
}

void testDestructorsAndActiveDropStateCppEmission() {
  const lang::FrontendResult frontend =
      gti_test::analyzeTrustedPrivateMemoryFixture("destructor.gti", R"(
class CleanupBuffer<std::storage_element T> {
  mut std::detail::prefix_owner<T> data;
  mut uint64_t count = 0;

public:
  CleanupBuffer(uint64_t capacity)
      : data(std::detail::prefix_owner<T>(capacity)) {}

  ~CleanupBuffer() {
    while (this.count > 0) {
      this.count--;
      this.data.pop();
    }
  }

  void push(T value) mut {
    this.data.append_value(std::move(value));
    this.count++;
  }
};

CleanupBuffer<int> transfer(CleanupBuffer<int> value) {
  return std::move(value);
}

int main() {
  mut CleanupBuffer<int> values = CleanupBuffer<int>(uint64_t(2));
  values.push(7);
  CleanupBuffer<int> moved = transfer(std::move(values));
  return 0;
}
)");
  expect(frontend.canGenerateCode(), "destructor fixture should analyze");
  if (!frontend.canGenerateCode())
    return;
  const lang::ClassDecl *cleanupClass =
      findTopLevelClass(frontend.program, "CleanupBuffer");
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const lang::ClassTypeInfo *cleanupType =
      cleanupClass == nullptr ? nullptr
                              : frontend.semantics.findClassType(*cleanupClass);
  const std::string lifecycleSuffix =
      cleanupType == nullptr ? "" : std::to_string(cleanupType->id);
  const std::string cleanupName = "__gti_lifecycle_cleanup_" + lifecycleSuffix;
  const std::string activeName = "__gti_lifecycle_active_" + lifecycleSuffix;
  const std::unordered_set<std::string> cleanupTypes =
      concreteBackendSymbols(artifact.contents, "__gti_type_", "CleanupBuffer");
  const std::string concreteCleanup =
      cleanupTypes.size() == 1 ? *cleanupTypes.begin() : std::string();
  expect(
      cleanupType != nullptr && !concreteCleanup.empty() &&
          artifact.contents.find("~" + concreteCleanup + "() noexcept { if (" +
                                 activeName + ") { " + cleanupName +
                                 "(); } }") != std::string::npos &&
          artifact.contents.find("bool " + activeName + " = true;") !=
              std::string::npos &&
          artifact.contents.find("if (!" + activeName + ") { return true; }") !=
              std::string::npos &&
          artifact.contents.find(concreteCleanup + "(const " + concreteCleanup +
                                 " &other) = delete;") != std::string::npos &&
          artifact.contents.find(concreteCleanup + "(" + concreteCleanup +
                                 " &&other) noexcept") != std::string::npos &&
          artifact.contents.find("other." + activeName + " = false;") !=
              std::string::npos &&
          artifact.contents.find(cleanupName + "();") != std::string::npos &&
          artifact.contents.find(
              "GTI verified-MIR body: scalar-cfg-destructor-failure-v1") !=
              std::string::npos &&
          hasMirIntrinsicKind(frontend, lang::IntrinsicKind::ObjectDestroy) &&
          artifact.contents.find("mir_prefix_pop_v1") == std::string::npos,
      "the backend should guard native retirement with the same initialized "
      "active drop state while retaining declared cleanup and safe moves");
}

void testRestrictedMemberOperatorsCppEmission() {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("member-operators.gti", R"(
struct Payload {
  mut int value = 0;

  void increment() mut { this.value += 1; }
public:
  Payload() = default;
};

class Handle {
  mut Payload payload = Payload();
  mut int values[2] = {1, 2};

public:
  Payload& operator->() { return this.payload; }
  mut Payload& operator->() mut { return this.payload; }
  int& operator*() { return this.values[0]; }
  mut int& operator*() mut { return this.values[0]; }
  int& operator[](uint64_t index) { return this.values[index]; }
  mut int& operator[](uint64_t index) mut { return this.values[index]; }
  bool operator==(nullptr_t other) { return false; }
  bool operator!=(nullptr_t other) { return true; }
  operator bool() { return true; }
public:
  Handle() = default;
};

class Number {
  mut int value = 0;

public:
  Number(int initial) : value(initial) {}

  Number operator+() { return Number(this.value); }
  Number operator-() { return Number(0 - this.value); }
  Number operator+(int other) { return Number(this.value + other); }
  Number operator-(int other) { return Number(this.value - other); }
  Number operator*(int other) { return Number(this.value * other); }
  Number operator/(int other) { return Number(this.value / other); }
  Number operator%(int other) { return Number(this.value % other); }
  Number operator~() { return Number(~this.value); }
  Number operator&(int other) { return Number(this.value & other); }
  Number operator|(int other) { return Number(this.value | other); }
  Number operator^(int other) { return Number(this.value ^ other); }
  Number operator<<(int amount) { return Number(this.value << amount); }
  Number operator>>(int amount) { return Number(this.value >> amount); }

  void operator+=(int other) mut { this.value += other; }
  void operator-=(int other) mut { this.value -= other; }
  void operator*=(int other) mut { this.value *= other; }
  void operator/=(int other) mut { this.value /= other; }
  void operator%=(int other) mut { this.value %= other; }
  void operator&=(int other) mut { this.value &= other; }
  void operator|=(int other) mut { this.value |= other; }
  void operator^=(int other) mut { this.value ^= other; }
  void operator<<=(int amount) mut { this.value <<= amount; }
  void operator>>=(int amount) mut { this.value >>= amount; }
  void operator--() mut { this.value--; }

  int get() { return this.value; }
public:
  Number() = default;
};

struct NumberHolder {
  mut Number value = Number(1);
public:
  NumberHolder() = default;
};

class NumberIndex {
  mut Number value = Number(2);

public:
  mut Number& operator[](int index) mut { return this.value; }
public:
  NumberIndex() = default;
};

class NumberOwner {
  mut Number value = Number(3);

public:
  mut Number& operator*() mut { return this.value; }
public:
  NumberOwner() = default;
};

class NumberArrow {
  mut NumberHolder holder = NumberHolder();

public:
  mut NumberHolder& operator->() mut { return this.holder; }
public:
  NumberArrow() = default;
};

int main() {
  mut Handle handle = Handle();
  handle->increment();
  *handle = 7;
  handle[uint64_t(1)] += 3;
  mut Number number = Number(9);
  Number unaryPositive = +number;
  Number unaryNegative = -number;
  mut Number sum = number + 2;
  Number difference = number - 2;
  Number product = number * 2;
  Number quotient = number / 3;
  Number remainder = number % 4;
  Number conjunction = number & 7;
  Number disjunction = number | 2;
  Number exclusive = number ^ 3;
  Number shiftedLeft = number << 1;
  Number shiftedRight = number >> 1;
  sum += 3;
  --sum;
  Number inverted = ~sum;
  mut Number mutations = Number(60);
  mutations -= 1;
  mutations *= 2;
  mutations /= 3;
  mutations %= 17;
  mutations &= 15;
  mutations |= 16;
  mutations ^= 3;
  mutations <<= 1;
  mutations >>= 1;
  mut NumberHolder holder = NumberHolder();
  holder.value += 2;
  mut NumberIndex indexed = NumberIndex();
  indexed[0] += 2;
  mut NumberOwner owner = NumberOwner();
  *owner += 2;
  mut NumberArrow arrow = NumberArrow();
  arrow->value += 2;
  if (handle and handle != nullptr) {
    return *handle + handle[uint64_t(1)] - 12;
  }
  return 1;
}
)");
  expect(frontend.canGenerateCode(),
         "restricted operator fixture should analyze");
  if (!frontend.canGenerateCode())
    return;
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const std::size_t emittedProgram =
      artifact.contents.find("namespace __gti_program {");
  expect(artifact.contents.find("___gti_operator_arrow") != std::string::npos &&
             artifact.contents.find("___gti_operator_dereference") !=
                 std::string::npos &&
             artifact.contents.find("___gti_operator_subscript") !=
                 std::string::npos &&
             artifact.contents.find(
                 "friend const std::int32_t & "
                 "__gti_operator_dereference(const Handle &__gti_receiver)") !=
                 std::string::npos &&
             artifact.contents.find("::operator!=(__gti_mir_v_") !=
                 std::string::npos &&
             // The field-returning arrow cannot fail and publishes its loan
             // directly; the checked-index dereference keeps its failure form.
             artifact.contents.find("___gti_operator_arrow());") !=
                 std::string::npos &&
             artifact.contents.find(
                 "___gti_operator_dereference__gti_mir_failure(&"
                 "__gti_mir_loan_") != std::string::npos &&
             artifact.contents.find("___gti_operator_bool()") !=
                 std::string::npos &&
             artifact.contents.find("___gti_operator_plus") !=
                 std::string::npos &&
             artifact.contents.find("___gti_operator_add_assign") !=
                 std::string::npos &&
             artifact.contents.find("___gti_operator_pre_decrement") !=
                 std::string::npos &&
             emittedProgram != std::string::npos &&
             artifact.contents.find(" operator*(", emittedProgram) ==
                 std::string::npos,
         "the backend should retain selected method calls while exposing only "
         "validated comparisons as C++ operators for generic representation");
}

void testCallableMemberOperatorsCppEmission() {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("call-operator.gti", R"(
class Accumulator {
  mut int total;

public:
  Accumulator(int initial) : total(initial) {}

  int operator()() { return this.total; }
  int operator()(int value) { return this.total + value; }
  int operator()(int value) mut {
    this.total += value;
    return this.total;
  }
  int operator()(int left, int right) { return this.total + left + right; }
};

class Identity<T> {
public:
  T operator()(T value) { return value; }
public:
  Identity() = default;
};

class Slot {
  mut int value = 1;

public:
  int& operator()() { return this.value; }
  mut int& operator()() mut { return this.value; }
public:
  Slot() = default;
};

int main() {
  Accumulator fixed = Accumulator(1);
  mut Accumulator changing = Accumulator(2);
  Identity<int> identity = Identity<int>();
  int first = fixed();
  int second = fixed(3);
  int third = changing(4);
  int fourth = fixed(5, 6);
  int generic = identity(7);
  mut Slot slot = Slot();
  mut int& slot_value = slot();
  slot_value = first + second + third + fourth + generic;
  return slot() - 25;
}
)");
  expect(frontend.canGenerateCode(), "call-operator fixture should analyze");
  if (!frontend.canGenerateCode())
    return;
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(artifact.contents.find("___gti_operator_call") != std::string::npos &&
             artifact.contents.find(" operator()(") == std::string::npos,
         "call operators should lower to the semantically selected member");
}

void testConsumingCallableOperatorsCppEmission() {
  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "consuming-callables.gti", R"(
class OnceValue {
  mut int value;

public:
  OnceValue(int initial) : value(initial) {}

  int operator()() && {
    return std::move(this.value);
  }
};

class ReadValue {
  int value;

public:
  ReadValue(int initial) : value(initial) {}
  int operator()() { return this.value; }
};

class MutableValue {
  mut int value;

public:
  MutableValue(int initial) : value(initial) {}
  int operator()() mut {
    this.value++;
    return this.value;
  }
};

class DualValue {
  mut int value;

public:
  DualValue(int initial) : value(initial) {}
  int operator()() { return this.value; }
  int operator()() && { return std::move(this.value); }
};

class MutableFallback {
  mut int value;

public:
  MutableFallback(int initial) : value(initial) {}
  int operator()(int amount) { return this.value - amount; }
  int operator()(int amount) mut {
    this.value += amount;
    return this.value;
  }
  int operator()() && { return std::move(this.value); }
};

int invoke_once<Operation>(Operation operation) {
  return std::move(operation)();
}

int invoke_once_with<Operation>(Operation operation, int amount) {
  return std::move(operation)(amount);
}

int relay_once<Operation>(Operation operation) {
  return invoke_once(std::move(operation));
}

int choose_relay_once<Operation>(Operation operation, bool first) {
  if (first) {
    return invoke_once(std::move(operation));
  }
  return invoke_once(std::move(operation));
}

int choose_once<Operation>(Operation operation, bool first) {
  if (first) {
    return std::move(operation)();
  }
  return std::move(operation)();
}

int main() {
  OnceValue once = OnceValue(7);
  ReadValue read = ReadValue(3);
  MutableValue mutate = MutableValue(4);
  DualValue dual = DualValue(6);
  MutableFallback fallback = MutableFallback(4);
  MutableFallback generic_fallback = MutableFallback(8);
  int consumed = invoke_once(once);
  int observed = invoke_once(read);
  int changed = invoke_once(mutate);
  int selected = choose_once(OnceValue(2), false);
  int relayed = relay_once(OnceValue(5));
  int branch_relayed = choose_relay_once(OnceValue(6), true);
  int peeked = dual();
  int taken = std::move(dual)();
  int changed_after_move = std::move(fallback)(3);
  int generic_changed_after_move = invoke_once_with(generic_fallback, 2);
  return consumed + observed + changed + selected + relayed + branch_relayed +
             peeked + taken + changed_after_move + generic_changed_after_move -
         57;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(frontend.canGenerateCode(),
         "consuming callable fixture should analyze");
  if (!frontend.canGenerateCode())
    return;
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(artifact.contents.find("___gti_operator_call() &&;") !=
                 std::string::npos &&
             artifact.contents.find("___gti_operator_call() const &;") !=
                 std::string::npos &&
             artifact.contents.find("___gti_operator_call(const "
                                    "std::int32_t amount) &;") !=
                 std::string::npos &&
             containsMangledFunction(artifact.contents, "std::int32_t ",
                                     "__gti_operator_call", "() &&;") &&
             containsMangledFunction(artifact.contents, "std::int32_t ",
                                     "__gti_operator_call", "() const &;") &&
             containsMangledFunction(artifact.contents, "std::int32_t ",
                                     "__gti_operator_call",
                                     "(const std::int32_t amount) &;") &&
             artifact.contents.find(
                 "std::move(__gti_mir_p_1.get()).::__gti_program::OnceValue::"
                 "__gti_fn_") != std::string::npos &&
             artifact.contents.find(
                 "__gti_mir_p_1.get().::__gti_program::MutableFallback::"
                 "__gti_fn_") != std::string::npos &&
             artifact.contents.find("___gti_operator_call__gti_mir_failure(") !=
                 std::string::npos &&
             artifact.contents.find(
                 "exact_call<::gti_internal::backend::callable_capability::"
                 "once_call>") != std::string::npos &&
             artifact.contents.find(
                 "exact_call<::gti_internal::backend::callable_capability::"
                 "mutable_call") != std::string::npos &&
             artifact.contents.find("callable_capability::read_call") !=
                 std::string::npos,
         "the C++ backend should preserve consuming member qualification and "
         "the exact frontend-selected callable capability");
}

void testRangeBasedForAndIteratorProtocolCppEmission() {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("range-for.gti", R"(
interface IteratorContract<T> {
  T& operator*();
  void operator++() mut;
};

struct CounterSentinel {
  int limit;
  CounterSentinel(int value) : limit(value) {}
};

class CounterIterator : public IteratorContract<int> {
  mut int current;

public:
  CounterIterator(int value) : current(value) {}
  int& operator*() override { return this.current; }
  void operator++() mut override { this.current++; }
  bool operator!=(CounterSentinel& sentinel) {
    return this.current != sentinel.limit;
  }
};

class CounterRange {
  int first;
  int last;

public:
  CounterRange(int first, int last) : first(first), last(last) {}
  CounterIterator begin() { return CounterIterator(this.first); }
  CounterSentinel end() { return CounterSentinel(this.last); }
};

int main() {
  CounterRange values{1, 5};
  mut int total = 0;
  for (auto& value : values) {
    if (value == 2) { continue; }
    total += value;
  }
  return total - 8;
}
)");
  expect(frontend.canGenerateCode(), "range-for fixture should analyze");
  if (!frontend.canGenerateCode())
    return;
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const bool emittedRangeCore =
      artifact.contents.find(
          "const ::__gti_program::CounterRange *__gti_mir_loan_") !=
          std::string::npos &&
      artifact.contents.find("_begin__gti_mir_failure(") != std::string::npos &&
      // Constructing the sentinel cannot fail, so end() is selected plainly.
      containsMangledFunction(artifact.contents, "CounterRange::", "end",
                              "()") &&
      artifact.contents.find(".__gti_operator_dereference__gti_mir_failure(") !=
          std::string::npos &&
      artifact.contents.find(
          ".__gti_operator_pre_increment__gti_mir_failure(") !=
          std::string::npos &&
      artifact.contents.find(".begin()") == std::string::npos &&
      artifact.contents.find(".end()") == std::string::npos;
  expect(emittedRangeCore,
         "the C++ backend should emit the verified range loans and selected "
         "GTI iterator methods without native C++ range lookup");
}

void testReadOnlyRangeLoansCppEmission() {
  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "read-only-range-loans.gti", R"(
interface IteratorContract<T> {
  T& operator*();
  void operator++() mut;
};

class CounterIterator : public IteratorContract<int> {
  mut int current;
public:
  CounterIterator(int value) : current(value) {}
  int& operator*() override { return this.current; }
  void operator++() mut override { this.current++; }
  bool operator!=(CounterIterator& other) {
    return this.current != other.current;
  }
};

class OwnedRange {
  int first;
  int last;
public:
  OwnedRange(int first, int last) : first(first), last(last) {}
  OwnedRange(OwnedRange& other) = delete;
  OwnedRange(OwnedRange&& other) = default;
  ~OwnedRange() {}
  CounterIterator begin() { return CounterIterator(this.first); }
  CounterIterator end() { return CounterIterator(this.last); }
};

int nested_arrays() {
  int first[3] = {1, 2, 3};
  int second[2] = {4, 5};
  mut int total = 0;
  for (auto& left : first) {
    for (int right : second) {
      if (right == 4) { continue; }
      total += left + right;
    }
  }
  return total - 21;
}

int temporary_complete() {
  mut int total = 0;
  for (auto& value : OwnedRange(1, 5)) {
    if (value == 2) { continue; }
    if (value == 4) { break; }
    total += value;
  }
  for (auto& value : OwnedRange(0, 0)) { return value; }
  return total - 4;
}

int temporary_return() {
  for (auto& value : OwnedRange(3, 6)) {
    if (value == 4) { return value - 4; }
  }
  return 1;
}

int main() {
  return nested_arrays() + temporary_complete() + temporary_return();
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(frontend.canGenerateCode(), "read-only range fixture should analyze");
  if (!frontend.canGenerateCode())
    return;
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(artifact.contents.find("std::array<std::int32_t, 3>") !=
                 std::string::npos &&
             artifact.contents.find("std::uint64_t{0}") != std::string::npos,
         "the C++ backend should consume the fixed-array MIR strategy rather "
         "than rediscovering source range syntax");
}

void testPassiveCleanupRangeTemporaryCppEmission() {
  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "passive-cleanup-range-temporary.gti", R"(
class Cursor {
  mut int32_t at;
public:
  Cursor(int32_t at) : at(at) {}
  int32_t& operator*() { return this.at; }
  void operator++() mut { this.at += 1; }
  bool operator!=(Cursor& end) { return this.at < end.at; }
};

class Span {
  int32_t last;
public:
  Span(int32_t last) : last(last) {}
  Cursor begin() { return Cursor(0); }
  Cursor end() { return Cursor(this.last); }
};

int32_t first_over(int32_t limit) {
  for (int32_t value : Span(10)) {
    if (value > limit) { return value; }
  }
  return -1;
}

int main() {
  mut int32_t total = 0;
  for (int32_t value : Span(4)) {
    if (value == 1) { continue; }
    total += value;
  }
  return total - 5 + first_over(6) - 7;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(frontend.canGenerateCode(), "passive range fixture should analyze");
  if (!frontend.canGenerateCode())
    return;
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(artifact.contents.find("Span") != std::string::npos,
         "the C++ backend should emit a passive-cleanup range temporary");
}

void testMutableRangeLoansCppEmission() {
  const lang::FrontendResult fixedArrays =
      lang::Frontend().analyze("mutable-fixed-array-ranges.gti", R"(
int mutate_arrays() {
  mut int32_t values[4] = {1, 2, 3, 4};
  int32_t observed[2] = {5, 6};
  mut int32_t total = 0;
  for (mut int32_t& value : values) {
    for (int32_t& item : observed) { total += item; }
    value += 10;
    if (value == 12) { continue; }
    if (value == 13) { break; }
  }
  return values[0] + values[1] + values[2] + values[3] + total - 73;
}

int return_from_mutable_loop() {
  mut int32_t values[2] = {1, 2};
  for (mut auto& value : values) {
    value += 1;
    if (value == 3) { return value - 3; }
  }
  return 1;
}

int main() { return mutate_arrays() + return_from_mutable_loop(); }
)");
  expect(fixedArrays.canGenerateCode(),
         "fixed-array range fixture should analyze");
  if (fixedArrays.canGenerateCode()) {
    const lang::OptimizationResult fixedArrayOptimizations =
        lang::OptimizationPipeline().run(fixedArrays.hir,
                                         lang::OptimizationLevel::O0);
    const lang::BackendArtifact fixedArrayArtifact = gti_test::emitCpp(
        fixedArrays, fixedArrays.mir, fixedArrays.mir, fixedArrayOptimizations);
    expect(fixedArrayArtifact.contents.find("GTI verified-MIR body") !=
                   std::string::npos &&
               fixedArrayArtifact.contents.find(
                   "std::int32_t *__gti_mir_loan_") != std::string::npos,
           "the C++ backend should consume mutable fixed-array loans from the "
           "verified MIR plan");
  }
  const lang::FrontendResult standardVector = lang::Frontend().analyze(
      "mutable-vector-range.gti", R"(
import std.vector;

std::vector<int32_t> make_values() {
  mut std::vector<int32_t> values = std::vector<int32_t>();
  values.push_back(2);
  values.push_back(3);
  return std::move(values);
}

int mutate_owned_temporary() {
  mut int32_t total = 0;
  for (mut auto& value : make_values()) {
    value += 4;
    total += value;
  }
  for (mut auto& value : std::vector<int32_t>()) { return value; }
  return total - 13;
}

int mutate_vector() {
  mut std::vector<int32_t> values = std::vector<int32_t>();
  values.push_back(1);
  values.push_back(2);
  values.push_back(3);
  for (mut auto& value : values) { value += 4; }
  return values[std::size_t(0)] + values[std::size_t(1)] +
         values[std::size_t(2)] - 18;
}

int main() { return mutate_vector() + mutate_owned_temporary(); }
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(standardVector.canGenerateCode(),
         "vector range fixture should analyze");
  if (standardVector.canGenerateCode()) {
    const lang::OptimizationResult vectorOptimizations =
        lang::OptimizationPipeline().run(standardVector.hir,
                                         lang::OptimizationLevel::O0);
    const lang::BackendArtifact vectorArtifact =
        gti_test::emitCpp(standardVector, standardVector.mir,
                          standardVector.mir, vectorOptimizations);
    expect(vectorArtifact.contents.find("vector_mut_iterator") !=
                   std::string::npos &&
               vectorArtifact.contents.find("begin_mut__gti_mir_failure") !=
                   std::string::npos,
           "the C++ backend should emit the mutable iterator's transferred "
           "reference capability and mutable storage read");
  }
}

void testLambdasCppEmission() {
  lang::FrontendResult frontend = lang::Frontend().analyze("lambdas.gti", R"(
int main() {
  int offset = 3;
  auto add = [offset](int value) -> int { return offset + value; };
  auto copied = add;
  auto compose = [copied](int value) -> int {
    return copied(value) + 1;
  };
  int direct = [](int value) -> int { return value * 2; }(4);
  auto inferred = direct;
  return compose(inferred) - 12;
}
)");
  expect(frontend.canGenerateCode(), "lambda fixture should analyze");
  if (frontend.canGenerateCode()) {
    const lang::OptimizationResult optimizations =
        lang::OptimizationPipeline().run(frontend.hir,
                                         lang::OptimizationLevel::O1);
    const lang::BackendArtifact artifact =
        gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
    expect(
        artifact.contents.find("auto __gti_mir_closure_factory_") !=
                std::string::npos &&
            artifact.contents.find("using __gti_mir_closure_type_") !=
                std::string::npos &&
            artifact.contents.find(
                "mir_lifetime_slot<__gti_mir_closure_type_") !=
                std::string::npos &&
            artifact.contents.find(".construct(__gti_mir_p_2.get())") !=
                std::string::npos &&
            artifact.contents.find("return __gti_mir_closure_environment_") !=
                std::string::npos &&
            artifact.contents.find("using __gti_mir_lambda_identity = ") !=
                std::string::npos &&
            artifact.contents.find(
                "GTI verified-MIR body: scalar-cfg-failure-v1 "
                "lambda-instance 3") != std::string::npos,
        "the MIR-owned C++ backend should materialize immutable closure "
        "copies, nested value captures, and fused direct invocation");
  }
  const lang::FrontendResult expectedLambda = lang::Frontend().analyze(
      "expected-lambda.gti", R"(
int main() {
  auto calculate = [](bool fail) -> expected<int, std::string_view> {
    if (fail) { return unexpected("failed"); }
    return 1;
  };
  expected<int, std::string_view> result = calculate(false);
  return result.value_or(0) - 1;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  const std::string cpp20 =
      expectedLambda.canGenerateCode()
          ? gti_test::emitCppText(expectedLambda, lang::CppStandard::Cpp20)
          : std::string{};
  // The result in the lambda signature is an ordinary class instance, so the
  // backend must define it rather than rely on a built-in helper.
  const auto lambdaResults =
      concreteBackendSymbols(cpp20, "__gti_type_", "expected");
  expect(expectedLambda.canGenerateCode() &&
             std::any_of(lambdaResults.begin(), lambdaResults.end(),
                         [&](const std::string &name) {
                           return cpp20.find("class " + name + " {") !=
                                  std::string::npos;
                         }),
         "backend support should account for types nested in lambda "
         "signatures");
}

void testConfinedCallableParametersCppEmission() {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("callable-parameters.gti", R"(
void apply_twice<T, Operation>(mut T& value, Operation operation) {
  operation(value);
  operation(value);
}

int apply_stateful<Operation>(mut Operation operation) {
  int first = operation(1);
  int second = operation(1);
  return first + second;
}

class Replace<T> {
  T replacement;

public:
  Replace(T replacement) : replacement(replacement) {}
  void operator()(mut T& value) { value = this.replacement; }
};

class Increasing {
  mut int next = 0;

public:
  int operator()(int value) mut {
    this.next++;
    return value + this.next;
  }
public:
  Increasing() = default;
};

int main() {
  mut int lambda_value = 1;
  auto increment = [](mut int& value) -> void { value++; };
  apply_twice(lambda_value, increment);
  mut int object_value = 2;
  Replace<int> object = Replace<int>(4);
  apply_twice(object_value, object);
  Increasing increasing = Increasing();
  int stateful = apply_stateful(increasing);
  auto doubled = [](int value) -> int { return value * 2; };
  int read_only = apply_stateful(doubled);
  return lambda_value + object_value + stateful + read_only - 16;
}
)");
  expect(frontend.canGenerateCode(), "callable fixture should analyze");
  if (!frontend.canGenerateCode())
    return;
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O1);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(
      artifact.contents.find("// GTI verified-MIR body: scalar-cfg-failure-v1 "
                             "function-instance") != std::string::npos &&
          artifact.contents.find("callable_capability::read_call") !=
              std::string::npos &&
          // The generic owner's bridge keeps its exact result while delaying
          // completeness checks until the owner is instantiated.
          artifact.contents.find("friend std::conditional_t<true, void, "
                                 "Replace> __gti_invoke(") !=
              std::string::npos &&
          artifact.contents.find(
              "::__gti_program::Replace<std::int32_t>::__gti_fn_") !=
              std::string::npos &&
          artifact.contents.find("___gti_operator_call__gti_mir_failure(") !=
              std::string::npos &&
          artifact.contents.find("___gti_operator_call") != std::string::npos &&
          [&] {
            const auto count = [&](std::string_view spelling) {
              std::size_t result = 0;
              for (std::size_t at = 0;
                   (at = artifact.contents.find(spelling, at)) !=
                   std::string::npos;
                   at += spelling.size())
                ++result;
              return result;
            };
            // Only compiler-generated closure environments expose a native
            // call operator. Source nominal operators still use exact bridges.
            return count(" operator()(") ==
                   count("struct __gti_mir_closure_environment_");
          }(),
      "C++ lowering should bridge exact GTI call operators without "
      "exposing native overload resolution");
}

void testConfinedCallableResultsCppEmission() {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("callable-predicates.gti", R"(
bool accepts<Predicate>(int value, Predicate predicate) {
  return predicate(value);
}

bool accepts_split<Predicate>(int left, int right, Predicate predicate) {
  if (predicate(left) && !predicate(right)) {
    return true;
  }
  return false;
}

bool stores_result<Predicate>(int value, Predicate predicate) {
  bool initialized = predicate(value);
  mut bool assigned = false;
  assigned = predicate(value);
  return initialized && assigned;
}

void checks_loops<Predicate>(Predicate predicate) {
  mut int value = 0;
  while (predicate(value)) {
    value++;
  }
  for (mut int index = 0; predicate(index); index++) {}
}

class GreaterThan {
  int threshold;

public:
  GreaterThan(int threshold) : threshold(threshold) {}
  bool operator()(int value) { return value > this.threshold; }
};

int main() {
  auto positive = [](int value) -> bool { return value > 0; };
  bool lambda_result = accepts(4, positive);
  bool split_result = accepts_split(4, -1, positive);
  bool stored_result = stores_result(4, positive);
  auto never = [](int value) -> bool { return value < 0; };
  checks_loops(never);
  GreaterThan greater_than = GreaterThan(3);
  bool object_result = accepts(4, greater_than);
  if (lambda_result && split_result && stored_result && object_result) {
    return 0;
  }
  return 1;
}
)");
  expect(frontend.canGenerateCode(), "callable fixture should analyze");
  if (!frontend.canGenerateCode())
    return;
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O1);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(
      artifact.contents.find("// GTI verified-MIR body: scalar-cfg-failure-v1 "
                             "function-instance") != std::string::npos &&
          artifact.contents.find("callable_capability::read_call") !=
              std::string::npos &&
          artifact.contents.find("friend bool __gti_invoke(") !=
              std::string::npos &&
          artifact.contents.find("::__gti_program::GreaterThan::__gti_fn_") !=
              std::string::npos &&
          artifact.contents.find("___gti_operator_call(__gti_mir_v_") !=
              std::string::npos &&
          artifact.contents.find("*__gti_mir_out_result = __gti_mir_v_4;") !=
              std::string::npos,
      "C++ lowering should preserve predicate values through the exact "
      "callable bridge");
}

void testExpectedValuesCppEmission() {
  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "expected-values.gti", R"(
class MutableValue {
  mut int stored = 0;
public:
  void set(int value) mut { this.stored = value; }
  int get() { return this.stored; }
public:
  MutableValue() = default;
};

expected<int, std::string_view> calculate(bool fail) {
  if (fail) { return unexpected("calculation failed"); }
  return 42;
}
expected<void, std::string_view> render(bool fail) {
  if (fail) { return unexpected("render failed"); }
  return;
}
int main() {
  expected<int, std::string_view> result = calculate(false);
  if (!result.has_value()) { return 1; }
  int value = result.value_or(0);
  expected<void, std::string_view> rendered = render(false);
  if (!rendered) { return 2; }
  rendered.value();
  mut expected<MutableValue, int> mutable_result = MutableValue();
  mutable_result.value().set(42);
  if (mutable_result.value().get() != 42) { return 3; }
  [[discard]] calculate(false);
  return value - 42;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(frontend.canGenerateCode(),
         "expected construction and observers should pass semantic checks");
  if (!frontend.canGenerateCode())
    return;
  const std::string cpp23 = gti_test::emitCppText(frontend);
  const std::string cpp20 =
      gti_test::emitCppText(frontend, lang::CppStandard::Cpp20);
  for (const std::string *cpp : {&cpp23, &cpp20}) {
    // Each of the three result types is an emitted instance of the prelude
    // class, and unexpected() is its ordinary source factory.
    const auto results =
        concreteBackendSymbols(*cpp, "__gti_type_", "expected");
    const auto defined = std::count_if(
        results.begin(), results.end(), [&](const std::string &name) {
          return cpp->find("class " + name + " {") != std::string::npos &&
                 cpp->find("::__gti_program::" + name) != std::string::npos;
        });
    expect(cpp->find("#include <expected>") == std::string::npos &&
               cpp->find("nonstd/expected.hpp") == std::string::npos &&
               cpp->find("::nonstd::") == std::string::npos &&
               cpp->find("std::expected<") == std::string::npos &&
               cpp->find("::gti_internal::backend::expected") ==
                   std::string::npos &&
               results.size() == 3 && defined == 3 &&
               containsMangledFunction(*cpp, "::__gti_program::", "unexpected",
                                       ""),
           "both C++ standards should lower expected values to GTI's own "
           "representation rather than a host or vendored library");
  }
}

void testFallibleBorrowsCppEmission() {
  const lang::FrontendResult frontend =
      analyzeTrustedPreludeFixture("fallible-borrows.gti", R"(
enum class lookup_errc { missing };
int32_t global_value = 19;

class Box {
  int32_t value;
public:
  Box(int32_t value) : value(value) {}
  expected<int32_t&, lookup_errc> find(bool present) {
    if (!present) { return unexpected(lookup_errc::missing); }
    return this.value;
  }
};

class owning_error {
  int32_t code;
public:
  owning_error(int32_t code) : code(code + 1) {}
  owning_error(owning_error& other) = delete;
  owning_error(owning_error&& other) = default;
  ~owning_error() {}
};

expected<int32_t&, lookup_errc> borrow_parameter(int32_t& value,
                                                  bool present) {
  if (!present) { return unexpected(lookup_errc::missing); }
  return value;
}

expected<int32_t&, lookup_errc> borrow_global(bool present) {
  if (!present) { return unexpected(lookup_errc::missing); }
  return global_value;
}

expected<int32_t&, lookup_errc> forward(Box& box, bool present) {
  auto result = box.find(present);
  return result;
}

expected<int32_t&, owning_error> borrow_with_owning_error(int32_t& value,
                                                           bool present) {
  if (!present) { return unexpected(owning_error(0)); }
  return value;
}

int main() {
  Box box{7};
  auto found = forward(box, true);
  int32_t& alias = found.value();
  int32_t local = 13;
  expected<int32_t&, lookup_errc> direct = local;
  auto copied = direct;
  auto moved = std::move(copied);
  auto parameter = borrow_parameter(local, true);
  auto global = borrow_global(true);
  auto owned = borrow_with_owning_error(local, true);
  return alias + moved.value() + parameter.value() + global.value() +
         owned.value() - 65;
}
)");
  expect(frontend.canGenerateCode(), "fallible-borrow fixture should analyze");
  if (!frontend.canGenerateCode())
    return;
  const std::string cpp23 = gti_test::emitCppText(frontend);
  const std::string cpp20 =
      gti_test::emitCppText(frontend, lang::CppStandard::Cpp20);
  // The checked-reference profile keeps its success in an ordinary payload
  // alternative, natively a rebindable wrapper rather than a reference member.
  const auto pointerBacked = [](const std::string &cpp) {
    return cpp.find("std::reference_wrapper<const std::int32_t> "
                    "__gti_field_0") != std::string::npos &&
           cpp.find("expected<std::int32_t &") == std::string::npos &&
           cpp.find("::gti_internal::backend::borrowed_expected") ==
               std::string::npos;
  };
  expect(pointerBacked(cpp23) && pointerBacked(cpp20),
         "both native standards should use GTI's private pointer-backed "
         "fallible-borrow carrier and initialize passive or owning-error "
         "representations from a verified loan");
}

void testExpectedPropagationOperatorCppEmission() {
  const lang::FrontendResult frontend =
      analyzeTrustedPreludeFixture("expected-propagation.gti", R"(
expected<int, int> leaf(bool fail, int error, int value) {
  if (fail) { return unexpected(error); }
  return value;
}

expected<int, int> sum(bool first, bool second) {
  return ?leaf(first, 11, 20) + ?leaf(second, 12, 22);
}

expected<void, int> complete(bool fail) {
  if (fail) { return unexpected(13); }
  return;
}

expected<void, int> forward_void(bool fail) {
  ?complete(fail);
  return;
}

expected<int, int> copy_named(bool fail) {
  expected<int, int> result = leaf(fail, 14, 40);
  return ?result;
}

int main() {
  auto forward = [](bool fail) -> expected<int, int> {
    return ?leaf(fail, 15, 42);
  };
  expected<int, int> value = forward(false);
  return value.value() - 42;
}
)");
  expect(frontend.canGenerateCode(), "propagation fixture should analyze");
  if (!frontend.canGenerateCode())
    return;
  const std::string cpp23 = gti_test::emitCppText(frontend);
  const std::string cpp20 =
      gti_test::emitCppText(frontend, lang::CppStandard::Cpp20);
  // The branches call the selected std::tryable conformance functions.
  const auto checkedBranches = [](const std::string &cpp) {
    return containsMangledFunction(cpp, "::", "has_value", "(") &&
           containsMangledFunction(cpp, "::", "take_value", "") &&
           containsMangledFunction(cpp, "::", "take_residual", "");
  };
  expect(checkedBranches(cpp23) && checkedBranches(cpp20),
         "both C++ backends should emit checked propagation branches instead "
         "of delegating the operator to C++");
}

const NamedTest kTests[] = {
    {"testExplicitValueMovesCppEmission", testExplicitValueMovesCppEmission},
    {"testNonNullReferencesCppEmission", testNonNullReferencesCppEmission},
    {"testGlobalBorrowReturnsCppEmission", testGlobalBorrowReturnsCppEmission},
    {"testPrefixStorageCapabilityCppEmission",
     testPrefixStorageCapabilityCppEmission},
    {"testReceiverTiedReferenceReturnsCppEmission",
     testReceiverTiedReferenceReturnsCppEmission},
    {"testArgumentTiedReferenceReturnsCppEmission",
     testArgumentTiedReferenceReturnsCppEmission},
    {"testStoredReferenceGroundworkCppEmission",
     testStoredReferenceGroundworkCppEmission},
    {"testUniqueOwnershipAndAllocationCppEmission",
     testUniqueOwnershipAndAllocationCppEmission},
    {"testUniqueOwnerNullStateCppEmission",
     testUniqueOwnerNullStateCppEmission},
    {"testUniqueOwnerSwapCppEmission", testUniqueOwnerSwapCppEmission},
    {"testPolymorphicUniqueOwnerUpcastCppEmission",
     testPolymorphicUniqueOwnerUpcastCppEmission},
    {"testSourceDefinedStorageCppEmission",
     testSourceDefinedStorageCppEmission},
    {"testRecoverableSourceStorageCppEmission",
     testRecoverableSourceStorageCppEmission},
    {"testCheckedConstructorStorageCppEmission",
     testCheckedConstructorStorageCppEmission},
    {"testFixedArrayPackForwardingCppEmission",
     testFixedArrayPackForwardingCppEmission},
    {"testVariadicStorageConstructionAndVectorCppEmission",
     testVariadicStorageConstructionAndVectorCppEmission},
    {"testAggregateOwnershipTraitsCppEmission",
     testAggregateOwnershipTraitsCppEmission},
    {"testDefaultedDefaultLifecycleCppEmission",
     testDefaultedDefaultLifecycleCppEmission},
    {"testDestructorsAndActiveDropStateCppEmission",
     testDestructorsAndActiveDropStateCppEmission},
    {"testRestrictedMemberOperatorsCppEmission",
     testRestrictedMemberOperatorsCppEmission},
    {"testCallableMemberOperatorsCppEmission",
     testCallableMemberOperatorsCppEmission},
    {"testConsumingCallableOperatorsCppEmission",
     testConsumingCallableOperatorsCppEmission},
    {"testRangeBasedForAndIteratorProtocolCppEmission",
     testRangeBasedForAndIteratorProtocolCppEmission},
    {"testReadOnlyRangeLoansCppEmission", testReadOnlyRangeLoansCppEmission},
    {"testPassiveCleanupRangeTemporaryCppEmission",
     testPassiveCleanupRangeTemporaryCppEmission},
    {"testMutableRangeLoansCppEmission", testMutableRangeLoansCppEmission},
    {"testLambdasCppEmission", testLambdasCppEmission},
    {"testConfinedCallableParametersCppEmission",
     testConfinedCallableParametersCppEmission},
    {"testConfinedCallableResultsCppEmission",
     testConfinedCallableResultsCppEmission},
    {"testExpectedValuesCppEmission", testExpectedValuesCppEmission},
    {"testFallibleBorrowsCppEmission", testFallibleBorrowsCppEmission},
    {"testExpectedPropagationOperatorCppEmission",
     testExpectedPropagationOperatorCppEmission},
};

} // namespace

int main(int argc, char **argv) {
  return runNamedTests(argc, argv, kTests, "cpp_mir_ownership_emission");
}
