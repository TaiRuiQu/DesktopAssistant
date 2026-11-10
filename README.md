# desktop assistant

使用 **C++20 / C++/WinRT + WinUI 3 + Windows App SDK** 开发的原生 Windows 桌面助手。应用使用一个 WinUI 3 窗口，在问答与设置页面之间切换；提问、语音识别状态和回答显示在同一个界面。应用不使用 C#、.NET 或 WebView。

双击快捷键后，应用获取当前各显示器截图与刚刚播放的桌面声音，再录下你的问题，转写后请求配置的模型。问题卡片从可编辑切换为只读，答案在下方流式显示；完整成功的问答用于后续追问。

**构建与运行**

目标系统：Windows 10 1809 或更新版本，推荐 Windows 11。开发环境：Visual Studio 2022 / 2026 或对应 Build Tools，安装“使用 C++ 的桌面开发”和 Windows 10/11 SDK。构建通过 MSBuild / NuGet 恢复固定版本的 `Microsoft.WindowsAppSDK 2.6.5` 和 `Microsoft.Windows.CppWinRT 2.0.250303.1`，生成 C++/WinRT 投影。XAML 嵌入 EXE，由 C++/WinRT 创建和绑定 WinUI 控件。无需 .NET SDK 或 C# 工程。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1
```

发布结果：

```text
artifacts/desktop-assistant/win-x64/DesktopAssistant.exe
```

复制整个 `win-x64` 文件夹，保留其中的 Windows App SDK DLL、PRI 资源和语言文件。WinUI 3 以自包含目录版分发，不是单 EXE。应用的 C++ 标准库和运行库静态链接，UCRT 使用系统组件；无需安装 .NET。默认仅显示设置窗口，保存后在托盘待命。双击托盘图标打开设置，右击托盘图标可提问、显示悬浮窗、新建对话、暂停或退出。

之前的 Win32 / WinUI 版本如果仍在运行，请先从其托盘菜单退出，再打开新版；两个版本使用同一个单实例标识。构建脚本先生成新版本，再通过暂存目录替换整个发布目录，防止旧运行时 DLL 混入。正在运行的旧版本可能阻止旧备份清理，此时脚本会给出备份位置。

需要保留正在运行的旧版本时，可以使用 `scripts/build.ps1 -Separate`，将新版另存为 `artifacts/desktop-assistant-winui/win-x64/DesktopAssistant.exe`；启动新版前仍需先退出旧版。

ARM64 交叉构建需要对应的 MSVC ARM64 工具：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1 -Runtime win-arm64
```

ARM64 未在本项目当前 x64 硬件上运行验证。也可以直接用 Visual Studio 打开 `DesktopAssistant.sln`，选择 x64 或 ARM64。Release 使用 C++ 原生工具链，Windows App SDK 运行文件随目录携带。

**配置**

设置页分为“模型接口”“语音识别”“唤起与历史”“高级设置”“语音合成”五个标签。

- 填写 API 根地址、API Key、支持图片的回答模型，选择 Responses 或 Chat Completions。根地址通常保留 `/v1`，不要填写完整的请求路径。默认模型为 `gpt-4.1-mini`。
- 远程服务必须使用 HTTPS。本机兼容服务可使用 `http://localhost:端口/v1` 或 `http://127.0.0.1:端口/v1`，不需要认证时可留空密钥。
- 背景声音和麦克风指令可以分别选择 Windows 本机或云端 API。默认背景声音使用 Windows，麦克风使用云端，默认云端转写模型为可编辑的 `gpt-transcribe`。实际模型名称与权限以所用服务为准。
- Windows 识别通过原生 SAPI COM 接口运行，需安装对应语言的语音识别组件。仅有输入法或显示语言不一定足够。缺少识别器时会提示，不会自动换成其他语言。
- “语音唤醒”默认关闭。在“语音识别”页开启后，可自定义唤醒词（默认“小弟小弟”）；说出唤醒词，等提问窗口打开后再说问题，无需双击 Shift。唤醒使用所选语言的 Windows SAPI 本机命令语法与语音活动检测，需安装对应语音组件；待命时持续使用麦克风，不上传待命声音。设置页、提问、朗读和暂停期间停止检测，结束后恢复。快捷键仍可使用。
- “提问时自动压低其他应用音量”默认开启：录制与识别用户问题期间，将其他应用的输出会话降至原音量的 20%，识别结束、取消或失败后恢复。覆盖多个输出设备和识别期间新出现的会话；保留期间手动修改的应用音量，不改系统总音量或麦克风音量。此项可在“语音识别”页关闭。
- 回答和 ASR 分别配置根地址、API Key 和协议。ASR 地址、密钥都留空时沿用回答接口，兼容旧配置；填写独立 ASR 地址后，只发送 ASR 密钥，不会带上回答密钥。背景声音和麦克风共用 ASR 接口，但可分别填写识别模型。
- ASR 支持 `Audio Transcriptions`（multipart WAV）、`Chat Completions`（`input_audio` 音频输入），以及 `DashScope Qwen-ASR`（音频 Data URI 和 `asr_options`）。Windows 本机识别无需云端接口。
- TTS 使用 DashScope Qwen-TTS，独立配置根地址、密钥、模型和音色，默认模型 `qwen3-tts-flash`、音色 `Cherry`。点击回答窗的“朗读”才会发送文字；可选中部分回答后只朗读选中内容。长回答按句分段合成和播放，点击“停止”、关闭回答窗或开始新问题都会取消合成与播放。音频只保留在内存中。

例如，回答可配置 DeepSeek 的根地址 `https://api.deepseek.com`，选择 Chat Completions 并填写模型名称。对于只接受文字的模型，关闭“发送屏幕截图给回答模型”；该模式不截屏，也不发送图片，仍发送问题、声音转写和文字历史。ASR 可选择以下任一方案：

| ASR 服务 | 根地址 | 识别接口格式 | 模型示例 |
| --- | --- | --- | --- |
| 兼容音频转写的服务 | 服务商提供的 API 根地址 | Audio Transcriptions | 服务商支持的转写模型 ID |
| OpenRouter 音频对话 | `https://openrouter.ai/api/v1` | Chat Completions（音频输入） | 填写支持音频输入的模型 ID |
| DashScope 北京 | `https://dashscope.aliyuncs.com/compatible-mode/v1` | DashScope Qwen-ASR | `qwen3-asr-flash` |
| DashScope 新加坡 | `https://dashscope-intl.aliyuncs.com/compatible-mode/v1` | DashScope Qwen-ASR | `qwen3-asr-flash` |

DashScope TTS 北京根地址为 `https://dashscope.aliyuncs.com/api/v1`，新加坡为 `https://dashscope-intl.aliyuncs.com/api/v1`；也可使用阿里百炼提供的业务空间专属域名，保留相应路径。ASR 和 TTS 的路径不同，API Key 需与地域一致。这些模型名称是配置示例，可用性取决于服务账户及提供方。

| 设置 | 默认值及范围 |
| --- | --- |
| 双击启用键 | 右 Shift；可选左 Shift、左右 Ctrl、右 Alt、F8、F9 |
| 双击间隔 | 400 毫秒；150–1000 毫秒 |
| 桌面音频回看 | 5 秒；0–60 秒，0 或关闭采集时停止缓冲 |
| Windows 语音语言 | zh-CN，使用已安装的识别器 |
| 语音唤醒 | 默认关闭；唤醒词“小弟小弟”，最多 64 字符 |
| 说完后等待 | 4 秒；0.5–15 秒 |
| 麦克风检测阈值 | 0.015；0.001–0.2 |
| 最长提问录音 | 60 秒；5–120 秒 |
| 截图最长边 | 每个屏幕 2560 像素；640–7680 像素，等比缩小 |
| 携带历史 | 最近 10 轮；0–100 轮 |
| 保存本地对话 | 默认开启；仅保存文字，不保存截图或录音 |
| 启动时隐藏到托盘 | 默认关闭；与开机自启无关 |

**使用**

- 双击右 Shift，或选择托盘菜单“截图并提问”，获取屏幕与桌面声音后打开麦克风；说话结束并等待配置的静音时间后自动提交。
- 录音时再次双击快捷键、点击“说完了”或“发送”，可手动结束录音。20 秒没有检测到持续说话会结束等待（仍受最长录音时长限制）。录音以 20 毫秒帧检测，连续约 120 毫秒的声音才算开口，续说采用更低的音量阈值，减少轻声或短暂停顿造成的提前结束。思考时间较长时可把静音等待调至 8–15 秒。
- 托盘菜单“显示悬浮窗”打开统一问答界面，输入后点击发送或按 Ctrl+Enter。回答完成后点击“追问”在同一问题卡片输入下一条问题。文字提问不打开麦克风，也不压低其他应用音量。
- 录音或转写期间可以修改问题，最终优先使用用户编辑的文字，迟到的转写不会覆盖输入。
- 未识别到问题时可以输入文字，继续使用本次已取得的截图。网络失败时可修改问题后重试。
- “取消”中断录音、转写或模型请求。失败、取消、长度截断或中途断开的回答不会加入历史。
- 问答页 Esc 或右上角关闭按钮取消当前操作并收起窗口，应用继续托盘待命。打开设置时在同一窗口切换页面；正在进行的录音或请求会取消。设置页“关闭”、Esc、标题栏关闭按钮或“保存并关闭”都会收起到托盘，保留当前问题与回答，不会自动打开提问窗；“关闭”和 Esc 不保存未提交的设置。设置页显示时唤起语音或文字提问会正常切回问答。
- 问答页采用精简的 WinUI 3 原生布局：系统 Mica 背景、主题色与强调色按钮、问题输入和可滚动的回答卡片，支持系统浅色与深色主题；底部保留复制、朗读和追问。点击“语音”也可开始录音。使用 MD4C 0.5.2 解析 CommonMark 与 GFM 表格、任务列表和删除线，原生 WinUI 控件显示标题、嵌套列表、引用、链接、代码块和表格。表格可横向滚动，代码块保留原始字符；中文正文显式使用 Microsoft YaHei UI，正文保持正常字重，加粗仅应用于 Markdown 强调和标题。
- 流式更新保留已完成段落，选中文字时暂缓刷新，释放选择后显示最新内容。复制回答与朗读使用解析后的文本，包含表格内容。
- WinUI 窗口支持顶部拖动、调整大小、DPI 缩放和 Windows 11 系统圆角；问题与回答均可复制。正常退出时记住位置并在恢复时限制在可用显示器内。
- “新对话”清空当前上下文及加密存档。“暂停采集与快捷键”停止音频缓冲、移除键盘 hook 并取消当前操作。托盘菜单“退出”结束应用。

**实现与数据**

| 功能 | 原生实现 |
| --- | --- |
| 窗口与控件 | C++/WinRT、WinUI 3 Window / AppWindow、TextBox、Button、NavigationView、NumberBox、ToggleSwitch |
| 设置与布局 | XAML、自动布局、ScrollViewer、系统 Fluent 控件样式 |
| 托盘与快捷键 | Shell_NotifyIcon、WH_KEYBOARD_LL；按键继续传递给其他应用 |
| 截图 | 多显示器 GDI BitBlt + WIC 内存 JPEG 编码 |
| 桌面音频 / 麦克风 | WASAPI shared mode，输出设备 loopback / 默认输入设备 |
| 本机语音识别 | SAPI COM，内存 PCM 音频流 |
| 模型与云端转写 | WinHTTP、HTTPS、SSE、multipart；不跟随 HTTP 重定向 |
| 配置与对话 | Windows.Data.Json、CryptProtectData / CryptUnprotectData |
| 取消与后台任务 | C++20 jthread / stop_token；取消时关闭 WinHTTP 请求 |

桌面声音仅来自当前默认多媒体输出设备，不包含麦克风。默认输出切换时重建缓冲；设备不可用时提示并重试。PCM 为 16kHz、16 位、单声道，原始数据约 32KB/秒，默认 5 秒约 160KB。静音按实际经过时间让旧声音过期。待命时不持续截图、不调用模型，也没有持续动画计时器。仅开启语音唤醒时持续使用麦克风进行本机检测；关闭语音唤醒后，待命不打开麦克风。

截图按显示器分别获取，支持负坐标副屏，截图前隐藏助手的悬浮窗。只能获取当前屏幕像素；受保护视频、安全桌面或锁屏可能无法获取。声音先转写为文字，问答模型不直接分析原始声音，音乐、重叠说话等可能无法可靠识别。

每次问答将当前截图、问题、桌面声音转写及选定的文字历史发送到回答接口。旧截图不再次上传。Windows 模式在本机处理该路声音；云端模式将该路 WAV 上传到识别接口。朗读文字发送到独立的 DashScope TTS 接口，签名音频下载请求不携带 API Key。兼容服务需要正确处理 `store=false`；该字段不能保证第三方服务的留存政策。

本地数据位置保持兼容：`%LOCALAPPDATA%\DesktopAssistant\`。

- `settings.json`：沿用 Version 1 格式，回答、ASR、TTS 三个 API Key 分别由当前 Windows 用户的 DPAPI 加密；缺少新增字段的旧配置默认关闭语音唤醒。旧版 1.5 秒默认静音等待升级为 4 秒，自定义值继续保留；新版保存后明确选择的 1.5 秒也会保留。
- `history.dat`：原有 DPAPI 加密的文字历史，最多最近 100 轮。
- `windows.json`：统一问答窗口的坐标与尺寸；旧版回答窗口位置可自动迁移。设置页面的临时窗口尺寸不会覆盖问答布局。

新版读写沿用原来的字段和加密格式，无需手动迁移配置；损坏的设置或历史读取失败时保留原文件。DPAPI 数据绑定当前 Windows 用户，不支持直接跨账户复制。截图和录音仅在内存中使用。

**验证**

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/check.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/check.ps1 -Hardware
```

检查脚本编译完整 C++ 解决方案，验证协议、Markdown 表格及嵌套列表、设置及历史加密、时间线缓冲、快捷键判定、唤醒词匹配与配置迁移、录音停顿和轻声续说、SAPI、设置关闭不唤起问答、单窗口页面切换、原生控件与各设置标签页布局。还使用 Windows PowerShell 的系统 DPAPI 接口生成固定测试存档，验证原有 .NET 格式和 C++ 格式能够双向读取。`-Hardware` 额外截图到内存，启动并停止真实 WASAPI 输出采集与麦克风、初始化中文唤醒词命令语法并检查取消释放，验证 WAV 播放可及时取消，并启动独立的静音音频进程验证音量压低、恢复、新会话检测、自身会话排除和手动改音量保留。

如果安装了 Python 3，还会启动仅绑定 127.0.0.1 的模拟 HTTP 服务，测试真实 WinHTTP 请求、multipart、OpenRouter 音频对话、DashScope ASR/TTS、签名音频下载、独立密钥路由、流式断开、重定向拒绝、密钥隐藏、阻塞读取取消，并通过生产程序的窗口消息循环测试完整文字问答和新对话流程。使用固定测试数据，不加载用户配置、不访问外部模型、不需要 API Key。

原生 UI 报告与 PNG 预览保存在 `artifacts/ui/`，使用 WinUI 的 RenderTargetBitmap 捕获示例界面。可以直接运行：

```powershell
artifacts/winui/x64/Release/DesktopAssistant.exe --ui-checks --outputdir artifacts/ui
```

这些检查不等同于真实账户的云端问答或云端语音识别；各音频设备、识别语言、兼容网关仍需在目标环境验证。

Markdown 解析器源码与许可保存在 `src/DesktopAssistant/ThirdParty/md4c/`，发布目录附带 `licenses/md4c.txt`。第三方源码不参与项目的 clang-format 检查。

**代码格式**

使用固定版本的 **clang-format 18.1.8**，规则位于 `.clang-format`，编辑器基础设置位于 `.editorconfig`。C++ 使用 4 空格缩进、100 列和展开的大括号，控制流与函数体保持多行。仅处理 `src/`、`tests/` 中手写的 C++ 文件，跳过构建输出和生成的 C++/WinRT 投影。

```powershell
# 如本机尚未安装 LLVM / clang-format：
python -m pip install clang-format==18.1.8

# 整理所有 C++ 源文件
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/format.ps1

# 只检查，不改文件；scripts/check.ps1 也会先运行此项
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/format.ps1 -Check
```

可通过 `-ClangFormat C:\路径\clang-format.exe` 指定工具位置。格式检查失败会返回非零状态。Python 仅用于可选开发工具安装和本机 HTTP 测试夹具，不是应用运行依赖。
