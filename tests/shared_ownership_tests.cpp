#include "gti/frontend.h"
#include "gti/mir_effects.h"
#include "gti/mir_verifier.h"
#include "gti/lowered_program_builder.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

const std::filesystem::path root =
    std::filesystem::path(__FILE__).parent_path().parent_path();

lang::FrontendResult
analyze(const std::string &source,
        lang::FrontendPhase phase = lang::FrontendPhase::Hir) {
  lang::FrontendOptions options;
  options.stopAfter = phase;
  options.target = lang::TargetInfo::host();
  options.target.runtimeCapabilities.threads = true;
  return lang::Frontend(options)
      .analyze("shared-owner-check.gti", "#include <std/memory>\n" + source,
               {root / "stdlib/prelude.gti"}, {}, {root / "stdlib"});
}

void require(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void dump(const lang::FrontendResult &frontend) {
  for (const auto &diagnostic : frontend.diagnostics) {
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
}

void checkPublicPolicy() {
  const auto frontend = analyze(R"(
struct Value {
  mut int value = 7;
  int read() { return value; }
  void change() mut { value++; }
};
int main() {
  mut std::shared_ptr<Value> first = std::make_shared<Value>();
  auto second = first.clone();
  Value& read = *second;
  first.reset();
  int result = read.read();
  mut std::shared_ptr<Value> empty{};
  mut std::shared_ptr<Value> other = std::move(first);
  empty.swap(other);
  other = nullptr;
  auto moved = std::move(second);
  return result + int(moved.use_count()) - 8;
}
)");
  if (!frontend.hirValid) {
    dump(frontend);
  }
  require(frontend.hirValid, "shared-owner source policy must concretize");
  std::size_t owners = 0;
  for (const auto &instance : frontend.hir.classInstances()) {
    require(instance.compilerCapability != lang::CompilerCapabilityTypeKind::SharedOwner,
            "the public source owner must not instantiate the private owner");
    if (!instance.source || instance.source->name().lexeme != "shared_ptr")
      continue;
    ++owners;
    require(!instance.traits.copyable && instance.traits.movable &&
                instance.traits.drop == lang::DropKind::Lexical &&
                instance.requiresActiveCleanup,
            "strong handles must be move-only cleanup owners");
  }
  require(owners != 0, "source shared-owner class must be instantiated");
}

void checkRejections() {
  // expected<T, E> is now an ordinary source class, so it is an admitted
  // shared-owner pointee rather than a rejected compiler carrier.
  const auto result = analyze(R"(int main() {
           std::shared_ptr<expected<int, int>> empty{};
           return int(empty.use_count());
         })",
                              lang::FrontendPhase::Mir);
  if (!result.canGenerateCode()) {
    dump(result);
  }
  require(result.canGenerateCode(),
          "a source result class is an ordinary shared-owner pointee");
  const std::vector<std::string> sources = {
      R"(int main() {
           auto owner = std::make_shared<int>(7);
           auto copied = owner;
           return *copied;
         })",
      R"(int main() {
           mut auto owner = std::make_shared<int>(7);
           *owner = 9;
           return *owner;
         })",
      R"(int main() {
           mut auto owner = std::make_shared<int>(7);
           int& borrowed = *owner;
           owner.reset();
           return borrowed;
         })",
      R"(int main() {
           auto owner = std::make_shared<int>(7);
           auto moved = std::move(owner);
           return *owner;
         })",
      R"(int& escape() {
           auto owner = std::make_shared<int>(7);
           return *owner;
         }
         int main() { return escape(); })",
      R"(struct Borrowed {
           int& value;
           Borrowed(int& source) : value(source) {}
         };
         int main() {
           int value = 7;
           auto owner = std::make_shared<Borrowed>(value);
           return 0;
         })",
      R"(class Base { public: virtual int read() { return 1; } };
         class Other { public: int read() { return 2; } };
         int main() {
           auto owner = std::make_shared<Other>();
           auto base = std::upcast_shared<Base, Other>(std::move(owner));
           return base->read();
         })",
      R"(class Base { public: int read() { return 1; } };
         class Derived : public Base {};
         int main() {
           auto owner = std::make_shared<Derived>();
           auto base = std::upcast_shared<Base, Derived>(std::move(owner));
           return base->read();
         })",
      R"(interface Base { int read(); };
         class Derived : public Base {
         public:
           int read() override { return 3; }
         };
         int main() {
           auto owner = std::make_shared<Derived>();
           auto base = std::upcast_shared<Base, Derived>(owner);
           return base->read();
         })",
      R"(class Base { public: virtual int read() { return 1; } };
         [[no_transfer]] class Derived : public Base {
         public: int read() override { return 2; }
         };
         int main() {
           auto owner = std::make_shared<Derived>();
           auto base = std::upcast_shared<Base, Derived>(std::move(owner));
           return base->read();
         })",
      R"(class Base { public: virtual int read() { return 1; } };
         [[no_share]] class Derived : public Base {
         public: int read() override { return 2; }
         };
         int main() {
           auto owner = std::make_shared<Derived>();
           auto base = std::upcast_shared<Base, Derived>(std::move(owner));
           return base->read();
         })",
      R"(class Base { public: virtual int read() { return 1; } };
         class Derived : public Base {
         public:
           ~Derived() {}
           int read() override { return 2; }
         };
         int main() {
           auto owner = std::make_shared<Derived>();
           auto base = std::upcast_shared<Base, Derived>(std::move(owner));
           return base->read();
         })",
  };
  for (const auto &source : sources) {
    const auto frontend = analyze(source);
    if (frontend.hirValid || !frontend.syntaxValid) {
      dump(frontend);
    }
    require(frontend.syntaxValid,
            "negative cases must reach semantic checking");
    require(!frontend.hirValid,
            "unsafe shared-owner operation must be rejected");
    require(std::any_of(frontend.diagnostics.begin(),
                        frontend.diagnostics.end(),
                        [](const auto &diagnostic) {
                          return diagnostic.code.starts_with("GTI-S");
                        }),
            "shared-owner rejection must be a semantic diagnostic");
    require(std::none_of(frontend.diagnostics.begin(),
                         frontend.diagnostics.end(),
                         [](const auto &diagnostic) {
                           return diagnostic.code == "GTI-B0001";
                         }),
            "invalid shared-owner source must not cause an ICE");
  }
}

void checkUpcastContract() {
  const auto frontend = analyze(R"(
[[transfer, share]] interface Sink { int read(); };
class Concrete : public Sink {
public:
  int read() override { return 7; }
};
int main() {
  mut auto concrete = std::make_shared<Concrete>();
  auto alias = concrete.clone();
  auto sink = std::upcast_shared<Sink, Concrete>(std::move(concrete));
  return sink->read() + int(alias.use_count()) - 9;
}
)",
                                lang::FrontendPhase::Mir);
  if (!frontend.canGenerateCode()) {
    dump(frontend);
    for (const auto &error : lang::verifyMirProgram(frontend.mir).errors) {
      std::cerr << "MIR: " << error.message << '\n';
    }
  }
  require(frontend.canGenerateCode(),
          "shared-owner polymorphic upcasts must lower to verified MIR");
  lang::HirClassInstanceId baseInstance = 0;
  lang::HirClassInstanceId sourceOwnerInstance = 0;
  std::size_t sourceOwners = 0;
  for (const auto &instance : frontend.hir.classInstances()) {
    require(instance.compilerCapability != lang::CompilerCapabilityTypeKind::SharedOwner,
            "upcasting must use ordinary source ownership");
    if (instance.source && instance.source->name().lexeme == "shared_ptr") {
      ++sourceOwners;
      sourceOwnerInstance = instance.id;
    }
    if (instance.polymorphic && instance.kind == lang::ClassKind::Interface)
      baseInstance = instance.id;
  }
  require(sourceOwners == 2 && baseInstance && sourceOwnerInstance,
          "upcast instantiates concrete and interface source handles");
  auto malformed = frontend.mir;
  auto *sourceOwner = const_cast<lang::MirClassInstance *>(
      malformed.findClassInstance(sourceOwnerInstance));
  sourceOwner->traits.transferCapable = false;
  sourceOwner->traits.shareCapable = false;
  require(!lang::verifyMirProgram(malformed).valid(),
          "source upcasts cannot forge their conditional capabilities");
}

void checkMirContract() {
  const auto frontend = analyze(R"(
int main() {
  mut auto first = std::make_shared<int>(7);
  mut auto second = first.clone();
  int& read = *second;
  first.reset();
  int value = read;
  first.swap(second);
  return value + int(first.use_count()) - 8;
}

)",
                                lang::FrontendPhase::Mir);
  if (!frontend.canGenerateCode()) {
    dump(frontend);
    for (const auto &error : lang::verifyMirProgram(frontend.mir).errors) {
      std::cerr << "MIR: " << error.message << '\n';
    }
  }
  require(frontend.canGenerateCode(),
          "shared owners must lower to verified MIR");
  bool sawSynchronizedDrop = false;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Drop) {
          continue;
        }
        const auto traits = lang::effects(frontend.mir, instruction);
        if (traits.maySynchronize && traits.readsUnknownMemory &&
            traits.writesUnknownMemory) {
          sawSynchronizedDrop = true;
          require(function.effectSummary.traits.maySynchronize,
                  "shared-release synchronization must reach callable summary");
        }
      }
    }
  }
  require(sawSynchronizedDrop, "shared release is a synchronization barrier");
  bool sawConstruction = false;
  bool sawAtomic = false;
  bool sawBorrow = false;
  bool sawEmptyFailure = false;
  bool sawAllocationFailure = false;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        require(instruction.intrinsic < lang::IntrinsicKind::SharedOwnerEmpty ||
                    instruction.intrinsic > lang::IntrinsicKind::SharedOwnerUpcast,
                "source policy cannot call specialized owner intrinsics");
        sawConstruction |= instruction.intrinsic == lang::IntrinsicKind::ObjectConstruct;
        sawAtomic |= instruction.synchronization.kind ==
                     lang::SynchronizationOperationKind::AtomicCompareExchange;
        const bool borrow = instruction.intrinsic == lang::IntrinsicKind::RawBorrow;
        const bool empty = instruction.intrinsic == lang::IntrinsicKind::FailEmptyOwner;
        const bool allocation = instruction.intrinsic == lang::IntrinsicKind::RequireAllocation;
        sawBorrow |= borrow;
        sawEmptyFailure |= empty;
        sawAllocationFailure |= allocation;
        if (!borrow && !empty && !allocation)
          continue;
        if (empty || allocation) {
          require(instruction.definedFailure.mayUnwind() == empty &&
                      instruction.definedFailure.mayAbortAllocation() == allocation,
                  "empty access unwinds while count/allocation exhaustion aborts");
        }
        for (int mutation = 0; mutation != 2; ++mutation) {
          auto malformed = frontend.mir;
          auto *target = const_cast<lang::MirFunctionInstance *>(
              malformed.findFunctionInstance(function.id));
          auto &changed = *std::find_if(
              target->body.blocks[block.id - 1].instructions.begin(),
              target->body.blocks[block.id - 1].instructions.end(),
              [&](const auto &candidate) { return candidate.id == instruction.id; });
          if (borrow) {
            if (mutation == 0) changed.parameterTypes.clear();
            else changed.borrowAccess = lang::AccessMode::Mutable;
          } else {
            if (mutation == 0) changed.localFailureSites.clear();
            else changed.definedFailure = {};
          }
          require(!lang::verifyMirProgram(malformed).valid(),
                  "general borrow and failure witnesses remain mandatory");
        }
      }
    }
  }
  require(sawConstruction && sawAtomic && sawBorrow && sawEmptyFailure && sawAllocationFailure,
          "source ownership exercises lifetime, atomic, checked borrow and failure facilities");
  for (const auto &source : frontend.hir.classInstances()) {
    if (!source.source || source.source->name().lexeme != "shared_ptr")
      continue;
    for (int mutation = 0; mutation != 2; ++mutation) {
      auto malformed = frontend.mir;
      auto *changed = const_cast<lang::MirClassInstance *>(
          malformed.findClassInstance(source.id));
      if (mutation == 0) changed->requiresActiveCleanup = false;
      else changed->traits.copyable = true;
      const auto sealed = lang::LoweredProgramBuilder().build(
          frontend.program, frontend.semantics, frontend.hir, malformed,
          malformed, {}, frontend.mir.target());
      require(!sealed.valid(),
              "source owner cleanup and explicit-clone policy retain their frontend provenance");
    }
  }
}

void checkConcurrencyCapabilities() {
  for (const std::string annotation : {"", "[[no_transfer]]", "[[no_share]]"}) {
    const auto frontend = analyze(annotation + R"(
class Payload { int value = 7; };
int main() {
  auto value = std::make_shared<Payload>();
  return int(value.use_count()) - 1;
}

)");
    if (!frontend.hirValid) {
      dump(frontend);
    }
    require(frontend.hirValid,
            "local shared ownership needs no thread permission");
    bool observed = false;
    for (const auto &instance : frontend.hir.classInstances()) {
      if (!instance.source || instance.source->name().lexeme != "shared_ptr")
        continue;
      observed = true;
      require(instance.traits.transferCapable == annotation.empty() &&
                  instance.traits.shareCapable == annotation.empty(),
              "cross-thread shared handles require both pointee capabilities");
    }
    require(observed, "concurrency check needs a concrete shared owner");
  }
}

void checkRecursiveOwnership() {
  const auto frontend = analyze(R"(
struct Node {
  int value = 7;
  std::shared_ptr<Node> next{};
};
int main() {
  auto root = std::make_shared<Node>();
  auto alias = root.clone();
  return alias->value - 7;
}
)",
                                lang::FrontendPhase::Mir);
  if (!frontend.canGenerateCode()) {
    dump(frontend);
  }
  require(
      frontend.canGenerateCode(),
      "recursive shared-owner shapes need a finite verified cleanup recipe");
}

void checkPassivePointee() {
  const auto frontend = analyze(R"(
struct Passive { int value = 11; };
int main() {
  auto owner = std::make_shared<Passive>();
  return owner->value - 11;
}
)",
                                lang::FrontendPhase::Mir);
  if (!frontend.canGenerateCode()) {
    dump(frontend);
  }
  require(frontend.canGenerateCode(),
          "passive allocated classes require allocation-only cleanup");
}

void checkArrayCleanup() {
  const auto frontend = analyze(R"(
int main() {
  mut std::shared_ptr<int> handles[2][2] = {};
  handles[0][1] = std::make_shared<int>(7);
  return 0;
}
)",
                                lang::FrontendPhase::Mir);
  if (!frontend.canGenerateCode()) {
    dump(frontend);
  }
  require(frontend.canGenerateCode(),
          "shared handle arrays need exact cleanup");
  bool observed = false;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &drop : function.body.dropObligations) {
      if (drop.dropType.type.kind != lang::SemanticType::Array ||
          drop.dropType.aggregate.empty() ||
          drop.dropType.type.arguments.front().kind !=
              lang::SemanticType::Array) {
        continue;
      }
      observed = true;
      require(drop.dropType.fixedArray &&
                  drop.dropType.fixedArray->extents ==
                      std::vector<std::size_t>{2, 2} &&
                  drop.dropType.aggregate.size() == 1,
              "array cleanup must preserve exact extents and element recipe");
      for (int mutation = 0; mutation != 3; ++mutation) {
        auto malformed = frontend.mir;
        auto *target = const_cast<lang::MirFunctionInstance *>(
            malformed.findFunctionInstance(function.id));
        auto &changed = target->body.dropObligations[drop.id - 1].dropType;
        if (mutation == 0) {
          changed.aggregate.clear();
        } else if (mutation == 1) {
          changed.aggregate.front().requiresActiveCleanup = false;
        } else {
          changed.fixedArray->extents.front() = 3;
        }
        require(!lang::verifyMirProgram(malformed).valid(),
                "array cleanup cannot erase its release or forge its extent");
      }
    }
  }
  require(observed, "array cleanup test needs an owned fixed-array obligation");
}

} // namespace

int main() {
  try {
    checkPublicPolicy();
    checkRejections();
    checkUpcastContract();
    checkMirContract();
    checkConcurrencyCapabilities();
    checkRecursiveOwnership();
    checkPassivePointee();
    checkArrayCleanup();
    std::cout << "shared ownership semantic checks passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
