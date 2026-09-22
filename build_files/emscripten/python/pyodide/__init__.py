"""Compatibility import surface for packages that optionally detect Pyodide."""

from .ffi import JsArray, JsException, JsProxy, to_js

__all__ = ["JsArray", "JsException", "JsProxy", "to_js"]
