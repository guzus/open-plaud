"""Pipeline orchestrator.

Resolves the three stages from config (by name) and runs them in order, updating
job status as it goes. It imports no concrete provider — only the registries.
That decoupling is the whole point: cloud or local, the orchestrator is identical.
"""
from __future__ import annotations

import logging

from open_plaud import diarization as diar_mod
from open_plaud import summarization as summ_mod
from open_plaud import transcription as tx_mod
from open_plaud.config import Config
from open_plaud.types import JobStatus, Recording, TranscribeOptions

log = logging.getLogger("open_plaud.pipeline")


class Pipeline:
    def __init__(self, config: Config) -> None:
        self.config = config
        # Resolve providers once at startup so misconfig fails fast and loud.
        self.transcriber = tx_mod.create(config.pipeline.transcription)
        self.diarizer = diar_mod.create(config.pipeline.diarization)
        self.summarizer = summ_mod.create(config.pipeline.summarization)
        log.info(
            "pipeline ready: transcribe=%s diarize=%s summarize=%s",
            self.transcriber.name, self.diarizer.name, self.summarizer.name,
        )

    async def run(self, rec: Recording, *, diarize: bool = False,
                  summarize: bool = True, language: str | None = None,
                  instructions: str | None = None) -> Recording:
        try:
            rec.status = JobStatus.TRANSCRIBING
            opts = TranscribeOptions(language=language, diarize=diarize)
            rec.transcript = await self.transcriber.transcribe(rec.audio, opts)

            if diarize:
                rec.status = JobStatus.DIARIZING
                rec.diarization = await self.diarizer.diarize(rec.audio)

            if summarize and rec.transcript and rec.transcript.text:
                rec.status = JobStatus.SUMMARIZING
                rec.summary = await self.summarizer.summarize(
                    rec.transcript, instructions=instructions
                )

            rec.status = JobStatus.DONE
        except Exception as exc:  # noqa: BLE001 — surface any stage failure on the job
            log.exception("pipeline failed for recording %s", rec.id)
            rec.status = JobStatus.FAILED
            rec.error = f"{type(exc).__name__}: {exc}"
        return rec
