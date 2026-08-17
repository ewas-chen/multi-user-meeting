# multi-user-meeting
Multi-person real-time audio and video meeting engine
# VCE：多人实时音视频会议引擎

VCE（Video Conference Engine）是一个基于 C++20 实现的桌面端实时音视频会议引擎，覆盖摄像头/麦克风采集、音视频编解码、WebRTC 推拉流、弱网处理、多用户渲染播放与音画同步。项目通过 WHIP/WHEP 对接 SRS SFU，并将媒体能力封装在独立引擎层中，UI 与媒体链路相互解耦。

## 核心能力

- **采集：** 基于 OBS/libobs 采集摄像头和麦克风，统一输出 I420 视频帧与 Float32 交错 PCM 音频帧。
- **传输：** 基于 libdatachannel 实现 WebRTC Push/Pull，使用 WHIP/WHEP 完成 SDP 协商及会话资源管理。
- **编解码：** OpenH264 Baseline Profile + Opus 20 ms 帧，兼容标准 WebRTC 浏览器媒体格式。
- **弱网处理：** RTP 帧级乱序重排、RTCP NACK/PLI、关键帧恢复及基于发送队列压力的自适应码率控制。
- **渲染播放：** OpenGL 3.3/GLSL 完成 BT.709 Full Range I420 到 RGB 的 GPU 转换，支持本地镜像和多用户视频渲染。
- **音频与同步：** 多用户 PCM 混音、自适应抖动缓冲、RTCP SR 时间映射，以及基于音频播放时钟的音画同步。


## 模块设计

### 1. Capture：音视频采集

`capture/` 对 OBS 采集能力进行模块化封装：

- 使用 `ISource`、`ISourceProperty` 抽象采集源及其属性，并派生摄像头、麦克风实现。
- 通过 OBS Raw Video/Audio Callback 接收原始帧，输出 I420 与 Float32 PCM。
- 支持设备枚举、采集格式配置、分辨率选择和运行时设备切换。
- 使用 `shared_ptr/weak_ptr` 管理 Source 与 Property 的关联关系，避免循环引用。
- 用户回调在释放内部互斥锁后执行，降低采集线程阻塞和死锁风险。

当前实现使用 OBS 的 V4L2 摄像头输入和 PulseAudio 麦克风输入，并启用 OBS GPU Shader 色彩转换。

### 2. Transport：编解码与 WebRTC 传输

`transport/` 负责本地媒体发布和远端媒体订阅：

- 基于 libdatachannel 构建独立的 Push/Pull Transport，并通过工作线程隔离采集回调与编解码、网络发送过程。
- 通过 WHIP/WHEP 发送 Offer SDP、接收 Answer SDP，并根据 `Location` 管理发布/订阅资源生命周期。
- 视频使用 OpenH264 Baseline Profile，SDP 配置为 `profile-level-id=42e01f`、`packetization-mode=1`。
- 音频使用 48 kHz Opus，默认 20 ms 帧，支持 VBR、带内 FEC、PLC 与采样率转换。
- 发送端配置 RTCP Sender Report、NACK Responder 与 PLI Handler；收到 PLI 后驱动编码器生成 IDR 帧。

弱网处理包含以下策略：

- **视频乱序重排：** 按 RTP 时间戳聚合同一视频帧，结合扩展序列号处理回绕、重复包和迟到包；超时或缓存溢出时丢弃不完整帧并请求关键帧。
- **自适应码率：** 根据编码发送队列深度、队列丢帧和发送失败进行快速降码率，在链路持续稳定后逐步恢复。
- **实时性优先：** 编解码队列达到上限时丢弃旧帧，避免队列持续累积导致延迟不可控。
- **公共时间线：** 使用 RTCP SR 将 90 kHz 视频和 48 kHz 音频 RTP 时间戳映射到 NTP 时间线；SR 不可用时采用连续的本地降级时间线。

### 3. Render：视频渲染、音频播放与同步

`render/` 为每个参会用户维护独立的音视频状态：

- 使用 OpenGL 3.3 三平面纹理和 GLSL Shader 在 GPU 端完成 BT.709 Full Range I420 到 RGB 转换。
- 本地预览始终选择最新帧并启用水平镜像；远端视频按时间戳缓冲，通过等待、渲染或丢帧控制播放节奏。
- 音频按用户维护时间戳 PCM 缓冲，以 10 ms 为粒度在独立线程完成多路 Float32 PCM 混音。
- 对混音结果执行峰值归一化，并过滤 NaN/Inf，避免多路叠加造成削波或异常噪音。
- 自适应抖动控制器根据媒体间隔与本地到达间隔估算网络抖动，在欠载时快速扩容、稳定播放后缓慢缩容。
- 以实际提交给声卡的音频播放时钟为主时钟，视频超前时等待、落后时丢帧，并在重连或时间戳跳变后重建同步基准。

## 阶段性测试结果

以下数据来自当前开发测试环境，实际表现会受到硬件、分辨率、编码参数及网络状况影响：

| 指标 | 测试结果 |
| --- | --- |
| 端到端音视频延迟 | 稳定在 200 ms 以内 |
| GPU I420 → RGB 转换耗时 | 低于 1 ms |
| 本地预览延迟 | 约 1 帧 |
| 公网远端视频 | 连续播放，无明显跳帧 |
| 多路音频混音 | 4 路并发混音无削波 |
| 浏览器兼容性 | H.264/Opus 码流可由标准 WebRTC 浏览器解码 |

## 工程结构

```text
meeting/
├── capture/                  # OBS 摄像头、麦克风采集与设备属性管理
├── transport/                # WebRTC、WHIP/WHEP、RTP/RTCP 与编解码
│   └── src/
│       ├── codec/            # OpenH264、Opus 编解码器封装
│       └── transport/        # Push/Pull、乱序重排、时钟映射与码率控制
├── render/                   # OpenGL 视频渲染、音频混音与音画同步
│   └── src/
│       ├── audio/            # PCM 缓冲、Mixer、Jitter、Playback Clock
│       ├── core/             # RenderEngine、AVSyncController
│       └── video/            # UserContext 与 OpenGL 渲染资源
├── VideoConferenceEngine/    # 对外门面接口及各媒体模块包装层
├── VideoConferenceClient/    # Qt 会议客户端
├── VideoConferenceServer/    # 本地联调用内存态 gRPC 测试服务
├── test/                     # 采集、编解码、传输、渲染和混音测试
├── config/                   # 客户端及端到端媒体测试配置
└── CMakeLists.txt
```

## 构建环境

当前采集实现主要面向 Linux x86-64，使用 V4L2、PulseAudio 和 OBS OpenGL 后端。

### 基础要求

- CMake 3.21+
- GCC 或 Clang，支持 C++20
- OpenGL 3.3 Core Profile
- Qt 6.5+（仅构建图形客户端时需要）
- pkg-config
- vcpkg 或等价的 C++ 依赖管理环境

### 主要依赖

| 类别 | 依赖 |
| --- | --- |
| 采集 | libobs、V4L2、PulseAudio |
| RTC | libdatachannel、libcurl、OpenSSL |
| 编解码 | OpenH264、Opus、libsamplerate |
| 渲染播放 | OpenGL、GLAD、GLFW、miniaudio |
| 业务接口 | Protobuf、gRPC |
| 公共组件 | fmt、spdlog、Threads |
| 可选 UI | Qt Core/Gui/Widgets/OpenGL/OpenGLWidgets |

## 构建与运行

`CMakePresets.json` 中保留了开发机的 Qt 与 vcpkg 路径，首次构建前需要按本机环境修改。也可以直接使用以下通用方式配置：

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DCMAKE_PREFIX_PATH="/path/to/Qt/6.5.x/gcc_64;/path/to/vcpkg/installed/x64-linux" \
  -DBUILD_QT_CLIENT=ON \
  -DBUILD_VCE_TEST_SERVER=ON \
  -DBUILD_VCE_TESTS=OFF

cmake --build build --parallel
```

### 客户端配置

客户端默认读取 `config/client.ini`，也可以通过 `--config` 指定其他配置文件。不要将真实公网地址或发布密钥提交到公共仓库。

```ini
[engine]
sample_rate=48000
channels=2
video_width=640
video_height=480
video_fps=30
camera_video_format=YUYV

[service]
server_address=127.0.0.1:50051
client_ip=<CLIENT_IPV4>
request_timeout_ms=5000
media_http_scheme=http
media_http_port=80
media_rtc_port=8000
whip_path=/rtc/v1/whip/
whep_path=/rtc/v1/whep/
app_name=live
publish_secret=<SRS_WHIP_SECRET>
```


## 测试

开启独立测试目标：

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DBUILD_QT_CLIENT=OFF \
  -DBUILD_VCE_TESTS=ON

cmake --build build --parallel
```

| 测试目标 | 覆盖内容 | 外部条件 |
| --- | --- | --- |
| `capture_test` | 摄像头/麦克风枚举、格式校验、连续采集与时间戳检查 | 摄像头、麦克风 |
| `codec_test` | H.264/Opus 编解码、码率与关键帧接口 | 原始媒体文件 |
| `render_test` | I420 渲染、PCM 播放与设备输出 | OpenGL、扬声器 |
| `audio_mixer_test` | 时间戳缓冲、混音、播放时钟与抖动策略 | 无网络依赖 |
| `rtc_transport_test` | WHIP/WHEP 推拉流与 RTP 媒体收发 | SRS、网络、原始媒体文件 |
| `vce_media_test` | 采集 → 传输 → 解码 → 渲染端到端链路 | 两端设备及 SRS |

例如：

```bash
./build/codec_test
./build/audio_mixer_test
./build/render_test \
  ./source/camera_640x480_i420.yuv \
  ./source/microphone_48000hz_2ch_f32le.pcm
```

端到端测试需要分别准备创建者与加入者配置，并使用不同的 `local_user_id`、设备 ID 和同一个 `room_id`：

```bash
./build/vce_media_test ./config/vce_media_creator.conf
./build/vce_media_test ./config/vce_media_joiner.conf
```

运行公网测试前，请先将配置中的服务地址、设备 ID 和密钥替换为当前环境的有效值。

## 对外接口

`VideoConferenceEngine/public/include/VceEngine.h` 提供统一门面，客户端无需直接依赖 OBS、OpenGL 或 libdatachannel 的内部对象。公开能力包括：

- 引擎初始化与资源释放；
- 用户注册、登录及会议创建/加入/退出；
- 摄像头、麦克风和扬声器枚举与切换；
- 本地音视频发布与远端用户订阅；
- 多用户渲染及会议事件观察者。


- 当前视频编码采用 OpenH264 软件编码；硬件编码和屏幕共享可在现有 Source/Codec 抽象上继续扩展。

