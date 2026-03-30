# Session Replay GUI

这是一个独立的 PySide6 小工具，用来读取当前项目保存出来的实验文件夹，并回放：

- `recording.bar`
- `stim_plan.json`
- `stim_log.csv`

## 安装

```bash
pip install -r tools/session_replay_gui/requirements.txt
```

## 运行

```bash
python tools/session_replay_gui/app.py
```

## 当前功能

- 打开实验文件夹
- 自动联动读取 `recording.bar / stim_plan.json / stim_log.csv`
- 根据文件大小推断 `num_streams` 候选值
- 回放单个 `stream/channel` 的波形
- 支持 `Amplifier (uV)` / `Amplifier (raw)` / `DC amplifier (raw)` 三种视图
- 支持播放、暂停、拖动时间轴
- 显示刺激事件表，并双击跳转到事件时间点

## 说明

- `.bar` 文件本身不包含采样率，所以 GUI 里提供了“采样率(Hz)”输入框，默认是 `30000`
- `.bar` 文件本身也不直接存 `num_streams`，GUI 会给出候选值，你也可以手动改
- 更多格式说明见：
  - [session_data_read_write_guide.md](C:/github_program/Intan-RHX/docs/session_data_read_write_guide.md)
