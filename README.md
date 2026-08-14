# proxify

> 给任意进程挂上代理。环境变量和浏览器内核参数会传给子进程、孙进程，**不用改 Windows / macOS 系统全局代理**。

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Version](https://img.shields.io/badge/version-1.1.0-blue.svg)](https://github.com/suifei/proxify/releases/tag/v1.1.0)
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-lightgrey.svg)]()
[![Build](https://github.com/suifei/proxify/actions/workflows/release.yml/badge.svg)](https://github.com/suifei/proxify/actions/workflows/release.yml)

命令行工具（curl、git、Python、Go、Node）认 `HTTP_PROXY`；Cursor、ChatGPT Desktop、VS Code、Antigravity 这类 **Electron / Chromium 桌面软件在 Windows 上不认环境变量**，要靠启动参数 `--proxy-server`。proxify 两种都会设，本机回环默认绕过。

---

## 中文简介

`proxify` 是一个很小的启动器，用法类似 `nice` / `nohup`：先写代理，再写你要打开的程序。它只影响**这一棵进程树**，不会把系统「设置 → 网络和 Internet → 代理」打开。改系统代理会让浏览器、更新、网盘全部走代理，所以不要用那条路。

适合：

- 终端里的 curl / git / npm
- Cursor、ChatGPT Desktop、VS Code、Windsurf、Trae、Antigravity 等套了浏览器内核的桌面客户端
- 不想给整个操作系统开全局代理，只想给某一个 App 出口

---

## 给 Cursor、ChatGPT Desktop 这类软件挂代理

下面以本地 HTTP 代理 `http://127.0.0.1:7890` 为例。Clash / Clash Verge / mihomo 默认混合端口经常是 **7890**，v2rayN 常见 **10808 / 10809**，请改成你自己软件里显示的本地端口。

### 0. 先装 proxify

到 [Releases](https://github.com/suifei/proxify/releases) 下载对应平台的文件，改个短名字放到 PATH 里。

| 你的系统 | 下载 | 放到哪里 |
|---|---|---|
| Windows x64 | `proxify-windows-amd64.exe` | 改名为 `proxify.exe`，放到 `C:\tools\` 或任意已在 PATH 的目录 |
| macOS Apple Silicon | `proxify-darwin-arm64` | `chmod +x` 后拷到 `/usr/local/bin/proxify` |
| macOS Intel | `proxify-darwin-amd64` | 同上 |
| Linux x64 | `proxify-linux-amd64` | `chmod +x` 后拷到 `/usr/local/bin/proxify` |
| Linux ARM64 | `proxify-linux-arm64` | 同上 |

macOS 第一次运行若提示「无法验证开发者」：

```bash
xattr -d com.apple.quarantine proxify
```

### 1. 必须先彻底退出原软件

Cursor、ChatGPT 关掉窗口后经常还在托盘里活着。托盘图标右键 **Quit / 退出**，或在任务管理器里把残留进程结束掉。否则你用 proxify 再开一次，连上的还是没挂代理的那份。

### 2. 找到真正的可执行文件

不要用开始菜单里那个「快捷方式自己猜」的方式。在任务管理器里对正在运行的 Cursor / ChatGPT 选「打开文件所在的位置」，复制那个 `.exe` / 二进制的完整路径。

常见位置（版本不同可能略有出入）：

| 软件 | Windows | macOS |
|---|---|---|
| Cursor | `%LOCALAPPDATA%\Programs\cursor\Cursor.exe` | `/Applications/Cursor.app/Contents/MacOS/Cursor` |
| ChatGPT Desktop | `%LOCALAPPDATA%\Programs\ChatGPT\ChatGPT.exe` | `/Applications/ChatGPT.app/Contents/MacOS/ChatGPT` |
| VS Code | `%LOCALAPPDATA%\Programs\Microsoft VS Code\Code.exe` | `/Applications/Visual Studio Code.app/Contents/MacOS/Electron` |
| Antigravity | 安装目录下的 `Antigravity.exe` | `Antigravity.app/Contents/MacOS/` 里的主程序 |

macOS **不要**用 `open -a Cursor`：`open` 会走 Launch Services，proxify 设好的环境和参数带不进去。必须直接启动 `.app/Contents/MacOS/` 里面那个二进制。

Microsoft Store 版 ChatGPT 路径在 `WindowsApps` 里，比较别扭。能装独立安装包的话更省事。

### 3. Windows：一条命令，或做一个启动脚本

先在命令行试一次（把端口和路径换成你的）：

```bat
proxify.exe -v http://127.0.0.1:7890 "%LOCALAPPDATA%\Programs\cursor\Cursor.exe"
```

```bat
proxify.exe -v http://127.0.0.1:7890 "%LOCALAPPDATA%\Programs\ChatGPT\ChatGPT.exe"
```

`-v` 会在启动前打印实际生效的代理。看到类似下面这样就对了：

```
[proxify] HTTP_PROXY  = http://127.0.0.1:7890
[proxify] desktop    = gui, chromium-kernel
[proxify] detach     = yes
[proxify] inject     = --proxy-server=http://127.0.0.1:7890 --proxy-bypass-list=localhost;127.0.0.1;::1
```

`chromium-kernel` 表示识别到了 Electron，已经自动加上浏览器内核要的参数。如果没有识别到，加上 `-g` 强制按桌面软件处理：

```bat
proxify.exe -g http://127.0.0.1:7890 "D:\path\to\ChatGPT.exe"
```

日常用的话，在桌面建一个 `Cursor-代理.bat`：

```bat
@echo off
set PROXY=http://127.0.0.1:7890
set APP=%LOCALAPPDATA%\Programs\cursor\Cursor.exe

if not exist "%APP%" (
    echo 找不到 Cursor: %APP%
    pause
    exit /b 1
)

proxify.exe %PROXY% "%APP%"
if errorlevel 1 pause
```

ChatGPT Desktop 同理，把 `APP=` 换成 ChatGPT 的 exe。

不想弹黑框：把 bat 发给自己做一个快捷方式，快捷方式属性里选「运行 → 最小化」。或者快捷方式目标直接写成：

```
C:\tools\proxify.exe http://127.0.0.1:7890 C:\Users\你的用户名\AppData\Local\Programs\cursor\Cursor.exe
```

「起始位置」填 Cursor 的安装目录。以后点这个快捷方式，不要点官方原来那个。

SOCKS 代理用 `-s`：

```bat
proxify.exe -s socks5://127.0.0.1:7891 "%LOCALAPPDATA%\Programs\cursor\Cursor.exe"
```

### 4. macOS：启动 .app 里面的二进制

```bash
proxify -v http://127.0.0.1:7890 /Applications/Cursor.app/Contents/MacOS/Cursor
```

```bash
proxify -v http://127.0.0.1:7890 /Applications/ChatGPT.app/Contents/MacOS/ChatGPT
```

想从 Dock / 访达双击启动，存成 `~/bin/cursor-proxy.command`（用文本编辑即可）：

```bash
#!/bin/bash
exec /usr/local/bin/proxify -g http://127.0.0.1:7890 \
  /Applications/Cursor.app/Contents/MacOS/Cursor
```

然后：

```bash
chmod +x ~/bin/cursor-proxy.command
```

双击这个 `.command` 即可。第一次若被拦截，右键 → 打开。

### 5. Linux

```bash
proxify -v http://127.0.0.1:7890 /usr/share/cursor/cursor
```

桌面项 `~/.local/share/applications/cursor-proxify.desktop`：

```ini
[Desktop Entry]
Name=Cursor (proxify)
Exec=/usr/local/bin/proxify http://127.0.0.1:7890 /usr/share/cursor/cursor
Terminal=false
Type=Application
Icon=cursor
```

### 6. 怎么确认真的走了代理

1. 启动时加 `-v`，确认打印了 `HTTP_PROXY` 和 `--proxy-server`。
2. 打开你的 Clash / v2rayN 连接日志，用 Cursor 聊天或 ChatGPT 发一条消息，日志里应出现对应连接。
3. 不要去系统设置里开「使用代理服务器」。那是全局的，和 proxify 无关。

本机回环（`localhost` / `127.0.0.1` / `::1`）默认已经绕过，不用写 `-n`。只有还要放过公司内网时才追加：

```bat
proxify.exe -n ".corp.local,10.0.0.0/8" http://127.0.0.1:7890 "%LOCALAPPDATA%\Programs\cursor\Cursor.exe"
```

### 7. 这类软件为什么不能只设环境变量

| 软件栈 | 认不认 `HTTP_PROXY` | proxify 实际做的 |
|---|---|---|
| Cursor / ChatGPT Desktop / VS Code / Antigravity（Electron） | Windows 上基本不认 | 追加 `--proxy-server`、`--proxy-bypass-list`，Node 侧仍靠环境变量 |
| WebView2 套壳 | 不认 | 设置 `WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS` |
| Qt WebEngine | 通常不认 | 设置 `QTWEBENGINE_CHROMIUM_FLAGS` |
| curl / git / Python / Go | 认 | 只设环境变量就够 |

也不要「先打开系统全局代理、等软件起来再关掉」。Chromium 会监视系统代理注册表，你一关它就直连了。

---

## Features

- Sets **6 proxy environment variables** automatically: `HTTP_PROXY` / `HTTPS_PROXY` / `ALL_PROXY` and their lowercase counterparts — compatible with curl, wget, Go, Node.js, Python requests, and virtually every other HTTP library
- Loopback (`localhost`, `127.0.0.1`, `::1`) is always on `NO_PROXY` and `--proxy-bypass-list`. `-n` only adds extra hosts — you do not pass loopback yourself
- **Desktop / browser-kernel apps**: Electron, CEF, Chromium, WebView2, and Qt WebEngine are detected automatically. On Windows those stacks ignore `HTTP_PROXY`, so proxify also injects `--proxy-server` and `--proxy-bypass-list` (or the matching framework environment variable)
- **Windows GUI**: PE subsystem `WINDOWS_GUI` is launched detached — the console does not stay open until you quit the app. Console programs still wait and forward the exit code. Force either side with `-g` / `-w`
- **Unix**: uses `execvp` to replace the current process — zero overhead. `-g` forks, `setsid()`, and returns immediately
- All child and grandchild processes inherit the environment automatically
- Pure C99, **zero external dependencies**

---

## Build

### Quick start

| Platform | Command |
|---|---|
| Linux / macOS | `gcc -O2 -o proxify proxify.c` |
| Windows (MinGW) | `gcc -O2 -o proxify.exe proxify.c` |
| Windows (MSVC) | `cl /O2 /Fe:proxify.exe proxify.c` |
| Windows (TCC) | `tcc -o proxify.exe proxify.c` |

GitHub Actions 会在打 `v*` 标签时自动编译 Windows / macOS / Linux（含 amd64 与 arm64）并发布到 Releases。

### Cross-compile for Windows from WSL2 / Linux

```bash
# Install MinGW toolchain (one-time)
sudo apt update && sudo apt install -y mingw-w64

# 64-bit Windows binary
x86_64-w64-mingw32-gcc -O2 -o proxify.exe proxify.c

# 32-bit Windows binary (if needed)
i686-w64-mingw32-gcc -O2 -o proxify.exe proxify.c
```

Verify:

```bash
file proxify.exe
# proxify.exe: PE32+ executable (console) x86-64, for MS Windows
```

---

## Usage

```bash
proxify [options] <command> [args...]
```

| Option | Description |
|---|---|
| `<url>` | HTTP/HTTPS proxy URL (positional, e.g. `http://127.0.0.1:8080`) |
| `-s <url>` | SOCKS proxy URL (e.g. `socks5://127.0.0.1:1080`) |
| `-n <hosts>` | Extra bypass hosts for `NO_PROXY` / `no_proxy` **and** `--proxy-bypass-list`. Loopback is always included |
| `-g`, `--gui` | Desktop mode: detach and always inject browser proxy flags |
| `-w`, `--wait` | Wait for the process (default for console programs) |
| `--no-flags` | Do not append `--proxy-server` / `--proxy-bypass-list` to argv (env vars are still set) |
| `-v` | Print effective proxy settings before launching |
| `-h` | Show help |

### Examples

```bash
# HTTP proxy — simplest form
proxify http://127.0.0.1:8080 my_app.exe

# SOCKS5 proxy
proxify socks5://127.0.0.1:1080 curl https://example.com

# HTTP + SOCKS at the same time
proxify http://127.0.0.1:8080 -s socks5://127.0.0.1:1080 my_app.exe arg1

# Verbose: show what was actually set
proxify -v http://127.0.0.1:8080 my_app.exe

# Extra bypass hosts only — loopback is already included
proxify -n "10.0.0.0/8,.corp.local" http://127.0.0.1:8080 my_app.exe

# Desktop app: detach + inject Chromium flags even if auto-detect misses
proxify -g http://127.0.0.1:8081 Antigravity.exe
```

---

## Desktop and browser-kernel apps

Command-line tools (`curl`, `git`, Python `requests`, Go, Node.js, …) read `HTTP_PROXY` / `HTTPS_PROXY` / `NO_PROXY`. That is enough.

Most desktop programs built on a browser kernel **do not**. On Windows they use the system proxy or their own switch parser:

| Stack | Honors `HTTP_PROXY`? | External parameter that works |
|---|---|---|
| Electron / Chromium / CEF (VS Code, Cursor, Antigravity, Chrome, Edge, CEF hosts) | No on Windows | `--proxy-server=` and `--proxy-bypass-list=` on the process command line |
| WebView2 (WinUI / WPF / WinForms hosts) | No | `WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS` |
| Qt WebEngine | Usually no | `QTWEBENGINE_CHROMIUM_FLAGS` |
| Ordinary Win32 / .NET with no browser kernel | N/A | Environment only — extra Chromium flags are **not** injected unless you pass `-g` |

proxify therefore does three things when it launches a target:

1. Sets the usual proxy environment variables, including `NO_PROXY` / `no_proxy`.
2. Always sets `WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS` and `QTWEBENGINE_CHROMIUM_FLAGS` to `--proxy-server=… --proxy-bypass-list=…`. Harmless if the app is not WebView2 / Qt.
3. If the executable looks like Electron / CEF / Chromium (sibling files such as `chrome_elf.dll`, `libcef.dll`, `resources/app.asar`, …) **or** you passed `-g`, appends those two switches to argv. Existing `--proxy-server` / `--proxy-bypass-list` on the command line are left untouched.

Loopback is always bypassed. `-n` only adds extras: `-n ".corp.local"` becomes `NO_PROXY=localhost,127.0.0.1,::1,.corp.local` and `--proxy-bypass-list=localhost;127.0.0.1;::1;.corp.local`.

Apps that parse argv strictly and are **not** Chromium-based will reject unknown switches — do not use `-g` on those. Auto-detect only injects flags when Chromium/Electron/CEF markers are present.

---

## Real-world example: Google Antigravity

[Google Antigravity](https://antigravity.thatworks.ai/) requires internet access through a proxy. Using `proxify` you can launch it with a single command without touching system-wide proxy settings.

**Command:**

```bat
proxify.exe http://127.0.0.1:8081 D:\Antigravity\Antigravity.exe
```

Antigravity is Electron. proxify detects `chrome_elf.dll` next to the exe, injects `--proxy-server` / `--proxy-bypass-list`, detaches so the console can close, and still sets `HTTP_PROXY` for the Node-side requests.

Add extra bypass hosts if needed (loopback is already included):

```bat
proxify.exe -n ".corp.local" http://127.0.0.1:8081 D:\Antigravity\Antigravity.exe
```

**Working directory:** `D:\Antigravity`

### Desktop shortcut (.bat)

Create a file named `Antigravity.bat` anywhere you like (e.g. your Desktop):

```bat
@echo off
rem Launch Antigravity through the local HTTP proxy at port 8081.
rem Requires proxify.exe to be in your PATH, or update the path below.

cd /d "D:\Antigravity"
proxify.exe http://127.0.0.1:8081 D:\Antigravity\Antigravity.exe
```

> **Tip:** If `proxify.exe` is not in your `PATH`, replace `proxify.exe` with its full path,  
> e.g. `C:\tools\proxify.exe http://127.0.0.1:8081 D:\Antigravity\Antigravity.exe`.

To keep the console window open on error, append `pause` at the end:

```bat
@echo off
cd /d "D:\Antigravity"
proxify.exe http://127.0.0.1:8081 D:\Antigravity\Antigravity.exe
if errorlevel 1 pause
```

Double-click the `.bat` file (or pin it to Start / taskbar) — Antigravity will start with the proxy already inherited by all its internal network calls.

---

## How it works

```
proxify http://proxy:8080  my_app.exe
   │
   ├─ sets HTTP_PROXY=http://proxy:8080
   ├─ sets HTTPS_PROXY=http://proxy:8080
   ├─ sets ALL_PROXY=http://proxy:8080
   ├─ sets http_proxy / https_proxy / all_proxy (lowercase)
   ├─ sets NO_PROXY / no_proxy          (always localhost,127.0.0.1,::1, plus -n / existing env)
   ├─ sets WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS
   ├─ sets QTWEBENGINE_CHROMIUM_FLAGS
   ├─ if Electron / CEF / Chromium / -g:
   │     appends --proxy-server=…
   │     appends --proxy-bypass-list=…   (same hosts as NO_PROXY)
   │
   └─ CreateProcess / execvp ──► my_app.exe
                                     └──► child processes  (inherit env + flags)
```

On **Windows**, console targets are launched via `CreateProcess` with inherited handles; proxify waits and forwards the exit code. GUI / `-g` targets are created with `DETACHED_PROCESS` and proxify returns immediately.

On **Unix/macOS** `execvp` replaces the `proxify` process entirely — no wrapper process remains in memory. `-g` forks first so the terminal comes back.

---

## Recommended Proxy Domains

The following domains benefit most from being routed through a proxy. Add them to your proxy rule list (e.g. in Clash, Surge, or any rule-based proxy tool):

```
https://open-vsx.org
https://*.visualstudio.com
https://*.microsoft.com
https://aka.ms
https://*.gallerycdn.vsassets.io
https://*.github.com
https://login.microsoftonline.com
https://*.vscode.dev
https://*.github.dev
https://gh.io
https://portal.azure.com
https://raw.githubusercontent.com
https://private-user-images.githubusercontent.com
https://avatars.githubusercontent.com
https://accounts.google.com
https://*.google.com
https://*.goog
https://*.google
```

These include VS Code extension marketplace, GitHub assets, Microsoft identity services, Azure portal, and Google accounts — all commonly blocked or throttled in restricted network environments.

---

## License

[MIT](LICENSE) © 2026 [suifei](https://github.com/suifei)
