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
from open_plaud.types import JobStatus, Recording, TranscribeOptions, Transcript, TranscriptSegment

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
                if rec.transcript is not None:
                    rec.transcript = _merge_speakers(rec.transcript, rec.diarization)

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


def _segment_overlap_seconds(
    transcript_segment: TranscriptSegment,
    speaker_segment: dict[str, object],
) -> float:
    if transcript_segment.start is None or transcript_segment.end is None:
        return 0.0
    raw_start = speaker_segment.get("start")
    raw_end = speaker_segment.get("end")
    if not isinstance(raw_start, int | float) or not isinstance(raw_end, int | float):
        return 0.0
    start = max(transcript_segment.start, float(raw_start))
    end = min(transcript_segment.end, float(raw_end))
    return max(0.0, end - start)


def _speaker_segments(diarization: object) -> list[dict[str, object]]:
    if not isinstance(diarization, dict):
        return []
    segments = diarization.get("segments")
    if not isinstance(segments, list):
        return []
    return [segment for segment in segments if isinstance(segment, dict)]


def _merge_speakers(transcript: Transcript, diarization: object) -> Transcript:
    if not transcript.segments:
        return transcript

    speakers = _speaker_segments(diarization)
    if not speakers:
        return transcript

    merged: list[TranscriptSegment] = []
    for segment in transcript.segments:
        best_speaker: str | None = None
        best_overlap = 0.0
        for speaker_segment in speakers:
            overlap = _segment_overlap_seconds(segment, speaker_segment)
            raw_speaker = speaker_segment.get("speaker")
            if overlap > best_overlap and isinstance(raw_speaker, str):
                best_overlap = overlap
                best_speaker = raw_speaker
        merged.append(
            TranscriptSegment(
                text=segment.text,
                start=segment.start,
                end=segment.end,
                speaker=best_speaker or segment.speaker,
            )
        )

    return Transcript(text=transcript.text, language=transcript.language, segments=merged)
