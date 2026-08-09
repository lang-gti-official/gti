; rainbow-delimiters.nvim uses @container to derive nesting and @delimiter for
; the paired tokens. Keep this structural: ordinary punctuation highlighting
; remains in highlights.scm.

(runtime_binding
  "(" @delimiter
  ")" @delimiter) @container

(enum_declaration
  "{" @delimiter
  "}" @delimiter) @container

(namespace_body
  "{" @delimiter
  "}" @delimiter) @container

(class_body
  "{" @delimiter
  "}" @delimiter) @container

(constructor_initializer_argument_list
  "(" @delimiter
  ")" @delimiter) @container

(destructor_declaration
  "(" @delimiter
  ")" @delimiter) @container

(parameter_clause
  "(" @delimiter
  ")" @delimiter) @container

(generic_parameter_clause
  "<" @delimiter
  ">" @delimiter) @container

(expected_type
  "<" @delimiter
  ">" @delimiter) @container

(type_argument_clause
  "<" @delimiter
  ">" @delimiter) @container

(array_extent
  "[" @delimiter
  "]" @delimiter) @container

(array_extent_parenthesized_expression
  "(" @delimiter
  ")" @delimiter) @container

(block
  "{" @delimiter
  "}" @delimiter) @container

(if_statement
  "(" @delimiter
  ")" @delimiter) @container

(while_statement
  "(" @delimiter
  ")" @delimiter) @container

(for_statement
  "(" @delimiter
  ")" @delimiter) @container

(switch_statement
  "(" @delimiter
  ")" @delimiter) @container

(switch_body
  "{" @delimiter
  "}" @delimiter) @container

(direct_initializer
  "{" @delimiter
  "}" @delimiter) @container

(array_initializer
  "{" @delimiter
  "}" @delimiter) @container

(argument_list
  "(" @delimiter
  ")" @delimiter) @container

(index_expression
  "[" @delimiter
  "]" @delimiter) @container

(lambda_capture_list
  "[" @delimiter
  "]" @delimiter) @container

(parenthesized_expression
  "(" @delimiter
  ")" @delimiter) @container

(unexpected_expression
  "(" @delimiter
  ")" @delimiter) @container

(numeric_conversion
  "(" @delimiter
  ")" @delimiter) @container
