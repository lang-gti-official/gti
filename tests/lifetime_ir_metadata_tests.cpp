#include "gti/mir_printer.h"

#include <cstdlib>
#include <iostream>

using namespace lang;
namespace {
void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
SemanticType reference(GenericParameterId id) {
  auto type = SemanticType::referenceTo(SemanticType(SemanticType::Int32));
  type.referenceLifetime = Lifetime::parameter(id);
  return type;
}
} // namespace
int main() {
  MirBody body;
  body.returnType = reference(1);
  body.entry = 1;
  MirBlock block;
  block.id = 1;
  MirInstruction call;
  call.id = 1;
  call.kind = MirInstructionKind::Call;
  call.info.type = reference(1);
  call.parameterTypes = {reference(1)};
  call.lifetimeArguments = {Lifetime::parameter(1)};
  call.lifetimeBindings = {{Lifetime::parameter(2), Lifetime::parameter(1)}};
  block.instructions.push_back(call);
  body.blocks.push_back(block);
  const MirPrinter printer;
  const auto original = printer.print(body);
  expect(
      original.starts_with("mir-body-v94\n"),
      "lifetime-bearing structural metadata requires its own format version");
  auto changed = body;
  changed.returnType.referenceLifetime = Lifetime::staticLifetime();
  expect(
      printer.print(changed) != original,
      "changing a return region must invalidate a structural body fingerprint");
  changed = body;
  changed.blocks[0].instructions[0].lifetimeArguments[0] =
      Lifetime::staticLifetime();
  expect(printer.print(changed) != original,
         "generic lifetime arguments must participate in structural identity");
  changed = body;
  changed.blocks[0].instructions[0].lifetimeBindings[0].second =
      Lifetime::staticLifetime();
  expect(printer.print(changed) != original,
         "changing an actual contract substitution must invalidate the proof "
         "fingerprint");
  changed = body;
  changed.blocks[0].instructions[0].lifetimeBindings[0].first =
      Lifetime::parameter(3);
  expect(printer.print(changed) != original,
         "a contract binding's declaration identity is part of its proof");
  auto callable = SemanticType::functionSignature(reference(4), {reference(4)});
  callable.quantifiedLifetimes = {4};
  body.returnType = callable;
  changed = body;
  changed.returnType.quantifiedLifetimes.clear();
  expect(printer.print(changed) != printer.print(body),
         "a quantified callable and a free-region callable cannot share a "
         "fingerprint");
  SemanticType view(SemanticType::StringView);
  view.lifetimeArguments = {Lifetime::parameter(1)};
  body.returnType = view;
  changed = body;
  changed.returnType.lifetimeArguments = {Lifetime::staticLifetime()};
  expect(
      printer.print(changed) != printer.print(body),
      "borrowing value regions must participate in structural type identity");
  auto projection = SemanticType::typeParameter(0);
  projection.projectionConcept = 77;
  projection.projectionMember = 0;
  projection.arguments = {SemanticType::typeParameter(5)};
  body.returnType = projection;
  changed = body;
  changed.returnType.projectionMember = 1;
  expect(printer.print(changed) != printer.print(body),
         "distinct associated members in exact type bounds need distinct "
         "fingerprints");
  changed = body;
  changed.returnType.projectionConcept = 78;
  expect(printer.print(changed) != printer.print(body),
         "associated concept identity cannot be replaced by its argument "
         "spelling");
  std::cout << "lifetime IR structural metadata passed\n";
}
