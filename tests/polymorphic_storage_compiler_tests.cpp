#include "compiler_test_fixture.h"

#include "gti/mir_effects.h"

#include <functional>

namespace {

[[nodiscard]] std::size_t
polymorphicIntrinsicIndex(lang::IntrinsicKind intrinsic) {
  switch (intrinsic) {
  case lang::IntrinsicKind::PolymorphicStorageHasValue:
    return 0;
  case lang::IntrinsicKind::PolymorphicStorageConstruct:
    return 1;
  case lang::IntrinsicKind::PolymorphicStorageRead:
    return 2;
  case lang::IntrinsicKind::PolymorphicStorageReadMut:
    return 3;
  case lang::IntrinsicKind::PolymorphicStorageReset:
    return 4;
  default:
    return 5;
  }
}

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

// Applies one mutation to the first matching MIR instruction and reports
// whether the mutation found a target.
bool mutateFirst(lang::MirProgram &program,
                 const std::function<bool(lang::MirInstruction &)> &mutation) {
  for (const lang::MirBodyAddress address :
       lang::enumerateMirBodyAddresses(program)) {
    lang::MirBody *body = lang::findMirBody(program, address);
    if (body == nullptr) {
      continue;
    }
    for (lang::MirBlock &block : body->blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (mutation(instruction)) {
          return true;
        }
      }
    }
  }
  return false;
}

void testPolymorphicStorageContract() {
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

  std::array<bool, 5> hirIntrinsics{};
  for (const lang::HirFunctionInstance &function :
       frontend.hir.functionInstances()) {
    for (const lang::HirValue &value : function.body.values) {
      const std::size_t index = polymorphicIntrinsicIndex(value.intrinsic);
      if (index < hirIntrinsics.size()) {
        hirIntrinsics[index] = true;
      }
    }
  }
  std::array<bool, 5> mirIntrinsics{};
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    for (const lang::MirBlock &block : function.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        const std::size_t index =
            polymorphicIntrinsicIndex(instruction.intrinsic);
        if (index < mirIntrinsics.size()) {
          mirIntrinsics[index] = true;
        }
      }
    }
  }
  const lang::MirClassInstance *slot = nullptr;
  for (const lang::MirClassInstance &instance : frontend.mir.classInstances()) {
    if (instance.compilerCapability ==
        lang::CompilerCapabilityTypeKind::PolymorphicStorage) {
      slot = &instance;
    }
  }
  expect(std::all_of(hirIntrinsics.begin(), hirIntrinsics.end(),
                     [](bool present) { return present; }) &&
             std::all_of(mirIntrinsics.begin(), mirIntrinsics.end(),
                         [](bool present) { return present; }),
         "all five declaration-bound slot operations should survive HIR and "
         "MIR");
  expect(slot != nullptr &&
             slot->copyConstructor == lang::SpecialMemberStatus::Deleted &&
             slot->moveConstructor == lang::SpecialMemberStatus::Generated &&
             slot->copyAssignment == lang::SpecialMemberStatus::Deleted &&
             slot->moveAssignment == lang::SpecialMemberStatus::Generated &&
             !slot->requiresActiveCleanup && !slot->traits.copyable &&
             slot->traits.movable && slot->traits.moveAssignable &&
             slot->traits.relocation == lang::RelocationKind::Structural &&
             lang::mirTypeRelocationKind(frontend.mir, slot->type) ==
                 lang::RelocationKind::Structural &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "the slot is a move-only, move-assignable, cleanup-inert structural "
         "owner, never a bitwise-relocated polymorphic object");

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

  const auto rejects =
      [&](const char *label,
          const std::function<bool(lang::MirInstruction &)> &mutation) {
        lang::MirProgram forged = frontend.mir;
        const bool changed = mutateFirst(forged, mutation);
        expect(
            changed && !lang::verifyMirProgram(forged).valid(),
            std::string("verification should reject a slot operation with ") +
                label);
      };
  rejects("a relabelled read", [](lang::MirInstruction &instruction) {
    if (instruction.intrinsic != lang::IntrinsicKind::PolymorphicStorageRead)
      return false;
    instruction.intrinsic = lang::IntrinsicKind::PolymorphicStorageHasValue;
    return true;
  });
  rejects("a read without its loan", [](lang::MirInstruction &instruction) {
    if (instruction.intrinsic != lang::IntrinsicKind::PolymorphicStorageReadMut)
      return false;
    instruction.loan.reset();
    return true;
  });
  rejects("an invented failure site", [](lang::MirInstruction &instruction) {
    if (instruction.intrinsic != lang::IntrinsicKind::PolymorphicStorageReset)
      return false;
    instruction.localFailureSites.push_back(1);
    return true;
  });
  // Re-type the payload as a model whose cleanup is not inert. The forged
  // instruction stays internally consistent, so only the payload contract
  // can reject it.
  lang::SemanticType ineligible = lang::SemanticType::Unknown;
  for (const lang::MirClassInstance &instance : frontend.mir.classInstances()) {
    if (instance.requiresActiveCleanup && !instance.bases.empty() &&
        instance.compilerCapability == lang::CompilerCapabilityTypeKind::None)
      ineligible = instance.type;
  }
  expect(ineligible != lang::SemanticType::Unknown,
         "the boxed model retains its concrete MIR class instance");
  rejects("a payload whose cleanup is not inert",
          [&](lang::MirInstruction &instruction) {
            if (instruction.intrinsic !=
                    lang::IntrinsicKind::PolymorphicStorageConstruct ||
                instruction.operands.size() != 2)
              return false;
            instruction.operands[1].type = ineligible;
            instruction.parameterTypes[1] = ineligible;
            return true;
          });
  // Forge the admitted model's own MIR facts. The payload instruction is
  // untouched, so only the slot's payload contract can reject these.
  const auto rejectsModel = [&](const char *label, auto mutation) {
    lang::MirProgram forged = frontend.mir;
    lang::SemanticType model = lang::SemanticType::Unknown;
    mutateFirst(forged, [&](lang::MirInstruction &instruction) {
      if (instruction.intrinsic !=
              lang::IntrinsicKind::PolymorphicStorageConstruct ||
          instruction.operands.size() != 2)
        return false;
      model = instruction.operands[1].type;
      return true;
    });
    auto &classes = const_cast<std::vector<lang::MirClassInstance> &>(
        forged.classInstances());
    bool changed = false;
    for (lang::MirClassInstance &instance : classes) {
      if (instance.type == model) {
        mutation(instance);
        changed = true;
      }
    }
    expect(changed && !lang::verifyMirProgram(forged).valid(),
           std::string("verification should reject a payload whose ") + label);
  };
  rejectsModel("interface base was removed",
               [](lang::MirClassInstance &instance) {
                 instance.bases.clear();
                 instance.structuralBases.clear();
               });
  rejectsModel("cleanup became active", [](lang::MirClassInstance &instance) {
    instance.requiresActiveCleanup = true;
  });
  // An unrelated class is not derived from the interface.
  lang::SemanticType unrelated = lang::SemanticType::Unknown;
  for (const lang::MirClassInstance &instance : frontend.mir.classInstances()) {
    if (instance.bases.empty() && !instance.requiresActiveCleanup &&
        instance.compilerCapability == lang::CompilerCapabilityTypeKind::None &&
        instance.type.kind == lang::SemanticType::Class)
      unrelated = instance.type;
  }
  expect(unrelated != lang::SemanticType::Unknown,
         "the program retains an unrelated cleanup-free class");
  rejects("a payload outside the interface",
          [&](lang::MirInstruction &instruction) {
            if (instruction.intrinsic !=
                    lang::IntrinsicKind::PolymorphicStorageConstruct ||
                instruction.operands.size() != 2)
              return false;
            instruction.operands[1].type = unrelated;
            instruction.parameterTypes[1] = unrelated;
            return true;
          });
}

void testPayloadAdmission() {
  const auto analyze = [](std::string_view body) {
    return lang::Frontend().analyze(
        "polymorphic-payload-admission.gti",
        std::string("import std.polymorphic;\nimport std.string;\n"
                    "interface meter { int read(); };\n") +
            std::string(body),
        {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  };
  const lang::FrontendResult inert = analyze(R"(
class gauge : public meter {
  int value;
public:
  gauge(int value) : value(value) {}
  int read() override { return this.value; }
};
int need<M>() requires std::inline_polymorphic_payload<M, meter> { return 0; }
int main() { return need<gauge>(); }
)");
  expect(inert.canGenerateCode(),
         "a cleanup-free relocatable model is an inline payload");
  const lang::FrontendResult destructor = analyze(R"(
class noisy : public meter {
public:
  noisy() {}
  ~noisy() {}
  int read() override { return 1; }
};
int need<M>() requires std::inline_polymorphic_payload<M, meter> { return 0; }
int main() { return need<noisy>(); }
)");
  expect(!destructor.canGenerateCode(),
         "a source destructor body makes a model's cleanup observable");
  const lang::FrontendResult owning = analyze(R"(
class labelled : public meter {
  std::string label;
public:
  labelled(std::string label) : label(std::move(label)) {}
  int read() override { return 1; }
};
int need<M>() requires std::inline_polymorphic_payload<M, meter> { return 0; }
int main() { return need<labelled>(); }
)");
  expect(!owning.canGenerateCode(),
         "an owning string field makes a model's cleanup active");
}

} // namespace

int main() {
  testPolymorphicStorageContract();
  testPayloadAdmission();
  if (failures != 0) {
    std::cerr << failures << " polymorphic storage compiler test(s) failed\n";
    return 1;
  }
  std::cout << "polymorphic storage compiler tests passed\n";
  return 0;
}
