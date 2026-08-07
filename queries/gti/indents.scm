(namespace_body "}" @indent.end) @indent.begin
(class_body "}" @indent.end) @indent.begin
(enum_declaration "}" @indent.end) @indent.begin
(block "}" @indent.end) @indent.begin
(array_initializer "}" @indent.end) @indent.begin

[
  ")"
  "]"
  "}"
  (access_specifier)
] @indent.branch

[
  (if_directive)
  (elif_directive)
  (else_directive)
  (endif_directive)
] @indent.zero

([
  (parameter_clause)
  (argument_list)
] @indent.align
  (#set! indent.open_delimiter "(")
  (#set! indent.close_delimiter ")"))

(comment) @indent.auto
