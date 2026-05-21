# AudioStream - 网络音频流传输工具

通过 TCP 网络，将一台电脑的系统音频实时传输到另一台电脑播放。基于 VB-CABLE 虚拟声卡捕获系统音频，使用 WASAPI 低延迟采集和播放。

## 原理

```
A 机（发送端）                         B 机（接收端）
App 播放声音 → CABLE Input
                    ↓
            CABLE Output ──→ TCP ──→ 指定播放设备（耳机/音箱）
         （WASAPI 捕获）        （WASAPI 播放）
```

- A 机应用将音频输出到 **CABLE Input**（VB-CABLE 虚拟输入设备）
- VB-CABLE 驱动将音频路由到 **CABLE Output**（捕获端点）
- 本工具通过 WASAPI 从 CABLE Output 捕获 PCM16 音频
- 通过 **TCP** 传输到 B 机
- B 机接收后通过 WASAPI 播放到指定输出设备

## 编译

### MinGW-w64（推荐）
```bash
g++ -std=c++11 -O2 audiostream.cpp -o audiostream.exe -lole32 -loleaut32 -lws2_32 -luuid
```

### MSVC (Visual Studio)
```bash
cl /EHsc audiostream.cpp /link ws2_32.lib ole32.lib oleaut32.lib uuid.lib
```

## 快速开始

### 1. 安装 VB-CABLE

A 机需安装 [VB-CABLE Virtual Audio Device](https://vb-audio.com/Cable/)。

### 2. 查看音频设备

两台电脑上都可运行，确认设备名称：
```bash
audiostream.exe --list-devices
```

输出示例：
```
=== Capture Devices (Input) ===
  [0] CABLE Output (VB-Audio Virtual Cable)

=== Render Devices (Output) ===
  [0] Speakers (Realtek Audio)
  [1] CABLE Input (VB-Audio Virtual Cable)
  [2] AB13X (Bluetooth Headset)
```

### 3. 设置直连网络（千兆网线直连）

| 电脑 | 网卡 | IP 设置 |
|------|------|---------|
| A | 主板板载网卡 | `192.168.0.1`，掩码 `255.255.255.0` |
| B | USB 有线网卡 | `192.168.0.2`，掩码 `255.255.255.0` |

### 4. 启动发送端（A 机）

```bash
audiostream.exe --mode sender --ip 0.0.0.0 --port 8888 --device CABLE
```

等待显示 `Waiting for receiver to connect...`

### 5. 启动接收端（B 机）

```bash
audiostream.exe --mode receiver --ip 192.168.0.1 --port 8888 --device AB13X
```

### 6. 在 A 机播放声音

在 A 机的**系统声音设置**中，将需要传输的应用的输出设备设为 **"CABLE Input"**。

## 命令行参数

| 参数 | 发送端 | 接收端 | 默认值 |
|------|--------|--------|--------|
| `--mode <sender\|receiver>` | 发送模式 | 接收模式 | `sender` |
| `--ip <地址>` | 监听地址 | 服务端 IP | `0.0.0.0` |
| `--port <端口>` | 监听端口 | 服务端端口 | `8888` |
| `--device <名称>` | 捕获设备名 | 播放设备名（空=默认） | `CABLE` |
| `--samplerate <Hz>` | 采样率 | 采样率 | `48000` |
| `--channels <n>` | 声道数 | 声道数 | `2` |
| `--bits <n>` | 位深 | 位深 | `16` |
| `--buffer <ms>` | 缓冲时间 | 缓冲时间 | `50` |
| `--list-devices` | 列出音频设备 | 列出音频设备 | - |
| `--help` | 显示帮助 | 显示帮助 | - |

### 参数说明

- **`--device`**：按名称子串匹配，如 `--device CABLE` 匹配 `"CABLE Output (VB-Audio Virtual Cable)"`；不指定则接收端使用系统默认播放设备
- **`--buffer`**：缓冲时间（毫秒）。越小延迟越低，但可能产生爆音；越大越稳定但延迟越高
- **`--samplerate` / `--channels` / `--bits`**：音频格式，发送端和接收端需保持一致

## 完整示例

**A 机（发送端）：**
```bash
audiostream.exe --mode sender --ip 0.0.0.0 --port 8888 --device CABLE --samplerate 48000 --channels 2 --bits 16 --buffer 50
```

**B 机（接收端）：**
```bash
audiostream.exe --mode receiver --ip 192.168.0.1 --port 8888 --device AB13X --samplerate 48000 --channels 2 --bits 16 --buffer 50
```

## 停止

按 `Ctrl+C` 优雅停止，程序会显示传输统计：
- 传输时长
- 发送/播放的总帧数和字节数
- 实际采样率
- 带宽占用（Kbps）
- 静默帧占比（接收端，反映网络欠载情况）

## 常见问题

### 没有声音
- 确保 A 机有应用正在向 "CABLE Input" 播放音频
- 确保接收端 `--device` 指定了正确的播放设备（用 `--list-devices` 确认）
- 检查防火墙是否阻止了 TCP 端口
- 观察控制台输出是否有错误信息

### 程序闪退/崩溃
- 确保发送端和接收端的音频格式参数一致（`--samplerate`、`--channels`、`--bits`）
- 如果 VB-CABLE 的采样率和设置不匹配，尝试用默认值 48000

### 延迟高
- 减少 `--buffer` 值（如 `--buffer 20`）
- 确保网线直连，避免经过交换机/路由器
- TCP_NODELAY 已默认启用，无需额外设置

### 爆音/杂音
- 适当增加 `--buffer` 值
- 检查网络是否稳定
- 减少其他网络占用

## 技术细节

- **音频格式**：16-bit PCM，立体声（可通过参数调整）
- **传输协议**：TCP（带长度前缀的帧协议）
- **音频 API**：WASAPI 共享模式（事件驱动）
- **捕获来源**：指定捕获设备的 WASAPI 混音输出
- **格式兼容**：如捕获设备不支持 PCM16，自动从 IEEE Float 转换
- **抖动缓冲**：接收端 2 秒环形缓冲区，容忍网络抖动

## MIT License

```
MIT License

Copyright (c) 2026 ProcessOptimizer

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
