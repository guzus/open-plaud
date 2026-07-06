from __future__ import annotations

from pathlib import Path

from pydantic import Field
from pydantic_settings import BaseSettings, SettingsConfigDict


class PipelineConfig(BaseSettings):
    transcription: str = "none"
    diarization: str = "none"
    summarization: str = "none"


class HookerConfig(BaseSettings):
    enabled: bool = False
    url: str = ""
    api_key: str = ""
    topic: str = "agent_in"
    priority: int = Field(default=3, ge=1, le=5)
    tags: str = "agent,user,voice,open-plaud"
    timeout_seconds: float = Field(default=15.0, gt=0)
    prompt_footer: str = (
        "Treat this as a spoken vibecoding command. If the request is ambiguous "
        "or could mutate files in a risky way, ask for clarification or propose "
        "a plan before changing code."
    )


class Config(BaseSettings):
    model_config = SettingsConfigDict(
        env_prefix="OP_",
        env_nested_delimiter="__",
        extra="ignore",
    )

    storage_dir: Path = Field(default=Path("storage"))
    upload_max_bytes: int = Field(default=512 * 1024 * 1024, ge=1)
    pipeline: PipelineConfig = Field(default_factory=PipelineConfig)
    hooker: HookerConfig = Field(default_factory=HookerConfig)
