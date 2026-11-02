# EdgeVision-AI

> 基于 i.MX6U 的端云协同视觉识别系统 —— 嵌入式 Linux 采集 + 局域网 TCP 推流 + PC 端 YOLOv8 推理 + 反向硬件控制闭环。

边缘设备算力不足、无法部署大型视觉模型，因此本系统把**重推理卸载到 PC**，端侧只负责高速采集与推流，并把云端识别结论反向下发为物理动作（点亮板载 LED）。

## 系统架构

```text
┌───────────────────────────────────┐              ┌───────────────────────────────────┐
│  端侧 (i.MX6U · Linux 4.1.15)      │              │  云端 (Windows PC)                 │
│                                   │              │                                   │
│  [USB 摄像头 UVC/V4L2]            │  TCP :8080   │  [Python Socket Server]           │
│   /dev/video2  1920x1080 MJPEG    │  192.168.1.21│        │                          │
│         │ mmap × 4               │ ──────────▶  │        ▼                          │
│         ▼                        │ 192.168.1.100│  [np.frombuffer + cv2.imdecode]   │
│  [主线程 环形队列]                │              │        │ 内存解码，不落盘          │
│   DQBUF → send → QBUF            │              │        ▼                          │
│         │                        │  "1" / "0"   │  [YOLOv8n 推理]                   │
│         ▼                        │ ◀──────────  │        │                          │
│  [子线程 receive_cmd]            │              │        ▼                          │
│   sysfs → sys-led/brightness     │              │  [cv2.imshow 实时窗口 + FPS]      │
└───────────────────────────────────┘              └───────────────────────────────────┘
```

## 版本演进

| 版本 | 目标 | 关键实现 | 实测 |
| --- | --- | --- | --- |
| **v1.0** | 链路跑通 | V4L2 单缓冲抓拍 1080P MJPEG，TCP 直发；云端落盘 + 推理 | 单帧 ~190KB，传输 0.1s，推理 12.1ms，置信度 0.93 |
| **v2.0** | 实时流 + 闭环 | 4 缓冲环形队列连续采集、4 字节长度前缀帧协议、pthread 双工收令控灯、内存解码不落盘、实时窗口显示 | **25~30 FPS**，`person`(conf>0.5) 下发 `"1"` 点亮 LED |

## 目录结构

```text
.
├── v1.0/
│   ├── camera_test.c      端侧：V4L2 单缓冲采集 + TCP 发送
│   ├── test.py            云端：TCP 接收落盘 + YOLOv8n 推理
│   ├── yolov8n.pt         YOLOv8 Nano COCO 预训练权重（约 6.2MB，两版共用）
│   └── 📄 EdgeVision-AI 开发文档 V1.0.md
└── v2.0/
    ├── camera2.c          端侧：4 缓冲环形队列 + 帧协议 + 收令控灯
    ├── test.py            云端：按长度收帧 + 内存解码 + 推理 + 回令
    └── 📄 EdgeVision-AI 开发文档 V2.0.md
```

> **关于测试图像**：链路运行时会生成 `board_photo.jpg` / `ai_result.jpg`。由于画面涉及测试者隐私，这些文件**未纳入仓库**，且已在 `.gitignore` 中排除。需要时按下方「快速开始」跑一次即可重新生成。

> **关于 `yolov8n.pt`**：两个版本使用的权重文件完全相同（md5 一致），仓库中只保留 `v1.0/yolov8n.pt` 一份作为真相源，`v2.0` 下的副本已被 `.gitignore` 排除。运行 v2.0 前请复制一份过去：
>
> ```bash
> copy v1.0\yolov8n.pt v2.0\yolov8n.pt      # Windows
> cp v1.0/yolov8n.pt v2.0/yolov8n.pt        # Linux / macOS
> ```
>
> 或直接让 Ultralytics 首次运行时自动下载（需联网）。

## 快速开始

### 环境

| 端 | 配置 |
| --- | --- |
| 端侧 | 正点原子 ALPHA i.MX6U（Cortex-A7，512MB DDR），Linux 4.1.15，UVC 免驱 USB 摄像头 |
| 云端 | Windows PC，Python + Anaconda，`ultralytics` / `opencv-python` / `numpy` |
| 工具链 | Ubuntu + `arm-linux-gnueabihf-gcc` |

### 网络

```
PC  : 192.168.1.100 / 255.255.255.0
板子: 192.168.1.21  / 255.255.255.0
ping 192.168.1.21    # 通即可
```

> IP 目前在代码中硬编码，换网段需同时修改 `camera2.c` 的 `inet_pton` 与 `test.py` 的 `PORT`。

### 编译（交叉编译，注意 v2.0 需链接 pthread）

```bash
arm-linux-gnueabihf-gcc v1.0/camera_test.c -o camera_test
arm-linux-gnueabihf-gcc v2.0/camera2.c     -o camera2 -lpthread
```

### 运行

```bash
# 1. 云端先启动（端侧 connect 不会重试，顺序不能反）
python v2.0/test.py

# 2. 端侧运行
./camera2
#    格式设置成功！分辨率: 1920 x 1080
#    connect successfully!
#    当前实时帧率: 27 帧/秒

# 3. 云端弹出窗口 "EdgeVision - Real-time Video"，左上角叠加 FPS，按 q 退出
# 4. 闭环验证：人入镜 → 板载 sys-led 亮；人离开 → 熄
```

## 端云协议

```text
 0        1        2        3        4                     4+len-1
 +--------+--------+--------+--------+---- - - - - - - - --+
 |      Length (uint32, big-endian)  |   Payload (MJPEG)   |
 +--------+--------+--------+--------+---- - - - - - - - --+
```

| 方向 | 内容 |
| --- | --- |
| 端 → 云 | `4B 大端长度 + payload`，payload 为完整一帧 MJPEG，典型 150~250KB |
| 云 → 端 | 每帧 1 字节指令：`"1"` 亮灯 / `"0"` 熄灯 |

## 文档

- [开发文档 V1.0](v1.0/📄%20EdgeVision-AI%20开发文档%20V1.0.md) —— 单帧链路的完整设计与联调记录
- [开发文档 V2.0](v2.0/📄%20EdgeVision-AI%20开发文档%20V2.0.md) —— 实时流架构、协议设计、缓冲区状态机、问题清单与 V3.0 规划

## 已知问题

详见 [V2.0 文档 §8.1](v2.0/📄%20EdgeVision-AI%20开发文档%20V2.0.md)。当前最需要修的四条：

- `camera2.c` 末尾 `munmap` 循环用的是最后一块残留的 `buf.length`，而非各块自身长度
- `camera2.c` 中两个 `send()` 返回值未校验，大帧可能只发了一部分却当成功
- `camera2.c` 中 `connect` 失败后仍打印 `connect successfully!` 并继续死循环
- `test.py` 断连时只 `break` 出内层 `while`，仍用残缺 `img_data` 解码，导致流位置错乱

## 许可

代码可自由参考。`yolov8n.pt` 为 Ultralytics 官方权重，遵循 AGPL-3.0。