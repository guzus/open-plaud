from open_plaud.summarization.base import available, create, register

# Register built-in providers.
from open_plaud.summarization import noop as _noop  # noqa: F401
from open_plaud.summarization import claude as _claude  # noqa: F401

__all__ = ["available", "create", "register"]
