#!/usr/bin/env python3
"""RFC 0004 signature-contract gate: borrow dependencies may only widen.

The owner decided borrow contracts come only from signatures; an unannotated
borrowed result borrows from every borrow-carrying input. Main infers
narrower contracts from bodies, so dependency sets may legitimately grow.
Given two `gti --emit-borrow-dependencies` dumps (schema
gti-borrow-dependencies-v1, see include/gti/driver/borrow_dependencies.h), this
check requires, for every call result and every callee contract row:

  - the row exists in both dumps;
  - new dependencies are a superset of the old ones (exact elements: root,
    source field path and result field path);
  - every added dependency's root is a borrow-carrying input of the callee
    in the new dump: `receiver` when the callee has a borrow-carrying
    receiver, or `parameter:<i>` for a listed reference-input parameter.

Each violation is reported individually. A coarser element that replaces a
field-specific one counts as a shrink: the check is deliberately exact.

Usage: borrow_dependency_monotonicity.py OLD.json NEW.json
       borrow_dependency_monotonicity.py --self-test
"""
import collections
import json
import sys

SCHEMA = "gti-borrow-dependencies-v1"


def _dependency(entry):
    return (entry["root"], tuple(entry["fields"]), tuple(entry["result_fields"]))


def _describe(dependency):
    root, fields, result_fields = dependency
    text = root + "".join("." + field for field in fields)
    if result_fields:
        text += " -> " + ".".join(result_fields)
    return text


def _rows(document, section, key_fields):
    """Rows keyed by identity; repeated identities are numbered in order."""
    if document.get("schema") != SCHEMA:
        raise ValueError(f"unexpected borrow-dependency schema {document.get('schema')!r}")
    rows = {}
    seen = collections.Counter()
    for row in document[section]:
        identity = tuple(row[field] for field in key_fields)
        seen[identity] += 1
        rows[identity + (seen[identity] - 1,)] = row
    return rows


def _label(section, key):
    if section == "calls":
        body, callee, kind, ordinal, repeat = key
        label = f"{kind} {callee} #{ordinal} in {body}"
    else:
        callee, kind, repeat = key
        label = f"{kind} contract {callee}"
    return label + (f" (repeat {repeat})" if repeat else "")


def check(old, new):
    """Returns (failures, statistics) for two parsed dumps."""
    failures = []
    stats = collections.Counter()
    sections = (("calls", ("body", "callee", "kind", "ordinal")),
                ("contracts", ("callee", "kind")))
    for section, key_fields in sections:
        old_rows = _rows(old, section, key_fields)
        new_rows = _rows(new, section, key_fields)
        for key in sorted(set(old_rows) | set(new_rows), key=repr):
            label = _label(section, key)
            if key not in new_rows:
                failures.append(f"{label}: missing from the new dump")
                continue
            if key not in old_rows:
                failures.append(f"{label}: missing from the old dump")
                continue
            stats[f"{section}_compared"] += 1
            before = {_dependency(entry) for entry in old_rows[key]["dependencies"]}
            after = {_dependency(entry) for entry in new_rows[key]["dependencies"]}
            inputs = new_rows[key]["reference_inputs"]
            allowed = {f"parameter:{index}" for index in inputs["parameters"]}
            if inputs["receiver"]:
                allowed.add("receiver")
            for dependency in sorted(before - after):
                failures.append(f"{label}: dependency {_describe(dependency)} removed")
            added = sorted(after - before)
            for dependency in added:
                if dependency[0] not in allowed:
                    failures.append(f"{label}: added dependency {_describe(dependency)} "
                                    "is not a borrow-carrying input of the callee")
            if added:
                stats[f"{section}_widened"] += 1
                stats[f"{section}_added"] += len(added)
    stats["failures"] = len(failures)
    return failures, stats


def _self_test():
    def dump(dependencies, parameters=(0, 1), receiver=False):
        row = {"dependencies": [{"root": root, "fields": list(fields),
                                 "result_fields": []}
                                for root, fields in dependencies],
               "reference_inputs": {"parameters": list(parameters),
                                    "receiver": receiver}}
        call = dict(row, body="function f()", ordinal=0,
                    callee="function g(int&,int&)", kind="call", hir_value=1)
        contract = dict(row, callee="function g(int&,int&)", kind="function",
                        explicit=False)
        return {"schema": SCHEMA, "calls": [call], "contracts": [contract]}

    narrow = dump([("parameter:0", ())])
    cases = {
        "identical passes": (narrow, narrow, 0),
        "superset by a reference input passes":
            (narrow, dump([("parameter:0", ()), ("parameter:1", ())]), 0),
        "shrink fails": (dump([("parameter:0", ()), ("parameter:1", ())]),
                         narrow, 2),
        "added non-reference input fails":
            (narrow, dump([("parameter:0", ()), ("parameter:2", ())]), 2),
        "added global fails":
            (narrow, dump([("parameter:0", ()), ("global:g", ())]), 2),
        "added receiver needs a borrow-carrying receiver":
            (narrow, dump([("parameter:0", ()), ("receiver", ())]), 2),
        "added borrow-carrying receiver passes":
            (dump([("parameter:0", ())], receiver=True),
             dump([("parameter:0", ()), ("receiver", ())], receiver=True), 0),
        "field path replaced by its root fails as a shrink":
            (dump([("parameter:0", ("f",))]), narrow, 2),
        "missing call fails":
            (narrow, dict(narrow, calls=[]), 1),
    }
    failed = 0
    for name, (old, new, expected) in cases.items():
        failures, stats = check(old, new)
        if len(failures) != expected:
            failed += 1
            print(f"FAIL: {name}: expected {expected} failure(s), got {failures}")
    widened = check(narrow, dump([("parameter:0", ()), ("parameter:1", ())]))[1]
    if (widened["calls_widened"], widened["calls_added"]) != (1, 1):
        failed += 1
        print(f"FAIL: widening statistics: {dict(widened)}")
    try:
        check({"schema": "other", "calls": [], "contracts": []}, narrow)
        failed += 1
        print("FAIL: an unknown schema is accepted")
    except ValueError:
        pass
    print(f"borrow dependency monotonicity self-test: "
          f"{len(cases) + 2 - failed}/{len(cases) + 2} passed")
    return 1 if failed else 0


def main(argv):
    if argv == ["--self-test"]:
        return _self_test()
    if len(argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    with open(argv[0], encoding="utf-8") as old, open(argv[1], encoding="utf-8") as new:
        failures, stats = check(json.load(old), json.load(new))
    for failure in failures:
        print(failure)
    print(", ".join(f"{name}={stats[name]}" for name in sorted(stats)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
