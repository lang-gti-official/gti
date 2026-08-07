(comment) @comment

(string_literal) @string
(standard_library_path) @string
(integer_literal) @number
(float_literal) @number.float
(boolean_literal) @boolean
(nullptr_literal) @constant.builtin
(self_expression) @variable.builtin

(primitive_type) @type.builtin
[
  "string"
  "nullptr_t"
  "void"
  "expected"
  "auto"
] @type.builtin

(user_type
  name: (identifier) @type)
(user_type
  name: (scoped_identifier
    name: (identifier) @type))

(class_declaration
  name: (identifier) @type.definition)
(generic_parameter
  !value_type
  name: (identifier) @type.parameter)
(generic_parameter
  value_type: "uint64" @type.builtin
  name: (identifier) @variable.parameter)
(array_extent
  size: (identifier) @variable.parameter)
(namespace_declaration
  name: (identifier) @module)
(namespace_alias_declaration
  name: (identifier) @module)

(function_declaration
  name: (identifier) @function)
(method_declaration
  name: (identifier) @function.method)
(operator_declaration
  "operator" @keyword.operator)
(constructor_declaration
  name: (identifier) @constructor)
(destructor_declaration
  name: (identifier) @constructor)
(constructor_initializer
  field: (identifier) @variable.member)
(variable_declaration
  name: (identifier) @variable)
(class_body
  (variable_declaration
    name: (identifier) @variable.member))
(parameter
  name: (identifier) @variable.parameter)
(lambda_capture
  name: (identifier) @variable)
(pack_expansion
  name: (identifier) @variable.parameter)
(reference_declarator) @operator

(scoped_identifier
  scope: (identifier) @module)
(scoped_identifier
  scope: (scoped_identifier
    name: (identifier) @module))

(member_expression
  member: (identifier) @variable.member)
(call_expression
  function: (member_expression
    member: (identifier) @function.method.call))
(call_expression
  function: (primary_expression
    (identifier) @function.call))
(call_expression
  function: (primary_expression
    (scoped_identifier
      name: (identifier) @function.call)))
(call_expression
  function: (generic_function
    name: (identifier) @function.call))
(call_expression
  function: (generic_function
    name: (scoped_identifier
      name: (identifier) @function.call)))

(target_condition
  "target" @constant.builtin
  property: [
    "os"
    "vendor"
    "arch"
  ] @property)
(runtime_binding
  "runtime" @attribute)

[
  "include"
] @keyword.import

[
  "namespace"
  "class"
  "struct"
] @keyword.type

[
  "public"
  "private"
  "mut"
] @keyword.modifier

[
  "if"
  "else"
  "for"
  "while"
  "break"
  "continue"
  "return"
  "unexpected"
] @keyword

[
  "#if"
  "#elif"
] @keyword.directive
(else_directive) @keyword.directive
(endif_directive) @keyword.directive

"discard" @attribute

[
  "="
  "+="
  "-="
  "or"
  "and"
  "|"
  "^"
  "&"
  "=="
  "!="
  "<"
  "<="
  ">"
  ">="
  "<<"
  ">>"
  "+"
  "-"
  "*"
  "/"
  "%"
  "!"
  "~"
  "++"
  "--"
  "::"
  "->"
  "..."
] @operator

[
  "("
  ")"
  "["
  "]"
  "{"
  "}"
] @punctuation.bracket

[
  ","
  ";"
  ":"
  "."
] @punctuation.delimiter
