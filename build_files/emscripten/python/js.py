"""Measured console-only subset of the browser ``js`` module."""

import sys as _sys


class _Console:
    @staticmethod
    def log(*values):
        print(*values, file=_sys.stdout)

    @staticmethod
    def warn(*values):
        print(*values, file=_sys.stderr)

    @staticmethod
    def error(*values):
        print(*values, file=_sys.stderr)

    def __getattr__(self, name):
        raise RuntimeError(f"browser JS console operation is unavailable: {name}")


console = _Console()
