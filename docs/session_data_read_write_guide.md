# 实验数据读写说明

这份说明对应当前工程保存出来的实验文件夹，以及新增的 PySide6 回放工具：

- GUI 工具入口：
  - [app.py](/C:/github_program/Intan-RHX/tools/session_replay_gui/app.py)
- Python 读取/写入模块：
  - [reader.py](/C:/github_program/Intan-RHX/tools/session_replay_gui/reader.py)

## 1. 一个实验文件夹里通常有什么

当前工程在“托管实验”模式下，通常会生成一个实验文件夹，里面至少包含：

- `recording.bar`
- `stim_plan.json`
- `stim_log.csv`

含义分别是：

- `recording.bar`
  - 原始二进制采样数据
  - 由采集线程边读边写，不是最后一次性拼出来的
- `stim_plan.json`
  - 实验级别的元数据
  - 包括实验类型、总时长、刺激参数、刺激事件列表
- `stim_log.csv`
  - 刺激事件日志
  - 更适合人工快速查看和排错

## 2. 当前程序是怎么写盘的

### 2.1 录制写盘调用链

当前工程里二进制录制链路是：

1. 采集线程拿到一个 `RHXDataBlock`
2. 调用 `enqueueBlockForRecording(...)`
3. 录制工作线程 `recordingWorkerLoop()` 取出数据块
4. 调用 `writeBlockToRecording(...)`
5. 最后调用 `RHXDataBlock::write(...)`

相关代码位置：

- [acquisitionengine.cpp](/C:/github_program/Intan-RHX/acquisitionengine.cpp)
- [rhxdatablock.cpp](/C:/github_program/Intan-RHX/Engine/API/Hardware/rhxdatablock.cpp)

所以：

- 当前 `.bar` 是边采集边写的
- 不是先全部攒在内存里，最后再写

### 2.2 `.bar` 的基本性质

当前项目使用的是 `ControllerStimRecord` 的写法。

每个采样点写入顺序如下：

1. `timestamp`
2. `amplifierData`，按 `channel 0..15`，每个 `channel` 内再按 `stream 0..N-1`
3. `dcAmplifierData`，顺序同上
4. `boardAdcData[0..7]`
5. `boardDacData[0..7]`
6. `ttlIn`
7. `ttlOut`

注意：

- 写入单位是 **16-bit little-endian word**
- `timestamp` 虽然在内存里是 `uint32_t`，但写盘时也是按一个 16-bit word 写出去
- 所以文件里的时间戳只有低 16 位，回放时需要自己做 rollover 展开

## 3. `.bar` 的二进制布局

### 3.1 常量

- 每个 block 固定 `128` 个 sample
- 每个 stream 固定 `16` 个 amplifier 通道
- ADC 固定 `8` 路
- DAC 固定 `8` 路

### 3.2 每个 sample 的 word 数

对 `ControllerStimRecord` 来说：

- `1` 个 timestamp word
- `16 * num_streams` 个 amplifier word
- `16 * num_streams` 个 dc amplifier word
- `8` 个 ADC word
- `8` 个 DAC word
- `1` 个 ttlIn word
- `1` 个 ttlOut word

所以：

```text
words_per_sample = 19 + 32 * num_streams
bytes_per_sample = 2 * words_per_sample
bytes_per_block  = 128 * bytes_per_sample
```

例如当 `num_streams = 2` 时：

```text
words_per_sample = 19 + 32*2 = 83
bytes_per_sample = 166
bytes_per_block  = 21248
```

### 3.3 一个 sample 的列索引

假设当前有 `num_streams = N`，那么二维矩阵的列顺序是：

```text
col 0                                  -> timestamp
col 1 .. 1 + 16*N - 1                  -> amplifier
col 1 + 16*N .. 1 + 32*N - 1           -> dc amplifier
接下来 8 列                              -> ADC
接下来 8 列                              -> DAC
倒数第 2 列                              -> ttlIn
倒数第 1 列                              -> ttlOut
```

其中 amplifier / dc amplifier 的单列索引公式是：

```text
amp_col = 1 + channel * num_streams + stream
dc_col  = 1 + 16 * num_streams + channel * num_streams + stream
```

## 4. 时间怎么还原

由于文件里只写了低 16 位 timestamp，所以工具里要做 wrap 展开。

思路是：

1. 先读出 `uint16` 时间戳序列
2. 一旦发现当前值比前一个值小，就认为发生了一次 `65536` 回卷
3. 后续样本统一加上累计 offset

工具里已经做好了这一层，见：

- [reader.py](/C:/github_program/Intan-RHX/tools/session_replay_gui/reader.py)

## 5. `stim_plan.json` 里通常有什么

### 5.1 闭环实验

`session_type = "closed_loop"`

常见字段：

- `experiment_duration_ms`
- `epoch_sec`
- `rounds_target`
- `rounds_completed`
- `max_stim_per_epoch`
- `stim_phase_us`
- `filter_bp_enabled`
- `filter_bp_low_hz`
- `filter_bp_high_hz`
- `stim_step_size_enum`
- `stim_step_size`
- `recording_file`
- `stim_log_file`
- `events`

每个 `event` 常见字段：

- `epoch_id`
- `source_phase_index`
- `source_phase`
- `item_index`
- `planned_time_ms`
- `fired_time_ms`
- `time_ms`
- `target_electrode`
- `amp_uA`
- `pulses`
- `channel_index`
- `spike_uV`
- `trigger_source`

### 5.2 固定刺激实验

`session_type = "fixed_stim"` 或 `fixed_stim_dual`

常见字段：

- `experiment_duration_ms`
- `rounds_target`
- `round_duration_ms`
- `collect_pre_ms`
- `stim_window_duration_ms`
- `collect_post_ms`
- `idle_ms`
- `stim_step_size_enum`
- `stim_step_size`
- `fixed_waveform`
- `fixed_interphase_us`
- 单路时：
  - `target_electrode`
  - `fixed_amplitude_uA`
  - `fixed_phase_us`
  - `fixed_frequency_hz`
- 双路时：
  - `target_A_electrode`
  - `target_B_electrode`
  - `fixed_A_*`
  - `fixed_B_*`
  - `stim_window_B_*`
  - `post_stim_window_A_*`

## 6. `stim_log.csv` 的字段

表头是：

```csv
time_iso,type,epochId,phase,item,offset_ms,electrode,amp_uA,pulses,ch,spike_uV
```

常见用途：

- 快速人工查看
- 排查某个刺激有没有触发
- 在 JSON 不完整时做兜底读取

## 7. PySide6 回放工具怎么用

### 7.1 安装

```bash
pip install -r tools/session_replay_gui/requirements.txt
```

### 7.2 启动

```bash
python tools/session_replay_gui/app.py
```

### 7.3 使用步骤

1. 点击“打开实验文件夹”
2. 选择一个包含 `recording.bar` 的实验目录
3. 工具会自动尝试：
   - 找到 `recording.bar`
   - 找到 `stim_plan.json`
   - 找到 `stim_log.csv`
4. GUI 会根据 `.bar` 文件大小给出 `num_streams` 候选值
5. 如果候选不唯一，可以手动改 `stream 数`
6. 设置采样率
   - 默认是 `30000 Hz`
   - 因为 `.bar` 本身不保存采样率
7. 选择 `stream / channel / 信号类型`
8. 用滑块或播放按钮回放
9. 双击右侧事件表，可以直接跳到某个刺激事件

### 7.4 当前 GUI 支持的信号视图

- `Amplifier (uV)`
- `Amplifier (raw)`
- `DC amplifier (raw)`

## 8. Python 里怎么读

最简单的用法：

```python
from tools.session_replay_gui.reader import load_session_bundle

bundle = load_session_bundle(
    r"C:\path\to\session_dir",
    num_streams=2,
    sample_rate_hz=30000,
)

recording = bundle.recording
signal_uv = recording.amplifier_uv(stream=0, channel=0, start=0, stop=30000)
events = bundle.events
```

### 8.1 常用接口

- `discover_session_files(path)`
  - 只解析实验目录结构，不读完整数据
- `load_session_bundle(path, num_streams, sample_rate_hz)`
  - 一次性把实验目录、元数据和 recording 句柄组织好
- `BarRecording.amplifier_uv(stream, channel, start, stop)`
  - 读取某个通道并转换成 `uV`
- `BarRecording.amplifier_words(...)`
  - 读取原始 amplifier 16-bit 值
- `BarRecording.dc_amplifier_words(...)`
  - 读取 DC amplifier 原始值

## 9. Python 里怎么写同格式 `.bar`

如果你想在 Python 里写出和当前程序兼容的 Stim/Record `.bar`，可以直接用：

```python
from tools.session_replay_gui.reader import write_stim_record_bar
import numpy as np

samples = 256
num_streams = 2

timestamps = np.arange(samples, dtype=np.uint16)
amplifier = np.zeros((samples, num_streams, 16), dtype=np.uint16)
dc_words = np.zeros((samples, num_streams, 16), dtype=np.uint16)

write_stim_record_bar(
    r"C:\path\to\demo.bar",
    timestamps_u16=timestamps,
    amplifier_words_per_sample=amplifier,
    dc_words_per_sample=dc_words,
)
```

这个接口会严格按当前 C++ 写盘顺序输出：

1. timestamp
2. amplifier
3. dc amplifier
4. ADC
5. DAC
6. ttlIn
7. ttlOut

### 9.1 写入接口的数据形状

- `timestamps_u16`
  - 形状：`(samples,)`
- `amplifier_words_per_sample`
  - 形状：`(samples, num_streams, 16)`
- `dc_words_per_sample`
  - 形状：`(samples, num_streams, 16)`
- `adc_words_per_sample`
  - 形状：`(samples, 8)`，可省略
- `dac_words_per_sample`
  - 形状：`(samples, 8)`，可省略

## 10. 当前格式的两个重要限制

### 10.1 采样率没有写进 `.bar`

所以：

- 单看 `.bar` 无法知道真实时间轴
- 当前工具默认 `30000 Hz`
- 如果实验不是这个采样率，必须在 GUI 里手动改

### 10.2 `num_streams` 也没有写进 `.bar`

所以：

- 工具只能通过文件大小反推候选值
- 如果候选不唯一，需要人工确认

## 11. 推荐实践

- 每次实验尽量保留完整实验文件夹，不要只拿走 `.bar`
- 优先用“实验文件夹”作为回放入口，而不是单独 `.bar`
- 如果后续还会扩展分析，尽量继续把元数据写进 `stim_plan.json`
- 如果以后你想让工具完全自动化，最值得补的两个字段是：
  - `sample_rate_hz`
  - `num_streams`

这样后续读取时就完全不需要人工猜测了。
