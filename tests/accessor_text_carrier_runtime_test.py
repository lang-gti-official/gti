#!/usr/bin/env python3
"""Carriers of an accessor's returned view run in every native mode."""

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    main(names=("accessor_text_carriers",),
         cases={"accessor_text_carriers": [
             ((), (0, "o\nl\nl\n5\nl\n2\n7\n5\no\no\n4\n4\n", ""))]})
