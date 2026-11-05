# 声桥

**简体中文** | [English](README.en.md)

把 Mac 用作蓝牙音频接收端，让 Nintendo Switch / Switch 2 等蓝牙音源通过 Mac 的扬声器播放声音。

项目起因：Switch 2 接到底座后，所用显示器没有喇叭，希望直接使用 Mac 播放游戏声音。当前版本 **1.0.7**，采用单音源接收、新设备自动接管的方式。

## AI 开发与维护说明

本项目由账号持有人提出需求，主要使用 AI（Codex）协助编写、修改、测试和维护。**账号持有人不具备编程知识，无法独立审查全部实现。** 公开源码是为了保存成果、分享实际使用经验，并方便其他开发者与 AI 继续改进。

这是个人使用项目，不承诺专业人工审计、持续维护或所有设备兼容。文档区分自动测试、模拟验证和真实设备体验；欢迎通过 Issues 报告问题，通过 Pull Requests 提交改进建议。公开查看、下载或 Fork 不授予本仓库写入权限，修改建议需由维护方决定是否合并。

## 使用前了解

**实验性项目，依赖 macOS 私有 Skywalk 蓝牙接口。** 主要在一台 M4 MacBook Pro 上开发和使用；源码声明的 macOS 14 构建目标不代表全版本兼容，也不保证其他芯片或未来系统更新能用。

接收期间由监督程序暂时接管 Mac 内置蓝牙控制器，可能影响蓝牙耳机、鼠标、键盘等系统蓝牙设备；停止接收后尝试恢复原状态。**不能同时把它当作完全不影响系统蓝牙的普通播放器。** 不修改 SIP、不安装内核补丁、不卸载系统蓝牙服务。编译和模拟测试不需要接管蓝牙，首次真实接收需要 macOS 蓝牙授权。

依赖中包含 BTstack 的非商用许可，详见下方“许可”；不要将整个仓库理解为统一 MIT 或无限制商用的项目。

## 当前功能

- 菜单栏开关音频接收，播放 SBC / A2DP 音频。
- 单音源播放：新设备连接自动替换旧音源，无需先在 Mac 手动断开。
- 音量、静音、设备备注，以及“允许新设备配对”开关；已信任设备可在关闭新配对时重认证。
- 本机控制命令可供已有 SSH / iOS 快捷指令使用；不自动开启 SSH 或配置凭据。

正常启动 App 默认不接收，用户明确开启后才接管控制器。主机需保持唤醒；本次发布不修改现有电脑上的接收或系统设置。

## 构建

需要 Apple Silicon Mac、Xcode Command Line Tools、CMake 3.20+、Python 3。**附带的 PortAudio 二进制最低系统标记为 macOS 26.0，因此使用该附带库的构建按 macOS 26+ 对待；更低系统需自行重建兼容音频库，且仍未经过验证。**仓库包含固定版本 BTstack 源码子集与 arm64 PortAudio 19.7.0 预编译库（该库是依赖，不是配对数据或本机运行文件）。

```sh
python3 build_app.py --output build/声桥.app
```

构建仅生成 App，不安装、不接管蓝牙。确认后退出旧版，将 App 复制到“应用程序”并打开。需要源码构建 PortAudio 时，可通过 `--portaudio-root` 指向自己的头文件及库目录。

## 测试与验证范围

```sh
./build/mixer-test
./build/receiver-policy-test
python3 pairing_test.py ./build
python3 supervisor_test.py ./build/session-supervisor
```

这些测试使用假传输、临时配对数据或模拟助手，不打开真实蓝牙。更多本机控制测试见 [技术说明](docs/technical-notes.md)。

使用者曾确认 Switch 2 实际连接和音频播放，以及另一台 Windows 设备的播放；原电脑更新系统后曾报告仍正常。新设备自动接管、关闭配对后的真实 NS2 重连及跨机器兼容不能仅凭模拟测试宣称全面通过。没有测得可保证的端到端延迟或最大设备覆盖范围。

## 远程控制

在已有同用户 SSH 连接中运行：

```sh
"/Applications/声桥.app/Contents/MacOS/btmixctl" status
"/Applications/声桥.app/Contents/MacOS/btmixctl" toggle
```

也支持 `on`、`off`。凭据由使用者自己的 SSH / 快捷指令管理，不放入仓库。控制接口为同用户 Unix socket，不新增 TCP 服务。

## 许可与来源

本项目新增的自有代码采用 [MIT](LICENSE)，**第三方代码及衍生文件仍遵循原许可**：

- [BTstack](https://github.com/bluekitchen/btstack)：固定提交 `51e11ce3692abfe31b4ee3d66cffdeb1ae1626f2`，[随附许可](BTSTACK-LICENSE.txt) 限定个人、非商用目的，商用需另行处理授权；sdp_server_trace.c 衍生自该版本。
- [PortAudio](https://www.portaudio.com/) 19.7.0：[版权与许可](PORTAUDIO-LICENSE.txt)，当前附带 arm64 动态库。
- SBC 编解码器及相关组件：[通知](SBC-NOTICES.txt)、[Apache 2.0 文本](APACHE-2.0.txt)，以及各源文件原有头部。
- 依赖固定版本见 [vendor/versions.json](vendor/versions.json)，其他第三方许可保留在源文件中。

分享包和仓库不包含真实配对密钥、设备信任名单、个人备注、日志、录音或私人快捷指令。问题反馈请先阅读 [参与说明](CONTRIBUTING.md)。
