#!/usr/bin/env python3
"""Parenthesized lambdas are their producer's closure in every consumer."""

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    main(names=("grouped_lambda_values",),
         cases={"grouped_lambda_values": [((), (0, "grouped lambdas ok\n", ""))]})
