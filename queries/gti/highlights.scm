; Match Neovim's C/C++ baseline: every ordinary identifier remains visible to
; Tree-sitter, while the more specific captures below win at normal priority.
((identifier) @variable
  (#set! priority 95))

(comment) @comment @spell

(string_literal) @string
(character_literal) @character
(standard_library_path) @string
(integer_literal) @number
(float_literal) @number.float
(boolean_literal) @boolean
(nullptr_literal) @constant.builtin
(this_expression) @variable.builtin

(designated_initializer
  field: (identifier) @variable.member)

(primitive_type) @type.builtin
[
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
(class_declaration
  name: (scoped_identifier
    name: (identifier) @type.definition))
(class_attribute_list
  attribute: (identifier) @attribute)
(class_attribute_list
  capability_parameter: (identifier) @type)
(function_declaration "unsafe" @keyword)
(function_contract_attribute ["noreturn" "track_caller" "discardable" "thread_confined_callback"] @attribute)
(borrow_origin_attribute "borrowed_from" @attribute)
(borrow_origin_attribute origin: (identifier) @variable.parameter)
(native_c_array_attribute
  attribute: (identifier) @attribute
  count: (identifier) @variable.parameter)
(native_link_name_attribute
  attribute: (identifier) @attribute
  symbol: (string_literal) @string)
(enum_declaration
  name: (identifier) @type.definition)
(enumerator
  name: (identifier) @constant)
(generic_parameter
  !value_type
  name: (identifier) @type.parameter)
(generic_parameter
  constraint: (identifier) @type)
(generic_parameter
  constraint: (scoped_identifier
    name: (identifier) @type))
(generic_parameter
  value_type: "uint64_t" @type.builtin
  name: (identifier) @variable.parameter)
(generic_parameter
  value_type: "uint64" @type.builtin
  name: (identifier) @variable.parameter)
(array_extent
  size: (identifier) @variable.parameter)
(namespace_declaration
  name: (identifier) @module)
(namespace_alias_declaration
  name: (identifier) @module)
(type_alias_declaration
  name: (identifier) @type.definition)
(concept_declaration
  name: (identifier) @type.definition
  parameter: (identifier) @type.parameter)
(concept_application
  name: (identifier) @type)
(member_requirement_expression
  binding: (identifier) @variable.parameter
  receiver: (identifier) @variable.parameter
  member: (identifier) @function.method)
(concept_application
  name: (scoped_identifier
    name: (identifier) @type))
(concept_application
  argument: (type
    (user_type
      name: (identifier) @type.parameter)))

(function_declaration
  name: (identifier) @function)
(function_declaration
  name: (scoped_identifier
    name: (identifier) @function.method))
(extern_c_function_prototype
  name: (identifier) @function)
(method_declaration
  name: (identifier) @function.method)
(operator_declaration
  "operator" @keyword.operator)
(constructor_declaration
  name: (identifier) @constructor)
(special_member_specifier
  policy: [
    "default"
    "delete"
  ] @keyword.modifier)
(destructor_declaration
  name: (identifier) @constructor)
(constructor_initializer
  target: (user_type
    name: (identifier) @variable.member))
(constructor_initializer
  target: (user_type
    name: (scoped_identifier
      name: (identifier) @variable.member)))
(variable_declaration
  name: (identifier) @variable)
(structured_binding_declaration
  binding: (identifier) @variable)
(range_for_declaration
  name: (identifier) @variable)
(static_variable_declaration
  name: (identifier) @variable)
(class_body
  (variable_declaration
    name: (identifier) @variable.member))
(class_body
  (static_variable_declaration
    name: (identifier) @variable.member))
(mutable_field_group
  (grouped_field_declaration
    name: (identifier) @variable.member))
(parameter
  name: (identifier) @variable.parameter)
(lambda_capture
  name: (identifier) @variable)
(pack_expansion
  name: (identifier) @variable.parameter)
(pointer_declarator) @operator
(reference_declarator) @operator

(scoped_identifier
  scope: (identifier) @module)
(scoped_identifier
  scope: (scoped_identifier
    name: (identifier) @module))

(generic_scoped_identifier
  scope: (identifier) @type
  name: (identifier) @constant)
(generic_scoped_identifier
  scope: (scoped_identifier
    name: (identifier) @type)
  name: (identifier) @constant)

(member_expression
  member: (identifier) @variable.member)
(call_expression
  function: (member_expression
    member: (identifier) @function.method.call))
(call_expression
  function: (generic_function
    name: (member_expression
      member: (identifier) @function.method.call)))
(call_expression
  function: (primary_expression
    (identifier) @function.call))
(call_expression
  function: (primary_expression
    (scoped_identifier
      name: (identifier) @function.call)))
(call_expression
  function: (primary_expression
    (generic_scoped_identifier
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
(compiler_constraint_binding
  "compiler_constraint" @attribute)
(static_assert_declaration
  "static_assert" @keyword)

[
  "#include"
  "import"
] @keyword.import

(module_declaration name: (identifier) @module)
(module_name (identifier) @module)

[
  "namespace"
  "module"
  "class"
  "concept"
  "interface"
  "enum"
  "struct"
  "union"
  "using"
] @keyword.type

[
  "public"
  "private"
  "const"
  "constexpr"
  "mut"
  "static"
  "virtual"
  "override"
  "extern"
  "export"
] @keyword.modifier

; The shipped ABI-14 parser preserves error recovery as an identifier. Keep
; editor classification aligned with the compiler-owned reserved-name rule.
((identifier) @keyword
  (#any-of? @keyword
    "alignas" "and_eq" "asm" "bitand" "bitor" "catch" "char8_t"
    "char16_t" "char32_t" "compl" "const_cast" "consteval" "constinit"
    "co_await" "co_return" "co_yield" "decltype" "delete" "dynamic_cast"
    "explicit" "friend" "goto" "inline" "long" "mutable" "new"
    "noexcept" "not" "not_eq" "or_eq" "protected" "register"
    "reinterpret_cast" "short" "signed" "static_cast"
    "template" "thread_local" "throw" "try" "typedef" "typeid" "typename"
    "unsigned" "volatile" "wchar_t" "xor" "xor_eq")
  (#set! priority 96))

[
  "if"
  "else"
  "switch"
  "case"
  "default"
] @keyword.conditional

[
  "do"
  "for"
  "while"
  "break"
  "continue"
] @keyword.repeat

"return" @keyword.return
(unsafe_statement "unsafe" @keyword)
"unexpected" @keyword
"requires" @keyword
(layout_query_expression
  operator: [
    "sizeof"
    "alignof"
  ] @keyword.operator)

[
  "#if"
  "#ifdef"
  "#ifndef"
  "#elif"
  "#error"
  "#undef"
] @keyword.directive
"#define" @keyword.directive.define
(else_directive) @keyword.directive
(endif_directive) @keyword.directive
(defined_condition "defined" @function.macro)
(defined_condition flag: (identifier) @constant.macro)
(if_directive flag: (identifier) @constant.macro)
(configuration_directive flag: (identifier) @constant.macro)

"discard" @attribute

[
  "="
  "+="
  "-="
  "*="
  "/="
  "%="
  "&="
  "|="
  "^="
  "<<="
  ">>="
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
  "&&"
  "||"
  "++"
  "--"
  "->"
] @operator

(unary_expression
  operator: "?" @operator)

(conditional_expression
  [
    "?"
    ":"
  ] @operator)

[
  "and"
  "or"
] @keyword.operator

(generic_parameter_clause
  [
    "<"
    ">"
  ] @punctuation.bracket)

(generic_signature_pattern_clause
  [
    "<"
    ">"
  ] @punctuation.bracket)

(concept_declaration
  [
    "<"
    ">"
  ] @punctuation.bracket)

(concept_application
  [
    "<"
    ">"
  ] @punctuation.bracket)

(type_argument_clause
  [
    "<"
    ">"
  ] @punctuation.bracket)

(expected_type
  [
    "<"
    ">"
  ] @punctuation.bracket)

"..." @punctuation.special

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
  "::"
] @punctuation.delimiter
