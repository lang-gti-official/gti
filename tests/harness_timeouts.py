"""One timeout scale for every harness subprocess wait.

Harness timeouts are sized for a developer host and separate a hang from a
slow compile. A slower host states its factor in ``GTI_TEST_TIMEOUT_SCALE``
instead of loosening the developer-host values: CTest exports the configured
``GTI_TEST_TIMEOUT_SCALE`` cache setting to every test and multiplies its own
``TIMEOUT`` properties by the same factor (``tests/TestTimeouts.cmake``); a
standalone harness reads the variable from its environment. The scale changes
no case, assertion or observation, and ``harness_timeouts_test.py`` rejects a
harness that bypasses it with a literal timeout.
"""

import os
import re

VARIABLE = "GTI_TEST_TIMEOUT_SCALE"


def timeout_scale(environment=None):
    """Return the positive integer scale; unset or empty means 1."""
    value = (os.environ if environment is None else environment).get(VARIABLE, "")
    if value == "":
        return 1
    if not re.fullmatch(r"[1-9][0-9]*", value):
        raise ValueError(f"{VARIABLE} must be a positive integer, not {value!r}")
    return int(value)


def scaled(seconds):
    """Scale a developer-host timeout for the current host."""
    return seconds * timeout_scale()
