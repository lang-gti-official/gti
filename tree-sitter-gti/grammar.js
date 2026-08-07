const PREC = {
  comma: 1,
  assignment: 2,
  logicalOr: 3,
  logicalAnd: 4,
  bitwiseOr: 5,
  bitwiseXor: 6,
  bitwiseAnd: 7,
  equality: 8,
  relational: 9,
  shift: 10,
  additive: 11,
  multiplicative: 12,
  unary: 13,
  postfix: 14,
};

const STANDARD_LIBRARY_COMPONENT_KEYWORDS = [
  "and",
  "bool",
  "break",
  "class",
  "continue",
  "else",
  "expected",
  "false",
  "float",
  "for",
  "if",
  "include",
  "int",
  "int8",
  "int16",
  "int32",
  "int64",
  "mut",
  "namespace",
  "nullptr",
  "nullptr_t",
  "operator",
  "or",
  "private",
  "public",
  "return",
  "self",
  "string",
  "struct",
  "true",
  "uint",
  "uint8",
  "uint16",
  "uint32",
  "uint64",
  "unexpected",
  "void",
  "while",
];

module.exports = grammar({
  name: "gti",

  extras: ($) => [/\s/, $.comment],
  word: ($) => $.identifier,

  supertypes: ($) => [$.declaration, $.statement, $.expression],

  conflicts: ($) => [
    [$.generic_function, $.user_type],
    [$.generic_function, $.primary_expression],
    [$.generic_function, $.user_type, $.primary_expression],
    [$.user_type, $.primary_expression],
    [$.expression, $.argument_list],
    [$._initializer_element, $.expression],
  ],

  rules: {
    source_file: ($) => repeat(choice($.include_directive, $.declaration)),

    include_directive: ($) =>
      prec.right(
        seq(
          "include",
          field("path", choice($.string_literal, $.standard_library_path)),
          optional(";"),
        ),
      ),

    standard_library_path: ($) =>
      seq(
        "<",
        $.standard_library_component,
        repeat1(seq("/", $.standard_library_component)),
        ">",
      ),

    standard_library_component: ($) =>
      choice($.identifier, ...STANDARD_LIBRARY_COMPONENT_KEYWORDS),

    declaration: ($) =>
      choice(
        $.conditional_declaration,
        $.namespace_declaration,
        $.namespace_alias_declaration,
        $.class_declaration,
        $.function_declaration,
        $.variable_declaration,
        $.empty_declaration,
      ),

    empty_declaration: () => ";",

    conditional_declaration: ($) =>
      seq(
        $.if_directive,
        repeat($.declaration),
        repeat(seq($.elif_directive, repeat($.declaration))),
        optional(seq($.else_directive, repeat($.declaration))),
        $.endif_directive,
      ),

    conditional_class_members: ($) =>
      seq(
        $.if_directive,
        repeat($._class_member),
        repeat(seq($.elif_directive, repeat($._class_member))),
        optional(seq($.else_directive, repeat($._class_member))),
        $.endif_directive,
      ),

    conditional_block_items: ($) =>
      seq(
        $.if_directive,
        repeat($._block_item),
        repeat(seq($.elif_directive, repeat($._block_item))),
        optional(seq($.else_directive, repeat($._block_item))),
        $.endif_directive,
      ),

    if_directive: ($) => seq("#if", field("condition", $.target_condition)),
    elif_directive: ($) => seq("#elif", field("condition", $.target_condition)),
    else_directive: () => "#else",
    endif_directive: () => "#endif",

    target_condition: ($) =>
      seq(
        "target",
        ".",
        field("property", choice("os", "vendor", "arch")),
        field("operator", choice("==", "!=")),
        field("value", $.string_literal),
      ),

    namespace_declaration: ($) =>
      seq(
        "namespace",
        field("name", $.identifier),
        field("body", $.namespace_body),
      ),

    namespace_alias_declaration: ($) =>
      seq(
        "namespace",
        field("name", $.identifier),
        "=",
        field("target", $._qualified_identifier),
        ";",
      ),

    namespace_body: ($) => seq("{", repeat($.declaration), "}"),

    runtime_binding: ($) =>
      seq("@", "runtime", "(", field("service", $.string_literal), ")"),

    class_declaration: ($) =>
      seq(
        field("kind", choice("class", "struct")),
        field("name", $.identifier),
        optional(field("type_parameters", $.generic_parameter_clause)),
        field("body", $.class_body),
        ";",
      ),

    class_body: ($) => seq("{", repeat($._class_member), "}"),

    _class_member: ($) =>
      choice(
        $.conditional_class_members,
        $.access_specifier,
        $.constructor_declaration,
        $.destructor_declaration,
        $.operator_declaration,
        $.method_declaration,
        $.variable_declaration,
        $.empty_declaration,
      ),

    access_specifier: () => seq(choice("public", "private"), ":"),

    constructor_declaration: ($) =>
      seq(
        field("name", $.identifier),
        field("parameters", $.parameter_clause),
        optional(field("initializers", $.constructor_initializer_list)),
        field("body", $.block),
      ),

    constructor_initializer_list: ($) =>
      seq(":", commaSep1($.constructor_initializer)),

    constructor_initializer: ($) =>
      seq(
        field("field", $.identifier),
        "(",
        field("value", $.initializer_expression),
        ")",
      ),

    destructor_declaration: ($) =>
      seq(
        "~",
        field("name", $.identifier),
        "(",
        ")",
        field("body", $.block),
      ),

    method_declaration: ($) =>
      seq(
        optional(field("return_mutable", "mut")),
        field("return_type", $.type),
        field("name", $.identifier),
        optional(field("type_parameters", $.generic_parameter_clause)),
        field("parameters", $.parameter_clause),
        optional(field("mutable", "mut")),
        choice(field("body", $.block), ";"),
      ),

    operator_declaration: ($) =>
      choice(
        seq(
          optional(field("return_mutable", "mut")),
          field("return_type", $.type),
          "operator",
          field(
            "operator",
            choice("*", "->", seq("[", "]"), "==", "!="),
          ),
          field("parameters", $.parameter_clause),
          optional(field("mutable", "mut")),
          choice(field("body", $.block), ";"),
        ),
        seq(
          "operator",
          field("conversion_type", "bool"),
          field("parameters", $.parameter_clause),
          optional(field("mutable", "mut")),
          choice(field("body", $.block), ";"),
        ),
      ),

    function_declaration: ($) =>
      seq(
        optional(field("binding", $.runtime_binding)),
        field("return_type", $.type),
        field("name", $.identifier),
        optional(field("type_parameters", $.generic_parameter_clause)),
        field("parameters", $.parameter_clause),
        choice(field("body", $.block), ";"),
      ),

    generic_parameter_clause: ($) =>
      seq("<", commaSep1($.generic_parameter), ">"),

    generic_parameter: ($) =>
      choice(
        seq(field("name", $.identifier), optional(field("pack", "..."))),
        seq(field("value_type", "uint64"), field("name", $.identifier)),
      ),

    parameter_clause: ($) => seq("(", optional(commaSep1($.parameter)), ")"),

    parameter: ($) =>
      seq(
        optional(field("mutable", "mut")),
        field("type", $.type),
        optional(field("pack", "...")),
        optional(field("name", $.identifier)),
        repeat(field("extent", $.array_extent)),
      ),

    variable_declaration: ($) =>
      seq(
        optional(field("mutable", "mut")),
        field("type", $.type),
        field("name", $.identifier),
        repeat(field("extent", $.array_extent)),
        optional(seq("=", field("value", $.initializer_expression))),
        ";",
      ),

    type: ($) =>
      prec.right(
        seq(
          $._base_type,
          repeat($.array_extent),
          optional(field("reference", $.reference_declarator)),
        ),
      ),

    reference_declarator: () => "&",

    _base_type: ($) =>
      choice(
        $.primitive_type,
        $.expected_type,
        $.user_type,
        "string",
        "nullptr_t",
        "void",
      ),

    primitive_type: () =>
      choice(
        "int",
        "int8",
        "int16",
        "int32",
        "int64",
        "uint",
        "uint8",
        "uint16",
        "uint32",
        "uint64",
        "float",
        "bool",
      ),

    expected_type: ($) => seq("expected", "<", $.type, ",", $.type, ">"),

    user_type: ($) =>
      seq(
        field("name", $._qualified_identifier),
        optional(field("arguments", $.type_argument_clause)),
      ),

    type_argument_clause: ($) =>
      seq("<", commaSep1(choice($.type, $.integer_literal)), ">"),

    array_extent: ($) =>
      seq("[", field("size", choice($.integer_literal, $.identifier)), "]"),

    block: ($) => seq("{", repeat($._block_item), "}"),

    _block_item: ($) =>
      choice($.conditional_block_items, $.variable_declaration, $.statement),

    statement: ($) =>
      choice(
        $.block,
        $.if_statement,
        $.while_statement,
        $.for_statement,
        $.break_statement,
        $.continue_statement,
        $.return_statement,
        $.discarded_expression_statement,
        $.expression_statement,
      ),

    if_statement: ($) =>
      prec.right(
        seq(
          "if",
          "(",
          field("condition", $.expression),
          ")",
          field("consequence", $.statement),
          optional(seq("else", field("alternative", $.statement))),
        ),
      ),

    while_statement: ($) =>
      seq(
        "while",
        "(",
        field("condition", $.expression),
        ")",
        field("body", $.statement),
      ),

    for_statement: ($) =>
      seq(
        "for",
        "(",
        field(
          "initializer",
          choice($.variable_declaration, $.expression_statement),
        ),
        optional(field("condition", $.expression)),
        ";",
        optional(field("update", $.expression)),
        ")",
        field("body", $.statement),
      ),

    break_statement: () => seq("break", ";"),
    continue_statement: () => seq("continue", ";"),

    return_statement: ($) =>
      seq("return", optional(field("value", $.initializer_expression)), ";"),

    discarded_expression_statement: ($) =>
      seq("[[", "discard", "]]", field("value", $.expression), ";"),

    expression_statement: ($) => seq(optional($.expression), ";"),

    initializer_expression: ($) => choice($.expression, $.array_initializer),

    array_initializer: ($) =>
      seq(
        "{",
        optional(seq(commaSep1($._initializer_element), optional(","))),
        "}",
      ),

    _initializer_element: ($) =>
      choice($._expression_not_comma, $.array_initializer),

    expression: ($) => choice($.comma_expression, $._expression_not_comma),

    _expression_not_comma: ($) =>
      choice(
        $.assignment_expression,
        $.binary_expression,
        $.unary_expression,
        $.call_expression,
        $.member_expression,
        $.index_expression,
        $.update_expression,
        $.generic_function,
        $.primary_expression,
      ),

    comma_expression: ($) =>
      prec.left(
        PREC.comma,
        seq(field("left", $.expression), ",", field("right", $.expression)),
      ),

    assignment_expression: ($) =>
      prec.right(
        PREC.assignment,
        seq(
          field("left", $.expression),
          field("operator", choice("=", "+=", "-=")),
          field("right", $.initializer_expression),
        ),
      ),

    binary_expression: ($) => {
      const operators = [
        ["or", PREC.logicalOr],
        ["and", PREC.logicalAnd],
        ["|", PREC.bitwiseOr],
        ["^", PREC.bitwiseXor],
        ["&", PREC.bitwiseAnd],
        [choice("==", "!="), PREC.equality],
        [choice("<", "<=", ">", ">="), PREC.relational],
        [choice("<<", ">>"), PREC.shift],
        [choice("+", "-"), PREC.additive],
        [choice("*", "/", "%"), PREC.multiplicative],
      ];
      return choice(
        ...operators.map(([operator, precedence]) =>
          prec.left(
            precedence,
            seq(
              field("left", $.expression),
              field("operator", operator),
              field("right", $.expression),
            ),
          ),
        ),
      );
    },

    unary_expression: ($) =>
      prec.right(
        PREC.unary,
        seq(
          field("operator", choice("!", "+", "-", "*", "~", "++", "--")),
          field("argument", $.expression),
        ),
      ),

    call_expression: ($) =>
      prec(
        PREC.postfix,
        seq(
          field("function", $.expression),
          field("arguments", $.argument_list),
        ),
      ),

    argument_list: ($) =>
      seq(
        "(",
        optional(
          choice(
            $.pack_expansion,
            seq(
              commaSep1($._expression_not_comma),
              optional(seq(",", $.pack_expansion)),
            ),
          ),
        ),
        ")",
      ),

    pack_expansion: ($) =>
      seq(field("name", $.identifier), field("operator", "...")),

    member_expression: ($) =>
      prec.left(
        PREC.postfix,
        seq(
          field("object", $.expression),
          field("operator", choice(".", "->")),
          field("member", choice($.identifier, $.generic_function)),
        ),
      ),

    index_expression: ($) =>
      prec.left(
        PREC.postfix,
        seq(field("value", $.expression), "[", field("index", $.expression), "]"),
      ),

    update_expression: ($) =>
      prec.left(
        PREC.postfix,
        seq(field("argument", $.expression), field("operator", choice("++", "--"))),
      ),

    generic_function: ($) =>
      seq(
        field("name", $._qualified_identifier),
        field("type_arguments", $.type_argument_clause),
      ),

    primary_expression: ($) =>
      choice(
        $.literal,
        $._qualified_identifier,
        $.unexpected_expression,
        $.numeric_conversion,
        $.self_expression,
        $.nullptr_literal,
        $.parenthesized_expression,
      ),

    parenthesized_expression: ($) => seq("(", $.expression, ")"),

    unexpected_expression: ($) =>
      seq("unexpected", "(", field("error", $._expression_not_comma), ")"),

    numeric_conversion: ($) =>
      seq(
        field("type", $.primitive_type),
        "(",
        field("value", $._expression_not_comma),
        ")",
      ),

    self_expression: () => "self",
    nullptr_literal: () => "nullptr",

    literal: ($) =>
      choice(
        $.integer_literal,
        $.float_literal,
        $.string_literal,
        $.boolean_literal,
      ),

    boolean_literal: () => choice("true", "false"),
    integer_literal: () => /[0-9]+/,
    float_literal: () => /[0-9]+\.[0-9]+/,
    string_literal: () => token(seq('"', repeat(choice(/[^"\\\n]/, /\\./)), '"')),

    _qualified_identifier: ($) => choice($.identifier, $.scoped_identifier),

    scoped_identifier: ($) =>
      prec.left(
        seq(
          field("scope", choice($.identifier, $.scoped_identifier)),
          "::",
          field("name", $.identifier),
        ),
      ),

    identifier: () => /[A-Za-z_][A-Za-z0-9_]*/,
    comment: () => token(seq("//", /[^\n]*/)),
  },
});

function commaSep1(rule) {
  return seq(rule, repeat(seq(",", rule)));
}
