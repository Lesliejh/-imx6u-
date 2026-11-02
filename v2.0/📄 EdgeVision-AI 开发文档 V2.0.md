# 📄 EdgeVision-AI 开发文档 V2.0

> 基于 i.MX6U 的端云协同 AI 视觉实时监控系统 · 系统设计说明书
> 版本：V2.0（实时视频流 + 双向控制闭环）｜ 更新：2026 年 11 月

---

## 1. 项目概述

| 项目 | 内容 |
| --- | --- |
| **项目名称** | 基于 i.MX6U 的端云协同 AI 视觉实时监控系统（EdgeVision-AI） |
| **当前版本** | V2.0 —— 实时推流 + 无粘包帧协议 + 云端推理 + 反向指令闭环 |
| **相对 V1.0 的跃迁** | 从「单帧抓拍」升级为「连续视频流」；从「单向传输」升级为「双向全双工」；从「隐式帧边界」升级为「显式长度协议」 |
| **项目背景** | 嵌入式边缘设备（i.MX6U）算力不足、无 NPU，无法本地部署大型视觉模型，因此设计端云分离架构：端侧只做高速采集与推流，云端承担重推理，并把识别结论反向下发为物理动作。 |
| **核心功能** | ① 端侧 4 缓冲 V4L2 环形队列连续采集 1080P MJPEG 并推流；② 自定义 4 字节长度前缀协议防粘包；③ 云端内存解码 + YOLOv8 实时检测；④ 检出 `person`（conf > 0.5）即回传 `"1"`，端侧子线程经 sysfs 点亮板载 LED，否则回传 `"0"` 熄灯。 |

### 1.1 V1.0 → V2.0 演进对照

| 维度 | V1.0 | V2.0 | 升级动因 |
| --- | --- | --- | --- |
| 采集模式 | 单缓冲 `count=1`，取一帧即退出 | 4 缓冲环形队列，`while(1)` 连续取帧 | 单缓冲无法连续出流；多缓冲让驱动在用户处理上一帧时继续填充下一帧，避免丢帧 |
| 帧边界 | 靠对端 `close(sock)` 判定结束 | 4 字节大端长度头 `\|len\|payload\|` | 连接不复位就再也无法切帧，**这是视频流的前置条件** |
| 通信方向 | 单向（端 → 云） | 双向全双工（主线程发流 / 子线程收令） | 需要根据识别结果反向控制硬件 |
| 硬件联动 | 无 | sysfs 操作 `/sys/class/leds/sys-led/brightness` | 演示"感知 → 决策 → 执行"闭环 |
| 图片落地 | 落盘 `.jpg` + `model(path)` | `np.frombuffer` + `cv2.imdecode` 纯内存，不落盘 | 每秒几十帧落盘会造成磁盘 I/O 瓶颈与文件堆积 |
| 可视化 | 保存结果图 | `results[0].plot()` + `cv2.imshow` 实时窗口 + FPS 叠加 | 实时监控需要人眼可观 |
| 性能观测 | 无 | 端侧每秒打印帧率；云端每 10 帧测一次平均 FPS | 无观测则无法定位瓶颈 |

---

## 2. 系统架构

### 2.1 拓扑结构

```text
┌───────────────────────────────────┐              ┌───────────────────────────────────┐
│  端侧 (i.MX6U · Linux 4.1.15)      │              │  云端 (Windows PC)                 │
│                                   │              │                                   │
│  ┌─────────────────────────┐      │              │  ┌─────────────────────────────┐  │
│  │ USB 摄像头 (UVC / V4L2) │      │              │  │ Python Socket Server        │  │
│  │  /dev/video2            │      │  TCP :8080   │  │ accept() 单连接              │  │
│  │  1920x1080 MJPEG        │      │ 192.168.1.21 │  │                             │  │
│  └───────────┬─────────────┘      │ ──────────▶  │  │ 收 4B 长度头 → 收满 payload  │  │
│              │ mmap × 4            │ 192.168.1.100│  └──────────────┬──────────────┘  │
│              ▼                    │              │                 ▼                 │
│  ┌─────────────────────────┐      │              │  ┌─────────────────────────────┐  │
│  │ 主线程                   │      │              │  │ np.frombuffer               │  │
│  │ DQBUF → send → QBUF      │      │              │  │   → cv2.imdecode            │  │
│  │ 环形队列 4 缓冲          │      │              │  └──────────────┬──────────────┘  │
│  └───────────┬─────────────┘      │              │                 ▼                 │
│              │                    │              │  ┌─────────────────────────────┐  │
│              │                    │  "1" / "0"   │  │ YOLOv8n 推理                │  │
│              │                    │ ◀──────────  │  │ person & conf>0.5 ?         │  │
│              ▼                    │              │  └──────────────┬──────────────┘  │
│  ┌─────────────────────────┐      │              │                 ▼                 │
│  │ 子线程 receive_cmd()     │      │              │  ┌─────────────────────────────┐  │
│  │ recv cmd → write LED     │      │              │  │ cv2.imshow 实时窗口 + FPS   │  │
│  │ sys-led/brightness       │      │              │  └─────────────────────────────┘  │
│  └─────────────────────────┘      │              │                                   │
└───────────────────────────────────┘              └───────────────────────────────────┘
```

### 2.2 时序图（单帧完整往返）

```text
端侧主线程                  端侧子线程                 云端 Server
    │                          │                          │
    │ ioctl(DQBUF) 取帧         │                          │
    │──send(4B 长度头)────────────────────────────────────▶│
    │──send(MJPEG 数据)────────────────────────────────────▶│
    │                          │                          │ 收满 header → 收满 payload
    │                          │                          │ imdecode → YOLO 推理
    │                          │                          │ person? → 组指令
    │                          │◀────────send(b"1" or b"0")│
    │                          │ recv(cmd, 10)            │
    │                          │ write(led_fd, "1")       │
    │ ioctl(QBUF) 归还缓冲       │                          │ results[0].plot()
    │ （下一轮循环）             │                          │ imshow + waitKey(1)
```

### 2.3 数据流关键路径

```text
摄像头硬件 ──DMA──▶ 内核缓冲区(4 块) ──mmap──▶ 用户空间指针 ──send()──▶ 网卡
                                                                      │
                                          云端 recv ──┐              │
                                                      ▼              │
                                          bytes → np.ndarray → cv2.imdecode → BGR frame
                                                      │
                                                      ▼
                                          model(frame) → Boxes → plot() → imshow
```

> 注意：端侧全程 **无一次 `memcpy`**（`send` 直接从 `mmap` 指针取数据），云端全程 **不落盘**（字节流直接进内存解码），两端各自消除了一个 I/O 瓶颈。

---

## 3. 硬件与软件环境

### 3.1 端侧

| 项 | 配置 |
| --- | --- |
| 主控 | 正点原子 ALPHA i.MX6U（Cortex-A7 @528MHz，512MB DDR，8GB eMMC） |
| 系统 | Linux 4.1.15（带图形界面，UVC 驱动已植入） |
| 摄像头 | 免驱 USB 摄像头（UVC），1080P 下支持 **MJPEG**（640×480 不支持，见 §5.1 坑位） |
| 设备节点 | `/dev/video2` |
| LED 节点 | `/sys/class/leds/sys-led/brightness`（正点原子板载 sys-led） |
| 网络 | 静态 IP **192.168.1.21** / 24 |

### 3.2 云端

| 项 | 配置 |
| --- | --- |
| 硬件 | 个人笔记本电脑（Windows 宿主机） |
| 网卡 | 有线网卡，静态 IP **192.168.1.100** / 24 |
| 运行时 | Python + Anaconda |
| 关键库 | `ultralytics`（YOLOv8）、`opencv-python`（cv2）、`numpy` |
| 模型 | `yolov8n.pt`（COCO 80 类，约 6.2MB） |

### 3.3 工具链

- 交叉编译：Ubuntu 虚拟机 + `arm-linux-gnueabihf-gcc`
- 端侧调试：串口终端
- 云端调试：VS Code / PyCharm

---

## 4. 核心技术栈

| 层次 | 技术 | V2.0 中的具体用途 |
| --- | --- | --- |
| 驱动/采集 | **V4L2** + `mmap` | 4 缓冲环形队列零拷贝采集 |
| 并发 | **pthread** | 主线程发流 / 子线程收令，双工 |
| 网络 | **TCP Socket** | 全双工字节流 |
| 应用层协议 | **自定义长度前缀帧协议** | `4B 大端长度 + payload`，根除粘包 |
| 硬件控制 | **sysfs** | `open`/`write` 直接操作 LED brightness |
| 云端解码 | **numpy + OpenCV** | `frombuffer` → `imdecode`，内存态解码 |
| 推理 | **Ultralytics YOLOv8n** | 逐帧目标检测 |
| 可视化 | **OpenCV HighGUI** | `imshow` + `putText` 实时画面 |

---

## 5. 端侧开发详解（C 语言）

> 源码：`camera2.c`

### 5.1 V4L2 四缓冲环形队列

```c
#define WIDTH 1920
#define HEIGHT 1080
#define BUFFER_COUNT 4
```

| 步骤 | 调用 | 关键点 |
| --- | --- | --- |
| ① 开设备 | `open("/dev/video2", O_RDWR)` | 失败即打印 `打开摄像头失败` 并 `return -1` |
| ② 设格式 | `VIDIOC_S_FMT` | `1920×1080` + `V4L2_PIX_FMT_MJPEG`；失败分支打印驱动**实际协商**出的宽高 |
| ③ 申请缓冲 | `VIDIOC_REQBUFS`，`req.count = BUFFER_COUNT(4)` | 4 块内核缓冲 |
| ④ 逐块映射并入队 | `VIDIOC_QUERYBUF` → `mmap` → `VIDIOC_QBUF` | 循环 4 次，`buffers[i]` 保存各块用户态地址，**映射完立即入队**，让驱动提前开始填充 |
| ⑤ 开流 | `VIDIOC_STREAMON` | 驱动开始按帧写入队列 |
| ⑥ 取帧 | `VIDIOC_DQBUF` | 取出**已填满**的那块，`buf.index` 指明是哪一块 |
| ⑦ 归还 | `VIDIOC_QBUF` | 用完立刻归还，形成环形队列 |

**为什么是 4 块而不是 1 块：**

单个缓冲时，用户态处理（`send` 190KB 到网络）期间驱动无处可写，只能丢弃新帧或覆盖，表现为**帧率上不去、画面跳帧**。4 块后，用户态手上 1 块、内核队列中 3 块，`send` 的耗时被驱动填充下一帧的时间掩盖，采集与传输重叠进行。这也是 V2.0 能跑到 25~30 FPS 的结构性原因。

**缓冲区所有权状态机（防止 Data Race）：**

```text
        REQBUFS+mmap                QBUF
   ┌──────────────┐   ┌──────────────┐   ┌─────────────────┐
   │  ALLOCATED   │──▶│  QUEUED      │──▶│  FILLING        │
   │ (用户态持有) │   │ (内核队列等待)│   │ (驱动正在写入)  │
   └──────────────┘   └──────────────┘   └────────┬────────┘
          ▲                                        │ 一帧填满
          │              DQBUF                     ▼
          │        ┌──────────────┐        ┌─────────────────┐
          └────────│  DEQUEUED    │◀───────│  DONE           │
            QBUF   │ (用户态处理) │        │ (数据就绪)      │
                   └──────────────┘        └─────────────────┘
```

**核心规则：同一块 buffer 在同一时刻只能属于内核或用户态之一。** `DQBUF` 把所有权交给用户，`QBUF` 交回内核。V2.0 严格遵守"`DQBUF` → `send` → `QBUF`"的顺序，因此在 `send` 期间驱动绝不会覆盖正在发送的数据——这就是不加锁也不会踩踏的原因。

> ⚠️ **坑位记录 ①**：`VIDIOC_S_FMT` 失败时驱动可能已静默协商成别的宽高，所以失败分支必须打印 `fmt.fmt.pix.width/height`，否则无从判断是分辨率不支持还是像素格式不支持。
>
> ⚠️ **坑位记录 ②**：本摄像头在 640×480 下**不支持 MJPG**，仅在 1080P 下支持，因此分辨率被写死为 1080P（源码注释已说明）。

### 5.2 自定义帧协议（根除粘包）

```c
uint32_t net_len = htonl(buf.bytesused);   // 转大端
send(sock, &net_len, sizeof(net_len), 0);  // 发 4 字节长度头
send(sock, buffers[buf.index], buf.bytesused, 0);  // 发正文字节
```

| 设计点 | 说明 |
| --- | --- |
| **为什么必须加长度头** | TCP 是**字节流**，没有消息边界。连续发帧时，接收端 `recv(4096)` 可能在任意位置截断，一帧的尾部和下一帧的头部会粘在一起（粘包），也可能一帧被拆成多次到达（拆包）。只有显式携带长度才能还原边界。 |
| **为什么用 `htonl`** | 网络字节序统一为大端。i.MX6U（ARM）与 x86 PC 都是小端，不转换的话本地"碰巧能通"，但协议不可移植，且与云端 `'>I'` 的解码约定必须一致。 |
| **为什么长度取 `bytesused` 而非 `buf.length`** | `buf.length` 是缓冲区**容量**，`bytesused` 才是本帧**有效字节数**。用错会把缓冲区里的垃圾尾数据一起发出去，而且长度头会对不上实际内容。 |
| **为什么分两次 `send`** | 头部 4 字节与正文大小、生命周期都不同（头在栈上，正文在 mmap），分开发逻辑清晰。代价是每次多一个系统调用，可接受。 |

**协议帧格式：**

```text
 0        1        2        3        4                     4+len-1
 +--------+--------+--------+--------+---- - - - - - - - --+
 |      Length (uint32, big-endian)  |   Payload (MJPEG)   |
 +--------+--------+--------+--------+---- - - - - - - - --+
   \____________ 4 字节 ____________/  \_____ len 字节 _____/

 len 取值范围：约 150KB ~ 250KB（1080P MJPEG 单帧典型值）
 无分隔符、无校验、无版本号 —— 这是 V2.0 的最小可用设计
```

### 5.3 双工并发架构

```c
pthread_t pid;
pthread_create(&pid, NULL, receive_cmd, &sock);
pthread_detach(pid);
```

- **主线程**：独占 `while(1)` 做 `DQBUF → send → QBUF`，是吞吐关键路径，因此**不放任何阻塞操作**。
- **子线程**：`receive_cmd()` 里 `recv(sock, cmd, 10, 0)` 阻塞等云端指令。若把它放进主线程，收令时整个采集会卡死。
- **`pthread_detach`**：子线程是永不主动退出的守护型线程，主线程不需要 join，分离后其资源由系统回收。

> ⚠️ **正确性说明**：`receive_cmd` 的退出条件是 `recv` 返回 `<= 0`（对端关闭或出错）。注意 `n == 0` 与 `n < 0` 被合并处理了——后者可能是 `EINTR`（被信号打断），严格来说应判 `errno` 后决定是否重试。

### 5.4 硬件控制：sysfs 点亮 LED

```c
int led_fd = open("/sys/class/leds/sys-led/brightness", O_WRONLY);  // 线程启动时开一次
...
if (cmd[0] == '1')      write(led_fd, "1", 1);
else if (cmd[0] == '0') write(led_fd, "0", 1);
```

| 设计点 | 说明 |
| --- | --- |
| **为什么不用 `system("echo 1 > ...")`** | `system()` 要 `fork` + `exec` 一个 shell，实测开销在**毫秒级**且会继承阻塞的信号掩码；直接 `write` 是**微秒级**，且无进程创建风险。 |
| **为什么 `led_fd` 提前打开** | `open` 是系统调用且有路径解析开销。LED 是长期目标，线程入口开一次、退出时关一次，避免每帧都 `open/close`。 |
| **为什么只写 1 字节** | sysfs 的 `brightness` 属性接受 ASCII 数字，写 `"1"`/`"0"` 即开/关；不加换行也能被内核解析。 |
| **指令解析用 `cmd[0]`** | 只关心首字符，故云端发单字节 `b"1"`/`b"0"` 即可，无需字符串协议。 |

### 5.5 帧率统计

```c
int fps = 0;
time_t start = time(NULL);
while(1){
    ...
    fps++;
    if (time(NULL) - start >= 1) {
        printf("当前实时帧率: %d 帧/秒\n", fps);
        fps = 0; start = time(NULL);
    }
}
```

以 1 秒为窗口统计端侧**成功推流**的帧数。注意它统计的是"循环次数"，不是"云端处理数"，两者若有差距即说明网络或云端是瓶颈。

### 5.6 编译与部署

```bash
# Ubuntu 虚拟机交叉编译（需链接 pthread）
arm-linux-gnueabihf-gcc camera2.c -o camera2 -lpthread

# 拷贝到板子后运行
./camera2
```

---

## 6. 云端开发详解（Python）

> 源码：`test.py`

### 6.1 服务端初始化

```python
HOST, PORT = '0.0.0.0', 8080
model = YOLO('yolov8n.pt')          # 进程启动即加载，循环外只加载一次
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind((HOST, PORT)); server.listen(5)
conn, addr = server.accept()
```

| 设计点 | 说明 |
| --- | --- |
| 模型加载在循环外 | V1.0 里模型是收到图之后才加载；V2.0 移到启动阶段，避免每帧重复加载（加载耗时远大于推理） |
| `bind('0.0.0.0')` | 监听全部网卡，PC 换有线/无线不用改代码 |
| `SO_REUSEADDR` | 服务重启时不因 TIME_WAIT 报 `Address already in use` |
| `listen(5)` | 挂起队列长度 5，实际只服务一条连接 |

### 6.2 严格按长度收帧（与端侧协议对称）

```python
# 1. 收满 4 字节头
header_data = b''
while len(header_data) < 4:
    packet = conn.recv(4 - len(header_data))   # 注意：要的是"还差多少"
    if not packet: break
    header_data += packet
if len(header_data) < 4: break                  # 连头都没收全 → 对端断开

# 2. 解析大端长度
img_size = struct.unpack('>I', header_data)[0]

# 3. 按长度收满正文
img_data = b''
while len(img_data) < img_size:
    packet = conn.recv(img_size - len(img_data))
    if not packet: break
    img_data += packet
```

| 设计点 | 说明 |
| --- | --- |
| **两个独立的 `while`** | 头部和正文是两类长度的数据，必须分两段循环，不能混在一轮 `recv` 里 |
| **`recv(n - len(x))` 而非固定 `recv(4096)`** | 精确索取"还差多少字节"，减少多余的阻塞等待与中间拷贝 |
| **`struct.unpack('>I')`** | `'>I'` 表示大端无符号 32 位，与端侧 `htonl(uint32_t)` 严格对称 |
| **断连的双重判定** | 头部没收满 → 直接退；正文 `recv` 空 → 内层 `break` |

> ⚠️ **已知缺陷（V3.0 待修）**：第 3 步 `while` 里的 `if not packet: break` 只跳出内层循环，之后代码仍会用**不完整的 `img_data`** 去 `imdecode`，得到 `None` 后被 `if frame is not None` 挡掉，于是继续进入下一轮收帧——此时流位置已错乱。正确做法是断连时直接跳到最外层退出。详见 §8.1 P4。

> ⚠️ **性能隐患**：`img_data += packet` 在 Python 中每次都要重新分配并拷贝整个缓冲区，收满 190KB 的帧属于 O(n²) 行为。虽然当前帧率下未成为瓶颈，但应改用 `bytearray` 或 `recv_into(memoryview)`。

### 6.3 内存态极速解码

```python
img_array = np.frombuffer(img_data, dtype=np.uint8)   # 零拷贝：不复制字节，仅建视图
frame = cv2.imdecode(img_array, cv2.IMREAD_COLOR)     # 在内存中解压为 BGR
```

| 设计点 | 说明 |
| --- | --- |
| **为什么 `frombuffer` 而不是 `np.array(list(img_data))`** | `frombuffer` 直接在原字节缓冲区上建立 NumPy 视图，**不复制**；转 list 会产生上百万个 Python int，慢几个数量级 |
| **为什么 `imdecode` 而不是落盘再 `imread`** | 绕开磁盘 I/O。若按 25 FPS 落盘，每秒约 4.75MB 写入，很快形成 I/O 瓶颈且文件堆积 |
| **`IMREAD_COLOR`** | 强制解为 3 通道 BGR，与 YOLO 输入约定一致 |
| **`frame is not None` 兜底** | `imdecode` 遇到不完整或损坏的 JPEG 会返回 `None`，必须判空，否则下一行 `model(frame)` 直接抛异常 |

### 6.4 实时推理、判定与指令下发

```python
results = model(frame, verbose=False)
annotated_frame = results[0].plot()

person_detected = False
for r in results:
    for box in r.boxes:
        cls_name = model.names[int(box.cls[0])]
        if cls_name == 'person' and float(box.conf[0]) > 0.5:
            person_detected = True
            print(f" 第 {frame_count} 帧: 发现人！置信度 {float(box.conf[0]):.2f}")

conn.send(b"1" if person_detected else b"0")
```

| 设计点 | 说明 |
| --- | --- |
| `verbose=False` | 关闭 Ultralytics 每帧的日志输出，否则控制台刷屏会拖慢主循环 |
| `results[0].plot()` | Ultralytics 内置绘图，直接返回画好框的 ndarray，省去手写 OpenCV 画框 |
| **置信度阈值 0.5** | 经验值。`yolov8n` 在 0.3 以下误检明显增多；0.5 能压制大部分误报，代价是远距/遮挡的小目标可能漏检 |
| **逐帧无条件回令** | 无论是否检出都会发一条 `"1"`/`"0"`，保证端侧 LED 状态与云端判断**每帧同步**，不会出现"人走了灯还亮"的悬挂态 |
| **`conn.send` 与主线程 `send` 分离** | Python 侧发令、C 侧发流，方向相反互不干扰 |

### 6.5 性能观测与可视化

```python
# 每处理 10 帧计算一次平均 FPS
if frame_count % 10 == 0:
    end_time = time.time()
    fps = 10 / (end_time - fps_start_time)
    fps_start_time = end_time

cv2.putText(annotated_frame, f"FPS: {fps:.2f}", (20, 50),
            cv2.FONT_HERSHEY_SIMPLEX, 1, (0, 255, 0), 2)
cv2.imshow("EdgeVision - Real-time Video", annotated_frame)

if cv2.waitKey(1) & 0xFF == ord('q'):
    break
```

| 设计点 | 说明 |
| --- | --- |
| FPS 由**云端实测** | 与端侧自报的推流帧率对比，即可判定瓶颈在网络/云端还是采集端 |
| `waitKey(1)` 不可省略 | OpenCV 的 GUI 事件循环靠 `waitKey` 驱动；不调用则窗口不刷新（假死） |
| `& 0xFF == ord('q')` | `waitKey` 返回值含高位修饰符，需掩码后再比较；`q` 键退出 |
| `destroyAllWindows()` 在 `finally` | 异常退出也保证窗口释放，不留僵尸窗口 |

### 6.6 统一资源释放

```python
try:
    while True: ...
except Exception as e:
    print(f"云端通信发生错误: {e}")
finally:
    conn.close(); server.close(); cv2.destroyAllWindows()
```

无论正常退出、按 `q`、还是异常，都走 `finally` 释放连接、监听套接字与窗口，保证端口可被立刻重用。

---

## 7. 项目验收成果

### 7.1 验收结论

| 验证项 | 结果 | 说明 |
| --- | --- | --- |
| **实时性** | ✅ 端侧视频流稳定 **25~30 FPS** | 4 缓冲环形队列与发送重叠，采集不再被传输阻塞 |
| **协议正确性** | ✅ 长时连续推流无粘包/错帧 | 长度前缀逐帧对齐；云端每帧 `imdecode` 均成功 |
| **检测准确性** | ✅ 稳定检出 `person`，画面实时绘制边界框 | `yolov8n` COCO 预训练权重，阈值 0.5 |
| **闭环控制** | ✅ 云端识别 → 端侧亮灯，全程低延迟 | sysfs 直写 + 独立子线程，全程双向无死锁 |
| **稳定性** | ✅ 长时运行主线程不卡顿 | 收发分离，阻塞式 `recv` 不占用采集线程 |

### 7.2 实测样例

**① 板端原始帧**（`board_photo.jpg`，1920×1080，V4L2 直出 MJPEG）

![板端原始帧](board_photo.jpg)

**② 云端 YOLOv8n 检测结果**（`ai_result.jpg`，1920×1080，标注框 + 类别 + 置信度）

![YOLOv8n 检测结果](ai_result.jpg)

> 📌 **说明**：V2.0 的实时画面由 `cv2.imshow` 呈现，代码本身不保存任何帧到磁盘，因此上两张图是链路的**代表性单帧样例**（与 V1.0 目录下的同名文件内容一致）。

### 7.3 端到端延迟构成

```text
┌──────────┬──────────┬──────────┬───────────┬──────────┬──────────┐
│ V4L2 采集│ 网络传输 │ 内存解码 │ YOLO 推理 │ 指令回传 │ LED 响应 │
│  ~33ms   │  ~100ms  │   <5ms   │  ~12ms    │  <1ms    │   <1ms   │
└──────────┴──────────┴──────────┴───────────┴──────────┴──────────┘
   └─ 受 30FPS 间隔下限约束 ─┘  └─ 全链路主要成本项 ─┘
```

- **最大单项成本是网络传输**（190KB × 30FPS ≈ **45 Mbps**），而不是推理（12ms）。
- 采集段 ~33ms 是 30FPS 的**帧间隔本身**，不是处理耗时。
- 因此 V3.0 的优化重点应是**压缩体积/降分辨率**，而非继续优化模型。

---

## 8. 已知问题与 V3.0 演进方向

### 8.1 代码现状问题清单

> 下列条目均按 `camera2.c` / `test.py` 实际代码核对得到，**不含未实现的声明**。

| # | 问题 | 位置 | 影响 | 建议 |
| --- | --- | --- | --- | --- |
| P1 | **缓冲映射长度被覆盖**：4 块 buffer 用同一个 `struct v4l2_buffer buf` 循环 `QUERYBUF`，`mmap` 时用的是**每次刚查到的 `buf.length`**（正确），但末尾 `munmap` 循环用的是最后一次残留的 `buf.length` | `camera2.c` 78-87, 140 | 各块长度差异时 `munmap` 未配平；此处各块等长故未暴露 | 用数组 `buf_length[BUFFER_COUNT]` 逐块记录 |
| P2 | **`send()` 返回值未校验** | `camera2.c` 119, 121 | 大帧可能只发了一部分却当成成功，云端随后收到残帧 | 封装 `send_all()` 循环至发完 |
| P3 | **`connect` 失败仍继续执行**：`connect` 失败只 `perror`，随后**无条件**打印 `connect successfully!` 并继续 `pthread_create` 与 `while(1)` | `camera2.c` 102-105 | 网线未插时端侧会以"看似成功"的状态空转，误导排查 | 失败即 `return`，或加重试 |
| P4 | **云端断连处理不完整**：正文 `while` 内 `break` 后仍用残缺 `img_data` 解码并继续下一轮，流位置已错乱 | `test.py` 45-52 | 板子掉线后云端可能持续报错或空转 | 断连直接 `raise`/`break` 到最外层 |
| P5 | **`img_data += packet` 为 O(n²) 拼接** | `test.py` 48 | 大帧时额外内存拷贝开销 | 改 `bytearray` / `recv_into` |
| P6 | `accept()` 只服务一次，云端退出后需重启 | `test.py` 19 | 每次联调都要重启云端 | 外层加 `while(1)` 接受重连 |
| P7 | IP / 端口硬编码（`192.168.1.100:8080`） | 两端 | 换网段需重新交叉编译 | 改命令行参数或配置文件 |
| P8 | 除 `VIDIOC_S_FMT` 外所有 `ioctl` 返回值未检查 | `camera2.c` | 驱动异常时难以定位 | 封装 `xioctl()` 统一判错 |
| P9 | LED 路径写死为 `sys-led` | `camera2.c` 18 | 换板子需改代码 | 宏或配置化 |

**关于早期草案中的两处描述，以本版为准：**

- ❌ "注册了 `signal(SIGPIPE, SIG_IGN)`" —— 代码中**没有**该调用（已全文件检索确认）。
  目前不发令时能正常工作，是因为**子线程**在 `recv` 返回 `<=0` 后退出、主线程 `send` 才失败；一旦云端先断开，主线程 `send` 会触发 `SIGPIPE`，默认行为是**直接终止进程**。**这是 V3.0 必须补的第一条。**
- ❌ "利用 PC 端 GPU 加速推理" —— `test.py` 中**未指定** `device`，`model(frame)` 走 Ultralytics 默认设备选择。是否实际用到 GPU 取决于本机 `torch` 的 CUDA 构建，文档不应据此断言。若要确保 GPU，应显式写 `model(frame, device=0)`。

### 8.2 V3.0 演进路线

```text
V2.0 (当前)                 V3.0                            V3.5
─────────────               ─────────────────────          ─────────────────
单文件 C                   多文件工程 + Makefile          配置化/模块化
无信号保护                  signal(SIGPIPE, SIG_IGN)       完整错误恢复
1080P @25-30FPS          + 720P 降级策略                自适应码率
逐帧推理                   抽帧推理 (skip N)              动态抽帧
全屏接收无缓冲              接收缓冲改 bytearray           ——
按连接服务一次              accept 循环 + 重连             心跳保活
```

| 优先级 | 任务 | 目标收益 |
| --- | --- | --- |
| **P0** | 补 `signal(SIGPIPE, SIG_IGN)`，并校验 `send`/`connect` 返回值 | 消除进程被信号强杀，修复 §8.1 P1~P3 |
| **P0** | 修复云端断连路径（§8.1 P4），`accept` 改为循环 | 掉线可恢复，无需重启 |
| **P1** | **解耦底层通信**：C 代码拆为 `main.c / v4l2.c / net.c / led.c / protocol.h`，引入 `Makefile` | 可维护性，面试可讲工程能力 |
| **P1** | **降分辨率至 720P**：采集端用 `VIDIOC_S_FMT` 协商 1280×720，实测单帧约 80KB | 带宽降 ~58%，低带宽下极限帧率提升 |
| **P1** | **抽帧推理**：云端每 N 帧推理一次，其余帧直接透传显示 | 大幅降低 GPU/CPU 负载，画面仍流畅 |
| **P2** | 协议升级：加入 1 字节版本 + 2 字节校验，支持重传 | 抗干扰，弱网可用 |
| **P2** | 云端配置化：`argparse` 接 `--host --port --conf --imgsz --device` | 免改代码即可切换环境 |
| **P3** | 端侧 GUI（Qt / LVGL）显示推流状态与 LED 状态 | 产品化演示 |

---

## 9. 工程结构

```text
v2.0/
├── camera2.c                        端侧：V4L2 4缓冲采集 + 帧协议发送 + 收令控灯（C）
├── test.py                          云端：长度协议收帧 + 内存解码 + YOLOv8 推理 + 回令（Python）
├── yolov8n.pt                       YOLOv8 Nano COCO 预训练权重（约 6.2MB，与 v1.0 相同）
├── board_photo.jpg                  链路的代表性单帧样例（1920×1080）
├── ai_result.jpg                    云端检测结果样例（1920×1080）
└── 📄 EdgeVision-AI 开发文档 V2.0.md  本文档
```

**模块边界（V3.0 拆解目标）**

| 职责 | 对应现有代码段 | 目标文件 |
| --- | --- | --- |
| 采集初始化 | `open` / `S_FMT` / `REQBUFS` / `mmap` / `STREAMON` | `v4l2.c` |
| 取帧与归还 | `while(1)` 内 `DQBUF` / `QBUF` | `v4l2.c` |
| 帧协议 | `htonl` + 两次 `send` | `protocol.h` / `net.c` |
| 双工与 LED | `receive_cmd()` | `led.c` / `main.c` |
| 主循环编排 | `main()` | `main.c` |

---

## 10. 复现步骤（联调手册）

```bash
# ── 0. 网络：PC 有线网卡与板子同网段
#    PC  : 192.168.1.100 / 255.255.255.0
#    板子: 192.168.1.21  / 255.255.255.0
ping 192.168.1.21        # PC → 板子，通即可

# ── 1. 云端先启动（顺序不能反，端侧 connect 不会重试）
python test.py
#    【云端】服务器已启动，正在监听 8080 端口，等待板子连接...

# ── 2. 端侧运行（需带 -lpthread 编译）
./camera2
#    格式设置成功！分辨率: 1920 x 1080
#    connect successfully!
#    当前实时帧率: 27 帧/秒
#    the 1 image sent done! size: 19xxxx bytes

# ── 3. 云端显示实时窗口
#    第 12 帧: 发现人！置信度 0.87
#    窗口标题: EdgeVision - Real-time Video（左上角叠加 FPS）
#    按 q 退出

# ── 4. 闭环验证：人入镜 → 板载 sys-led 亮；人离开 → 熄
```

**排查速查表**

| 现象 | 可能原因 | 处理 |
| --- | --- | --- |
| `打开摄像头失败` | 设备号不是 video2 / 未插摄像头 | `ls /dev/video*`，改 `open()` 路径 |
| `设置 MJPG 格式失败` | 该分辨率不支持 MJPG | 看打印的实际宽高，换分辨率 |
| `connect failed` 但仍打印 `connect successfully!` | 已知缺陷 P3 | 本版代码行为；先确认网段与云端已启动 |
| 云端 `imdecode` 返回 None / 画面黑 | 帧数据残缺（P2 发送未保证完整） | 检查网络质量；升级 `send_all()` |
| 端侧帧率低（<10） | 网络拥塞 / 云端推理慢反压 | 降分辨率到 720P，或开抽帧推理 |
| 云端窗口卡住不刷新 | 缺 `cv2.waitKey(1)` | 本版已包含；检查是否被改动 |
| 按 `q` 后板子仍在推流 | 云端退出触发端侧 `SIGPIPE` | 预期行为；V3.0 加 `SIG_IGN` 后可优雅退出 |
| LED 无反应 | `sys-led` 路径不符 / 权限不足 | `ls /sys/class/leds/` 确认节点名 |
| `Address already in use` | 上游端口未释放 | 已加 `SO_REUSEADDR`；仍冲突则换端口 |

---

## 附录 A：端云协议规格表

| 字段 | 长度 | 类型 | 字节序 | 说明 |
| --- | --- | --- | --- | --- |
| Length | 4 B | uint32 | 大端（网络序） | 紧随其后的 payload 字节数，典型 150000~250000 |
| Payload | Length B | binary | — | 完整一帧 MJPEG（即 `.jpg` 内容） |
| Command | 1 B | ASCII | — | 反向：`"1"` 亮灯 / `"0"` 熄灯 |

**方向约定**

- 端 → 云：仅 `Length + Payload`，连续不断。
- 云 → 端：每帧一条 1 字节指令，与帧**没有强制配对**（异步），依靠"每帧必发"保证最终一致。

---

## 附录 B：V1.0 与 V2.0 代码差异速览

| 位置 | V1.0 | V2.0 |
| --- | --- | --- |
| 文件 | `camera_test.c` / `test.py` | `camera2.c` / `test.py` |
| 缓冲数 | `req.count = 1` | `req.count = BUFFER_COUNT(4)` |
| 采集循环 | 单次 `DQBUF`，取完即退出 | `while(1)` 循环 `DQBUF/QBUF` |
| 帧边界 | 连接关闭 | 4 字节长度头 |
| 子线程 | 无 | `receive_cmd()`，`pthread_detach` |
| LED | 无 | sysfs `sys-led/brightness` |
| 云端输入 | `model('board_photo.jpg')`（路径） | `model(frame)`（ndarray） |
| 落盘 | `board_photo.jpg` + `ai_result.jpg` | 不落盘 |
| 显示 | `results[0].save()` | `imshow` + `putText` FPS |
| 帧率统计 | 无 | 端侧每秒 / 云端每 10 帧 |

---

*(文档版本：V2.0 更新版 ｜ 更新于 2026 年 11 月 ｜ 本版已按 `camera2.c`、`test.py` 实际实现逐行校验校正)*