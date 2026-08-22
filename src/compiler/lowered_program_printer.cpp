#include "gti/lowered_program_printer.h"

#include "gti/lowered_program.h"
#include "gti/mir_printer.h"

#include <iomanip>
#include <sstream>
#include <string_view>

namespace lang {
namespace {

[[nodiscard]] std::string_view bodyKindName(MirBodyKind kind) {
  switch (kind) {
  case MirBodyKind::Module:
    return "module";
  case MirBodyKind::FieldInitializers:
    return "field-initializers";
  case MirBodyKind::StaticFieldInitializers:
    return "static-field-initializers";
  case MirBodyKind::Function:
    return "function";
  case MirBodyKind::Constructor:
    return "constructor";
  case MirBodyKind::Destructor:
    return "destructor";
  case MirBodyKind::Lambda:
    return "lambda";
  case MirBodyKind::HostedStartup:
    return "hosted-startup";
  }
  return "invalid";
}

[[nodiscard]] std::string_view
bodyDefinitionName(LoweredBodyDefinitionKind kind) {
  switch (kind) {
  case LoweredBodyDefinitionKind::ImplicitSource:
    return "implicit-source";
  case LoweredBodyDefinitionKind::Source:
    return "source";
  case LoweredBodyDefinitionKind::CompilerGenerated:
    return "compiler-generated";
  case LoweredBodyDefinitionKind::RuntimeBinding:
    return "runtime-binding";
  case LoweredBodyDefinitionKind::Declaration:
    return "declaration";
  case LoweredBodyDefinitionKind::Count:
    return "invalid";
  }
  return "invalid";
}

[[nodiscard]] std::string_view bodyRoleName(LoweredBodyRole role) {
  switch (role) {
  case LoweredBodyRole::SourceExecutable:
    return "source-executable";
  case LoweredBodyRole::AbiDeclaration:
    return "abi-declaration";
  case LoweredBodyRole::DataOnly:
    return "data-only";
  case LoweredBodyRole::Count:
    return "invalid";
  }
  return "invalid";
}

[[nodiscard]] std::string_view
declarationKindName(LoweredDeclarationKind kind) {
  switch (kind) {
  case LoweredDeclarationKind::Namespace:
    return "namespace";
  case LoweredDeclarationKind::NamespaceAlias:
    return "namespace-alias";
  case LoweredDeclarationKind::TypeAlias:
    return "type-alias";
  case LoweredDeclarationKind::Class:
    return "class";
  case LoweredDeclarationKind::Enum:
    return "enum";
  case LoweredDeclarationKind::Function:
    return "function";
  case LoweredDeclarationKind::Constructor:
    return "constructor";
  case LoweredDeclarationKind::Destructor:
    return "destructor";
  case LoweredDeclarationKind::Storage:
    return "storage";
  case LoweredDeclarationKind::Access:
    return "access";
  case LoweredDeclarationKind::LanguageLinkage:
    return "language-linkage";
  case LoweredDeclarationKind::Concept:
    return "concept";
  case LoweredDeclarationKind::Empty:
    return "empty";
  case LoweredDeclarationKind::Other:
    return "other";
  case LoweredDeclarationKind::Count:
    return "invalid";
  }
  return "invalid";
}

[[nodiscard]] std::string_view
generatedItemKindName(LoweredGeneratedItemKind kind) {
  switch (kind) {
  case LoweredGeneratedItemKind::ProgramInitialization:
    return "program-initialization";
  case LoweredGeneratedItemKind::HostedEntry:
    return "hosted-entry";
  case LoweredGeneratedItemKind::StructuralOperatorAdapter:
    return "structural-operator-adapter";
  case LoweredGeneratedItemKind::CallableAdapter:
    return "callable-adapter";
  case LoweredGeneratedItemKind::LifecycleCleanup:
    return "lifecycle-cleanup";
  case LoweredGeneratedItemKind::NativeInteropAdapter:
    return "native-interop-adapter";
  case LoweredGeneratedItemKind::ConcreteInstanceAdapter:
    return "concrete-instance-adapter";
  case LoweredGeneratedItemKind::Count:
    return "invalid";
  }
  return "invalid";
}

void printGeneratedIdentity(std::ostream &output,
                            const LoweredGeneratedItemIdentity &identity) {
  output << generatedItemKindName(identity.kind) << '/' << identity.owner << '/'
         << identity.ordinal;
}

} // namespace

std::string LoweredProgramPrinter::print(const LoweredProgram &program) const {
  std::ostringstream output;
  output << "lowered-program-v1\n";
  output << "target os=" << std::quoted(program.target().os)
         << " vendor=" << std::quoted(program.target().vendor)
         << " arch=" << std::quoted(program.target().arch) << " profile="
         << executionProfileName(program.target().executionProfile)
         << " endian="
         << targetEndiannessName(program.target().dataLayout.endianness())
         << " pointer-width=" << program.target().dataLayout.pointerWidthBits()
         << '\n';
  for (std::size_t index = 0; index < targetScalarKindCount; ++index) {
    const auto kind = static_cast<TargetScalarKind>(index);
    const std::optional<TargetTypeLayout> layout =
        program.target().dataLayout.scalarLayout(kind);
    output << "target-layout " << targetScalarKindName(kind);
    if (layout) {
      output << " size=" << layout->sizeBytes
             << " abi-align=" << layout->abiAlignmentBytes
             << " preferred-align=" << layout->preferredAlignmentBytes;
    } else {
      output << " unsupported";
    }
    output << '\n';
  }

  output << "bodies " << program.bodies().size() << '\n';
  for (const LoweredBody &body : program.bodies()) {
    const LoweredBodyIdentity &identity = body.identity;
    output << "body " << bodyKindName(identity.address.kind) << '/'
           << identity.address.owner
           << " domain=" << (identity.placeDomain.snapshot == 0 ? 0 : 1) << ':'
           << identity.placeDomain.body << ':' << identity.placeDomain.revision
           << " definition=" << bodyDefinitionName(identity.definition)
           << " role=" << bodyRoleName(body.role)
           << " declaration=" << identity.declaration
           << " concrete-owner=" << identity.concreteOwner
           << " source=" << std::quoted(identity.source.source) << ':'
           << identity.source.start << ':' << identity.source.end << ':'
           << identity.source.line
           << " roots=" << body.requiredGeneratedItems.size();
    for (const LoweredGeneratedItemIdentity &required :
         body.requiredGeneratedItems) {
      output << ' ';
      printGeneratedIdentity(output, required);
    }
    output << '\n';
  }

  output << "declarations " << program.declarations().size() << '\n';
  for (const LoweredDeclaration &declaration : program.declarations()) {
    output << "declaration " << declaration.id
           << " kind=" << declarationKindName(declaration.kind)
           << " parent=" << declaration.parent
           << " semantic=" << declaration.semanticIdentity
           << " owner-class=" << declaration.ownerClass
           << " ordinal=" << declaration.ordinal
           << " generic=" << (declaration.generic ? "true" : "false")
           << " name=" << std::quoted(declaration.name) << " namespace=";
    std::string separator;
    for (const std::string &scope : declaration.namespaceScope) {
      output << separator << scope;
      separator = "::";
    }
    output << " source=" << std::quoted(declaration.source.source) << ':'
           << declaration.source.start << ':' << declaration.source.end << ':'
           << declaration.source.line << '\n';
  }

  output << "generated-items " << program.generatedItems().size() << '\n';
  for (const LoweredGeneratedItem &item : program.generatedItems()) {
    output << "generated-item ";
    printGeneratedIdentity(output, item.identity);
    output << " source=" << bodyKindName(item.sourceBody.kind) << '/'
           << item.sourceBody.owner
           << " dependencies=" << item.dependencies.size();
    for (const LoweredGeneratedItemIdentity &dependency : item.dependencies) {
      output << ' ';
      printGeneratedIdentity(output, dependency);
    }
    if (const auto *callback =
            std::get_if<LoweredNativeCallbackItem>(&item.payload)) {
      output << " payload=native-callback:" << callback->adapter.id << ':'
             << callback->adapter.target << ':'
             << (callback->adapter.targetMayRaiseDefinedFailure ? "raise"
                                                                : "no-raise")
             << ':'
             << (callback->adapter.catchesNativeExceptions ? "catch" : "leak");
    } else {
      output << " payload=none";
    }
    output << '\n';
  }

  output << "mir\n" << MirPrinter().print(program.mir());
  return output.str();
}

} // namespace lang
