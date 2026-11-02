# 📄 EdgeVision-AI 开发文档 V1.0

> 基于 i.MX6U 的端云协同视觉识别系统 · 系统设计说明书
> 版本：V1.0（基础链路跑通）｜ 更新：2026 年 10 月

---

## 1. 项目概述

| 项目 | 内容 |
| --- | --- |
| **项目名称** | 基于 i.MX6U 的端云协同视觉识别系统（EdgeVision-AI） |
| **当前版本** | V1.0 —— 单帧抓拍 + 局域网直传 + 云端推理全链路打通 |
| **项目定位** | 嵌入式 Linux 边缘设备算力不足，无法本地部署大型视觉模型，因此采用 **"端侧采集 + 云端推理"** 的分布式架构，把重计算卸载到 PC 上，端侧只负责采集与传输。 |
| **核心功能** | 板端 USB 摄像头抓拍 1080P MJPEG 图像 → C 语言 Socket 客户端经局域网 TCP 直传 PC → Python 服务端收图落盘 → YOLOv8n 推理并输出带标注框的结果图。 |

### 1.1 设计取舍（为什么这么架构）

- **为什么不端侧推理**：i.MX6U 是 Cortex-A7 @528MHz、(约 512MB DDR)，没有 NPU，纯 CPU 跑 YOLOv8n 单帧推理在数十秒量级，不具备实时性；而把推理放到 PC 上，实测单帧推理仅 **12.1ms**。
- **为什么端侧仍要用 C 而不是 Python**：板端无 Python 运行时、内存紧张，C 直接调用 V4L2 ioctl，内存占用以 KB 计，启动无解释器开销。
- **为什么先做单帧而不是视频流**：V1.0 的目标是**先验证最危险的环节**——"V4L2 能否出图" 与 "双机网络能否通"。这两件事一旦验证通过，后续视频流只是循环与协议的事；反之若先写视频流框架，出问题时无法区分是采集、网络还是缓存的锅。

---

## 2. 系统架构

### 2.1 拓扑结构

```text
        ┌───────────────────────┐                ┌───────────────────────────┐
        │   端侧 (i.MX6U)        │                │  云端 (Windows PC)         │
        │                       │                │                           │
        │  [USB 摄像头]          │   TCP/IP       │  [Python Socket Server]   │
        │       │ UVC/V4L2      │  局域网直连     │        │                  │
        │       ▼               │  ─────────────▶ │        ▼                  │
        │  [C Client]           │  192.168.1.21  │  board_photo.jpg (落盘)   │
        │  /dev/video2          │  :8080         │        │                  │
        │  1920x1080 MJPEG      │                │        ▼                  │
        └───────────────────────┘                │  [YOLOv8n 推理引擎]       │
                                                 │        │                  │
                                                 │        ▼                  │
                                                 │  ai_result.jpg (结果图)   │
                                                 └───────────────────────────┘
```

### 2.2 数据流与控制流

```text
[1] open(/dev/video2)                 端侧打开摄像头设备节点
[2] VIDIOC_S_FMT  → 1920x1080 MJPEG   设定分辨率与像素格式
[3] VIDIOC_REQBUFS (count=1)          申请 1 块内核缓冲区
[4] mmap()                            内核缓冲区映射到用户空间（零拷贝）
[5] VIDIOC_QBUF → VIDIOC_STREAMON     入队并开流
[6] VIDIOC_DQBUF                      阻塞取一帧，bytesused = 帧字节数
[7] send(sock, buffer_start, bytesused, 0)   整帧一次性发出
[8] conn.recv(4096) 循环累加          云端拼成完整字节流（空包 = 结束）
[9] f.write(total_data)               写出 board_photo.jpg
[10] model('board_photo.jpg')         云端 YOLOv8n 推理
[11] results[0].save()                另存带框结果图 ai_result.jpg
```

---

## 3. 硬件与软件环境

### 3.1 端侧（边缘）

| 项 | 配置 |
| --- | --- |
| 主控 | 正点原子 ALPHA i.MX6U（Cortex-A7，512MB DDR，8GB eMMC） |
| 操作系统 | Linux 4.1.15（带图形界面，已植入 UVC 驱动） |
| 摄像头 | 免驱 USB 摄像头（UVC 标准，支持 1080P **MJPEG**） |
| 设备节点 | `/dev/video2`（板载多路 video 节点，USB 摄像头实际落在 video2） |
| 网络 | 板载以太网，静态 IP **192.168.1.21** / 24 |

### 3.2 云端（宿主 PC）

| 项 | 配置 |
| --- | --- |
| 硬件 | 个人笔记本电脑（Windows 宿主机） |
| 网卡 | 有线网卡，静态 IP **192.168.1.100** / 24 |
| 运行时 | Python + Anaconda |
| AI 依赖 | Ultralytics（YOLOv8），模型 `yolov8n.pt` |

### 3.3 工具链

- **交叉编译**：Ubuntu 虚拟机 + `arm-linux-gnueabihf-gcc`
- **编辑器 / IDE**：VS Code（端侧 C）、PyCharm / VS Code（云端 Python）
- **调试**：串口终端（板端）/ Windows 端 PowerShell

---

## 4. 核心技术栈

| 层次 | 技术 | 说明 |
| --- | --- | --- |
| 设备驱动层 | **V4L2**（Video for Linux 2） | 标准 Linux 视频采集框架，`ioctl` + `mmap` |
| 端侧应用层 | **C 语言**（gcc 交叉编译） | 调度 ioctl、管理缓冲、Socket 发送 |
| 传输层 | **TCP/IP Socket** | `SOCK_STREAM`，可靠字节流 |
| 云端服务层 | **Python 3** | Socket Server + 文件 I/O |
| 推理层 | **Ultralytics YOLOv8n** | COCO 80 类目标检测 |

---

## 5. 端侧开发详解（C 语言）

> 源码：`camera_test.c`

### 5.1 V4L2 图像采集流程

端侧基于 V4L2 接口实现**零拷贝**采集，对应代码逐段说明：

| 步骤 | 关键调用 | 代码要点 |
| --- | --- | --- |
| ① 打开设备 | `open("/dev/video2", O_RDWR)` | 失败即 `perror("打开摄像头失败")` 并退出 |
| ② 设置格式 | `ioctl(fd, VIDIOC_S_FMT, &fmt)` | `width=1920, height=1080, pixelformat=V4L2_PIX_FMT_MJPEG` |
| ③ 申请缓冲区 | `ioctl(fd, VIDIOC_REQBUFS, &req)` | `req.count = 1`，`memory = V4L2_MEMORY_MMAP` |
| ④ 查询并映射 | `VIDIOC_QUERYBUF` + `mmap()` | `PROT_READ\|PROT_WRITE`、`MAP_SHARED`、偏移 `buf.m.offset` |
| ⑤ 入队开流 | `VIDIOC_QBUF` + `VIDIOC_STREAMON` | 缓冲区交给驱动，开始填充 |
| ⑥ 阻塞取帧 | `ioctl(fd, VIDIOC_DQBUF, &buf)` | 阻塞直到一帧就绪，`buf.bytesused` 为有效字节数 |
| ⑦ 释放 | `VIDIOC_STREAMOFF` → `munmap` → `close` | 正常退出前的清理 |

**为什么选 MJPEG 而不是 YUV/RAW：**

1. 1080P 的 YUYV 单帧约 **3.1MB**，MJPEG 压缩后约 **190KB**，跨网络传输体积差一个数量级；
2. YOLO 的输入管线本就接受压缩图，MJPEG 可直接落盘成 `.jpg`，云端无需解码转换，少一步 CPU 与代码；
3. 该摄像头在 640x480 下并不支持 MJPG，仅 1080P 模式支持——这也是代码里固定 1080P 的原因（源码注释已说明）。

> ⚠️ **坑位记录**：`VIDIOC_S_FMT` 失败时驱动可能已经"静默协商"成别的宽高，因此失败分支里把 `fmt.fmt.pix.width/height` 打印出来，便于定位是分辨率不支持还是像素格式不支持。

### 5.2 Socket 网络传输

```c
int sock = socket(AF_INET, SOCK_STREAM, 0);          // TCP
server_addr.sin_family = AF_INET;
server_addr.sin_port   = htons(8080);                // 端口 8080
inet_pton(AF_INET, "192.168.1.100", &server_addr.sin_addr);  // 云端有线网卡 IP
connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr));
send(sock, buffer_start, buf.bytesused, 0);          // 整帧一次性发送
close(sock);
```

**关键设计点：**

- **直接发 `mmap` 指针**：`buffer_start` 指向内核缓冲区映射，`send()` 直接从中读取，全程没有一次 `memcpy`，这是"零拷贝"的落点。
- **发送长度用 `bytesused` 而非 `buf.length`**：`length` 是缓冲区容量，`bytesused` 才是本帧真实数据长度，用错会把缓冲区里的垃圾尾数据一起发出去。
- **用 `inet_pton` 而非 `inet_addr`**：`inet_addr` 已废弃且无法表达错误值，`inet_pton` 更严谨。
- **`connect` 失败不返回**：当前实现只是 `perror` 后继续走到清理流程，属于 V1.0 的简化处理，V1.1 需补上重试与退出码。

### 5.3 编译与部署

```bash
# Ubuntu 虚拟机上交叉编译
arm-linux-gnueabihf-gcc camera_test.c -o camera_test

# 拷贝到板子（NFS / scp / TF 卡均可）
# 板端运行
./camera_test
```

---

## 6. 云端开发详解（Python）

> 源码：`test.py`

### 6.1 一、网络接收与重组

```python
HOST, PORT = '0.0.0.0', 8080
server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server_socket.bind((HOST, PORT))
server_socket.listen(1)
conn, addr = server_socket.accept()
total_data = b''
while True:
    data = conn.recv(4096)
    if not data:            # 对端关闭 → 空包 → 传输结束
        break
    total_data += data
```

| 设计点 | 说明 |
| --- | --- |
| `bind('0.0.0.0')` | 监听本机所有网卡，PC 换网络（有线/无线）不用改代码 |
| `SO_REUSEADDR` | 避免服务重启时 `Address already in use`（TIME_WAIT 占用端口） |
| `recv(4096)` + 循环 | 单次 `recv` 拿不全一帧 190KB，必须循环累加 |
| `break` 条件 | 依赖对端 `close(sock)` 产生的空包判定帧结束——**这就是 V1.0 的隐式帧协议** |
| `finally` 统一释放 | 异常路径也保证 `conn` / `server_socket` 被关闭，不留僵尸端口 |

> ⚠️ **已知局限（V2.0 首要解决项）**：当前以"连接关闭"作为帧边界，只能传单帧；一旦要传视频流，必须引入**长度包头**，否则 TCP 字节流会被粘连成一条无法切分的流（详见 §8.2）。

### 6.2 二、AI 模型推理

```python
model = YOLO('yolov8n.pt')          # Nano 级轻量模型
results = model('board_photo.jpg')  # 推理
for r in results:
    for box in r.boxes:
        cls_name = model.names[int(box.cls[0])]
        conf     = float(box.conf[0])
results[0].save('ai_result.jpg')    # 另存带标注框的结果图
```

| 设计点 | 说明 |
| --- | --- |
| `yolov8n.pt` | Ultralytics 官方 COCO 预训练 Nano 权重（约 6.2MB，约 320 万参数），CPU 上可到毫秒级 |
| 模型只加载一次 | 模型加载在函数中、接收之后，当前每进程只推理一帧；V2.0 要挪到循环外复用 |
| 遍历 `box.cls` / `box.conf` | 直接读张量取类别名与置信度并打印 |
| `results[0].save()` | 由 Ultralytics 直接绘制标注框并写盘，省去手写 OpenCV 画框 |

---

## 7. 当前进度与联调结果（V1.0 实测）

### 7.1 联调结论

| 验证项 | 结果 |
| --- | --- |
| **网络联通** | ✅ 私有局域网搭好，PC ↔ 板子双向 `ping` 通（192.168.1.100 ↔ 192.168.1.21） |
| **图像采集** | ✅ V4L2 在 `/dev/video2` 上成功协商 1920x1080、MJPEG |
| **图像传输** | ✅ 单帧 1080P（约 **190KB**）稳定送达，**丢包率 0**（TCP 可靠性保证），端到端耗时约 **0.1s** |
| **AI 识别** | ✅ PC 端推理耗时约 **12.1ms**，识别出人物、电脑、杯子等目标，最高置信度 **0.93** |
| **自动落盘** | ✅ 云端产出 `board_photo.jpg`（原始帧）与 `ai_result.jpg`（标注结果图），均为 1920x1080 |

### 7.2 实测样例

**① 板端原始抓拍**（`board_photo.jpg`，1920×1080，V4L2 直出 MJPEG）

![板端原始抓拍](board_photo.jpg)

**② 云端 YOLOv8n 识别结果**（`ai_result.jpg`，1920×1080，标注框 + 类别 + 置信度）

![YOLOv8n 识别结果](ai_result.jpg)

### 7.3 性能小结

```
端侧采集 ──► 传输 ──► 云端落盘 ──► 推理 ──► 保存结果
  ~? ms       ~100ms     <1ms       12.1ms     ~10ms
                        └──────── 端到端约 0.1~0.2s ────────┘
```

- 瓶颈在**网络传输与大图 I/O**，而非推理（12.1ms 对 1080P 属于正常水平）。
- 推理耗时测试为 PC 端 CPU 推理；若后续启用 GPU，该段可再降。

---

## 8. 已知问题与 V2.0 迭代规划

### 8.1 已知问题清单（V1.0 代码现状）

| # | 问题 | 位置 | 影响 |
| --- | --- | --- | --- |
| P1 | `send()` 返回值未校验 | `camera_test.c` | 大帧可能未发完整而静默"成功" |
| P2 | `connect` 失败后仍继续执行清理流程、无重试 | `camera_test.c` | 网络抖动即整次抓拍作废 |
| P3 | 所有 `ioctl` 返回值未检查（除 `S_FMT`） | `camera_test.c` | 驱动异常时难以定位 |
| P4 | 以"连接关闭"作为帧边界 | `test.py` | **无法扩展为视频流** |
| P5 | `total_data += data` 为 O(n²) 拼接 | `test.py` | 帧小时无感，帧大时会有额外拷贝 |
| P6 | 服务端 `accept` 一次即退出 | `test.py` | 每次抓拍都要重启服务 |
| P7 | 板子 IP / 端口硬编码 | 两端 | 换网段需重新交叉编译 |

### 8.2 V2.0 Todo List

- [x] **① 单帧采集 + 局域网传输 + 云端推理**（V1.0 已完成）
- [ ] **② 视频流采集**：`req.count` 提升至 4，改为 `while(1)` 循环 `DQBUF → 处理 → QBUF`，实现连续抓图。
- [ ] **③ 应用层帧协议**：加入 **4 字节长度包头**（`| len:hdr | payload |`），彻底解决 TCP 连续传输的粘包/分帧问题。发送端先发 `htonl(bytesused)` 再发帧数据；接收端先收满 4 字节得长度，再循环收满该长度。
- [ ] **④ 反向控制反馈**：云端识别到特定目标后，经同一条 Socket 回传指令，端侧解析后通过 sysfs 点亮板载 LED（`/sys/class/leds/.../brightness`）。
- [ ] **⑤ GUI 界面**：Qt 或 LVGL 为端侧做本地显示界面（先显示抓拍缩略图，再显示云端回传的识别结果）。
- [ ] **⑥ 健壮性加固**：修复 §8.1 的 P1~P7——校验 `send`/`ioctl` 返回值、`connect` 重试、服务端 `while(1)` 常驻、IP/端口改为命令行参数、接收缓冲改用 `bytearray`。

### 8.3 迭代路线（版本节奏）

| 版本 | 目标 | 关键交付 |
| --- | --- | --- |
| **V1.0** | 链路跑通 | 单帧 1080P 抓拍 → 传输 → 推理 → 结果图 ✅ |
| **V1.1** | 稳定可重复 | 修 P1~P7，服务端常驻，可连续多次抓拍 |
| **V2.0** | 真实视频流 | 多缓冲 + 长度包头帧协议，实时率 ≥ 数 fps（受网络与板端编码限制） |
| **V2.1** | 闭环控制 | 反向指令通道 + LED 联动 |
| **V3.0** | 产品化 | 端侧 GUI 显示 + 日志 + 配置化 |

---

## 9. 工程结构

```text
v1.0/
├── camera_test.c                    端侧：V4L2 采集 + TCP 发送（C）
├── test.py                          云端：TCP 接收 + YOLOv8n 推理（Python）
├── yolov8n.pt                       YOLOv8 Nano COCO 预训练权重（约 6.2MB）
├── board_photo.jpg                  云端落盘的板端原始抓拍（实测样例）
├── ai_result.jpg                    云端推理输出的标注结果图（实测样例）
└── 📄 EdgeVision-AI 开发文档 V1.0.md  本文档
```

---

## 10. 复现步骤（联调手册）

```bash
# ── 0. 网络：PC 有线网卡与板子同网段
#    PC   : 192.168.1.100 / 255.255.255.0
#    板子 : 192.168.1.21  / 255.255.255.0
ping 192.168.1.21        # PC → 板子，通即可

# ── 1. 云端：先启动服务（顺序不能反）
python test.py
#    【云端】AI 服务器已启动，正在 0.0.0.0:8080 等待板子连接...

# ── 2. 端侧：运行抓拍程序
./camera_test
#    格式设置成功！分辨率: 1920 x 1080
#    【成功抓拍 JPG】大小: 190xxx 字节
#    connected successfully! ...
#    data sending successfully!

# ── 3. 云端：自动落盘 + 推理
#    📦 【云端】图片接收完毕，共 19xxxx 字节。
#    🎯 发现目标: person, 置信度: 0.93
#    🎉 【完成】带识别框的图片已保存为 ai_result.jpg
```

**排查速查表**

| 现象 | 可能原因 | 处理 |
| --- | --- | --- |
| `打开摄像头失败` | 设备号不是 video2 / 未插摄像头 | `ls /dev/video*`，改 `open()` 路径 |
| `设置 MJPG 格式失败` | 摄像头该分辨率不支持 MJPG | 看打印出的实际宽高，降分辨率或换格式 |
| 抓拍成功但云端没收全 | 连接被提前关闭 / IP 写错 | 确认 `inet_pton` 的 IP 与 PC 网卡一致 |
| `Address already in use` | 上次服务未释放端口 | 已加 `SO_REUSEADDR`；仍冲突则换端口 |
| 识别结果为空 | 图片本身无 COCO 类目标 / 图像损坏 | 先打开 `board_photo.jpg` 确认是否正常 |

---

*(文档版本：V1.0 更新版 ｜ 创建于 2026 年 10 月 ｜ 本版已按 `camera_test.c`、`test.py` 实际实现校验校正)*