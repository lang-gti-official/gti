; rainbow-delimiters.nvim uses @container to derive nesting and @delimiter for
; the paired tokens. Keep this structural: ordinary punctuation highlighting
; remains in highlights.scm.

(runtime_binding
  "(" @delimiter
  ")" @delimiter) @container

(compiler_constraint_binding
  "(" @delimiter
  ")" @delimiter) @container

(concept_declaration
  "<" @delimiter
  ">" @delimiter) @container

(concept_application
  "<" @delimiter
  ">" @delimiter) @container

(extern_c_declaration
  "{" @delimiter
  "}" @delimiter) @container

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

(structured_binding_declaration
  "[" @delimiter
  "]" @delimiter) @container

(discarded_expression_statement
  "[[" @delimiter
  "]]" @delimiter) @container

(class_attribute_list
  "[[" @delimiter
  "]]" @delimiter) @container

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

(do_while_statement
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

(pack_fold_expression
  "(" @delimiter
  ")" @delimiter) @container

(unexpected_expression
  "(" @delimiter
  ")" @delimiter) @container

(numeric_conversion
  "(" @delimiter
  ")" @delimiter) @container

(layout_query_expression
  "(" @delimiter
  ")" @delimiter) @container
