#include "gti/bindgen/native_model.h"

#include <iomanip>
#include <sstream>

namespace lang::bindgen {

NativeScopeId NativeModel::addScope(NativeScope scope) {
  scopes.push_back(std::move(scope));
  return static_cast<NativeScopeId>(scopes.size());
}

NativeTypeId NativeModel::addType(NativeType type) {
  types.push_back(std::move(type));
  return static_cast<NativeTypeId>(types.size());
}

NativeDeclarationId NativeModel::addDeclaration(NativeDeclaration value) {
  declarations.push_back(std::move(value));
  return static_cast<NativeDeclarationId>(declarations.size());
}

const NativeScope *NativeModel::findScope(NativeScopeId id) const {
  return id == 0 || id > scopes.size() ? nullptr : &scopes[id - 1];
}

const NativeType *NativeModel::findType(NativeTypeId id) const {
  return id == 0 || id > types.size() ? nullptr : &types[id - 1];
}

const NativeDeclaration *
NativeModel::findDeclaration(NativeDeclarationId id) const {
  return id == 0 || id > declarations.size() ? nullptr : &declarations[id - 1];
}

NativeDeclaration *NativeModel::findDeclaration(NativeDeclarationId id) {
  return id == 0 || id > declarations.size() ? nullptr : &declarations[id - 1];
}

const NativeSourceFile *NativeModel::findSource(NativeSourceId id) const {
  return id == 0 || id > sources.size() ? nullptr : &sources[id - 1];
}

namespace {

void printType(const NativeModel &model, NativeTypeId id, std::ostream &out) {
  const NativeType *type = model.findType(id);
  if (type == nullptr) {
    out << "<invalid-type " << id << '>';
    return;
  }
  switch (type->kind) {
  case NativeTypeKind::Void:
    out << "void";
    return;
  case NativeTypeKind::Bool:
    out << "bool";
    return;
  case NativeTypeKind::Char:
    out << "char";
    return;
  case NativeTypeKind::Integer:
    if (type->scalarIdentity != NativeCScalarKind::None) {
      out << nativeCScalarKindName(type->scalarIdentity) << '[';
    }
    out << (type->isSigned ? "i" : "u") << type->bitWidth;
    if (type->scalarIdentity != NativeCScalarKind::None) {
      out << ']';
    }
    return;
  case NativeTypeKind::Floating:
    out << 'f' << type->bitWidth;
    return;
  case NativeTypeKind::Pointer:
    out << "ptr(";
    if (type->constPointee) {
      out << "const ";
    }
    printType(model, type->pointee, out);
    out << ')';
    return;
  case NativeTypeKind::Record:
  case NativeTypeKind::Enum:
  case NativeTypeKind::Alias: {
    const NativeDeclaration *target = model.findDeclaration(type->declaration);
    out << (type->kind == NativeTypeKind::Record ? "record:"
            : type->kind == NativeTypeKind::Enum ? "enum:"
                                                 : "alias:")
        << (target == nullptr ? "<invalid>" : target->name);
    return;
  }
  case NativeTypeKind::Function: {
    out << "fn(";
    for (std::size_t index = 0; index < type->parameters.size(); ++index) {
      if (index != 0) {
        out << ", ";
      }
      printType(model, type->parameters[index], out);
    }
    if (type->variadic) {
      out << (type->parameters.empty() ? "..." : ", ...");
    }
    out << ") -> ";
    printType(model, type->result, out);
    return;
  }
  case NativeTypeKind::Array:
    out << "array[";
    printType(model, type->element, out);
    out << " x " << type->count << ']';
    return;
  case NativeTypeKind::Unsupported:
    out << "unsupported('" << type->spelling << "': " << type->reason << ')';
    return;
  }
}

} // namespace

std::string printNativeModel(const NativeModel &model) {
  std::ostringstream out;
  for (const NativeDeclarationId id : model.order) {
    const NativeDeclaration *declaration = model.findDeclaration(id);
    if (declaration == nullptr) {
      continue;
    }
    const char *external = declaration->inPolicy ? "" : " (external)";
    switch (declaration->kind) {
    case NativeDeclarationKind::Record: {
      out << (declaration->recordIsUnion ? "union " : "record ")
          << declaration->name
          << (declaration->complete ? "" : " (incomplete)");
      if (declaration->layout.packed) {
        out << " [packed]";
      }
      if (declaration->layout.alignment != 0) {
        out << " [align " << declaration->layout.alignment << ']';
      }
      out << external << '\n';
      for (const NativeRecordField &field : declaration->fields) {
        out << "  field " << field.name << ": ";
        printType(model, field.type, out);
        out << '\n';
      }
      break;
    }
    case NativeDeclarationKind::Enum: {
      out << "enum " << declaration->name << " : ";
      printType(model, declaration->enumIntegerType, out);
      if (!declaration->enumLayoutMatchesInteger) {
        out << " [distinct layout]";
      }
      out << external << '\n';
      for (const NativeEnumerator &enumerator : declaration->enumerators) {
        out << "  " << enumerator.name << " = ";
        std::visit([&out](auto value) { out << value; }, enumerator.value);
        out << '\n';
      }
      break;
    }
    case NativeDeclarationKind::Typedef: {
      out << "typedef " << declaration->name << " = ";
      printType(model, declaration->aliased, out);
      out << external << '\n';
      break;
    }
    case NativeDeclarationKind::Function: {
      const NativeType *type = model.findType(declaration->type);
      out << "function " << declaration->name << '(';
      if (type != nullptr) {
        for (std::size_t index = 0; index < type->parameters.size(); ++index) {
          if (index != 0) {
            out << ", ";
          }
          if (index < declaration->parameterNames.size() &&
              !declaration->parameterNames[index].name.empty()) {
            out << declaration->parameterNames[index].name << ": ";
          }
          printType(model, type->parameters[index], out);
        }
        if (type->variadic) {
          out << (type->parameters.empty() ? "..." : ", ...");
        }
      }
      out << ") -> ";
      if (type != nullptr) {
        printType(model, type->result, out);
      }
      if (!declaration->asmLabel.empty()) {
        out << " [asm \"" << declaration->asmLabel << "\"]";
      }
      if (!declaration->callingConvention.empty()) {
        out << " [callconv " << declaration->callingConvention << ']';
      }
      out << external << '\n';
      break;
    }
    case NativeDeclarationKind::Constant:
      out << "constant " << declaration->name << " = ";
      switch (declaration->constantKind) {
      case NativeConstantKind::Integer:
        out << declaration->constantValue;
        break;
      case NativeConstantKind::Floating:
        // A fixed precision keeps the golden rendering host-independent.
        out << std::fixed << std::setprecision(6)
            << declaration->constantFloating << std::defaultfloat;
        break;
      case NativeConstantKind::String:
        out << '"' << declaration->constantText << '"';
        break;
      }
      out << external << '\n';
      break;
    }
  }
  return out.str();
}

} // namespace lang::bindgen
