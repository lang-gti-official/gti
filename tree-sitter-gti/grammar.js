const PREC = {
  comma: 1,
  assignment: 2,
  conditional: 3,
  logicalOr: 4,
  logicalAnd: 5,
  bitwiseOr: 6,
  bitwiseXor: 7,
  bitwiseAnd: 8,
  equality: 9,
  relational: 10,
  shift: 11,
  additive: 12,
  multiplicative: 13,
  unary: 14,
  postfix: 15,
};

const STANDARD_LIBRARY_COMPONENT_KEYWORDS = [
  "and",
  "alignof",
  "auto",
  "bool",
  "break",
  "case",
  "char",
  "class",
  "concept",
  "continue",
  "const",
  "constexpr",
  "default",
  "double",
  "else",
  "enum",
  "expected",
  "extern",
  "false",
  "float",
  "for",
  "if",
  "interface",
  "int",
  "int8_t",
  "int16_t",
  "int32_t",
  "int64_t",
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
  "override",
  "private",
  "public",
  "requires",
  "return",
  "static",
  "this",
  "struct",
  "sizeof",
  "switch",
  "true",
  "uint",
  "uint8_t",
  "uint16_t",
  "uint32_t",
  "uint64_t",
  "uint8",
  "uint16",
  "uint32",
  "uint64",
  "unsafe",
  "unexpected",
  "using",
  "void",
  "virtual",
  "while",
  "do",
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
    [$.direct_initializer, $.expression],
    [$.constructor_initializer_argument_list, $.expression],
    [$.constructor_declaration, $._qualified_identifier],
    [$._initializer_element, $.expression],
    [$._expression_not_comma, $._generic_member_function],
    [$._postfix_expression, $._generic_member_function],
  ],

  rules: {
    source_file: ($) => repeat(choice($.include_directive, $.declaration)),

    include_directive: ($) =>
      prec.right(
        seq(
          "#include",
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
        $.configuration_directive,
        $.compile_error_directive,
        $.extern_c_declaration,
        $.namespace_declaration,
        $.namespace_alias_declaration,
        $.concept_declaration,
        $.type_alias_declaration,
        $.enum_declaration,
        $.class_declaration,
        $.function_declaration,
        $.static_variable_declaration,
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

    if_directive: ($) =>
      choice(
        seq("#if", field("condition", $.compile_condition)),
        seq("#ifdef", field("flag", $.identifier)),
        seq("#ifndef", field("flag", $.identifier)),
      ),
    elif_directive: ($) =>
      seq("#elif", field("condition", $.compile_condition)),
    else_directive: () => "#else",
    endif_directive: () => "#endif",
    configuration_directive: ($) =>
      choice(
        seq("#define", field("flag", $.identifier)),
        seq("#undef", field("flag", $.identifier)),
      ),
    compile_error_directive: ($) =>
      seq("#error", field("message", $.string_literal)),

    extern_c_declaration: ($) =>
      seq(
        "extern",
        field("language", alias('"C"', $.string_literal)),
        "{",
        repeat(field("declaration", $.extern_c_function_prototype)),
        "}",
      ),

    extern_c_function_prototype: ($) =>
      seq(
        optional(field("attribute", $.native_c_array_attribute)),
        field("return_type", $.type),
        field("name", $.identifier),
        field("parameters", $.parameter_clause),
        ";",
      ),

    compile_condition: ($) =>
      choice(
        prec.left(
          1,
          seq(
            field("left", $.compile_condition),
            field("operator", "||"),
            field("right", $.compile_condition),
          ),
        ),
        prec.left(
          2,
          seq(
            field("left", $.compile_condition),
            field("operator", "&&"),
            field("right", $.compile_condition),
          ),
        ),
        prec(3, seq(field("operator", "!"), $.compile_condition)),
        $.defined_condition,
        $.target_condition,
        seq("(", $.compile_condition, ")"),
      ),

    defined_condition: ($) =>
      seq("defined", "(", field("flag", $.identifier), ")"),

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

    type_alias_declaration: ($) =>
      seq(
        "using",
        field("name", $.identifier),
        "=",
        field("target", $.type),
        ";",
      ),

    enum_declaration: ($) =>
      seq(
        "enum",
        "class",
        field("name", $.identifier),
        optional(seq(":", field("underlying_type", $.type))),
        "{",
        repeat(seq($.enumerator, ",")),
        optional($.enumerator),
        "}",
        ";",
      ),

    enumerator: ($) =>
      prec(
        1,
        seq(
          field("name", $.identifier),
          optional(field("payload", $.parameter_clause)),
          optional(seq("=", field("value", $._expression_not_comma))),
        ),
      ),

    namespace_body: ($) => seq("{", repeat($.declaration), "}"),

    runtime_binding: ($) =>
      seq("@", "runtime", "(", field("service", $.string_literal), ")"),

    compiler_constraint_binding: ($) =>
      seq(
        "@",
        "compiler_constraint",
        "(",
        field("capability", $.string_literal),
        ")",
      ),

    concept_declaration: ($) =>
      choice(
        seq(
          field("binding", $.compiler_constraint_binding),
          "concept",
          field("name", $.identifier),
          "<",
          commaSep1(field("parameter", $.identifier)),
          ">",
          ";",
        ),
        seq(
          "concept",
          field("name", $.identifier),
          "<",
          commaSep1(field("parameter", $.identifier)),
          ">",
          "=",
          field("requirement", $.concept_application),
          repeat(
            seq(
              field("operator", choice("&&", "and")),
              field("requirement", $.concept_application),
            ),
          ),
          ";",
        ),
      ),

    concept_application: ($) =>
      seq(
        field("name", $._qualified_identifier),
        "<",
        commaSep1(field("argument", $.identifier)),
        ">",
      ),

    requires_clause: ($) =>
      seq(
        "requires",
        field("requirement", $.concept_application),
        repeat(
          seq(
            field("operator", choice("&&", "and")),
            field("requirement", $.concept_application),
          ),
        ),
      ),

    class_declaration: ($) =>
      seq(
        optional(
          field("attributes", $.class_attribute_list),
        ),
        field("kind", choice("class", "struct", "interface", "union")),
        field("name", $.identifier),
        optional(field("type_parameters", $.generic_parameter_clause)),
        optional(field("bases", $.base_clause)),
        choice(
          seq(field("body", $.class_body), ";"),
          field("forward_declaration", ";"),
        ),
      ),

    class_attribute_list: ($) =>
      seq(
        "[[",
        field("attribute", $.identifier),
        repeat(seq(",", field("attribute", $.identifier))),
        "]]",
      ),

    native_c_array_attribute: ($) =>
      seq(
        "[[",
        field("attribute", alias("c_array", $.identifier)),
        "(",
        field("count", $.identifier),
        ")",
        "]]",
      ),

    base_clause: ($) => seq(":", commaSep1($.base_specifier)),

    base_specifier: ($) =>
      seq(
        optional(field("access", choice("public", "private"))),
        field("type", $._base_type),
      ),

    class_body: ($) => seq("{", repeat($._class_member), "}"),

    _class_member: ($) =>
      choice(
        $.conditional_class_members,
        $.configuration_directive,
        $.compile_error_directive,
        $.access_specifier,
        $.constructor_declaration,
        $.destructor_declaration,
        $.operator_declaration,
        $.method_declaration,
        $.static_variable_declaration,
        $.variable_declaration,
        $.empty_declaration,
      ),

    access_specifier: () => seq(choice("public", "private"), ":"),

    constructor_declaration: ($) =>
      seq(
        field("name", $.identifier),
        optional(field("type_parameters", $.generic_parameter_clause)),
        field("parameters", $.parameter_clause),
        optional(field("initializers", $.constructor_initializer_list)),
        choice(
          field("body", $.block),
          field("specifier", $.special_member_specifier),
        ),
      ),

    special_member_specifier: () =>
      seq("=", field("policy", choice("default", "delete")), ";"),

    constructor_initializer_list: ($) =>
      seq(":", commaSep1($.constructor_initializer)),

    constructor_initializer: ($) =>
      seq(
        field("target", $._base_type),
        field("arguments", $.constructor_initializer_argument_list),
      ),

    constructor_initializer_argument_list: ($) =>
      seq(
        "(",
        optional(
          commaSep1(choice($._expression_not_comma, $.array_initializer)),
        ),
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
        optional(field("virtual", "virtual")),
        optional(field("storage", "static")),
        optional(field("constant", "constexpr")),
        optional(field("return_mutable", "mut")),
        $._method_declaration_body,
      ),

    _method_declaration_body: ($) =>
      seq(
        field("return_type", $.type),
        field("name", $.identifier),
        optional(field("type_parameters", $.generic_parameter_clause)),
        field("parameters", $.parameter_clause),
        optional(field("mutable", "mut")),
        optional(field("constraints", $.requires_clause)),
        optional(field("override", "override")),
        choice(field("body", $.block), $.pure_specifier, ";"),
      ),

    operator_declaration: ($) =>
      choice(
        seq(
          optional(field("virtual", "virtual")),
          optional(field("constant", "constexpr")),
          optional(field("return_mutable", "mut")),
          $._operator_declaration_body,
        ),
        seq(
          optional(field("virtual", "virtual")),
          "operator",
          field("conversion_type", "bool"),
          field("parameters", $.parameter_clause),
          optional(field("mutable", "mut")),
          optional(field("constraints", $.requires_clause)),
          optional(field("override", "override")),
          choice(field("body", $.block), $.pure_specifier, ";"),
        ),
      ),

    _operator_declaration_body: ($) =>
      choice(
        seq(
          field("return_type", $.type),
          "operator",
          field("operator", seq("(", ")")),
          field("parameters", $.parameter_clause),
          optional(
            choice(field("mutable", "mut"), field("consuming", "&&")),
          ),
          optional(field("constraints", $.requires_clause)),
          optional(field("override", "override")),
          choice(field("body", $.block), $.pure_specifier, ";"),
        ),
        seq(
          field("return_type", $.type),
          "operator",
          field(
            "operator",
            choice(
              "*",
              "->",
              "++",
              "=",
              seq("[", "]"),
              "==",
              "!=",
              "<",
              "<=",
              ">",
              ">=",
            ),
          ),
          field("parameters", $.parameter_clause),
          optional(field("mutable", "mut")),
          optional(field("constraints", $.requires_clause)),
          optional(field("override", "override")),
          choice(field("body", $.block), $.pure_specifier, ";"),
        ),
      ),

    pure_specifier: ($) => seq("=", field("value", $.integer_literal), ";"),

    function_declaration: ($) =>
      seq(
        optional(field("attribute", $.native_c_array_attribute)),
        optional(field("binding", $.runtime_binding)),
        optional(field("virtual", "virtual")),
        optional(field("storage", "static")),
        optional(field("constant", "constexpr")),
        optional(field("return_mutable", "mut")),
        $._function_declaration_body,
      ),

    _function_declaration_body: ($) =>
      seq(
        field("return_type", $.type),
        field("name", $.identifier),
        optional(field("type_parameters", $.generic_parameter_clause)),
        field("parameters", $.parameter_clause),
        optional(field("constraints", $.requires_clause)),
        optional(field("override", "override")),
        choice(field("body", $.block), $.pure_specifier, ";"),
      ),

    generic_parameter_clause: ($) =>
      seq("<", commaSep1($.generic_parameter), ">"),

    generic_parameter: ($) =>
      choice(
        seq(
          field("constraint", $._qualified_identifier),
          field("name", $.identifier),
          optional(field("pack", "...")),
        ),
        seq(field("name", $.identifier), optional(field("pack", "..."))),
        seq(
          field("value_type", choice("uint64_t", "uint64")),
          field("name", $.identifier),
        ),
      ),

    parameter_clause: ($) => seq("(", optional(commaSep1($.parameter)), ")"),

    parameter: ($) =>
      prec(
        1,
        seq(
          optional(field("mutable", "mut")),
          field("type", $.type),
          optional(field("pack", "...")),
          optional(field("name", $.identifier)),
          repeat(field("extent", $.array_extent)),
          optional(seq("=", field("default", $._expression_not_comma))),
        ),
      ),

    variable_declaration: ($) =>
      seq(
        optional(field("constant", "constexpr")),
        $._variable_declaration_body,
      ),

    static_variable_declaration: ($) =>
      seq(
        field("storage", "static"),
        optional(field("constant", "constexpr")),
        $._variable_declaration_body,
      ),

    _variable_declaration_body: ($) =>
      seq(
        optional(field("mutable", "mut")),
        field("type", $.type),
        field("name", $.identifier),
        repeat(field("extent", $.array_extent)),
        optional(
          choice(
            seq("=", field("value", $.initializer_expression)),
            field("value", $.direct_initializer),
          ),
        ),
        ";",
      ),

    type: ($) =>
      prec.right(
        choice(
          seq(
            field("pointee_const", "const"),
            $._base_type,
            field("pointer", $.pointer_declarator),
            repeat($.array_extent),
            optional(field("reference", $.reference_declarator)),
          ),
          seq(
            $._base_type,
            optional(field("pointer", $.pointer_declarator)),
            repeat($.array_extent),
            optional(field("reference", $.reference_declarator)),
          ),
        ),
      ),

    pointer_declarator: () => prec.right(seq("*", optional("*"))),

    reference_declarator: () => choice("&", "&&"),

    _base_type: ($) =>
      choice(
        $.primitive_type,
        $.expected_type,
        $.native_function_type,
        $.user_type,
        "auto",
        "nullptr_t",
        "void",
      ),

    primitive_type: () =>
      choice(
        "int",
        "int8_t",
        "int16_t",
        "int32_t",
        "int64_t",
        "int8",
        "int16",
        "int32",
        "int64",
        "uint",
        "uint8_t",
        "uint16_t",
        "uint32_t",
        "uint64_t",
        "uint8",
        "uint16",
        "uint32",
        "uint64",
        "float",
        "double",
        "bool",
        "char",
      ),

    expected_type: ($) => seq("expected", "<", $.type, ",", $.type, ">"),

    native_function_type: ($) =>
      seq(
        "(",
        optional(commaSep1(field("parameter", $.type))),
        ")",
        "->",
        field("return_type", $.type),
      ),

    user_type: ($) =>
      seq(
        field("name", $._qualified_identifier),
        optional(field("arguments", $.type_argument_clause)),
      ),

    type_argument_clause: ($) =>
      seq("<", commaSep1(choice($.type, $.integer_literal)), ">"),

    array_extent: ($) =>
      seq("[", field("size", $._array_extent_expression), "]"),

    _array_extent_expression: ($) =>
      choice(
        $.integer_literal,
        $.identifier,
        $.array_extent_parenthesized_expression,
        $.array_extent_binary_expression,
      ),

    array_extent_parenthesized_expression: ($) =>
      seq("(", $._array_extent_expression, ")"),

    array_extent_binary_expression: ($) =>
      choice(
        ...[
          [choice("+", "-"), PREC.additive],
          [choice("*", "/", "%"), PREC.multiplicative],
        ].map(([operator, precedence]) =>
          prec.left(
            precedence,
            seq(
              field("left", $._array_extent_expression),
              field("operator", operator),
              field("right", $._array_extent_expression),
            ),
          ),
        ),
      ),

    block: ($) => seq("{", repeat($._block_item), "}"),

    _block_item: ($) =>
      choice(
        $.conditional_block_items,
        $.configuration_directive,
        $.compile_error_directive,
        $.structured_binding_declaration,
        $.variable_declaration,
        $.statement,
      ),

    structured_binding_declaration: ($) =>
      prec(
        1,
        seq(
          "auto",
          "[",
          field("binding", $.identifier),
          repeat(seq(",", field("binding", $.identifier))),
          "]",
          "=",
          field("value", $.initializer_expression),
          ";",
        ),
      ),

    statement: ($) =>
      choice(
        $.block,
        $.if_statement,
        $.unsafe_statement,
        $.while_statement,
        $.for_statement,
        $.switch_statement,
        $.break_statement,
        $.continue_statement,
        $.return_statement,
        $.discarded_expression_statement,
        $.expression_statement,
        $.do_while_statement,
      ),

    unsafe_statement: ($) => seq("unsafe", field("body", $.block)),

    if_statement: ($) =>
      prec.right(
        seq(
          "if",
          optional(field("constant", "constexpr")),
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
        choice(
          seq(
            field(
              "initializer",
              choice($.variable_declaration, $.expression_statement),
            ),
            optional(field("condition", $.expression)),
            ";",
            optional(field("update", $.expression)),
          ),
          seq(
            field("binding", $.range_for_declaration),
            ":",
            field("range", $.expression),
          ),
        ),
        ")",
        field("body", $.statement),
      ),

    range_for_declaration: ($) =>
      seq(
        optional(field("mutable", "mut")),
        field("type", $.type),
        field("name", $.identifier),
        repeat(field("extent", $.array_extent)),
      ),

    switch_statement: ($) =>
      seq(
        "switch",
        "(",
        field("value", $.expression),
        ")",
        field("body", $.switch_body),
      ),

    switch_body: ($) =>
      seq(
        "{",
        repeat(choice($.case_label, $.default_label, $._block_item)),
        "}",
      ),

    case_label: ($) =>
      seq("case", field("value", $._expression_not_comma), ":"),

    default_label: () => seq("default", ":"),

    break_statement: () => seq("break", ";"),
    continue_statement: () => seq("continue", ";"),

    return_statement: ($) =>
      seq("return", optional(field("value", $.initializer_expression)), ";"),

    discarded_expression_statement: ($) =>
      seq("[[", "discard", "]]", field("value", $.expression), ";"),

    expression_statement: ($) => seq(optional($.expression), ";"),

    initializer_expression: ($) => choice($.expression, $.array_initializer),

    direct_initializer: ($) =>
      seq(
        "{",
        optional(seq(commaSep1($._expression_not_comma), optional(","))),
        "}",
      ),

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
        $.conditional_expression,
        nonAssignmentExpression($),
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
          field("left", nonAssignmentExpression($)),
          field(
            "operator",
            choice(
              "=",
              "+=",
              "-=",
              "*=",
              "/=",
              "%=",
              "&=",
              "|=",
              "^=",
              "<<=",
              ">>=",
            ),
          ),
          field("right", $.initializer_expression),
        ),
      ),

    binary_expression: ($) => {
      const operators = [
        [choice("or", "||"), PREC.logicalOr],
        [choice("and", "&&"), PREC.logicalAnd],
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
          field(
            "operator",
            choice("!", "+", "-", "*", "&", "~", "++", "--"),
          ),
          field(
            "argument",
            choice($.unary_expression, $._postfix_expression),
          ),
        ),
      ),

    call_expression: ($) =>
      choice(
        prec.dynamic(
          1,
          seq(
            field(
              "function",
              alias($._generic_member_function, $.generic_function),
            ),
            field("arguments", $.argument_list),
          ),
        ),
        prec(
          PREC.postfix,
          seq(
            field("function", $._postfix_expression),
            field("arguments", $.argument_list),
          ),
        ),
      ),

    _generic_member_function: ($) =>
      seq(
        field("name", $.member_expression),
        field("type_arguments", $.type_argument_clause),
      ),

    argument_list: ($) =>
      seq(
        "(",
        optional(
          choice(
            $.pack_expansion,
            seq(
              commaSep1(choice($._expression_not_comma, $.array_initializer)),
              optional(seq(",", $.pack_expansion)),
            ),
          ),
        ),
        ")",
      ),

    pack_expansion: ($) =>
      seq(field("name", $.identifier), field("operator", "...")),

    pack_fold_expression: ($) =>
      prec(
        PREC.postfix,
        seq(
          "(",
          field("pattern", $.call_expression),
          field("operator", ","),
          field("ellipsis", "..."),
          ")",
        ),
      ),

    member_expression: ($) =>
      prec.left(
        PREC.postfix,
        seq(
          field("object", $._postfix_expression),
          field("operator", choice(".", "->")),
          field("member", $.identifier),
        ),
      ),

    index_expression: ($) =>
      prec.left(
        PREC.postfix,
        seq(
          field("value", $._postfix_expression),
          "[",
          field("index", $.expression),
          "]",
        ),
      ),

    update_expression: ($) =>
      prec.left(
        PREC.postfix,
        seq(
          field("argument", $._postfix_expression),
          field("operator", choice("++", "--")),
        ),
      ),

    _postfix_expression: ($) =>
      choice(
        $.call_expression,
        $.member_expression,
        $.index_expression,
        $.generic_function,
        $.primary_expression,
      ),

    lambda_expression: ($) =>
      seq(
        field("captures", $.lambda_capture_list),
        field("parameters", $.parameter_clause),
        "->",
        field("return_type", $.type),
        field("body", $.block),
      ),

    lambda_capture_list: ($) =>
      seq("[", optional(commaSep1($.lambda_capture)), "]"),

    lambda_capture: ($) =>
      prec(
        1,
        seq(
          field("name", $.identifier),
          optional(seq("=", field("initializer", $._expression_not_comma))),
        ),
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
        $.lambda_expression,
        $.layout_query_expression,
        $.this_expression,
        $.nullptr_literal,
        $.pack_fold_expression,
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

    layout_query_expression: ($) =>
      seq(
        field("operator", choice("sizeof", "alignof")),
        "(",
        field("type", $.type),
        ")",
      ),

    this_expression: () => "this",
    nullptr_literal: () => "nullptr",

    literal: ($) =>
      choice(
        $.integer_literal,
        $.float_literal,
        $.string_literal,
        $.character_literal,
        $.boolean_literal,
      ),

    boolean_literal: () => choice("true", "false"),
    integer_literal: () => /0[xX][0-9a-fA-F]+|0[bB][01]+|[0-9]+/,
    float_literal: () => /[0-9]+\.[0-9]+[dD]?/,
    string_literal: () => token(seq('"', repeat(choice(/[^"\\\n]/, /\\./)), '"')),
    character_literal: () => token(seq("'", choice(/[^'\\\n]/, /\\./), "'")),

    _qualified_identifier: ($) => choice($.identifier, $.scoped_identifier),

    scoped_identifier: ($) =>
      prec.left(
        seq(
          field("scope", choice($.identifier, $.scoped_identifier)),
          "::",
          field("name", $.identifier),
        ),
      ),

    do_while_statement: ($) =>
      seq(
        "do",
        field("body", $.statement),
        "while",
        "(",
        field("condition", $.expression),
        ")",
        ";",
      ),

    conditional_expression: ($) =>
      prec.right(
        PREC.conditional,
        seq(
          field("condition", nonAssignmentExpression($)),
          "?",
          field("consequence", $.expression),
          ":",
          field(
            "alternative",
            choice(
              $.assignment_expression,
              $.conditional_expression,
              nonAssignmentExpression($),
            ),
          ),
        ),
      ),

    identifier: () => /[A-Za-z_][A-Za-z0-9_]*/,
    comment: () =>
      token(
        choice(
          seq("//", /[^\n]*/),
          seq("/*", /[^*]*\*+([^/*][^*]*\*+)*/, "/"),
        ),
      ),
  },
});

function commaSep1(rule) {
  return seq(rule, repeat(seq(",", rule)));
}

function nonAssignmentExpression($) {
  return choice(
    $.binary_expression,
    $.unary_expression,
    $.call_expression,
    $.member_expression,
    $.index_expression,
    $.update_expression,
    $.generic_function,
    $.primary_expression,
  );
}
