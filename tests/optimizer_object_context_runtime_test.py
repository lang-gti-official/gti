#!/usr/bin/env python3
"""Ordinary constructor/result/receiver facts preserve checked execution."""

import os
import pathlib
import shutil
import tempfile

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixture = pathlib.Path(__file__).parent / "fixtures/optimizer_object_context.gti"
    source = fixture.read_text()
    marker = "return danger.read();"
    if source.count(marker) != 1:
        raise AssertionError("the object's final runtime observation must be unique")
    lines = source.splitlines()
    hazard_line = next(index for index, line in enumerate(lines, 1)
                       if line == "class Hazard {")
    read_line = next(index for index, line in enumerate(lines, 1)
                     if "int read() { return this.value + 1; }" in line and index > hazard_line)
    changed_line = next(index for index, line in enumerate(lines, 1)
                        if "return this.value + 1;" in line and index > read_line)
    raw_line = next(index for index, line in enumerate(lines, 1)
                    if "return this.value + 1;" in line and index > changed_line)
    alias_start = next(index for index, line in enumerate(lines, 1)
                       if line == "class Aliased {")
    alias_line = next(index for index, line in enumerate(lines, 1)
                      if line.strip() == "return this.value + 1;" and index > alias_start)
    raw_constructor_start = next(index for index, line in enumerate(lines, 1)
                                 if line == "class RawConstructed {")
    raw_constructor_line = next(index for index, line in enumerate(lines, 1)
                                if "int read() { return this.value + 1; }" in line
                                and index > raw_constructor_start)
    variants = {
        "object_success": ("return 0;", None),
        "object_overflow": (marker, read_line),
        "object_changed": ("return danger.changed();", changed_line),
        "object_raw": ("return danger.raw_changed();", raw_line),
        "object_alias": ("return alias_case();", alias_line),
        "object_raw_constructor": ("return raw_constructor_case();", raw_constructor_line),
    }
    with tempfile.TemporaryDirectory(prefix="gti-object-context-source-") as temporary:
        paths = {}
        cases = {}
        for name, (replacement, line) in variants.items():
            path = pathlib.Path(temporary) / f"{name}.gti"
            path.write_text(source.replace(marker, replacement))
            paths[name] = path
            error = (f"{path.name}:{line}: runtime error[GTI-R0001]: "
                     "integer_overflow in addition\n") if line else ""
            cases[name] = [((), (70 if line else 0, "", error))]
        chain = []
        # Direct-mode discovery currently caps depth at 64. Keep this native
        # client within that independent frontend boundary; the owning C++
        # unit exercises the optimizer itself with 128 ordinary functions.
        chain_length = 48
        for index in reversed(range(chain_length)):
            value = "value" if index == chain_length - 1 else f"step{index + 1}(value)"
            chain.append(f"int step{index}(int value) {{ return {value} + 1; }}")
        chain.append(f"int main() {{ return step0(7) == {7 + chain_length} ? 0 : 1; }}")
        path = pathlib.Path(temporary) / "object_chain.gti"
        path.write_text("\n".join(chain))
        paths["object_chain"] = path
        cases["object_chain"] = [((), (0, "", ""))]
        budget = [f"int child{index}() {{ return 0; }}" for index in range(40)]
        budget.append("int budget(int input) { int first = input + 1;")
        budget.extend(f"[[discard]] child{index}();" for index in range(40))
        budget.append("return first; }")
        budget.append("int independent(int input) { return input + 1; }")
        budget.append("int main() { int first = budget(7); int second = independent(7); "
                      "return first == 8 && second == 8 ? 0 : 1; }")
        path = pathlib.Path(temporary) / "object_budget.gti"
        path.write_text("\n".join(budget))
        paths["object_budget"] = path
        cases["object_budget"] = [((), (0, "", ""))]
        for name in ("optimizer_nested_write", "optimizer_reference_write",
                     "optimizer_nested_raw"):
            path = fixture.parent / f"{name}.gti"
            sites = [index for index, text in enumerate(path.read_text().splitlines(), 1)
                     if "int read() { return this.value + 1; }" in text]
            assert len(sites) == 1, name
            error = (f"{path.name}:{sites[0]}: runtime error[GTI-R0001]: "
                     "integer_overflow in addition\n")
            paths[name] = path
            cases[name] = [((), (70, "", error))]
        for construction_kind in ("scalar", "record"):
            construction = fixture.parent / f"optimizer_{construction_kind}_construction.gti"
            construction_source = construction.read_text()
            sites = [index for index, text in enumerate(construction_source.splitlines(), 1)
                     if "return record.value + 1;" in text]
            assert len(sites) == (2 if construction_kind == "scalar" else 1)
            paths[f"{construction_kind}_reconstruction"] = construction
            cases[f"{construction_kind}_reconstruction"] = [((), (70, "",
                f"{construction.name}:{sites[-1]}: runtime error[GTI-R0001]: "
                "integer_overflow in addition\n"))]
            path = pathlib.Path(temporary) / f"{construction_kind}_construction.gti"
            path.write_text(construction_source.replace("return reconstruct(record);", "return 0;"))
            paths[f"{construction_kind}_construction"] = path
            cases[f"{construction_kind}_construction"] = [((), (0, "", ""))]
        construction_paths = fixture.parent / "optimizer_scalar_construction_paths.gti"
        paths_source = construction_paths.read_text()
        sites = [index for index, text in enumerate(paths_source.splitlines(), 1)
                 if text.strip() == "return layer.nested.value + 1;"]
        assert len(sites) == 2
        paths["scalar_paths_reconstruction"] = construction_paths
        cases["scalar_paths_reconstruction"] = [((), (70, "",
            f"{construction_paths.name}:{sites[1]}: runtime error[GTI-R0001]: "
            "integer_overflow in addition\n"))]
        path = pathlib.Path(temporary) / "scalar_paths_construction.gti"
        path.write_text(paths_source.replace("return nested_reconstruct();", "return 0;"))
        paths["scalar_paths_construction"] = path
        cases["scalar_paths_construction"] = [((), (0, "", ""))]
        witness = fixture.parent / "optimizer_witness_formation.gti"
        witness_source = witness.read_text()
        witness_changed_line = next(index for index, text in
                                    enumerate(witness_source.splitlines(), 1)
                                    if text == "int changed(mut Witness& witness) {")
        witness_failure_line = next(index for index, text in
                                    enumerate(witness_source.splitlines(), 1)
                                    if index > witness_changed_line and
                                    text.strip() == "return witness.value + 1;")
        witness_other_line = next(index for index, text in
                                  enumerate(witness_source.splitlines(), 1)
                                  if text.strip() == "return witness.read_other() + 1;")
        paths["witness_changed"] = witness
        cases["witness_changed"] = [((), (70, "",
            f"{witness.name}:{witness_failure_line}: runtime error[GTI-R0001]: "
            "integer_overflow in addition\n"))]
        witness_tail = "  int changed_result = changed(witness);\n  return changed_result + other_value(witness);"
        assert witness_source.count(witness_tail) == 1
        for name, replacement, failure_line in (
                ("witness_success", "  return 0;", None),
                ("witness_other", "  return other_value(witness);", witness_other_line)):
            path = pathlib.Path(temporary) / f"{name}.gti"
            path.write_text(witness_source.replace(witness_tail, replacement))
            paths[name] = path
            error = (f"{path.name}:{failure_line}: runtime error[GTI-R0001]: "
                     "integer_overflow in addition\n") if failure_line else ""
            cases[name] = [((), (70 if failure_line else 0, "", error))]
        owned = fixture.parent / "optimizer_owned_value_transfer.gti"
        owned_source = owned.read_text()
        owned_lines = owned_source.splitlines()
        negative = owned_lines.index("int original_after_sanitize() {")
        owned_line = next(index for index, text in enumerate(owned_lines, 1)
                          if index > negative and
                          text.strip() == "return packet.part.value + 1;")
        paths["owned_value_original_unchanged"] = owned
        cases["owned_value_original_unchanged"] = [((), (70, "",
            f"{owned.name}:{owned_line}: runtime error[GTI-R0001]: "
            "integer_overflow in addition\n"))]
        path = pathlib.Path(temporary) / "owned_value_success.gti"
        path.write_text(owned_source.replace("return original_after_sanitize();",
                                             "return 0;"))
        paths["owned_value_success"] = path
        cases["owned_value_success"] = [((), (0, "", ""))]
        movement = fixture.parent / "optimizer_scalar_move.gti"
        move_source = movement.read_text()
        sites = [index for index, text in enumerate(move_source.splitlines(), 1)
                 if text.strip() in ("return input + 1;", "return this.value + 1;")]
        assert len(sites) == 2
        paths["scalar_move_reinitialized_parameter"] = movement
        cases["scalar_move_reinitialized_parameter"] = [((), (70, "",
            f"{movement.name}:{sites[0]}: runtime error[GTI-R0001]: "
            "integer_overflow in addition\n"))]
        path = pathlib.Path(temporary) / "scalar_move_reinitialized_field.gti"
        path.write_text(move_source.replace(
            "if (true) return gauge.replace_parameter(3);", ""))
        paths["scalar_move_reinitialized_field"] = path
        # The removed guard occupies its own line; source line identity remains.
        cases["scalar_move_reinitialized_field"] = [((), (70, "",
            f"{path.name}:{sites[1]}: runtime error[GTI-R0001]: "
            "integer_overflow in addition\n"))]
        path = pathlib.Path(temporary) / "scalar_move_success.gti"
        path.write_text(move_source.replace(
            "if (true) return gauge.replace_parameter(3);", "return 0;"))
        paths["scalar_move_success"] = path
        cases["scalar_move_success"] = [((), (0, "", ""))]
        staging = fixture.parent / "optimizer_value_staging.gti"
        stage_source = staging.read_text()
        sites = [index for index, text in enumerate(stage_source.splitlines(), 1)
                 if text.strip() == "return this.value + 1;"]
        assert len(sites) == 4
        paths["value_stage_raw_write"] = staging
        cases["value_stage_raw_write"] = [((), (70, "",
            f"{staging.name}:{sites[2]}: runtime error[GTI-R0001]: "
            "integer_overflow in addition\n"))]
        path = pathlib.Path(temporary) / "value_stage_unsafe_call.gti"
        path.write_text(stage_source.replace(
            "if (true) return gauge.after_raw_write();", ""))
        paths["value_stage_unsafe_call"] = path
        cases["value_stage_unsafe_call"] = [((), (70, "",
            f"{path.name}:{sites[3]}: runtime error[GTI-R0001]: "
            "integer_overflow in addition\n"))]
        path = pathlib.Path(temporary) / "value_stage_success.gti"
        path.write_text(stage_source.replace(
            "if (true) return gauge.after_raw_write();", "return 0;"))
        paths["value_stage_success"] = path
        cases["value_stage_success"] = [((), (0, "", ""))]
        nested = fixture.parent / "optimizer_nested_getter_loop.gti"
        nested_source = nested.read_text()
        nested_line = next(index for index, text in
                           enumerate(nested_source.splitlines(), 1)
                           if "return counter.read() + uint64_t(1);" in text)
        paths["nested_getter_changed"] = nested
        cases["nested_getter_changed"] = [((), (70, "",
            f"{nested.name}:{nested_line}: runtime error[GTI-R0001]: "
            "integer_overflow in addition\n"))]
        path = pathlib.Path(temporary) / "nested_getter_success.gti"
        path.write_text(nested_source.replace("return int(changed_nested(counter));",
                                              "return 0;"))
        paths["nested_getter_success"] = path
        cases["nested_getter_success"] = [((), (0, "", ""))]
        getters = fixture.parent / "optimizer_getter_fields.gti"
        getter_source = getters.read_text()
        getter_lines = getter_source.splitlines()
        getter_dispatch = [text for text in getter_lines
                           if text.strip().startswith(("if (true) return int(",
                                                       "if (false) return int("))]
        assert len(getter_dispatch) == 6
        getter_marker = "return int(copy_snapshot(meter));"
        assert getter_source.count(getter_marker) == 1
        clean_getters = getter_source
        for dispatch in getter_dispatch:
            clean_getters = clean_getters.replace(dispatch, "")
        getter_cases = {"getter_success": ("return 0;", None)}
        for function in ("order_changed", "cached_local", "ambiguous",
                         "direct_changed", "mutable_changed", "raw_changed",
                         "copy_snapshot"):
            start = next(index for index, text in enumerate(getter_lines, 1)
                         if text.startswith(f"uint64_t {function}("))
            line = next(index for index, text in enumerate(getter_lines, 1)
                        if index > start and "return meter.count + uint64_t(1);" in text)
            argument = "huge" if function == "ambiguous" else "meter"
            getter_cases[f"getter_{function}"] = (f"return int({function}({argument}));", line)
        for name, (replacement, line) in getter_cases.items():
            path = pathlib.Path(temporary) / f"{name}.gti"
            path.write_text(clean_getters.replace(getter_marker, replacement))
            paths[name] = path
            error = (f"{path.name}:{line}: runtime error[GTI-R0001]: "
                     "integer_overflow in addition\n") if line else ""
            cases[name] = [((), (70 if line else 0, "", error))]
        globals_fixture = fixture.parent / "optimizer_global_getter_alias.gti"
        global_source = globals_fixture.read_text()
        global_lines = global_source.splitlines()
        global_dispatch = [text for text in global_lines
                           if text.strip().startswith(("if (true) return case_",
                                                       "if (false) return case_"))]
        assert len(global_dispatch) == 6
        global_marker = "return case_global_initializer(shared_meter);"
        assert global_source.count(global_marker) == 1
        clean_globals = global_source
        for dispatch in global_dispatch:
            clean_globals = clean_globals.replace(dispatch, "")
        global_cases = {"getter_global_success": ("return 0;", None)}
        for function in ("case_global_alias", "case_global_forwarded",
                         "case_global_method", "case_global_initializer",
                         "case_global_reference", "case_global_scalar_reference",
                         "case_global_reborrow"):
            start = next(index for index, text in enumerate(global_lines, 1)
                         if text.startswith(f"int {function}("))
            line = next(index for index, text in enumerate(global_lines, 1)
                        if index > start and "return meter.read() + 1;" in text)
            global_cases[f"getter_{function}"] = (f"return {function}(shared_meter);", line)
        for name, (replacement, line) in global_cases.items():
            path = pathlib.Path(temporary) / f"{name}.gti"
            path.write_text(clean_globals.replace(global_marker, replacement))
            paths[name] = path
            error = (f"{path.name}:{line}: runtime error[GTI-R0001]: "
                     "integer_overflow in addition\n") if line else ""
            cases[name] = [((), (70 if line else 0, "", error))]
        guard = fixture.parent / "optimizer_boolean_guards.gti"
        guard_source = guard.read_text()
        # The detectors are compiler-private. Test them behind a temporary
        # ordinary trusted source module, never by relaxing application access.
        library = pathlib.Path(temporary) / "stdlib"
        shutil.copytree(pathlib.Path(os.environ.get("GTI_STDLIB_PATH",
            fixture.parent.parent.parent / "stdlib")), library)
        module, entry = guard_source.split("int main() {", 1)
        (library / "std" / guard.name).write_text(module)
        entry_source = "#include <std/optimizer_boolean_guards>\nint main() {" + entry
        sites = [index for index, text in enumerate(guard_source.splitlines(), 1)
                 if text.strip() == "return input + uint64_t(1);"]
        assert len(sites) == 3
        path = pathlib.Path(temporary) / "guard_changed.gti"
        path.write_text(entry_source)
        paths["guard_changed"] = path
        cases["guard_changed"] = [((), (70, "",
            f"<std/optimizer_boolean_guards>:{sites[2]}: runtime error[GTI-R0001]: "
            "integer_overflow in addition\n"))]
        path = pathlib.Path(temporary) / "guard_success.gti"
        path.write_text(entry_source.replace(
            "return changed(uint64_t(0)) == uint64_t(0) ? 0 : 3;", "return 0;"))
        paths["guard_success"] = path
        cases["guard_success"] = [((), (0, "", ""))]
        path = pathlib.Path(temporary) / "guard_rejected.gti"
        path.write_text(entry_source.replace("if (true)", "if (false)", 1))
        paths["guard_rejected"] = path
        cases["guard_rejected"] = [((), (70, "",
            "<std/optimizer_boolean_guards>:4: runtime error[GTI-R0010]: "
            "invalid_storage_state in relocation_capacity\n"))]
        previous_library = os.environ.get("GTI_STDLIB_PATH")
        os.environ["GTI_STDLIB_PATH"] = str(library)
        try:
            main(names=tuple(paths), cases=cases, fixture_paths=paths)
        finally:
            if previous_library is None:
                os.environ.pop("GTI_STDLIB_PATH", None)
            else:
                os.environ["GTI_STDLIB_PATH"] = previous_library
