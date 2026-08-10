; Keep structural scope information aligned with the C/C++ queries. Resolved
; binding roles, including parameter references, are supplied by gti_lsp.

(source_file) @local.scope

(namespace_declaration
  name: (identifier) @local.definition.namespace
  body: (namespace_body) @local.scope)

(class_declaration
  name: (identifier) @local.definition.type
  body: (class_body) @local.scope)

(enum_declaration
  name: (identifier) @local.definition.type)

(enumerator
  name: (identifier) @local.definition.var)

(type_alias_declaration
  name: (identifier) @local.definition.type)

(concept_declaration
  name: (identifier) @local.definition.type
  parameter: (identifier) @local.definition.type) @local.scope

(generic_parameter
  !value_type
  name: (identifier) @local.definition.type)

(generic_parameter
  value_type: "uint64_t"
  name: (identifier) @local.definition.parameter)

(generic_parameter
  value_type: "uint64"
  name: (identifier) @local.definition.parameter)

(function_declaration
  name: (identifier) @local.definition.function) @local.scope

(extern_c_function_prototype
  name: (identifier) @local.definition.function) @local.scope

(method_declaration
  name: (identifier) @local.definition.method) @local.scope

(constructor_declaration
  name: (identifier) @local.definition.method) @local.scope

(destructor_declaration
  name: (identifier) @local.definition.method) @local.scope

(operator_declaration) @local.scope
(lambda_expression) @local.scope
(block) @local.scope
(for_statement) @local.scope
(switch_body) @local.scope

(parameter
  name: (identifier) @local.definition.parameter)

(variable_declaration
  name: (identifier) @local.definition.var)

(structured_binding_declaration
  binding: (identifier) @local.definition.var)

(range_for_declaration
  name: (identifier) @local.definition.var)

(static_variable_declaration
  name: (identifier) @local.definition.var)

(class_body
  (variable_declaration
    name: (identifier) @local.definition.field))

(class_body
  (static_variable_declaration
    name: (identifier) @local.definition.field))

(lambda_capture
  name: (identifier) @local.definition.var)

(identifier) @local.reference
