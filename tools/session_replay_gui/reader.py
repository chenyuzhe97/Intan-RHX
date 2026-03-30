from __future__ import annotations

import csv
import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable

import numpy as np


SAMPLES_PER_BLOCK = 128
CHANNELS_PER_STREAM = 16
ADC_CHANNEL_COUNT = 8
DAC_CHANNEL_COUNT = 8
DEFAULT_SAMPLE_RATE_HZ = 30000
MIN_STREAMS = 1
MAX_STREAMS = 8


def words_per_sample(num_streams: int) -> int:
    return 19 + 32 * int(num_streams)


def bytes_per_sample(num_streams: int) -> int:
    return words_per_sample(num_streams) * 2


def bytes_per_block(num_streams: int) -> int:
    return bytes_per_sample(num_streams) * SAMPLES_PER_BLOCK


def infer_num_stream_candidates(file_size_bytes: int) -> list[int]:
    candidates: list[int] = []
    for num_streams in range(MIN_STREAMS, MAX_STREAMS + 1):
        block_bytes = bytes_per_block(num_streams)
        if block_bytes > 0 and (file_size_bytes % block_bytes) == 0:
            candidates.append(num_streams)
    return candidates


def _coerce_int(value: Any, default: int = 0) -> int:
    if value is None:
        return default
    if isinstance(value, bool):
        return int(value)
    try:
        return int(value)
    except (TypeError, ValueError):
        text = str(value).strip()
        if not text:
            return default
        try:
            return int(float(text))
        except (TypeError, ValueError):
            return default


def _coerce_float(value: Any, default: float = 0.0) -> float:
    if value is None:
        return default
    try:
        return float(value)
    except (TypeError, ValueError):
        text = str(value).strip()
        if not text:
            return default
        try:
            return float(text)
        except (TypeError, ValueError):
            return default


def _coerce_str(value: Any, default: str = "") -> str:
    if value is None:
        return default
    return str(value).strip()


@dataclass(slots=True)
class ReplayEvent:
    time_ms: int
    planned_time_ms: int = -1
    fired_time_ms: int = -1
    electrode: str = ""
    amp_uA: float = 0.0
    pulses: int = 0
    epoch_id: int = -1
    item_index: int = -1
    source_phase: str = ""
    source_phase_index: int = -1
    channel_index: int = -1
    spike_uV: float = 0.0
    trigger_source: int = 0
    phase_us: int = 0
    frequency_hz: float = 0.0
    raw: dict[str, Any] = field(default_factory=dict)

    @classmethod
    def from_mapping(cls, data: dict[str, Any]) -> "ReplayEvent":
        planned = _coerce_int(data.get("planned_time_ms"), -1)
        fired = _coerce_int(data.get("fired_time_ms"), -1)
        time_ms = _coerce_int(data.get("time_ms"), fired if fired >= 0 else planned)
        return cls(
            time_ms=time_ms,
            planned_time_ms=planned,
            fired_time_ms=fired,
            electrode=_coerce_str(data.get("target_electrode") or data.get("electrode")),
            amp_uA=_coerce_float(data.get("amp_uA"), 0.0),
            pulses=_coerce_int(data.get("pulses"), 0),
            epoch_id=_coerce_int(data.get("epoch_id"), _coerce_int(data.get("epochId"), -1)),
            item_index=_coerce_int(data.get("item_index"), _coerce_int(data.get("item"), -1)),
            source_phase=_coerce_str(data.get("source_phase")),
            source_phase_index=_coerce_int(data.get("source_phase_index"), _coerce_int(data.get("phase"), -1)),
            channel_index=_coerce_int(data.get("channel_index"), _coerce_int(data.get("ch"), -1)),
            spike_uV=_coerce_float(data.get("spike_uV"), 0.0),
            trigger_source=_coerce_int(data.get("trigger_source"), 0),
            phase_us=_coerce_int(data.get("phase_us"), 0),
            frequency_hz=_coerce_float(data.get("frequency_hz"), 0.0),
            raw=dict(data),
        )


@dataclass(slots=True)
class SessionMetadata:
    session_type: str = ""
    schema_version: int = 0
    created_at_iso: str = ""
    experiment_duration_ms: int = 0
    recording_file: str = ""
    stim_log_file: str = ""
    raw: dict[str, Any] = field(default_factory=dict)


@dataclass(slots=True)
class SessionFiles:
    session_dir: Path
    recording_path: Path | None = None
    stim_plan_path: Path | None = None
    stim_log_path: Path | None = None
    stream_candidates: list[int] = field(default_factory=list)


class BarRecording:
    def __init__(self, file_path: Path, num_streams: int) -> None:
        self.file_path = Path(file_path)
        self.num_streams = int(num_streams)
        self._file_size = self.file_path.stat().st_size
        self.words_per_sample = words_per_sample(self.num_streams)
        self.bytes_per_sample = bytes_per_sample(self.num_streams)
        self.bytes_per_block = bytes_per_block(self.num_streams)

        if self._file_size % self.bytes_per_sample != 0:
            raise ValueError(
                f"文件大小 {self._file_size} 不能被当前 stream 数 {self.num_streams} 的每采样字节数 {self.bytes_per_sample} 整除。"
            )

        self.total_samples = self._file_size // self.bytes_per_sample
        self.total_blocks = self.total_samples // SAMPLES_PER_BLOCK
        if (self.total_samples % SAMPLES_PER_BLOCK) != 0:
            raise ValueError(
                f"样本数 {self.total_samples} 不是 {SAMPLES_PER_BLOCK} 的整数倍，文件可能不完整。"
            )

        words = np.memmap(self.file_path, dtype="<u2", mode="r")
        self._matrix = words.reshape(self.total_samples, self.words_per_sample)
        self._timestamp_cache: np.ndarray | None = None

    @property
    def raw_matrix(self) -> np.ndarray:
        return self._matrix

    @property
    def timestamp_words(self) -> np.ndarray:
        return self._matrix[:, 0]

    @property
    def timestamps_unwrapped(self) -> np.ndarray:
        if self._timestamp_cache is None:
            ts16 = np.asarray(self.timestamp_words, dtype=np.uint32)
            if ts16.size == 0:
                self._timestamp_cache = ts16
            else:
                diff = np.diff(ts16.astype(np.int64), prepend=int(ts16[0]))
                wraps = (diff < 0).astype(np.uint32) * np.uint32(65536)
                offset = np.cumsum(wraps, dtype=np.uint32)
                self._timestamp_cache = ts16 + offset
        return self._timestamp_cache

    def sample_times_seconds(self, sample_rate_hz: int, start: int = 0, stop: int | None = None) -> np.ndarray:
        stop = self.total_samples if stop is None else max(start, min(stop, self.total_samples))
        sample_rate = max(1, int(sample_rate_hz))
        sample_indices = np.arange(start, stop, dtype=np.float64)
        return sample_indices / float(sample_rate)

    def _check_stream_channel(self, stream: int, channel: int) -> None:
        if not (0 <= stream < self.num_streams):
            raise IndexError(f"stream 超出范围：{stream}")
        if not (0 <= channel < CHANNELS_PER_STREAM):
            raise IndexError(f"channel 超出范围：{channel}")

    def amplifier_words(self, stream: int, channel: int, start: int = 0, stop: int | None = None, step: int = 1) -> np.ndarray:
        self._check_stream_channel(stream, channel)
        stop = self.total_samples if stop is None else min(stop, self.total_samples)
        column = 1 + channel * self.num_streams + stream
        return self._matrix[start:stop:step, column]

    def dc_amplifier_words(self, stream: int, channel: int, start: int = 0, stop: int | None = None, step: int = 1) -> np.ndarray:
        self._check_stream_channel(stream, channel)
        stop = self.total_samples if stop is None else min(stop, self.total_samples)
        column = 1 + (CHANNELS_PER_STREAM * self.num_streams) + channel * self.num_streams + stream
        return self._matrix[start:stop:step, column]

    def amplifier_uv(self, stream: int, channel: int, start: int = 0, stop: int | None = None, step: int = 1) -> np.ndarray:
        words = np.asarray(self.amplifier_words(stream, channel, start, stop, step), dtype=np.float32)
        return (words - 32768.0) * 0.195

    def board_adc_words(self, adc_index: int, start: int = 0, stop: int | None = None, step: int = 1) -> np.ndarray:
        if not (0 <= adc_index < ADC_CHANNEL_COUNT):
            raise IndexError(f"ADC 索引超出范围：{adc_index}")
        stop = self.total_samples if stop is None else min(stop, self.total_samples)
        column = 1 + (2 * CHANNELS_PER_STREAM * self.num_streams) + adc_index
        return self._matrix[start:stop:step, column]

    def board_dac_words(self, dac_index: int, start: int = 0, stop: int | None = None, step: int = 1) -> np.ndarray:
        if not (0 <= dac_index < DAC_CHANNEL_COUNT):
            raise IndexError(f"DAC 索引超出范围：{dac_index}")
        stop = self.total_samples if stop is None else min(stop, self.total_samples)
        column = 1 + (2 * CHANNELS_PER_STREAM * self.num_streams) + ADC_CHANNEL_COUNT + dac_index
        return self._matrix[start:stop:step, column]

    def ttl_in_words(self, start: int = 0, stop: int | None = None, step: int = 1) -> np.ndarray:
        stop = self.total_samples if stop is None else min(stop, self.total_samples)
        column = self.words_per_sample - 2
        return self._matrix[start:stop:step, column]

    def ttl_out_words(self, start: int = 0, stop: int | None = None, step: int = 1) -> np.ndarray:
        stop = self.total_samples if stop is None else min(stop, self.total_samples)
        column = self.words_per_sample - 1
        return self._matrix[start:stop:step, column]


@dataclass(slots=True)
class SessionBundle:
    files: SessionFiles
    metadata: SessionMetadata | None
    recording: BarRecording | None
    events: list[ReplayEvent]
    sample_rate_hz: int = DEFAULT_SAMPLE_RATE_HZ

    @property
    def session_type(self) -> str:
        return self.metadata.session_type if self.metadata else ""

    @property
    def duration_seconds(self) -> float:
        if self.metadata and self.metadata.experiment_duration_ms > 0:
            return self.metadata.experiment_duration_ms / 1000.0
        if self.recording:
            return self.recording.total_samples / float(max(1, self.sample_rate_hz))
        return 0.0


def discover_session_files(path: str | Path) -> SessionFiles:
    target = Path(path).expanduser().resolve()
    if not target.exists():
        raise FileNotFoundError(f"路径不存在：{target}")

    if target.is_dir():
        session_dir = target
        recording_path = session_dir / "recording.bar"
        stim_plan_path = session_dir / "stim_plan.json"
        stim_log_path = session_dir / "stim_log.csv"
    else:
        session_dir = target.parent
        recording_path = target if target.suffix.lower() == ".bar" else session_dir / "recording.bar"
        stim_plan_path = target if target.suffix.lower() == ".json" else session_dir / "stim_plan.json"
        stim_log_path = target if target.suffix.lower() == ".csv" else session_dir / "stim_log.csv"

    if not recording_path.exists():
        bars = sorted(session_dir.glob("*.bar"))
        recording_path = bars[0] if bars else None
    if not stim_plan_path.exists():
        stim_plan_path = None
    if not stim_log_path.exists():
        stim_log_path = None

    stream_candidates: list[int] = []
    if recording_path is not None and recording_path.exists():
        stream_candidates = infer_num_stream_candidates(recording_path.stat().st_size)

    return SessionFiles(
        session_dir=session_dir,
        recording_path=recording_path,
        stim_plan_path=stim_plan_path,
        stim_log_path=stim_log_path,
        stream_candidates=stream_candidates,
    )


def load_session_metadata(stim_plan_path: Path | None) -> tuple[SessionMetadata | None, list[ReplayEvent]]:
    if stim_plan_path is None or not stim_plan_path.exists():
        return None, []

    with stim_plan_path.open("r", encoding="utf-8") as fp:
        root = json.load(fp)

    events = [ReplayEvent.from_mapping(item) for item in root.get("events", []) if isinstance(item, dict)]
    metadata = SessionMetadata(
        session_type=_coerce_str(root.get("session_type")),
        schema_version=_coerce_int(root.get("schema_version"), 0),
        created_at_iso=_coerce_str(root.get("created_at_iso")),
        experiment_duration_ms=_coerce_int(root.get("experiment_duration_ms"), 0),
        recording_file=_coerce_str(root.get("recording_file")),
        stim_log_file=_coerce_str(root.get("stim_log_file")),
        raw=root,
    )
    return metadata, events


def load_events_from_csv(csv_path: Path | None) -> list[ReplayEvent]:
    if csv_path is None or not csv_path.exists():
        return []

    events: list[ReplayEvent] = []
    with csv_path.open("r", encoding="utf-8-sig", newline="") as fp:
        reader = csv.DictReader(fp)
        for row in reader:
            if not row:
                continue
            mapping = {
                "time_ms": row.get("offset_ms", ""),
                "planned_time_ms": row.get("offset_ms", ""),
                "fired_time_ms": row.get("offset_ms", ""),
                "target_electrode": row.get("electrode", ""),
                "amp_uA": row.get("amp_uA", ""),
                "pulses": row.get("pulses", ""),
                "epoch_id": row.get("epochId", ""),
                "item_index": row.get("item", ""),
                "source_phase_index": row.get("phase", ""),
                "channel_index": row.get("ch", ""),
                "spike_uV": row.get("spike_uV", ""),
            }
            events.append(ReplayEvent.from_mapping(mapping))
    return events


def load_session_bundle(path: str | Path,
                        num_streams: int | None = None,
                        sample_rate_hz: int = DEFAULT_SAMPLE_RATE_HZ) -> SessionBundle:
    files = discover_session_files(path)
    metadata, events = load_session_metadata(files.stim_plan_path)

    if not events:
        events = load_events_from_csv(files.stim_log_path)

    recording: BarRecording | None = None
    if files.recording_path is not None:
        if num_streams is None:
            if len(files.stream_candidates) == 1:
                num_streams = files.stream_candidates[0]
            else:
                raise ValueError(
                    f"无法唯一确定 num_streams；候选值={files.stream_candidates or '无'}。"
                )
        recording = BarRecording(files.recording_path, num_streams)

    return SessionBundle(
        files=files,
        metadata=metadata,
        recording=recording,
        events=sorted(events, key=lambda event: event.time_ms),
        sample_rate_hz=max(1, int(sample_rate_hz)),
    )


def write_stim_record_bar(file_path: str | Path,
                          timestamps_u16: Iterable[int],
                          amplifier_words_per_sample: np.ndarray,
                          dc_words_per_sample: np.ndarray,
                          adc_words_per_sample: np.ndarray | None = None,
                          dac_words_per_sample: np.ndarray | None = None,
                          ttl_in_words: Iterable[int] | None = None,
                          ttl_out_words: Iterable[int] | None = None) -> Path:
    output_path = Path(file_path).expanduser().resolve()
    timestamps = np.asarray(list(timestamps_u16), dtype=np.uint16).reshape(-1)
    amplifier = np.asarray(amplifier_words_per_sample, dtype=np.uint16)
    dc_words = np.asarray(dc_words_per_sample, dtype=np.uint16)

    if amplifier.ndim != 3:
        raise ValueError("amplifier_words_per_sample 需要是 (samples, num_streams, 16) 的三维数组。")
    if dc_words.shape != amplifier.shape:
        raise ValueError("dc_words_per_sample 的形状必须与 amplifier_words_per_sample 一致。")

    sample_count, num_streams, channel_count = amplifier.shape
    if channel_count != CHANNELS_PER_STREAM:
        raise ValueError(f"每个 stream 必须包含 {CHANNELS_PER_STREAM} 个通道。")
    if timestamps.shape[0] != sample_count:
        raise ValueError("timestamps_u16 的长度必须和 sample 数一致。")

    adc = np.zeros((sample_count, ADC_CHANNEL_COUNT), dtype=np.uint16) if adc_words_per_sample is None else np.asarray(adc_words_per_sample, dtype=np.uint16)
    dac = np.zeros((sample_count, DAC_CHANNEL_COUNT), dtype=np.uint16) if dac_words_per_sample is None else np.asarray(dac_words_per_sample, dtype=np.uint16)
    ttl_in = np.zeros(sample_count, dtype=np.uint16) if ttl_in_words is None else np.asarray(list(ttl_in_words), dtype=np.uint16).reshape(-1)
    ttl_out = np.zeros(sample_count, dtype=np.uint16) if ttl_out_words is None else np.asarray(list(ttl_out_words), dtype=np.uint16).reshape(-1)

    if adc.shape != (sample_count, ADC_CHANNEL_COUNT):
        raise ValueError(f"adc_words_per_sample 需要是 ({sample_count}, {ADC_CHANNEL_COUNT})。")
    if dac.shape != (sample_count, DAC_CHANNEL_COUNT):
        raise ValueError(f"dac_words_per_sample 需要是 ({sample_count}, {DAC_CHANNEL_COUNT})。")
    if ttl_in.shape[0] != sample_count or ttl_out.shape[0] != sample_count:
        raise ValueError("ttl_in_words / ttl_out_words 的长度必须和 sample 数一致。")

    with output_path.open("wb") as fp:
        for sample_index in range(sample_count):
            fp.write(timestamps[sample_index].astype("<u2").tobytes())
            for channel in range(CHANNELS_PER_STREAM):
                fp.write(amplifier[sample_index, :, channel].astype("<u2").tobytes())
            for channel in range(CHANNELS_PER_STREAM):
                fp.write(dc_words[sample_index, :, channel].astype("<u2").tobytes())
            fp.write(adc[sample_index].astype("<u2").tobytes())
            fp.write(dac[sample_index].astype("<u2").tobytes())
            fp.write(np.asarray([ttl_in[sample_index]], dtype="<u2").tobytes())
            fp.write(np.asarray([ttl_out[sample_index]], dtype="<u2").tobytes())

    return output_path
