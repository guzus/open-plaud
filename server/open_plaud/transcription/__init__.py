from open_plaud.transcription.base import available, create, register

# Register built-in providers.
from open_plaud.transcription import noop as _noop  # noqa: F401
from open_plaud.transcription import openai_whisper as _openai_whisper  # noqa: F401
from open_plaud.transcription import whisper_cpp as _whisper_cpp  # noqa: F401

__all__ = ["available", "create", "register"]
