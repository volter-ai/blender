"""Small explicit Pyodide compatibility surface; browser interop is unavailable."""

_DEFERRED = object()


class JsException(RuntimeError):
    pass


class JsProxy:
    def __init__(self, value=_DEFERRED):
        self._value = value

    def __getattr__(self, name):
        raise JsException(f"browser JS proxy operation is unavailable: {name}")


class JsArray(JsProxy):
    pass


def to_js(_value, **_kwargs):
    raise JsException("Python-to-JavaScript conversion is unavailable in Blender WASM")
