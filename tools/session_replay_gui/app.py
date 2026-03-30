from __future__ import annotations

import json
import math
import sys
from pathlib import Path

try:
    from PySide6.QtCore import QTimer, Qt
    from PySide6.QtGui import QColor, QPainter, QPen
    from PySide6.QtWidgets import (
        QApplication,
        QComboBox,
        QFileDialog,
        QFormLayout,
        QGridLayout,
        QGroupBox,
        QHeaderView,
        QHBoxLayout,
        QLabel,
        QLineEdit,
        QMainWindow,
        QMessageBox,
        QPushButton,
        QPlainTextEdit,
        QSlider,
        QSpinBox,
        QSplitter,
        QStatusBar,
        QTableWidget,
        QTableWidgetItem,
        QVBoxLayout,
        QWidget,
    )
except ModuleNotFoundError as exc:
    raise SystemExit(
        "PySide6 未安装。请先执行：\n"
        "    pip install -r tools/session_replay_gui/requirements.txt"
    ) from exc

if __package__ in (None, ""):
    from reader import (  # type: ignore
        DEFAULT_SAMPLE_RATE_HZ,
        ReplayEvent,
        SessionBundle,
        discover_session_files,
        load_session_bundle,
    )
else:
    from .reader import (
        DEFAULT_SAMPLE_RATE_HZ,
        ReplayEvent,
        SessionBundle,
        discover_session_files,
        load_session_bundle,
    )


class WaveformCanvas(QWidget):
    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setMinimumHeight(320)
        self._times_s = []
        self._values = []
        self._cursor_time_s = 0.0
        self._window_start_s = 0.0
        self._window_end_s = 0.0
        self._events: list[ReplayEvent] = []
        self._y_unit = "uV"
        self._title = ""

    def set_waveform(self,
                     times_s,
                     values,
                     cursor_time_s: float,
                     window_start_s: float,
                     window_end_s: float,
                     events: list[ReplayEvent],
                     title: str,
                     y_unit: str) -> None:
        self._times_s = times_s
        self._values = values
        self._cursor_time_s = cursor_time_s
        self._window_start_s = window_start_s
        self._window_end_s = window_end_s
        self._events = list(events)
        self._title = title
        self._y_unit = y_unit
        self.update()

    def paintEvent(self, event) -> None:  # type: ignore[override]
        painter = QPainter(self)
        painter.setRenderHint(QPainter.Antialiasing, False)
        painter.fillRect(self.rect(), QColor("#11161b"))

        plot_rect = self.rect().adjusted(50, 30, -20, -30)
        painter.setPen(QColor("#2a323c"))
        painter.drawRect(plot_rect)

        if len(self._values) == 0:
            painter.setPen(QColor("#c9d1d9"))
            painter.drawText(plot_rect, Qt.AlignCenter, "暂无波形数据")
            return

        values = self._values
        y_min = float(min(values))
        y_max = float(max(values))
        if math.isclose(y_min, y_max, rel_tol=0.0, abs_tol=1e-9):
            y_min -= 1.0
            y_max += 1.0

        pad = max(1e-6, 0.1 * (y_max - y_min))
        y_min -= pad
        y_max += pad

        def map_x(time_s: float) -> int:
            span = max(1e-9, self._window_end_s - self._window_start_s)
            ratio = (time_s - self._window_start_s) / span
            return int(plot_rect.left() + ratio * plot_rect.width())

        def map_y(value: float) -> int:
            ratio = (value - y_min) / max(1e-9, (y_max - y_min))
            return int(plot_rect.bottom() - ratio * plot_rect.height())

        painter.setPen(QPen(QColor("#202833"), 1, Qt.DashLine))
        zero_visible = y_min <= 0.0 <= y_max
        if zero_visible:
            y0 = map_y(0.0)
            painter.drawLine(plot_rect.left(), y0, plot_rect.right(), y0)

        for replay_event in self._events:
            event_time_s = replay_event.time_ms / 1000.0
            if not (self._window_start_s <= event_time_s <= self._window_end_s):
                continue
            color = QColor("#ff9e64")
            if replay_event.source_phase.upper() == "B":
                color = QColor("#c678dd")
            painter.setPen(QPen(color, 1))
            x = map_x(event_time_s)
            painter.drawLine(x, plot_rect.top(), x, plot_rect.bottom())

        painter.setPen(QPen(QColor("#66d9ef"), 1))
        sample_count = len(values)
        pixel_width = max(1, plot_rect.width())
        block = max(1, math.ceil(sample_count / pixel_width))

        if block == 1:
            last_x = None
            last_y = None
            for idx, value in enumerate(values):
                x = map_x(self._times_s[idx])
                y = map_y(float(value))
                if last_x is not None:
                    painter.drawLine(last_x, last_y, x, y)
                last_x = x
                last_y = y
        else:
            for pixel_index in range(pixel_width):
                start = pixel_index * block
                if start >= sample_count:
                    break
                stop = min(sample_count, start + block)
                chunk = values[start:stop]
                if len(chunk) == 0:
                    continue
                x = plot_rect.left() + pixel_index
                painter.drawLine(x, map_y(float(min(chunk))), x, map_y(float(max(chunk))))

        painter.setPen(QPen(QColor("#98c379"), 1))
        cursor_x = map_x(self._cursor_time_s)
        painter.drawLine(cursor_x, plot_rect.top(), cursor_x, plot_rect.bottom())

        painter.setPen(QColor("#e6edf3"))
        painter.drawText(8, 18, self._title)
        painter.drawText(
            plot_rect.left(),
            self.height() - 8,
            f"{self._window_start_s:.3f}s - {self._window_end_s:.3f}s",
        )
        painter.drawText(
            plot_rect.right() - 180,
            18,
            f"Y: {y_min:.1f} ~ {y_max:.1f} {self._y_unit}",
        )


class ReplayMainWindow(QMainWindow):
    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("Session Replay Viewer")
        self.resize(1400, 900)

        self._session_path: Path | None = None
        self._bundle: SessionBundle | None = None
        self._current_sample = 0
        self._play_timer = QTimer(self)
        self._play_timer.setInterval(33)
        self._play_timer.timeout.connect(self._advance_playback)

        self._build_ui()

    def _build_ui(self) -> None:
        central = QWidget(self)
        root = QVBoxLayout(central)

        open_group = QGroupBox("数据源")
        open_layout = QGridLayout(open_group)
        self.path_edit = QLineEdit()
        self.path_edit.setReadOnly(True)
        self.btn_open = QPushButton("打开实验文件夹")
        self.btn_open.clicked.connect(self._choose_session_dir)
        self.btn_reload = QPushButton("重新加载")
        self.btn_reload.clicked.connect(self._reload_session)
        self.stream_count_spin = QSpinBox()
        self.stream_count_spin.setRange(1, 8)
        self.stream_count_spin.setValue(2)
        self.stream_count_spin.valueChanged.connect(self._reload_session)
        self.stream_hint_label = QLabel("候选 stream 数：-")
        self.sample_rate_spin = QSpinBox()
        self.sample_rate_spin.setRange(1000, 50000)
        self.sample_rate_spin.setSingleStep(1000)
        self.sample_rate_spin.setValue(DEFAULT_SAMPLE_RATE_HZ)
        self.sample_rate_spin.valueChanged.connect(self._refresh_waveform)

        open_layout.addWidget(QLabel("实验目录"), 0, 0)
        open_layout.addWidget(self.path_edit, 0, 1, 1, 4)
        open_layout.addWidget(self.btn_open, 0, 5)
        open_layout.addWidget(self.btn_reload, 0, 6)
        open_layout.addWidget(QLabel("stream 数"), 1, 0)
        open_layout.addWidget(self.stream_count_spin, 1, 1)
        open_layout.addWidget(self.stream_hint_label, 1, 2, 1, 2)
        open_layout.addWidget(QLabel("采样率(Hz)"), 1, 4)
        open_layout.addWidget(self.sample_rate_spin, 1, 5)

        view_group = QGroupBox("回放控制")
        view_layout = QGridLayout(view_group)
        self.signal_combo = QComboBox()
        self.signal_combo.addItems(["Amplifier (uV)", "Amplifier (raw)", "DC amplifier (raw)"])
        self.signal_combo.currentIndexChanged.connect(self._refresh_waveform)
        self.stream_combo = QComboBox()
        self.stream_combo.currentIndexChanged.connect(self._refresh_waveform)
        self.channel_combo = QComboBox()
        self.channel_combo.currentIndexChanged.connect(self._refresh_waveform)
        self.window_ms_spin = QSpinBox()
        self.window_ms_spin.setRange(100, 60000)
        self.window_ms_spin.setSingleStep(100)
        self.window_ms_spin.setValue(2000)
        self.window_ms_spin.valueChanged.connect(self._refresh_waveform)
        self.speed_combo = QComboBox()
        self.speed_combo.addItems(["0.25x", "0.5x", "1x", "2x", "4x"])
        self.speed_combo.setCurrentText("1x")
        self.btn_play = QPushButton("播放")
        self.btn_play.clicked.connect(self._toggle_playback)
        self.slider = QSlider(Qt.Horizontal)
        self.slider.setMinimum(0)
        self.slider.setMaximum(0)
        self.slider.valueChanged.connect(self._on_slider_changed)
        self.time_label = QLabel("0.000 s / 0.000 s")

        view_layout.addWidget(QLabel("信号类型"), 0, 0)
        view_layout.addWidget(self.signal_combo, 0, 1)
        view_layout.addWidget(QLabel("Stream"), 0, 2)
        view_layout.addWidget(self.stream_combo, 0, 3)
        view_layout.addWidget(QLabel("Channel"), 0, 4)
        view_layout.addWidget(self.channel_combo, 0, 5)
        view_layout.addWidget(QLabel("窗口(ms)"), 1, 0)
        view_layout.addWidget(self.window_ms_spin, 1, 1)
        view_layout.addWidget(QLabel("速度"), 1, 2)
        view_layout.addWidget(self.speed_combo, 1, 3)
        view_layout.addWidget(self.btn_play, 1, 4)
        view_layout.addWidget(self.time_label, 1, 5)
        view_layout.addWidget(self.slider, 2, 0, 1, 6)

        self.waveform = WaveformCanvas()

        right_panel = QSplitter(Qt.Vertical)
        self.event_table = QTableWidget(0, 6)
        self.event_table.setHorizontalHeaderLabels(["time_ms", "phase", "electrode", "amp_uA", "pulses", "epoch"])
        self.event_table.horizontalHeader().setSectionResizeMode(QHeaderView.Stretch)
        self.event_table.cellDoubleClicked.connect(self._jump_to_event_row)
        right_panel.addWidget(self.event_table)

        self.meta_text = QPlainTextEdit()
        self.meta_text.setReadOnly(True)
        right_panel.addWidget(self.meta_text)
        right_panel.setStretchFactor(0, 3)
        right_panel.setStretchFactor(1, 2)

        content_splitter = QSplitter(Qt.Horizontal)
        content_splitter.addWidget(self.waveform)
        content_splitter.addWidget(right_panel)
        content_splitter.setStretchFactor(0, 4)
        content_splitter.setStretchFactor(1, 2)

        root.addWidget(open_group)
        root.addWidget(view_group)
        root.addWidget(content_splitter, 1)

        self.setCentralWidget(central)
        self.setStatusBar(QStatusBar(self))

    def _choose_session_dir(self) -> None:
        start_dir = str(self._session_path.parent if self._session_path else Path.cwd())
        directory = QFileDialog.getExistingDirectory(self, "选择实验文件夹", start_dir)
        if not directory:
            return
        self._session_path = Path(directory)
        self.path_edit.setText(directory)
        self._reload_session()

    def _reload_session(self) -> None:
        if self._session_path is None:
            return
        try:
            files = discover_session_files(self._session_path)
        except Exception as exc:  # pragma: no cover - GUI only
            QMessageBox.critical(self, "打开失败", str(exc))
            return

        if files.stream_candidates:
            self.stream_hint_label.setText(
                "候选 stream 数：" + ", ".join(str(item) for item in files.stream_candidates)
            )
            if self.stream_count_spin.value() not in files.stream_candidates:
                self.stream_count_spin.blockSignals(True)
                self.stream_count_spin.setValue(files.stream_candidates[0])
                self.stream_count_spin.blockSignals(False)
        else:
            self.stream_hint_label.setText("候选 stream 数：无法从文件大小推断")

        try:
            self._bundle = load_session_bundle(
                self._session_path,
                num_streams=self.stream_count_spin.value(),
                sample_rate_hz=self.sample_rate_spin.value(),
            )
        except Exception as exc:  # pragma: no cover - GUI only
            QMessageBox.critical(self, "加载失败", str(exc))
            return

        self.statusBar().showMessage("已加载实验文件夹", 3000)
        self._populate_bundle()

    def _populate_bundle(self) -> None:
        if not self._bundle or not self._bundle.recording:
            return

        recording = self._bundle.recording
        self.stream_combo.blockSignals(True)
        self.stream_combo.clear()
        for stream in range(recording.num_streams):
            self.stream_combo.addItem(f"{stream}", stream)
        self.stream_combo.blockSignals(False)

        self.channel_combo.blockSignals(True)
        self.channel_combo.clear()
        for channel in range(16):
            self.channel_combo.addItem(f"{channel}", channel)
        self.channel_combo.blockSignals(False)

        self.slider.blockSignals(True)
        self.slider.setMaximum(max(0, recording.total_samples - 1))
        self.slider.setValue(0)
        self.slider.blockSignals(False)
        self._current_sample = 0

        metadata_text = ""
        if self._bundle.metadata is not None:
            meta_root = dict(self._bundle.metadata.raw)
            if "events" in meta_root:
                meta_root["events_count"] = len(self._bundle.events)
                meta_root.pop("events", None)
            metadata_text = json.dumps(meta_root, ensure_ascii=False, indent=2)
        self.meta_text.setPlainText(metadata_text)

        self.event_table.setRowCount(0)
        for row_index, replay_event in enumerate(self._bundle.events):
            self.event_table.insertRow(row_index)
            self.event_table.setItem(row_index, 0, QTableWidgetItem(str(replay_event.time_ms)))
            self.event_table.setItem(row_index, 1, QTableWidgetItem(replay_event.source_phase or "-"))
            self.event_table.setItem(row_index, 2, QTableWidgetItem(replay_event.electrode or "-"))
            self.event_table.setItem(row_index, 3, QTableWidgetItem(f"{replay_event.amp_uA:g}"))
            self.event_table.setItem(row_index, 4, QTableWidgetItem(str(replay_event.pulses)))
            self.event_table.setItem(row_index, 5, QTableWidgetItem(str(replay_event.epoch_id)))

        self._refresh_waveform()

    def _selected_stream(self) -> int:
        return int(self.stream_combo.currentData()) if self.stream_combo.currentIndex() >= 0 else 0

    def _selected_channel(self) -> int:
        return int(self.channel_combo.currentData()) if self.channel_combo.currentIndex() >= 0 else 0

    def _selected_speed(self) -> float:
        text = self.speed_combo.currentText().replace("x", "").strip()
        try:
            return float(text)
        except ValueError:
            return 1.0

    def _on_slider_changed(self, value: int) -> None:
        self._current_sample = max(0, value)
        self._refresh_waveform()

    def _toggle_playback(self) -> None:
        if not self._bundle or not self._bundle.recording:
            return
        if self._play_timer.isActive():
            self._play_timer.stop()
            self.btn_play.setText("播放")
        else:
            self._play_timer.start()
            self.btn_play.setText("暂停")

    def _advance_playback(self) -> None:
        if not self._bundle or not self._bundle.recording:
            return
        step = max(1, int(self.sample_rate_spin.value() * self._selected_speed() * self._play_timer.interval() / 1000.0))
        next_sample = min(self._bundle.recording.total_samples - 1, self._current_sample + step)
        self.slider.setValue(next_sample)
        if next_sample >= self._bundle.recording.total_samples - 1:
            self._play_timer.stop()
            self.btn_play.setText("播放")

    def _jump_to_event_row(self, row: int, _column: int) -> None:
        if not self._bundle or row < 0 or row >= len(self._bundle.events):
            return
        event_time_ms = self._bundle.events[row].time_ms
        target_sample = int(event_time_ms * self.sample_rate_spin.value() / 1000.0)
        if self._bundle.recording:
            target_sample = max(0, min(target_sample, self._bundle.recording.total_samples - 1))
        self.slider.setValue(target_sample)

    def _refresh_waveform(self) -> None:
        if not self._bundle or not self._bundle.recording:
            return

        recording = self._bundle.recording
        sample_rate = self.sample_rate_spin.value()
        window_samples = max(1, int(sample_rate * self.window_ms_spin.value() / 1000.0))
        start = max(0, self._current_sample - window_samples // 4)
        stop = min(recording.total_samples, start + window_samples)
        if stop <= start:
            return

        stream = self._selected_stream()
        channel = self._selected_channel()
        signal_mode = self.signal_combo.currentText()

        if signal_mode == "Amplifier (uV)":
            values = recording.amplifier_uv(stream, channel, start, stop)
            y_unit = "uV"
            title = f"Amplifier stream={stream} channel={channel}"
        elif signal_mode == "Amplifier (raw)":
            values = recording.amplifier_words(stream, channel, start, stop)
            y_unit = "raw"
            title = f"Amplifier(raw) stream={stream} channel={channel}"
        else:
            values = recording.dc_amplifier_words(stream, channel, start, stop)
            y_unit = "raw"
            title = f"DC amplifier stream={stream} channel={channel}"

        times_s = recording.sample_times_seconds(sample_rate, start, stop)
        cursor_time_s = self._current_sample / float(sample_rate)
        window_start_s = start / float(sample_rate)
        window_end_s = (stop - 1) / float(sample_rate)
        self.waveform.set_waveform(
            times_s=times_s,
            values=values,
            cursor_time_s=cursor_time_s,
            window_start_s=window_start_s,
            window_end_s=window_end_s,
            events=self._bundle.events,
            title=title,
            y_unit=y_unit,
        )

        total_seconds = recording.total_samples / float(sample_rate)
        self.time_label.setText(f"{cursor_time_s:.3f} s / {total_seconds:.3f} s")


def main() -> int:
    app = QApplication(sys.argv)
    window = ReplayMainWindow()
    window.show()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
