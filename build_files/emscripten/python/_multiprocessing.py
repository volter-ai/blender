"""Single-process compatibility for libraries that use multiprocessing locks."""

import threading
import time as _time

RECURSIVE_MUTEX = 0
SEMAPHORE = 1


class SemLock:
    SEM_VALUE_MAX = 2**31 - 1

    def __init__(self, kind, value, maxvalue, name=None, unlink=False):
        if kind not in (RECURSIVE_MUTEX, SEMAPHORE):
            raise ValueError(f"unsupported semaphore kind: {kind}")
        if not 0 <= value <= maxvalue <= self.SEM_VALUE_MAX:
            raise ValueError("invalid semaphore value")
        self.kind = kind
        self.maxvalue = maxvalue
        # The browser runtime has no named/process-shared semaphore namespace.
        # Keeping these None also prevents multiprocessing.resource_tracker
        # from trying to start an unsupported helper process.
        self.handle = None
        self.name = None
        self._value = value
        self._last_owner = None
        self._local_count = 0
        self._condition = threading.Condition()

    def acquire(self, block=True, timeout=None):
        ident = threading.get_ident()
        with self._condition:
            if self.kind == RECURSIVE_MUTEX and self._last_owner == ident and self._local_count:
                self._local_count += 1
                return True
            deadline = None if timeout is None else _time.monotonic() + max(timeout, 0)
            while self._value == 0:
                if not block:
                    return False
                remaining = None if deadline is None else deadline - _time.monotonic()
                if remaining is not None and remaining <= 0:
                    return False
                self._condition.wait(remaining)
            self._value -= 1
            self._last_owner = ident
            self._local_count += 1
            return True

    __enter__ = acquire

    def release(self):
        ident = threading.get_ident()
        with self._condition:
            if self.kind == RECURSIVE_MUTEX:
                if self._last_owner != ident or not self._local_count:
                    raise AssertionError("attempt to release recursive lock not owned by thread")
                self._local_count -= 1
                if self._local_count:
                    return
                self._last_owner = None
            if self._value >= self.maxvalue:
                raise ValueError("semaphore or lock released too many times")
            self._value += 1
            if self.kind == SEMAPHORE:
                # Match the measured browser shim: release without a local
                # acquire does not create a negative process-local count.
                self._local_count = max(0, self._local_count - 1)
                if not self._local_count:
                    self._last_owner = None
            self._condition.notify()

    def __exit__(self, *_exc):
        self.release()

    def _count(self):
        return self._local_count

    def _get_value(self):
        return self._value

    def _is_mine(self):
        return self._local_count > 0 and self._last_owner == threading.get_ident()

    def _is_zero(self):
        return self._value == 0

    def _after_fork(self):
        return None

    @classmethod
    def _rebuild(cls, _handle, kind, maxvalue, name):
        # The existing one-process browser contract rebuilds a fresh unlocked
        # semaphore; no cross-process handle exists to reattach.
        return cls(kind, maxvalue, maxvalue, name)


def sem_unlink(_name):
    return None
