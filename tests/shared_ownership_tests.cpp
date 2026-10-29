#include "gti/frontend.h"
#include "gti/mir_effects.h"
#include "gti/mir_verifier.h"

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
  return lang::Frontend({.stopAfter = phase})
      .analyze("shared-owner-check.gti", "import std.memory;\n" + source,
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
public:
  Value() = default;
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
    if (instance.compilerCapability !=
        lang::CompilerCapabilityTypeKind::SharedOwner) {
      continue;
    }
    ++owners;
    require(!instance.traits.copyable && instance.traits.movable &&
                instance.traits.drop == lang::DropKind::Lexical &&
                instance.requiresActiveCleanup,
            "strong handles must be move-only cleanup owners");
  }
  require(owners != 0, "trusted shared-owner capability must be instantiated");
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
      R"(class Base { public: virtual int read() { return 1; }  public: Base() = default; };
         class Other { public: int read() { return 2; }  public: Other() = default; };
         int main() {
           auto owner = std::make_shared<Other>();
           auto base = std::upcast_shared<Base, Other>(std::move(owner));
           return base->read();
         })",
      R"(class Base { public: int read() { return 1; }  public: Base() = default; };
         class Derived : public Base { public: Derived() = default; };
         int main() {
           auto owner = std::make_shared<Derived>();
           auto base = std::upcast_shared<Base, Derived>(std::move(owner));
           return base->read();
         })",
      R"(interface Base { int read(); };
         class Derived : public Base {
         public:
           int read() override { return 3; }
          public: Derived() = default; };
         int main() {
           auto owner = std::make_shared<Derived>();
           auto base = std::upcast_shared<Base, Derived>(owner);
           return base->read();
         })",
      R"(class Base { public: virtual int read() { return 1; }  public: Base() = default; };
         [[no_transfer]] class Derived : public Base {
         public: int read() override { return 2; }
          public: Derived() = default; };
         int main() {
           auto owner = std::make_shared<Derived>();
           auto base = std::upcast_shared<Base, Derived>(std::move(owner));
           return base->read();
         })",
      R"(class Base { public: virtual int read() { return 1; }  public: Base() = default; };
         [[no_share]] class Derived : public Base {
         public: int read() override { return 2; }
          public: Derived() = default; };
         int main() {
           auto owner = std::make_shared<Derived>();
           auto base = std::upcast_shared<Base, Derived>(std::move(owner));
           return base->read();
         })",
      R"(class Base { public: virtual int read() { return 1; }  public: Base() = default; };
         class Derived : public Base {
         public:
           ~Derived() {}
           int read() override { return 2; }
          public: Derived() = default; };
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
public:
  Concrete() = default;
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
  bool observed = false;
  lang::HirClassInstanceId baseInstance = 0;
  lang::HirClassInstanceId sourceOwnerInstance = 0;
  for (const auto &instance : frontend.mir.classInstances()) {
    if (instance.type.kind == lang::SemanticType::Class &&
        instance.type.classId != 0 && instance.polymorphic &&
        instance.kind == lang::ClassKind::Interface) {
      baseInstance = instance.id;
    }
  }
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.intrinsic != lang::IntrinsicKind::SharedOwnerUpcast) {
          continue;
        }
        observed = true;
        for (const auto &instance : frontend.mir.classInstances()) {
          if (instance.type == instruction.operands.front().type) {
            sourceOwnerInstance = instance.id;
          }
        }
        require(
            instruction.operands.size() == 1 && instruction.result &&
                instruction.definedFailure.empty() &&
                instruction.localFailureSites.empty() &&
                std::count_if(instruction.lifecycle.begin(),
                              instruction.lifecycle.end(),
                              [](const auto &event) {
                                return event.kind ==
                                       lang::MirLifecycleEventKind::TransferOut;
                              }) == 1 &&
                std::count_if(instruction.lifecycle.begin(),
                              instruction.lifecycle.end(),
                              [](const auto &event) {
                                return event.kind ==
                                       lang::MirLifecycleEventKind::Initialize;
                              }) == 1,
            "shared-owner upcasts must consume one token without a "
            "failure edge");
        for (int mutation = 0; mutation != 6; ++mutation) {
          auto malformed = frontend.mir;
          auto *target = const_cast<lang::MirFunctionInstance *>(
              malformed.findFunctionInstance(function.id));
          auto &changed = *std::find_if(
              target->body.blocks[block.id - 1].instructions.begin(),
              target->body.blocks[block.id - 1].instructions.end(),
              [&](const auto &candidate) {
                return candidate.id == instruction.id;
              });
          if (mutation == 0) {
            changed.lifecycle.clear();
          } else if (mutation == 1) {
            changed.parameterTypes.clear();
          } else if (mutation == 2) {
            changed.info.type = changed.operands.front().type;
          } else if (mutation == 3) {
            changed.localFailureSites.push_back(1);
          } else {
            const auto activation = std::find_if(
                changed.lifecycle.begin(), changed.lifecycle.end(),
                [](const auto &event) {
                  return event.kind == lang::MirLifecycleEventKind::Initialize;
                });
            require(activation != changed.lifecycle.end(),
                    "shared upcast mutation should find result activation");
            if (activation == changed.lifecycle.end()) {
              continue;
            }
            if (mutation == 4) {
              changed.lifecycle.erase(activation);
            } else {
              activation->target = 0;
            }
          }
          require(!lang::verifyMirProgram(malformed).valid(),
                  "forged shared-owner upcast contracts must reject");
        }
      }
    }
  }
  require(observed && baseInstance != 0 && sourceOwnerInstance != 0,
          "upcast test must instantiate its polymorphic base capability");
  auto malformed = frontend.mir;
  auto *base = const_cast<lang::MirClassInstance *>(
      malformed.findClassInstance(baseInstance));
  base->polymorphic = false;
  require(!lang::verifyMirProgram(malformed).valid(),
          "shared-owner upcast cannot forge polymorphic destruction");

  malformed = frontend.mir;
  auto *sourceOwner = const_cast<lang::MirClassInstance *>(
      malformed.findClassInstance(sourceOwnerInstance));
  sourceOwner->traits.transferCapable = false;
  sourceOwner->traits.shareCapable = false;
  require(!lang::verifyMirProgram(malformed).valid(),
          "shared-owner upcast cannot amplify erased concurrency "
          "capabilities");
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
  bool sawPromotion = false;
  bool sawClone = false;
  bool sawBorrow = false;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.intrinsic !=
                lang::IntrinsicKind::SharedOwnerFromUnique &&
            instruction.intrinsic != lang::IntrinsicKind::SharedOwnerClone &&
            instruction.intrinsic != lang::IntrinsicKind::SharedOwnerBorrow) {
          continue;
        }
        const bool borrow =
            instruction.intrinsic == lang::IntrinsicKind::SharedOwnerBorrow;
        const bool invokes =
            block.terminator.kind == lang::MirTerminatorKind::Invoke &&
            block.terminator.invokeInstruction == instruction.id;
        require(invokes == borrow,
                "empty shared borrowing must Invoke; promotion and retain "
                "exhaustion must not gain cleanup edges");
        require(instruction.definedFailure.mayUnwind() == borrow &&
                    instruction.definedFailure.mayAbortAllocation() != borrow,
                "shared operations retain exact contained/terminal outcomes");
        for (int mutation = 0; mutation != 5; ++mutation) {
          auto malformed = frontend.mir;
          auto *target = const_cast<lang::MirFunctionInstance *>(
              malformed.findFunctionInstance(function.id));
          auto &changed = *std::find_if(
              target->body.blocks[block.id - 1].instructions.begin(),
              target->body.blocks[block.id - 1].instructions.end(),
              [&](const auto &candidate) {
                return candidate.id == instruction.id;
              });
          if (mutation == 1) {
            changed.parameterTypes.clear();
          } else if (mutation == 2) {
            changed.definedFailure.localOrigins.front()
                .outcomes.front()
                .detail = lang::DefinedFailureDetail::None;
          } else if (mutation == 3) {
            if (borrow) {
              target->body.blocks[block.id - 1].terminator.invokeInstruction =
                  0;
            } else {
              // Terminal allocation still needs its exact source record;
              // clearing a nonexistent Invoke would be a no-op mutation.
              changed.localFailureSites.clear();
            }
          } else if (mutation == 4) {
            changed.borrowArguments = {0, 1};
          } else if (instruction.intrinsic ==
                     lang::IntrinsicKind::SharedOwnerFromUnique) {
            sawPromotion = true;
            std::erase_if(changed.lifecycle, [](const auto &event) {
              return event.kind == lang::MirLifecycleEventKind::TransferOut;
            });
          } else if (instruction.intrinsic ==
                     lang::IntrinsicKind::SharedOwnerClone) {
            sawClone = true;
            changed.info.traits.copyable = true;
          } else {
            sawBorrow = true;
            changed.borrowAccess = lang::AccessMode::Mutable;
          }
          require(!lang::verifyMirProgram(malformed).valid(),
                  "forged shared ownership, failure or borrow contracts must "
                  "reject");
        }
      }
    }
  }
  require(sawPromotion && sawClone && sawBorrow,
          "MIR tests must exercise promotion, cloning and checked borrowing");
  for (const auto &instance : frontend.mir.classInstances()) {
    if (instance.compilerCapability !=
        lang::CompilerCapabilityTypeKind::SharedOwner) {
      continue;
    }
    for (int mutation = 0; mutation != 4; ++mutation) {
      auto malformed = frontend.mir;
      auto *changed = const_cast<lang::MirClassInstance *>(
          malformed.findClassInstance(instance.id));
      if (mutation == 0) {
        changed->requiresActiveCleanup = false;
      }
      if (mutation == 1) {
        changed->traits.copyable = true;
      }
      if (mutation == 2) {
        changed->traits.containsBorrowedState = true;
      }
      if (mutation == 3) {
        changed->compilerCapability = lang::CompilerCapabilityTypeKind::None;
      }
      require(!lang::verifyMirProgram(malformed).valid(),
              "forged shared capability identity or lifecycle must reject");
    }
  }
}

void checkConcurrencyCapabilities() {
  for (const std::string annotation : {"", "[[no_transfer]]", "[[no_share]]"}) {
    const auto frontend = analyze(annotation + R"(
class Payload { int value = 7;  public: Payload() = default; };
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
      if (instance.compilerCapability !=
          lang::CompilerCapabilityTypeKind::SharedOwner) {
        continue;
      }
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
public:
  Node() = default;
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
struct Passive { int value = 11;  public: Passive() = default; };
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
