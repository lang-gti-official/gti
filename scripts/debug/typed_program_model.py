#!/usr/bin/env python3

"""Bounded typed programs for GTI differential and metamorphic auditing.

This module is intentionally a small shared-semantics model, not another GTI
frontend. It only constructs int32/bool programs whose operations are defined
identically by GTI, the evaluator below, and portable C++23.
"""

from __future__ import annotations

from dataclasses import asdict, dataclass
import hashlib
import json
import random
from typing import Any, Literal


ValueType = Literal["int32", "bool"]
Dialect = Literal["gti", "cpp"]
INT32_MIN = -(2**31)
INT32_MAX = 2**31 - 1


class ModelError(RuntimeError):
    """The generator constructed an invalid or out-of-bounds model."""


def checked_int32(value: int) -> int:
    if value < INT32_MIN or value > INT32_MAX:
        raise ModelError(f"model arithmetic escaped int32_t: {value}")
    return value


@dataclass(frozen=True)
class Expression:
    kind: str
    type: ValueType
    value: int | bool | str | None = None
    children: tuple["Expression", ...] = ()

    def evaluate(
        self,
        environment: dict[str, int | bool],
        functions: dict[str, "Function"],
    ) -> int | bool:
        if self.kind == "literal":
            assert self.value is not None
            return self.value
        if self.kind == "variable":
            assert isinstance(self.value, str)
            if self.value not in environment:
                raise ModelError(f"unknown model variable {self.value!r}")
            return environment[self.value]
        if self.kind == "call":
            assert isinstance(self.value, str)
            function = functions.get(self.value)
            if function is None:
                raise ModelError(f"unknown model function {self.value!r}")
            arguments = [child.evaluate(environment, functions) for child in self.children]
            return function.evaluate(arguments, functions)

        values = [child.evaluate(environment, functions) for child in self.children]
        if self.kind == "add":
            return checked_int32(int(values[0]) + int(values[1]))
        if self.kind == "sub":
            return checked_int32(int(values[0]) - int(values[1]))
        if self.kind == "mul":
            return checked_int32(int(values[0]) * int(values[1]))
        if self.kind == "less":
            return int(values[0]) < int(values[1])
        if self.kind == "equal":
            return values[0] == values[1]
        if self.kind == "and":
            return bool(values[0]) and bool(values[1])
        if self.kind == "or":
            return bool(values[0]) or bool(values[1])
        if self.kind == "not":
            return not bool(values[0])
        raise ModelError(f"unknown expression kind {self.kind!r}")

    def emit(self, dialect: Dialect, extra_parentheses: bool = False) -> str:
        if self.kind == "literal":
            if self.type == "bool":
                text = "true" if self.value else "false"
            else:
                text = str(self.value)
        elif self.kind == "variable":
            text = str(self.value)
        elif self.kind == "call":
            arguments = ", ".join(
                child.emit(dialect, extra_parentheses) for child in self.children
            )
            text = f"{self.value}({arguments})"
        elif self.kind == "not":
            text = f"(!{self.children[0].emit(dialect, extra_parentheses)})"
        else:
            operators = {
                "add": "+",
                "sub": "-",
                "mul": "*",
                "less": "<",
                "equal": "==",
                "and": "&&",
                "or": "||",
            }
            operator = operators.get(self.kind)
            if operator is None:
                raise ModelError(f"cannot emit expression kind {self.kind!r}")
            left = self.children[0].emit(dialect, extra_parentheses)
            right = self.children[1].emit(dialect, extra_parentheses)
            text = f"({left} {operator} {right})"
        return f"({text})" if extra_parentheses else text

    def node_kinds(self) -> set[str]:
        kinds = {f"expr:{self.kind}"}
        for child in self.children:
            kinds.update(child.node_kinds())
        return kinds

    def record(self) -> dict[str, Any]:
        return {
            "kind": self.kind,
            "type": self.type,
            "value": self.value,
            "children": [child.record() for child in self.children],
        }


def int_literal(value: int) -> Expression:
    return Expression("literal", "int32", checked_int32(value))


def variable(name: str, type_: ValueType = "int32") -> Expression:
    return Expression("variable", type_, name)


def binary(kind: str, left: Expression, right: Expression) -> Expression:
    if kind in {"add", "sub", "mul"}:
        if left.type != "int32" or right.type != "int32":
            raise ModelError(f"{kind} requires int32 operands")
        type_: ValueType = "int32"
    elif kind == "less":
        if left.type != "int32" or right.type != "int32":
            raise ModelError("less requires int32 operands")
        type_ = "bool"
    elif kind == "equal":
        if left.type != right.type:
            raise ModelError("equal requires operands of the same type")
        type_ = "bool"
    elif kind in {"and", "or"}:
        if left.type != "bool" or right.type != "bool":
            raise ModelError(f"{kind} requires bool operands")
        type_ = "bool"
    else:
        raise ModelError(f"unsupported binary kind {kind!r}")
    return Expression(kind, type_, children=(left, right))


def logical_not(value: Expression) -> Expression:
    if value.type != "bool":
        raise ModelError("not requires a bool operand")
    return Expression("not", "bool", children=(value,))


def call(name: str, arguments: tuple[Expression, ...]) -> Expression:
    if any(argument.type != "int32" for argument in arguments):
        raise ModelError("the bounded model only calls int32 functions")
    return Expression("call", "int32", name, arguments)


@dataclass(frozen=True)
class Binding:
    name: str
    type: ValueType
    mutable: bool
    initializer: Expression

    def record(self) -> dict[str, Any]:
        return {
            "kind": "binding",
            "name": self.name,
            "type": self.type,
            "mutable": self.mutable,
            "initializer": self.initializer.record(),
        }


@dataclass(frozen=True)
class Assignment:
    name: str
    value: Expression

    def record(self) -> dict[str, Any]:
        return {"kind": "assignment", "name": self.name, "value": self.value.record()}


@dataclass(frozen=True)
class IfElse:
    condition: Expression
    then_body: tuple["Statement", ...]
    else_body: tuple["Statement", ...]

    def record(self) -> dict[str, Any]:
        return {
            "kind": "if_else",
            "condition": self.condition.record(),
            "then": [statement_record(statement) for statement in self.then_body],
            "else": [statement_record(statement) for statement in self.else_body],
        }


@dataclass(frozen=True)
class ForLoop:
    index: str
    iterations: int
    body: tuple["Statement", ...]

    def record(self) -> dict[str, Any]:
        return {
            "kind": "for_loop",
            "index": self.index,
            "iterations": self.iterations,
            "body": [statement_record(statement) for statement in self.body],
        }


@dataclass(frozen=True)
class Return:
    value: Expression

    def record(self) -> dict[str, Any]:
        return {"kind": "return", "value": self.value.record()}


Statement = Binding | Assignment | IfElse | ForLoop | Return


def statement_record(statement: Statement) -> dict[str, Any]:
    return statement.record()


def statement_node_kinds(statement: Statement) -> set[str]:
    if isinstance(statement, Binding):
        return {"stmt:binding"} | statement.initializer.node_kinds()
    if isinstance(statement, Assignment):
        return {"stmt:assignment"} | statement.value.node_kinds()
    if isinstance(statement, IfElse):
        kinds = {"stmt:if_else"} | statement.condition.node_kinds()
        for child in statement.then_body + statement.else_body:
            kinds.update(statement_node_kinds(child))
        return kinds
    if isinstance(statement, ForLoop):
        kinds = {"stmt:for_loop"}
        for child in statement.body:
            kinds.update(statement_node_kinds(child))
        return kinds
    return {"stmt:return"} | statement.value.node_kinds()


def execute_statements(
    statements: tuple[Statement, ...],
    environment: dict[str, int | bool],
    mutable: set[str],
    functions: dict[str, "Function"],
) -> int | bool | None:
    for statement in statements:
        if isinstance(statement, Binding):
            value = statement.initializer.evaluate(environment, functions)
            if statement.name in environment:
                raise ModelError(f"duplicate model binding {statement.name!r}")
            environment[statement.name] = value
            if statement.mutable:
                mutable.add(statement.name)
        elif isinstance(statement, Assignment):
            if statement.name not in mutable:
                raise ModelError(f"assignment to immutable model binding {statement.name!r}")
            environment[statement.name] = statement.value.evaluate(environment, functions)
        elif isinstance(statement, IfElse):
            condition = statement.condition.evaluate(environment, functions)
            branch = statement.then_body if bool(condition) else statement.else_body
            result = execute_statements(branch, environment, mutable, functions)
            if result is not None:
                return result
        elif isinstance(statement, ForLoop):
            if statement.iterations < 0 or statement.iterations > 8:
                raise ModelError("model loop bound is outside the accepted range")
            for index in range(statement.iterations):
                environment[statement.index] = index
                mutable.add(statement.index)
                result = execute_statements(statement.body, environment, mutable, functions)
                if result is not None:
                    return result
            environment.pop(statement.index, None)
            mutable.discard(statement.index)
        else:
            return statement.value.evaluate(environment, functions)
    return None


def type_spelling(type_: ValueType, dialect: Dialect) -> str:
    if type_ == "bool":
        return "bool"
    return "int32_t" if dialect == "gti" else "std::int32_t"


def emit_statements(
    statements: tuple[Statement, ...],
    dialect: Dialect,
    indent: int,
    extra_parentheses: bool,
) -> list[str]:
    prefix = "  " * indent
    lines: list[str] = []
    for statement in statements:
        if isinstance(statement, Binding):
            type_name = type_spelling(statement.type, dialect)
            if dialect == "gti":
                declaration = f"{'mut ' if statement.mutable else ''}{type_name}"
            else:
                declaration = f"{'const ' if not statement.mutable else ''}{type_name}"
            initializer = statement.initializer.emit(dialect, extra_parentheses)
            lines.append(f"{prefix}{declaration} {statement.name} = {initializer};")
        elif isinstance(statement, Assignment):
            lines.append(
                f"{prefix}{statement.name} = "
                f"{statement.value.emit(dialect, extra_parentheses)};"
            )
        elif isinstance(statement, IfElse):
            condition = statement.condition.emit(dialect, extra_parentheses)
            lines.append(f"{prefix}if ({condition}) {{")
            lines.extend(
                emit_statements(
                    statement.then_body, dialect, indent + 1, extra_parentheses
                )
            )
            lines.append(f"{prefix}}} else {{")
            lines.extend(
                emit_statements(
                    statement.else_body, dialect, indent + 1, extra_parentheses
                )
            )
            lines.append(f"{prefix}}}")
        elif isinstance(statement, ForLoop):
            index_type = type_spelling("int32", dialect)
            mutable_prefix = "mut " if dialect == "gti" else ""
            lines.append(
                f"{prefix}for ({mutable_prefix}{index_type} {statement.index} = 0; "
                f"{statement.index} < {statement.iterations}; {statement.index}++) {{"
            )
            lines.extend(
                emit_statements(statement.body, dialect, indent + 1, extra_parentheses)
            )
            lines.append(f"{prefix}}}")
        else:
            lines.append(
                f"{prefix}return {statement.value.emit(dialect, extra_parentheses)};"
            )
    return lines


@dataclass(frozen=True)
class Function:
    name: str
    parameters: tuple[str, ...]
    body: tuple[Statement, ...]
    is_main: bool = False

    def evaluate(
        self,
        arguments: list[int | bool],
        functions: dict[str, "Function"],
    ) -> int | bool:
        if len(arguments) != len(self.parameters):
            raise ModelError(f"wrong argument count for {self.name}")
        environment = dict(zip(self.parameters, arguments, strict=True))
        result = execute_statements(self.body, environment, set(), functions)
        if result is None:
            raise ModelError(f"model function {self.name} did not return")
        return result

    def emit(self, dialect: Dialect, extra_parentheses: bool) -> str:
        return_type = "int" if self.is_main else type_spelling("int32", dialect)
        parameters = ", ".join(
            f"{type_spelling('int32', dialect)} {name}" for name in self.parameters
        )
        lines = [f"{return_type} {self.name}({parameters}) {{"]
        lines.extend(emit_statements(self.body, dialect, 1, extra_parentheses))
        lines.append("}")
        return "\n".join(lines)

    def node_kinds(self) -> set[str]:
        kinds = {"function:main" if self.is_main else "function:free"}
        for statement in self.body:
            kinds.update(statement_node_kinds(statement))
        return kinds

    def record(self) -> dict[str, Any]:
        return {
            "name": self.name,
            "parameters": list(self.parameters),
            "is_main": self.is_main,
            "body": [statement_record(statement) for statement in self.body],
        }


@dataclass(frozen=True)
class Program:
    functions: tuple[Function, ...]
    extra_parentheses: bool = False

    def evaluate(self) -> int:
        functions = {function.name: function for function in self.functions}
        main = next((function for function in self.functions if function.is_main), None)
        if main is None:
            raise ModelError("model program has no main function")
        result = main.evaluate([], functions)
        if not isinstance(result, int) or isinstance(result, bool):
            raise ModelError("model main did not return int32")
        if result < 0 or result > 125:
            raise ModelError(f"model exit result is outside the portable range: {result}")
        return result

    def emit(self, dialect: Dialect) -> str:
        prefix = "#include <cstdint>\n\n" if dialect == "cpp" else ""
        return prefix + "\n\n".join(
            function.emit(dialect, self.extra_parentheses)
            for function in self.functions
        ) + "\n"

    def node_kinds(self) -> set[str]:
        kinds: set[str] = set()
        for function in self.functions:
            kinds.update(function.node_kinds())
        return kinds

    def record(self) -> dict[str, Any]:
        return {
            "extra_parentheses": self.extra_parentheses,
            "functions": [function.record() for function in self.functions],
        }


@dataclass(frozen=True)
class ModelParameters:
    base: int
    bias: int
    iterations: int
    increment: int
    multiplier: int
    delta: int
    threshold: int
    cap: int


TRANSFORMATIONS = ("alpha", "parentheses", "unused", "extract", "identity")


def build_program(parameters: ModelParameters, transformation: str) -> Program:
    if transformation not in ("original",) + TRANSFORMATIONS:
        raise ModelError(f"unknown transformation {transformation!r}")
    alpha = transformation == "alpha"
    names = {
        "adjust": "f" if alpha else "adjust",
        "left": "a" if alpha else "left",
        "right": "b" if alpha else "right",
        "product": "p" if alpha else "product",
        "total": "t" if alpha else "total",
        "bias": "v" if alpha else "bias",
        "enabled": "e" if alpha else "enabled",
        "index": "i" if alpha else "index",
        "unused": "u" if alpha else "unused_probe",
    }

    product_expression = binary(
        "mul", variable(names["left"]), variable(names["right"])
    )
    helper_prefix: tuple[Statement, ...] = ()
    if transformation == "extract":
        helper_prefix = (
            Binding(names["product"], "int32", False, product_expression),
        )
        product_expression = variable(names["product"])
    helper_condition = binary(
        "and",
        binary("less", int_literal(parameters.threshold), product_expression),
        logical_not(
            binary("equal", variable(names["left"]), variable(names["right"]))
        ),
    )
    helper = Function(
        names["adjust"],
        (names["left"], names["right"]),
        helper_prefix
        + (
            IfElse(
                helper_condition,
                (
                    Return(
                        binary("sub", product_expression, int_literal(parameters.delta))
                    ),
                ),
                (
                    Return(
                        binary("add", product_expression, int_literal(parameters.delta))
                    ),
                ),
            ),
        ),
    )

    adjustment = call(
        names["adjust"],
        (variable(names["bias"]), int_literal(parameters.multiplier)),
    )
    updated_total = binary("add", variable(names["total"]), adjustment)
    if transformation == "identity":
        updated_total = binary("add", updated_total, int_literal(0))
    main_body: list[Statement] = [
        Binding(names["total"], "int32", True, int_literal(parameters.base)),
        Binding(names["bias"], "int32", False, int_literal(parameters.bias)),
        Binding(
            names["enabled"],
            "bool",
            False,
            logical_not(
                binary("equal", variable(names["bias"]), int_literal(0))
            ),
        ),
        ForLoop(
            names["index"],
            parameters.iterations,
            (
                Assignment(
                    names["total"],
                    binary(
                        "add",
                        variable(names["total"]),
                        int_literal(parameters.increment),
                    ),
                ),
            ),
        ),
        IfElse(
            binary(
                "or",
                binary(
                    "less", variable(names["total"]), int_literal(parameters.cap)
                ),
                variable(names["enabled"], "bool"),
            ),
            (Assignment(names["total"], updated_total),),
            (
                Assignment(
                    names["total"],
                    binary("sub", variable(names["total"]), int_literal(1)),
                ),
            ),
        ),
    ]
    if transformation == "unused":
        main_body.append(Binding(names["unused"], "int32", False, int_literal(17)))
    main_body.append(Return(variable(names["total"])))
    main = Function("main", (), tuple(main_body), is_main=True)
    return Program((helper, main), extra_parentheses=transformation == "parentheses")


@dataclass(frozen=True)
class GeneratedCase:
    index: int
    seed: int
    parameters: ModelParameters
    transformation: str
    original: Program
    transformed: Program

    @property
    def expected(self) -> int:
        original = self.original.evaluate()
        transformed = self.transformed.evaluate()
        if original != transformed:
            raise ModelError(
                f"metamorphic transform {self.transformation} changed {original} to {transformed}"
            )
        return original

    def semantic_record(self) -> dict[str, Any]:
        sources = {
            "original_gti": self.original.emit("gti"),
            "transformed_gti": self.transformed.emit("gti"),
            "original_cpp": self.original.emit("cpp"),
            "transformed_cpp": self.transformed.emit("cpp"),
        }
        return {
            "schema": "gti-typed-audit-case-v1",
            "index": self.index,
            "seed": self.seed,
            "parameters": asdict(self.parameters),
            "transformation": self.transformation,
            "expected": self.expected,
            "node_kinds": sorted(self.original.node_kinds()),
            "source_sha256": {
                name: hashlib.sha256(source.encode("utf-8")).hexdigest()
                for name, source in sources.items()
            },
            "original": self.original.record(),
            "transformed": self.transformed.record(),
        }

    def semantic_json(self) -> str:
        return json.dumps(self.semantic_record(), indent=2, sort_keys=True) + "\n"


def generated_case(index: int, seed: int) -> GeneratedCase:
    generator = random.Random(seed + index * 7919)
    parameters = ModelParameters(
        base=generator.randint(1, 8),
        bias=generator.randint(0, 4),
        iterations=generator.randint(1, 4),
        increment=generator.randint(1, 3),
        multiplier=generator.randint(1, 3),
        delta=generator.randint(0, 1),
        threshold=generator.randint(0, 6),
        cap=generator.randint(4, 20),
    )
    transformation = TRANSFORMATIONS[index % len(TRANSFORMATIONS)]
    case = GeneratedCase(
        index=index,
        seed=seed,
        parameters=parameters,
        transformation=transformation,
        original=build_program(parameters, "original"),
        transformed=build_program(parameters, transformation),
    )
    case.expected
    return case


@dataclass(frozen=True)
class CompositionParameters:
    base: int
    delta: int
    replacement: int


COMPOSITION_SCENARIOS = (
    "local-class-method",
    "global-class-free-function",
    "generic-class-function",
    "class-return-readonly-borrow",
)


COMPOSITION_FEATURES: dict[str, tuple[str, ...]] = {
    "local-class-method": (
        "storage:local",
        "type:class",
        "access:member-read",
        "access:member-write",
        "call:method",
    ),
    "global-class-free-function": (
        "storage:global",
        "type:class",
        "access:member-read",
        "access:member-write",
        "call:free",
        "call:method",
    ),
    "generic-class-function": (
        "storage:local",
        "type:generic-class",
        "access:member-read",
        "access:member-write",
        "call:generic-free",
        "call:method",
    ),
    "class-return-readonly-borrow": (
        "storage:local",
        "type:class",
        "flow:class-return",
        "flow:readonly-borrow-argument",
        "call:free",
        "call:method",
    ),
}


@dataclass(frozen=True)
class CompositionProgram:
    scenario: str
    parameters: CompositionParameters
    transformed: bool

    def evaluate(self) -> int:
        state = {"value": checked_int32(self.parameters.base)}
        if self.scenario in {
            "local-class-method",
            "generic-class-function",
        }:
            state["value"] = checked_int32(
                state["value"] + self.parameters.delta
            )
        elif self.scenario == "global-class-free-function":
            state["value"] = checked_int32(self.parameters.replacement)
        elif self.scenario != "class-return-readonly-borrow":
            raise ModelError(f"unknown composition scenario {self.scenario!r}")
        result = state["value"]
        if result < 0 or result > 125:
            raise ModelError(
                f"composition exit result is outside the portable range: {result}"
            )
        return result

    def emit(self, dialect: Dialect) -> str:
        if self.scenario not in COMPOSITION_FEATURES:
            raise ModelError(f"unknown composition scenario {self.scenario!r}")
        integer = "int32_t" if dialect == "gti" else "std::int32_t"
        prefix = "#include <cstdint>\n\n" if dialect == "cpp" else ""
        mutable = "mut " if dialect == "gti" else ""
        receiver_mut = " mut" if dialect == "gti" else ""
        receiver_const = "" if dialect == "gti" else " const"
        member = "this.value" if dialect == "gti" else "this->value"

        if self.scenario == "local-class-method":
            operation = (
                f"  counter.value = counter.value + {self.parameters.delta};\n"
                "  return counter.value;"
                if self.transformed
                else f"  counter.add({self.parameters.delta});\n"
                "  return counter.read();"
            )
            source = f"""class Counter {{
public:
  {mutable}{integer} value;

  Counter({integer} initial) : value(initial) {{}}

  {integer} read(){receiver_const} {{ return {member}; }}

  void add({integer} amount){receiver_mut} {{
    {member} = {member} + amount;
  }}
}};

int main() {{
  {mutable}Counter counter = Counter({self.parameters.base});
{operation}
}}
"""
        elif self.scenario == "global-class-free-function":
            write = (
                "  shared.value = next;"
                if self.transformed
                else "  shared.set(next);"
            )
            read = (
                "  return shared.value;"
                if self.transformed
                else "  return shared.read();"
            )
            source = f"""class Cell {{
public:
  {mutable}{integer} value;

  Cell({integer} initial) : value(initial) {{}}

  {integer} read(){receiver_const} {{ return {member}; }}

  void set({integer} next){receiver_mut} {{ {member} = next; }}
}};

{mutable}Cell shared = Cell({self.parameters.base});

void update({integer} next) {{
{write}
}}

{integer} observe() {{
{read}
}}

int main() {{
  update({self.parameters.replacement});
  return observe();
}}
"""
        elif self.scenario == "generic-class-function":
            class_head = (
                "class Box<T>"
                if dialect == "gti"
                else "template <typename T>\nclass Box"
            )
            identity_head = (
                "T identity<T>(T value)"
                if dialect == "gti"
                else "template <typename T>\nT identity(T value)"
            )
            operation = (
                f"  box.value = box.value + {self.parameters.delta};\n"
                "  return box.value;"
                if self.transformed
                else f"  box.set(identity({self.parameters.base} + "
                f"{self.parameters.delta}));\n  return identity(box.read());"
            )
            source = f"""{class_head} {{
public:
  {mutable}T value;

  Box(T initial) : value(initial) {{}}

  T read(){receiver_const} {{ return {member}; }}

  void set(T next){receiver_mut} {{ {member} = next; }}
}};

{identity_head} {{ return value; }}

int main() {{
  {mutable}Box<{integer}> box = Box<{integer}>({self.parameters.base});
{operation}
}}
"""
        else:
            reference = "Reading&" if dialect == "gti" else "const Reading&"
            result = (
                "  return reading.read();"
                if self.transformed
                else "  return inspect(reading);"
            )
            source = f"""class Reading {{
public:
  {integer} value;

  Reading({integer} initial) : value(initial) {{}}

  {integer} read(){receiver_const} {{ return {member}; }}
}};

Reading make_reading({integer} value) {{
  return Reading(value);
}}

{integer} inspect({reference} reading) {{
  return reading.read();
}}

int main() {{
  Reading reading = make_reading({self.parameters.base});
{result}
}}
"""
        return prefix + source

    def node_kinds(self) -> set[str]:
        return {f"composition:{self.scenario}"} | {
            f"composition:{feature}"
            for feature in COMPOSITION_FEATURES[self.scenario]
        }

    def record(self) -> dict[str, Any]:
        return {
            "scenario": self.scenario,
            "features": list(COMPOSITION_FEATURES[self.scenario]),
            "parameters": asdict(self.parameters),
            "transformed": self.transformed,
        }


@dataclass(frozen=True)
class CompositionCase:
    index: int
    seed: int
    parameters: CompositionParameters
    transformation: str
    original: CompositionProgram
    transformed: CompositionProgram

    @property
    def expected(self) -> int:
        original = self.original.evaluate()
        transformed = self.transformed.evaluate()
        if original != transformed:
            raise ModelError(
                f"composition transform {self.transformation} changed "
                f"{original} to {transformed}"
            )
        return original

    def semantic_record(self) -> dict[str, Any]:
        sources = {
            "original_gti": self.original.emit("gti"),
            "transformed_gti": self.transformed.emit("gti"),
            "original_cpp": self.original.emit("cpp"),
            "transformed_cpp": self.transformed.emit("cpp"),
        }
        return {
            "schema": "gti-typed-composition-case-v1",
            "index": self.index,
            "seed": self.seed,
            "parameters": asdict(self.parameters),
            "transformation": self.transformation,
            "features": list(COMPOSITION_FEATURES[self.transformation]),
            "expected": self.expected,
            "node_kinds": sorted(self.original.node_kinds()),
            "source_sha256": {
                name: hashlib.sha256(source.encode("utf-8")).hexdigest()
                for name, source in sources.items()
            },
            "original": self.original.record(),
            "transformed": self.transformed.record(),
        }

    def semantic_json(self) -> str:
        return json.dumps(self.semantic_record(), indent=2, sort_keys=True) + "\n"


TypedAuditCase = GeneratedCase | CompositionCase


def composition_case(index: int, seed: int) -> CompositionCase:
    generator = random.Random(seed ^ (index * 104729) ^ 0x434F4D50)
    base = generator.randint(1, 20)
    delta = generator.randint(1, 5)
    parameters = CompositionParameters(
        base=base,
        delta=delta,
        replacement=generator.randint(1, 25),
    )
    scenario = COMPOSITION_SCENARIOS[index % len(COMPOSITION_SCENARIOS)]
    case = CompositionCase(
        index=index,
        seed=seed,
        parameters=parameters,
        transformation=scenario,
        original=CompositionProgram(scenario, parameters, False),
        transformed=CompositionProgram(scenario, parameters, True),
    )
    case.expected
    return case
