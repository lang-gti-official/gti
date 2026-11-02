#include "gti/frontend.h"
#include "gti/mir_effects.h"
#include "gti/optimization/safety.h"
#include "private_stdlib_fixture.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

int main() {
  const auto root = std::filesystem::path(__FILE__).parent_path();
  std::ifstream file(root / "fixtures/optimizer_prefix_relocation.gti");
  const std::string source{std::istreambuf_iterator<char>(file), {}};
  auto frontend = gti_test::analyzeTrustedPrivateMemoryFixture(
      "optimizer-prefix-relocation.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return 1;
  }
  const auto customMove = lang::Frontend().analyze(
      "prefix-custom-move-negative.gti", std::string(R"(
class Changed { int value; public:
  Changed(int value) : value(value) {}
  Changed(Changed&& other) : value(7) {}
};
int main() { return 0; }
)"));
  bool rejectedCustomMove = false;
  for (const auto &diagnostic : customMove.diagnostics)
    rejectedCustomMove |= diagnostic.code == "GTI-S2020";
  if (customMove.canGenerateCode() || !rejectedCustomMove) {
    std::cerr
        << "custom move support changed; review relocation proof exclusion\n";
    return 1;
  }
  const auto selfMove = gti_test::analyzeTrustedPrivateMemoryFixture(
      "metadata-self-move.gti", std::string(R"(
import std.memory;
class Shell { public: Shell() {} Shell(Shell& other) = delete;
  Shell(Shell&& other) = default; ~Shell() {} };
int main() { mut Shell value{}; value = std::move(value); return 0; }
)"));
  bool rejectedSelfMove = false;
  for (const auto &diagnostic : selfMove.diagnostics)
    rejectedSelfMove |= diagnostic.code == "GTI-S2018";
  if (selfMove.canGenerateCode() || !rejectedSelfMove) {
    std::cerr << "self replacement must retain its ownership rejection\n";
    return 1;
  }
  const auto analysis = lang::analyzeMirClosedCallSafetyChecks(frontend.mir);
  if (!analysis.converged) {
    std::cerr << "relocation contextual analysis did not converge\n";
    return 1;
  }
  const auto countDischarges = [&](const auto &frontend, const auto &analysis,
                                   const std::string &name,
                                   lang::MirOperation selected =
                                       lang::MirOperation::Multiply) {
    std::size_t found = 0;
    for (const auto &function : frontend.hir.functionInstances()) {
      if (!function.source || function.source->name().lexeme != name)
        continue;
      const auto *body = analysis.find(
          {.kind = lang::MirBodyKind::Function, .owner = function.id});
      if (!body)
        continue;
      const auto *mir = frontend.mir.findFunctionInstance(function.id);
      if (!mir)
        continue;
      for (const auto &proof : body->safety.proofs) {
        if (proof.discharge.family !=
            lang::MirSafetyCheckFamily::ArithmeticOverflow)
          continue;
        for (const auto &block : mir->body.blocks)
          for (const auto &instruction : block.instructions)
            if (instruction.id == proof.instruction &&
                instruction.operation == selected)
              ++found;
      }
    }
    return found;
  };
  std::ifstream equalFile(root / "fixtures/optimizer_equal_scalar_store.gti");
  const std::string equalSource{std::istreambuf_iterator<char>(equalFile), {}};
  const auto equal = gti_test::analyzeTrustedPrivateMemoryFixture(
      "equal-scalar-store.gti", equalSource);
  if (!equal.canGenerateCode()) {
    for (const auto &d : equal.diagnostics)
      std::cerr << d.code << ": " << d.message << '\n';
    return 1;
  }
  const auto equalAnalysis = lang::analyzeMirClosedCallSafetyChecks(equal.mir);
  if (!equalAnalysis.converged ||
      countDischarges(equal, equalAnalysis, "changed_store") != 0 ||
      countDischarges(equal, equalAnalysis, "raw_store") != 0 ||
      countDischarges(equal, equalAnalysis, "conditional_store") != 0 ||
      countDischarges(equal, equalAnalysis, "overlapping_byte_store") != 0) {
    std::cerr << "equal scalar store mutation boundary failed\n";
    return 1;
  }
  for (const auto *name : {"equal_store", "same_nonzero"}) {
    const auto isolated = gti_test::analyzeTrustedPrivateMemoryFixture(
        "equal-scalar-isolated.gti",
        equalSource.substr(0, equalSource.find("int main")) +
            "int main() { return int32_t(" + name + "()); }");
    const auto isolatedAnalysis =
        lang::analyzeMirClosedCallSafetyChecks(isolated.mir);
    if (!isolated.canGenerateCode() || !isolatedAnalysis.converged ||
        countDischarges(isolated, isolatedAnalysis, name) != 1) {
      std::cerr << "equal scalar isolated postcondition failed: " << name
                << '\n';
      return 1;
    }
  }
  std::ifstream freshFile(root / "fixtures/optimizer_fresh_storage.gti");
  const std::string freshSource{std::istreambuf_iterator<char>(freshFile), {}};
  const auto freshControl = [&](std::string name, std::string declarations,
                                std::string scratch, bool shouldProve) {
    auto source = freshSource.substr(0, freshSource.find("int main"));
    source.insert(0, "import \"../std/detail/memory_capabilities.gti\"; " +
                         declarations);
    const auto first = source.find("  // scratch-begin");
    const auto last = source.find("  // scratch-end");
    source.replace(first, last - first, std::move(scratch));
    source += "int main() { return fresh_probe(int32_t(9)); }";
    const auto fixture =
        gti_test::analyzeTrustedPrivateMemoryFixture(name, source);
    if (!fixture.canGenerateCode()) {
      for (const auto &d : fixture.diagnostics)
        std::cerr << d.code << ": " << d.message << '\n';
      return false;
    }
    const auto analysis = lang::analyzeMirClosedCallSafetyChecks(fixture.mir);
    const auto count = countDischarges(fixture, analysis, "fresh_probe");
    if (!analysis.converged || count != std::size_t(shouldProve)) {
      std::cerr << "fresh-storage noninterference " << name
                << ": multiply=" << count << '\n';
      return false;
    }
    return true;
  };
  if (!freshControl("fresh-storage.gti", "",
                    "void* pointer = fresh_pointer(); unsafe { "
                    "release_pointer(pointer); }\n",
                    true) ||
      !freshControl("distinct-invocations.gti",
                    "void release_pair(void* a, void* b) { unsafe { "
                    "release_pointer(a); release_pointer(b); } }\n",
                    "void* a = fresh_pointer(); void* b = fresh_pointer(); "
                    "release_pair(a, b);\n",
                    true) ||
      !freshControl(
          "aliased-invocations.gti",
          "void release_pair(void* a, void* b) { unsafe { release_pointer(a); "
          "release_pointer(b); } }\n",
          "void* a = fresh_pointer(); void* b = a; release_pair(a, b);\n",
          false) ||
      !freshControl(
          "null-storage.gti", "",
          "void* pointer = nullptr; unsafe { release_pointer(pointer); }\n",
          true) ||
      !freshControl("fresh-escape.gti",
                    "extern \"C\" { void escape_pointer(void*); }\n",
                    "void* pointer = fresh_pointer(); unsafe { "
                    "escape_pointer(pointer); release_pointer(pointer); }\n",
                    false) ||
      !freshControl("foreign-storage.gti",
                    "extern \"C\" { void* foreign_pointer(); }\n",
                    "unsafe { void* pointer = foreign_pointer(); "
                    "release_pointer(pointer); }\n",
                    false) ||
      !freshControl("overwritten-storage.gti",
                    "extern \"C\" { void* foreign_pointer(); }\n",
                    "mut void* pointer = fresh_pointer(); unsafe { pointer = "
                    "foreign_pointer(); release_pointer(pointer); }\n",
                    false) ||
      !freshControl("double-release.gti", "",
                    "void* pointer = fresh_pointer(); unsafe { "
                    "release_pointer(pointer); release_pointer(pointer); }\n",
                    false) ||
      !freshControl("released-return.gti",
                    "void* dangling_pointer() { void* p = fresh_pointer(); "
                    "unsafe { release_pointer(p); } return p; }\n",
                    "void* pointer = dangling_pointer(); unsafe { "
                    "release_pointer(pointer); }\n",
                    false) ||
      !freshControl(
          "conditionally-released-return.gti",
          "void* maybe_dangling(bool release) { void* p = fresh_pointer(); if "
          "(release) { unsafe { release_pointer(p); } } return p; }\n",
          "void* pointer = maybe_dangling(false); unsafe { "
          "release_pointer(pointer); }\n",
          false) ||
      !freshControl("global-release.gti", "mut void* old_pointer = nullptr;\n",
                    "unsafe { release_pointer(old_pointer); }\n", false) ||
      !freshControl("fresh-global-escape.gti",
                    "mut void* old_pointer = nullptr;\n",
                    "void* pointer = fresh_pointer(); old_pointer = pointer; "
                    "unsafe { release_pointer(pointer); }\n",
                    false))
    return 1;
  const auto discharges = [&](const std::string &name) {
    return countDischarges(frontend, analysis, name);
  };
  std::ifstream endpointFile(root / "fixtures/optimizer_prefix_endpoints.gti");
  const std::string endpointSource{std::istreambuf_iterator<char>(endpointFile),
                                   {}};
  const auto endpoints = gti_test::analyzeTrustedPrivateMemoryFixture(
      "prefix-endpoints.gti", endpointSource);
  if (!endpoints.canGenerateCode()) {
    for (const auto &d : endpoints.diagnostics)
      std::cerr << d.code << ": " << d.message << '\n';
    return 1;
  }
  const auto selfTransfer = gti_test::analyzeTrustedPrivateMemoryFixture(
      "prefix-overlapping-carriers.gti",
      endpointSource.substr(
          0, endpointSource.find("uint64_t fresh_endpoint_positive")) +
          "int main() { mut Parcel value{uint64_t(4)}; value.send(value); "
          "return 0; }");
  bool rejectedOverlap = false;
  for (const auto &d : selfTransfer.diagnostics)
    rejectedOverlap |= d.code == "GTI-S2017";
  if (selfTransfer.canGenerateCode() || !rejectedOverlap) {
    std::cerr << "overlapping mutable carrier transfer lost its rejection\n";
    return 1;
  }
  const auto endpointAnalysis =
      lang::analyzeMirClosedCallSafetyChecks(endpoints.mir);
  if (!endpointAnalysis.converged)
    return 1;
  std::ifstream crossFile(root / "fixtures/optimizer_prefix_cross_field.gti");
  const std::string crossSource{std::istreambuf_iterator<char>(crossFile), {}};
  const auto cross = gti_test::analyzeTrustedPrivateMemoryFixture(
      "prefix-cross-field.gti", crossSource);
  if (!cross.canGenerateCode()) {
    for (const auto &d : cross.diagnostics)
      std::cerr << d.code << ": " << d.message << '\n';
    return 1;
  }
  const auto crossAnalysis = lang::analyzeMirClosedCallSafetyChecks(cross.mir);
  if (!crossAnalysis.converged ||
      countDischarges(cross, crossAnalysis, "cross_field_failure",
                      lang::MirOperation::Add) != 0) {
    std::cerr
        << "cross-field replay erased the nonzero source endpoint overflow\n";
    return 1;
  }
  if (countDischarges(endpoints, endpointAnalysis,
                      "reserved_endpoint_positive") != 1) {
    std::cerr << "completed reserve length postcondition was not replayed\n";
    return 1;
  }
  if (countDischarges(endpoints, endpointAnalysis,
                      "opaque_postcondition_negative") != 0 ||
      countDischarges(endpoints, endpointAnalysis,
                      "conditional_postcondition_negative") != 0) {
    std::cerr << "unproved opaque or conditional receiver mutation retained an "
                 "old value\n";
    return 1;
  }
  const auto cleanupStates = [&](const std::string &name) {
    std::pair<unsigned, unsigned> states{};
    for (const auto &function : endpoints.hir.functionInstances()) {
      if (!function.source || function.source->name().lexeme != name)
        continue;
      const auto *mir = endpoints.mir.findFunctionInstance(function.id);
      if (!mir)
        continue;
      for (const auto &block : mir->body.blocks)
        for (const auto &instruction : block.instructions) {
          if (instruction.kind != lang::MirInstructionKind::Drop ||
              instruction.lifecycle.size() != 1)
            continue;
          const auto &event = instruction.lifecycle.front();
          const auto *drop = mir->body.findDropObligation(event.source);
          if (!drop || drop->kind != lang::MirDropObligationKind::Binding ||
              !drop->dropType.destructor)
            continue;
          if (lang::mirDropFollowsDefiniteMove(mir->body, block, drop->id,
                                               instruction.id))
            ++states.first;
          else
            ++states.second;
        }
    }
    return states;
  };
  if (!cleanupStates("moved_cleanup_positive").first ||
      !cleanupStates("conditional_cleanup").second ||
      !cleanupStates("reinitialized_cleanup").second) {
    std::cerr << "moved/live cleanup profile controls were not exercised\n";
    return 1;
  }
  const auto empty = discharges("empty_endpoint_positive");
  const auto fresh =
      countDischarges(endpoints, endpointAnalysis, "fresh_endpoint_positive");
  const auto grown =
      countDischarges(endpoints, endpointAnalysis, "grown_endpoint_positive");
  const auto grownValue =
      countDischarges(endpoints, endpointAnalysis, "grown_value_control");
  const auto metadata = discharges("metadata_move_positive");
  const auto metadataCaptured = discharges("metadata_move_captured");
  const auto positive = discharges("transferred_borrow_positive");
  const auto view = discharges("view_borrow_positive");
  const auto replaced = discharges("replaced_base");
  const auto scalar =
      discharges("transferred_positive") + discharges("view_positive");
  const auto changed = discharges("changed_destination");
  const auto partial = discharges("partial_transfer");
  const auto failed = discharges("failing_transfer");
  if (empty != 1 || fresh != 1 || grown != 1 || grownValue != 0 ||
      metadata != 1 || metadataCaptured != 0 || positive != 1 || view != 1 ||
      changed != 0 || failed != 0 || replaced != 0 || partial != 0 ||
      scalar != 0) {
    std::cerr << "prefix relocation proof mismatch: successful=" << positive
              << " empty=" << empty << " fresh=" << fresh << " grown=" << grown
              << " grown-value=" << grownValue << " metadata=" << metadata
              << " captured=" << metadataCaptured << " raw-view=" << view
              << " replaced=" << replaced << " overwritten=" << changed
              << " scalar-getters=" << scalar << " partial=" << partial
              << " failed=" << failed << '\n';
    return 1;
  }
  const auto sums = gti_test::analyzeTrustedPrivateMemoryFixture(
      "passive-sum-transfer.gti", std::string(R"(
class Active { public: Active() {} ~Active() {} };
enum Passive { number(uint64_t value), flag(bool value) };
enum Owning { owned(std::unique_ptr<int32_t> value) };
enum Cleaned { active(Active value) };
Passive move_passive(Passive value) { return std::move(value); }
Owning move_owning(Owning value) { return std::move(value); }
Cleaned move_cleaned(Cleaned value) { return std::move(value); }
int main() {
  mut Passive p = move_passive(Passive::number(uint64_t(9)));
  mut Owning o = move_owning(Owning::owned(std::make_unique<int32_t>(int32_t(9))));
  mut Cleaned c = move_cleaned(Cleaned::active(Active()));
  return 0;
}
)"));
  if (!sums.canGenerateCode()) {
    for (const auto &d : sums.diagnostics)
      std::cerr << d.code << ": " << d.message << '\n';
    return 1;
  }
  std::size_t passiveTransfers = 0, activeRejections = 0;
  for (const auto &function : sums.hir.functionInstances()) {
    if (!function.source)
      continue;
    const auto name = function.source->name().lexeme;
    if (name != "move_passive" && name != "move_owning" &&
        name != "move_cleaned")
      continue;
    const auto *mir = sums.mir.findFunctionInstance(function.id);
    for (const auto &block : mir->body.blocks)
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Move ||
            instruction.info.type.kind != lang::SemanticType::Enum)
          continue;
        const bool structural =
            lang::isMirStructuralValueTransfer(sums.mir, instruction);
        if ((name == "move_passive") != structural) {
          std::cerr << "incorrect passive enum transfer classification: "
                    << name << '\n';
          return 1;
        }
        if (structural) {
          ++passiveTransfers;
          const auto effects = lang::effects(sums.mir, instruction);
          auto malformed = instruction;
          malformed.rawMemoryAccess = true;
          if (effects.invokesUserCode || effects.invokesRuntime ||
              effects.writesUnknownMemory || effects.removableWhenUnused ||
              lang::isMirStructuralValueTransfer(sums.mir, malformed))
            return 1;
        } else {
          ++activeRejections;
        }
      }
  }
  if (!passiveTransfers || activeRejections < 2) {
    std::cerr << "passive enum controls did not exercise all move shapes\n";
    return 1;
  }
  lang::FrontendOptions processOptions;
  processOptions.target.executionProfile = lang::ExecutionProfile::Concurrent;
  const auto process = gti_test::analyzeTrustedPrivateMemoryFixture(
      "process-scalar-profile.gti", std::string(R"(
import "../std/detail/memory_capabilities.gti";
import std.atomic;
extern "C" { void* ordinary_allocator(uint64_t bytes); void mutate_address(uint64_t* address); }
class ScalarBox { public: mut uint64_t value; mut int32_t status;
  ScalarBox() : value(9), status(9) {} };
uint64_t sealed_allocate(mut ScalarBox& box) {
  unsafe {
#if target.os == "windows"
    void* storage = gti_internal::runtime::gti_rt_aligned_malloc_v1(uint64_t(16), uint64_t(8));
    gti_internal::runtime::gti_rt_aligned_free_v1(storage);
#else
    void* storage = gti_internal::runtime::gti_rt_malloc_v1(uint64_t(16));
    gti_internal::runtime::gti_rt_posix_free_v1(storage);
#endif
  }
  return box.value * uint64_t(2);
}
uint64_t foreign_allocate(mut ScalarBox& box) {
  unsafe { void* ignored = ordinary_allocator(uint64_t(16)); }
  return box.value * uint64_t(2);
}
int32_t status_overlap(mut ScalarBox& box) {
  unsafe {
#if target.os == "windows"
    // Windows has no POSIX status-pointer adapter. Exercise the foreign
    // boundary here; the POSIX platforms own the status-overlap assertion.
    void* ignored = ordinary_allocator(uint64_t(16));
#else
    void* storage = gti_internal::runtime::gti_rt_posix_memalign_v1(
        uint64_t(16), uint64_t(8), &box.status);
    gti_internal::runtime::gti_rt_posix_free_v1(storage);
#endif
  }
  return box.status * int32_t(2);
}
class AllocatedShell { mut void* storage = nullptr; public:
  AllocatedShell() {
    std::layout shape = std::layout_of<uint64_t>();
    unsafe {
#if target.os == "windows"
      this.storage = gti_internal::runtime::gti_rt_aligned_malloc_v1(shape.bytes, shape.alignment);
#else
      this.storage = gti_internal::runtime::gti_rt_malloc_v1(shape.bytes);
#endif
    }
  }
  ~AllocatedShell() {
    unsafe {
#if target.os == "windows"
      gti_internal::runtime::gti_rt_aligned_free_v1(this.storage);
#else
      gti_internal::runtime::gti_rt_posix_free_v1(this.storage);
#endif
    }
  }
};
class ForeignShell { public:
  ForeignShell() { unsafe { void* ignored = ordinary_allocator(uint64_t(16)); } }
};
class ReplacingShell { public:
  ReplacingShell(mut ScalarBox& box) { box.value = uint64_t(18446744073709551615); }
};
uint64_t layout_preserves(mut ScalarBox& box) {
  std::layout descriptor = std::layout_of<ScalarBox>();
  return box.value * uint64_t(2);
}
class AddressPacket { public: uint64_t* address = nullptr;
  AddressPacket(uint64_t* address) : address(address) {}
};
uint64_t transport_preserves(mut ScalarBox& box, uint64_t* address) {
  AddressPacket packet{address};
  return box.value * uint64_t(2);
}
uint64_t transport_then_write(mut ScalarBox& box, uint64_t* address) {
  AddressPacket packet{address};
  unsafe { mutate_address(packet.address); }
  return box.value * uint64_t(2);
}
class NumericPacket { mut uint64_t value; public:
  NumericPacket(uint64_t value) : value(value) {}
  uint64_t take() && { return std::move(this.value); }
  uint64_t replace(mut ScalarBox& box) && {
    box.value = uint64_t(18446744073709551615);
    return std::move(this.value);
  }
};
class PointerPacket { mut uint64_t value; void* pointer = nullptr; public:
  PointerPacket() : value(9), pointer(nullptr) {}
  uint64_t take() && { return std::move(this.value); }
};
class WritingPointerPacket { uint64_t* pointer = nullptr; public:
  WritingPointerPacket(uint64_t* pointer) : pointer(pointer) {}
  void replace() && { unsafe { *this.pointer = uint64_t(18446744073709551615); } }
  uint64_t* release() && { return this.pointer; }
};
uint64_t prepared_pointer_write(mut ScalarBox& box) {
  unsafe {
    mut WritingPointerPacket packet{&box.value};
    std::move(packet).replace();
  }
  return box.value * uint64_t(2);
}
uint64_t prepared_pointer_escape(mut ScalarBox& box) {
  unsafe {
    mut WritingPointerPacket packet{&box.value};
    uint64_t* pointer = std::move(packet).release();
    mutate_address(pointer);
  }
  return box.value * uint64_t(2);
}
class CleanupPacket { mut uint64_t value; public:
  CleanupPacket() : value(9) {}
  uint64_t take() && { return std::move(this.value); }
  ~CleanupPacket() { unsafe { void* ignored = ordinary_allocator(uint64_t(16)); } }
};
uint64_t prepared_preserves(mut ScalarBox& box) {
  mut NumericPacket packet{uint64_t(9)};
  uint64_t value = std::move(packet).take();
  return box.value * uint64_t(2);
}
uint64_t prepared_argument_replaces(mut ScalarBox& box) {
  mut NumericPacket packet{uint64_t(9)};
  uint64_t value = std::move(packet).replace(box);
  return box.value * uint64_t(2);
}
uint64_t prepared_pointer(mut ScalarBox& box) {
  mut PointerPacket packet{};
  uint64_t value = std::move(packet).take();
  return box.value * uint64_t(2);
}
uint64_t prepared_cleanup(mut ScalarBox& box) {
  mut CleanupPacket packet{};
  uint64_t value = std::move(packet).take();
  return box.value * uint64_t(2);
}
enum ScalarChoice { value(uint64_t value), other(uint64_t value) };
uint64_t payload_preserves(mut ScalarBox& box) {
  mut ScalarChoice choice = ScalarChoice::value(uint64_t(7));
  switch (std::move(choice)) {
    case ScalarChoice::value(value): return box.value * uint64_t(2);
    case ScalarChoice::other(value): return box.value * uint64_t(2);
  }
}
uint64_t payload_replaces(mut ScalarBox& box) {
  mut ScalarChoice choice = ScalarChoice::value(uint64_t(7));
  switch (std::move(choice)) {
    case ScalarChoice::value(value):
      box.value = uint64_t(18446744073709551615);
      return box.value * uint64_t(2);
    case ScalarChoice::other(value):
      box.value = uint64_t(18446744073709551615);
      return box.value * uint64_t(2);
  }
}
uint64_t allocated_owner_preserves(mut ScalarBox& box) {
  mut std::detail::prefix_owner<uint64_t> storage{uint64_t(4)};
  return box.value * uint64_t(2);
}
uint64_t allocated_owner_synchronizes(mut ScalarBox& box) {
  mut std::detail::prefix_owner<uint64_t> storage{uint64_t(4)};
  std::atomic<uint64_t> signal{uint64_t(0)};
  uint64_t observed = signal.load(std::memory_order_acquire());
  return box.value * uint64_t(2);
}
uint64_t allocated_owner_opaque(mut ScalarBox& box) {
  mut std::detail::prefix_owner<uint64_t> storage{uint64_t(4)};
  unsafe { void* ignored = ordinary_allocator(uint64_t(16)); }
  return box.value * uint64_t(2);
}
uint64_t constructor_preserves(mut ScalarBox& box) {
  AllocatedShell storage{};
  return box.value * uint64_t(2);
}
uint64_t constructor_foreign(mut ScalarBox& box) {
  ForeignShell storage{};
  return box.value * uint64_t(2);
}
uint64_t constructor_replaces(mut ScalarBox& box) {
  ReplacingShell storage{box};
  return box.value * uint64_t(2);
}
int main() {
  mut ScalarBox first{}; uint64_t a = sealed_allocate(first);
  mut ScalarBox second{}; uint64_t b = foreign_allocate(second);
  mut ScalarBox third{}; int32_t c = status_overlap(third);
  mut ScalarBox layoutBox{}; uint64_t l = layout_preserves(layoutBox);
  mut ScalarBox transported{}; uint64_t t = transport_preserves(transported, nullptr);
  mut ScalarBox written{}; uint64_t w = transport_then_write(written, nullptr);
  mut ScalarBox seventh{}; uint64_t g = prepared_preserves(seventh);
  mut ScalarBox eighth{}; uint64_t h = prepared_argument_replaces(eighth);
  mut ScalarBox ninth{}; uint64_t i = prepared_pointer(ninth);
  mut ScalarBox tenth{}; uint64_t j = prepared_cleanup(tenth);
  mut ScalarBox pointerWrite{}; uint64_t pw = prepared_pointer_write(pointerWrite);
  mut ScalarBox pointerEscape{}; uint64_t pe = prepared_pointer_escape(pointerEscape);
  mut ScalarBox payload{}; uint64_t p = payload_preserves(payload);
  mut ScalarBox replacedPayload{}; uint64_t q = payload_replaces(replacedPayload);
  mut ScalarBox allocated{}; uint64_t ao = allocated_owner_preserves(allocated);
  mut ScalarBox synchronized{}; uint64_t as = allocated_owner_synchronizes(synchronized);
  mut ScalarBox opaque{}; uint64_t ax = allocated_owner_opaque(opaque);
  mut ScalarBox fourth{}; uint64_t d = constructor_preserves(fourth);
  mut ScalarBox fifth{}; uint64_t e = constructor_foreign(fifth);
  mut ScalarBox sixth{}; uint64_t f = constructor_replaces(sixth);
  return 0;
}
)"), processOptions);
  if (!process.canGenerateCode()) {
    for (const auto &diagnostic : process.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return 1;
  }
  const auto processAnalysis =
      lang::analyzeMirClosedCallSafetyChecks(process.mir);
  const auto processProofs = [&](const std::string &name) {
    std::size_t count = 0;
    for (const auto &function : process.hir.functionInstances()) {
      if (!function.source || function.source->name().lexeme != name)
        continue;
      const auto *analysisBody = processAnalysis.find(
          {.kind = lang::MirBodyKind::Function, .owner = function.id});
      const auto *mir = process.mir.findFunctionInstance(function.id);
      if (!analysisBody || !mir)
        continue;
      for (const auto &proof : analysisBody->safety.proofs)
        for (const auto &block : mir->body.blocks)
          for (const auto &instruction : block.instructions)
            if (instruction.id == proof.instruction &&
                instruction.operation == lang::MirOperation::Multiply &&
                proof.discharge.family ==
                    lang::MirSafetyCheckFamily::ArithmeticOverflow)
              ++count;
    }
    return count;
  };
  if (!processAnalysis.converged || processProofs("sealed_allocate") != 1 ||
      processProofs("foreign_allocate") != 0 ||
      processProofs("status_overlap") != 0 ||
      processProofs("layout_preserves") != 1 ||
      processProofs("constructor_preserves") != 1 ||
      processProofs("allocated_owner_preserves") != 1 ||
      processProofs("allocated_owner_synchronizes") != 0 ||
      processProofs("allocated_owner_opaque") != 0 ||
      processProofs("constructor_foreign") != 0 ||
      processProofs("constructor_replaces") != 0 ||
      processProofs("prepared_preserves") != 1 ||
      processProofs("prepared_argument_replaces") != 0 ||
      processProofs("prepared_pointer") != 1 ||
      processProofs("prepared_pointer_write") != 0 ||
      processProofs("prepared_pointer_escape") != 0 ||
      processProofs("prepared_cleanup") != 0 ||
      processProofs("transport_preserves") != 1 ||
      processProofs("transport_then_write") != 0 ||
      processProofs("payload_preserves") != 2 ||
      processProofs("payload_replaces") != 0) {
    std::cerr << "process scalar profile: sealed="
              << processProofs("sealed_allocate")
              << " foreign=" << processProofs("foreign_allocate")
              << " owner=" << processProofs("allocated_owner_preserves")
              << " sync=" << processProofs("allocated_owner_synchronizes")
              << " opaque=" << processProofs("allocated_owner_opaque")
              << " pointer=" << processProofs("prepared_pointer")
              << " pointer-write=" << processProofs("prepared_pointer_write")
              << " pointer-escape=" << processProofs("prepared_pointer_escape")
              << " payload=" << processProofs("payload_preserves")
              << " payload-replaced=" << processProofs("payload_replaces")
              << " status=" << processProofs("status_overlap") << '\n';
    return 1;
  }
  std::ifstream concurrentFile(root / "fixtures/optimizer_prefix_receiver_concurrent.gti");
  std::string concurrentSource{std::istreambuf_iterator<char>(concurrentFile), {}};
  std::ifstream queryFile(root / "fixtures/optimizer_prefix_query_probe.gti");
  std::string querySource{std::istreambuf_iterator<char>(queryFile), {}};
  const auto replaceText = [](std::string &text, const std::string &from,
                              const std::string &to) {
    const auto where = text.find(from);
    if (where == std::string::npos)
      return false;
    text.replace(where, from.size(), to);
    return true;
  };
  if (!replaceText(concurrentSource, "import std.prefix_query_probe;", "") ||
      !replaceText(querySource, "export module prefix_query_probe;", "") ||
      !replaceText(querySource, "export int repeated_alignment", "int repeated_alignment") ||
      !replaceText(querySource, "\"detail/memory_capabilities.gti\"",
                   "\"../std/detail/memory_capabilities.gti\"")) {
    std::cerr << "repeated-observation fixture assembly changed\n";
    return 1;
  }
  const auto repeated = gti_test::analyzeTrustedPrivateMemoryFixture(
      "repeated-observations.gti", querySource + concurrentSource, processOptions);
  if (!repeated.canGenerateCode()) {
    for (const auto &diagnostic : repeated.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return 1;
  }
  const auto repeatedAnalysis = lang::analyzeMirClosedCallSafetyChecks(repeated.mir);
  if (countDischarges(repeated, repeatedAnalysis,
                      "synchronized_receiver_postcondition") != 0) {
    std::cerr << "synchronizing receiver call synthesized an unproved "
                 "postcondition\n";
    return 1;
  }
  unsigned checkedAdds = 0;
  unsigned dischargedAdds = 0;
  bool atomicLoad = false, alignmentQuery = false;
  for (const auto &function : repeated.hir.functionInstances()) {
    const auto *mir = repeated.mir.findFunctionInstance(function.id);
    if (!mir)
      continue;
    for (const auto &block : mir->body.blocks)
      for (const auto &instruction : block.instructions) {
        atomicLoad |= instruction.synchronization.kind ==
                      lang::SynchronizationOperationKind::AtomicLoad;
        if (instruction.functionTarget)
          if (const auto *target = repeated.mir.findFunctionInstance(*instruction.functionTarget))
            alignmentQuery |= target->processMemoryOperation ==
                              lang::ProcessMemoryRuntimeOperation::AddressIsAligned;
        if (!function.source ||
            (function.source->name().lexeme != "repeated_atomic" &&
             function.source->name().lexeme != "repeated_alignment") ||
            instruction.operation != lang::MirOperation::Add)
          continue;
        const auto isMaximum = [&](const auto &self, const lang::MirOperand &operand,
                                    unsigned remaining) -> bool {
          if (!remaining)
            return false;
          if (operand.literal)
            if (const auto *value = std::get_if<std::uint64_t>(&*operand.literal))
              return *value == 2147483647;
          if (operand.kind != lang::MirOperandKind::Value)
            return false;
          for (const auto &defBlock : mir->body.blocks)
            for (const auto &definition : defBlock.instructions)
              if (definition.result == operand.value) {
                if (definition.literal)
                  if (const auto *value = std::get_if<std::uint64_t>(&*definition.literal))
                    return *value == 2147483647;
                if (definition.operands.size() == 1 &&
                    (definition.operation == lang::MirOperation::Convert ||
                     definition.operation == lang::MirOperation::Identity ||
                     definition.intrinsic == lang::IntrinsicKind::NumericAliasConversion ||
                     definition.intrinsic == lang::IntrinsicKind::NumericTypeParameterConversion))
                  return self(self, definition.operands.front(), remaining - 1);
              }
          return false;
        };
        if (instruction.operands.size() != 2 ||
            !isMaximum(isMaximum, instruction.operands.back(), 4))
          continue; // The loop's increment is unrelated to the observation.
        ++checkedAdds;
        if (const auto *summary = repeatedAnalysis.find(
                {.kind = lang::MirBodyKind::Function, .owner = function.id}))
          for (const auto &proof : summary->safety.proofs)
            dischargedAdds += proof.instruction == instruction.id &&
                              proof.discharge.family == lang::MirSafetyCheckFamily::ArithmeticOverflow;
      }
  }
  if (!repeatedAnalysis.converged || checkedAdds != 2 || dischargedAdds ||
      !atomicLoad || !alignmentQuery) {
    std::cerr << "repeated observation checks: adds=" << checkedAdds
              << " discharged=" << dischargedAdds << " atomic=" << atomicLoad
              << " query=" << alignmentQuery << '\n';
    return 1;
  }
  return 0;
}
