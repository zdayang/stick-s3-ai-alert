# StickS3 AI Alert

这是一个把 M5Stack StickS3 做成 AI 编程提醒器的小项目。

设备可以通过 BLE 接收 Codex 和 Claude Code 的 hook 通知。平时黑屏省电；需要你介入时短暂闪屏和蜂鸣，然后自动黑屏。黑屏后按 `A` 可以查看上一条提醒。

[English README](README.md)

## 当前固件范围

这个仓库目前编译出来的是完整的 StickS3 mini launcher 固件，不是只包含 AI Alert 的单功能固件。

当前固件包含：

- `AI Alert`
- `Dodge`
- `Stone`
- `MineZ`
- `Settings`

`AI Alert` 是主功能，小游戏和设置界面作为当前 playground 风格固件的一部分保留。

## 功能

- 竖屏 StickS3 小启动器，包含多个 App。
- `AI Alert` App：接收 Codex / Claude Code 提醒。
- 使用 BLE GATT 接收消息，UUID 采用 Nordic UART 风格。
- 提醒协议：`TOOL|SESSION|STATE|HH:MM`。
- 可区分：
  - `CX`：Codex
  - `CC`：Claude Code
  - `ASK`：需要审批或介入
  - `DONE`：当前轮次或任务完成
- 屏幕显示 AI 工具、会话/项目标签、提醒类型、本地时间。
- 提醒时闪烁 3 次、蜂鸣 3 次，之后自动黑屏。
- 黑屏状态按 `A` 可查看上一条提醒。
- 项目里还保留了开发过程中做的小游戏和系统设置。

## 硬件

- M5Stack StickS3
- USB-C 数据线
- macOS 电脑，用于运行 Codex / Claude Code hooks 并通过 BLE 发提醒

固件使用 PlatformIO + Arduino 构建。

## 固件安装

安装 PlatformIO：

```sh
python3 -m pip install --user -U platformio
```

编译：

```sh
pio run
```

烧录：

```sh
pio run -t upload --upload-port /dev/cu.usbmodemXXXX
```

查看串口：

```sh
ls /dev/cu.*
```

如果烧录无法连接，让 StickS3 进入下载模式：连接 USB，长按侧边复位/电源键，直到内部绿色 LED 闪烁，然后重新执行烧录命令。

## macOS BLE Hook 配置

创建 Python 虚拟环境并安装 BLE 依赖：

```sh
python3 -m venv ~/.stick-s3-ai-alert-venv
~/.stick-s3-ai-alert-venv/bin/python -m pip install -U pip bleak
```

复制辅助脚本到固定位置：

```sh
mkdir -p ~/.stick-s3-ai-alert
cp scripts/stick_alert.py ~/.stick-s3-ai-alert/stick_alert.py
chmod +x ~/.stick-s3-ai-alert/stick_alert.py
```

手动测试：

```sh
printf '{"hook_event_name":"ManualTest"}' | \
~/.stick-s3-ai-alert-venv/bin/python ~/.stick-s3-ai-alert/stick_alert.py codex done
```

测试时 StickS3 需要进入 `AI Alert` App。这个 App 会开启 BLE 广播，设备名是 `StickS3-AI`。

第一次使用时，macOS 可能会请求 Terminal、Python 或终端应用的蓝牙权限，允许即可。

## Codex 配置

把下面配置加入 `~/.codex/config.toml`：

```toml
[features]
codex_hooks = true

[[hooks.PermissionRequest]]
matcher = ".*"

[[hooks.PermissionRequest.hooks]]
type = "command"
command = "~/.stick-s3-ai-alert-venv/bin/python ~/.stick-s3-ai-alert/stick_alert.py codex ask"
timeout = 10
statusMessage = "Sending StickS3 alert"

[[hooks.Stop]]

[[hooks.Stop.hooks]]
type = "command"
command = "~/.stick-s3-ai-alert-venv/bin/python ~/.stick-s3-ai-alert/stick_alert.py codex done"
timeout = 10
statusMessage = "Sending StickS3 alert"
```

修改配置后需要重启 Codex。

## Claude Code 配置

把下面配置加入 `~/.claude/settings.json`：

```json
{
  "hooks": {
    "Notification": [
      {
        "hooks": [
          {
            "type": "command",
            "command": "~/.stick-s3-ai-alert-venv/bin/python ~/.stick-s3-ai-alert/stick_alert.py claude ask"
          }
        ]
      }
    ],
    "Stop": [
      {
        "hooks": [
          {
            "type": "command",
            "command": "~/.stick-s3-ai-alert-venv/bin/python ~/.stick-s3-ai-alert/stick_alert.py claude done"
          }
        ]
      }
    ]
  }
}
```

修改配置后需要重启 Claude Code。

## 设备按键

启动器：

- `B` 短按：下一个 App
- `B` 长按：上一个 App
- `A` 短按：进入 App
- `A` 长按：打开 Settings
- `A+B` 长按：回 Home

AI Alert：

- 空闲：READY 预览后黑屏。
- 新提醒：闪烁和蜂鸣 3 次，然后自动黑屏。
- 提醒显示时按 `A`：立即黑屏。
- 黑屏时按 `A`：显示上一条提醒。
- `B`：静音 / 取消静音。
- `A` 长按：回 Home。

## 隐私说明

仓库刻意排除了：

- PlatformIO 构建产物
- Arduino CLI 缓存
- 下载的二进制文件
- 本地 hook 安装目录
- 本地日志
- 个人 Codex / Claude 配置文件

辅助脚本只会通过 BLE 向 StickS3 发送一条短消息，不会调用任何云端 API。

## 许可证

MIT
