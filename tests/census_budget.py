"""The per-example budget shared by the MIR and LoweredProgram census checks.

A census baseline records reviewed counts for every shipped example. Requiring
exact equality made every pull request that touched the standard library
rewrite dozens of rows (one more stdlib function is one more declaration or
instance in every importing example), so any two such pull requests conflicted
and dropped out of the merge queue. The census exists to catch real growth or
loss in what an example lowers to, not single-instance ripples, so a recorded
count may drift by the larger of ABSOLUTE counts or RELATIVE of itself before
the check fails. A pull request then edits a baseline only for the examples it
changes materially, and only those rows (``--update``); ``--refresh-all``
re-records every example, including ones added since the last refresh.
"""

from __future__ import annotations

import math

ABSOLUTE = 5
RELATIVE = 0.05


def allowance(recorded: int) -> int:
    """How far an observed count may drift from a recorded one."""
    return max(ABSOLUTE, math.ceil(recorded * RELATIVE))


def within(recorded: int, observed: int) -> bool:
    return abs(observed - recorded) <= allowance(recorded)


def ordered(previous: dict, values: dict) -> dict:
    """``values`` in ``previous``'s key order, new keys appended in sorted order.

    Keeping the recorded order makes a refresh touch only the rows it changes.
    """
    result = {name: values[name] for name in previous if name in values}
    for name in sorted(set(values) - set(previous)):
        result[name] = values[name]
    return result
