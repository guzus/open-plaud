from __future__ import annotations

from pathlib import Path

from pydantic import Field
from pydantic_settings import BaseSettings, SettingsConfigDict


class PipelineConfig(BaseSettings):
    transcription: str = "none"
    diarization: str = "none"
    summarization: str = "none"


class Config(BaseSettings):
    model_config = SettingsConfigDict(
        env_prefix="OP_",
        env_nested_delimiter="__",
        extra="ignore",
    )

    storage_dir: Path = Field(default=Path("storage"))
    pipeline: PipelineConfig = Field(default_factory=PipelineConfig)
