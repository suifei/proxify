# proxify

> 给**任意一个程序**挂上代理，连同它的子进程、孙进程一起 —— **不碰 Windows / macOS 的系统全局代理**。

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Version](https://img.shields.io/badge/version-1.4.0-blue.svg)](https://github.com/suifei/proxify/releases/latest)
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-lightgrey.svg)](#安装)
[![Build](https://github.com/suifei/proxify/actions/workflows/release.yml/badge.svg)](https://github.com/suifei/proxify/actions/workflows/release.yml)

```bash
proxify http://127.0.0.1:8080 -app chatgpt                 # ChatGPT Desktop / Codex
proxify http://127.0.0.1:8080 "D:\cursor\Cursor.exe"       # Cursor、VS Code 等桌面软件
proxify http://127.0.0.1:8080 git clone https://github.com/suifei/proxify
```

一个单文件 C 程序，零依赖，用法和 `nice` / `nohup` 一样：**先写代理，再写要启动的程序**。

---

## 目录

- [为什么需要它](#为什么需要它)
- [安装](#安装)
- [桌面软件指南](#桌面软件指南)：[ChatGPT](#chatgpt-desktop--codex) · [Cursor / VS Code](#cursor--vs-code) · [其他 Electron 软件](#其他-electron--chromium-软件) · [做成快捷方式](#做成快捷方式)
- [确认真的走了代理](#确认真的走了代理)
- [常见问题](#常见问题)
- [命令参考](#命令参考)
- [工作原理](#工作原理)
- [编译](#编译)
- [推荐走代理的域名](#推荐走代理的域名)
- [更新记录](#更新记录)

---

## 为什么需要它

给单个软件挂代理，常见的两条路都有坑：

- **开系统全局代理**：浏览器、系统更新、网盘全都跟着走代理。而且不能「开一下等软件起来再关」—— Chromium 会监视系统代理设置，你一关它就直连了。
- **只设 `HTTP_PROXY` 环境变量**：命令行工具认，但桌面软件大多不认。

| 软件类型 | 认 `HTTP_PROXY` 吗 | 真正管用的 |
|---|---|---|
| curl / git / npm / Python / Go | 认 | 环境变量 |
| Electron / Chromium / CEF（Cursor、VS Code、ChatGPT、Antigravity…） | Windows 上基本不认 | 启动参数 `--proxy-server` |
| 上述软件里的 Node 进程（Cursor Agent 的 HTTP/2） | 不认，也不吃启动参数 | 往 Node 入口注入 hook |
| WebView2 套壳 | 不认 | `WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS` |
| Qt WebEngine | 通常不认 | `QTWEBENGINE_CHROMIUM_FLAGS` |

proxify 把这几层**一次全做了**，并且只影响它启动的这一棵进程树。本机回环（`localhost` / `127.0.0.1` / `::1`）默认绕过。

---

## 安装

到 [Releases](https://github.com/suifei/proxify/releases/latest) 下载对应平台的文件，改成短名字放进 PATH。

| 系统 | 下载 | 放到哪里 |
|---|---|---|
| Windows x64 | `proxify-windows-amd64.exe` | 改名 `proxify.exe`，放到 `C:\tools\` 或任意 PATH 目录 |
| macOS Apple Silicon | `proxify-darwin-arm64` | `chmod +x` 后拷到 `/usr/local/bin/proxify` |
| macOS Intel | `proxify-darwin-amd64` | 同上 |
| Linux x64 | `proxify-linux-amd64` | 同上 |
| Linux ARM64 | `proxify-linux-arm64` | 同上 |

macOS 首次运行若提示「无法验证开发者」：

```bash
xattr -d com.apple.quarantine proxify
```

下文都以本地 HTTP 代理 `http://127.0.0.1:8080` 为例。Clash / mihomo 的混合端口通常是 **7890**，v2rayN 常见 **10808 / 10809**，请换成你自己代理软件里显示的端口。

---

## 桌面软件指南

> **第一步永远是：彻底退出原软件。**
> Cursor、ChatGPT 关掉窗口后通常还活在托盘里。Electron 是单实例的，你再开一次只会唤醒旧的那份，新参数会被直接丢掉。请在托盘图标上右键 **Quit / 退出**，或者 `taskkill /F /IM Cursor.exe`。proxify 发现目标已在运行时会打印警告。

### ChatGPT Desktop / Codex

```bat
proxify.exe http://127.0.0.1:8080 -app chatgpt
```

就这一行。`-app codex` 是同一个东西 —— Windows 上 ChatGPT 和 Codex 现在是同一个商店包。

为什么要内置名字：ChatGPT 是 Windows 商店（MSIX）软件，装在 `C:\Program Files\WindowsApps\OpenAI.Codex_<版本号>_x64__...\` 下面，**每次自动更新路径都会变**，写死在快捷方式里过几天就失效。`-app chatgpt` 会在启动时查出当前的安装目录和主程序。

它内部是 Chromium 内核加一个 Rust 写的 `codex.exe` 子进程：前者吃 `--proxy-server`，后者认 `HTTPS_PROXY`，proxify 两样都给了，**不需要改它的任何文件**。实测界面、登录、对话、Agent 的连接全部走代理。

macOS 上 `-app chatgpt` 指向 `/Applications/ChatGPT.app/Contents/MacOS/ChatGPT`。

**其他商店软件**没有内置名字时，用通用写法 `appx:<PackageFamilyName>`：

```bat
proxify.exe http://127.0.0.1:8080 appx:OpenAI.Codex_2p2nqsd0c76g0
```

```powershell
# 查 PackageFamilyName
Get-AppxPackage *关键字* | select Name, PackageFamilyName
```

### Cursor / VS Code

```bat
proxify.exe -v http://127.0.0.1:8080 "%LOCALAPPDATA%\Programs\cursor\Cursor.exe"
```

路径不确定的话：让软件先正常跑起来，在任务管理器里右键 →「打开文件所在的位置」，复制那个 `.exe` 的完整路径。

| 软件 | Windows 默认位置 | macOS |
|---|---|---|
| Cursor | `%LOCALAPPDATA%\Programs\cursor\Cursor.exe` | `/Applications/Cursor.app/Contents/MacOS/Cursor` |
| VS Code | `%LOCALAPPDATA%\Programs\Microsoft VS Code\Code.exe` | `/Applications/Visual Studio Code.app/Contents/MacOS/Electron` |

**Cursor 多出来的一层。** `--proxy-server` 只管 Chromium 窗口；Cursor 的 Agent 和模型列表走的是 **Node.js 的 HTTP/2**，既不吃启动参数也不认 `HTTP_PROXY`。所以 proxify 识别到 Cursor / VS Code 家族（`resources/app/product.json`）时会额外做一件事：

1. 把一个 Node hook 写到安装目录的 `resources/app/out/proxify-hook.cjs`；
2. 在 `resources/app/out/bootstrap-fork.js` 开头加一行加载它。

这个 hook 把 `http` / `https` / `http2.connect` 改成经 HTTP CONNECT（或 SOCKS5）隧道出去，并让 fork 出来的子进程也带上它。只改这一个入口文件 —— Cursor 会校验自带扩展的文件哈希，动那些文件会让它的连接层罢工。不经 proxify 启动时没有代理环境变量，hook 不改变任何网络行为。

启动时看到下面这行就说明注入成功：

```
[proxify] patched Node entry ...\resources\app\out\bootstrap-fork.js
```

- Cursor 自动更新会把这个文件覆盖回去，**再用 proxify 启动一次就会重新注入**。
- 不想让 proxify 改文件就加 `--no-hook`。这时需要自己在 Cursor 的 settings.json 里写 `"http.proxy"` 和 `"cursor.general.disableHttp2": true`，Agent 才会走代理。
- 调试日志在系统临时目录的 `proxify-hook.log`。

### 其他 Electron / Chromium 软件

Antigravity、Windsurf、Trae、Chrome、Edge 以及各种 CEF 套壳，都是同一个用法：

```bat
proxify.exe http://127.0.0.1:8081 D:\Antigravity\Antigravity.exe
```

proxify 靠 exe 同目录的特征文件识别浏览器内核（`chrome_elf.dll`、`libcef.dll`、`resources/app.asar`、macOS 上的 `Electron Framework.framework` 等）。识别不到时加 `-g` 强制按桌面软件处理：

```bat
proxify.exe -g http://127.0.0.1:8080 "D:\path\to\App.exe"
```

> 不要对**非** Chromium 的程序用 `-g`：严格解析命令行的程序会因为不认识 `--proxy-server` 而拒绝启动。

**macOS 不要用 `open -a Cursor`。** `open` 走 Launch Services，proxify 设好的环境变量和参数带不进去。必须直接启动 `.app/Contents/MacOS/` 里的那个二进制：

```bash
proxify http://127.0.0.1:8080 /Applications/Cursor.app/Contents/MacOS/Cursor
```

### 做成快捷方式

**Windows** —— 桌面上建一个 `ChatGPT-代理.bat`：

```bat
@echo off
proxify.exe http://127.0.0.1:8080 -app chatgpt
if errorlevel 1 pause
```

Cursor 这类有固定路径的：

```bat
@echo off
set PROXY=http://127.0.0.1:8080
set APP=%LOCALAPPDATA%\Programs\cursor\Cursor.exe

if not exist "%APP%" (
    echo 找不到 Cursor: %APP%
    pause
    exit /b 1
)

proxify.exe %PROXY% "%APP%"
if errorlevel 1 pause
```

图形程序启动后 proxify 立刻返回，黑框一闪就没。想连闪都不要：给 bat 建个快捷方式，属性里「运行」选「最小化」；或者快捷方式的目标直接写
`C:\tools\proxify.exe http://127.0.0.1:8080 -app chatgpt`。以后点这个快捷方式，别点官方原来那个。

**macOS** —— 存成 `~/bin/cursor-proxy.command`，`chmod +x` 后双击（首次被拦截就右键 → 打开）：

```bash
#!/bin/bash
exec /usr/local/bin/proxify -g http://127.0.0.1:8080 \
  /Applications/Cursor.app/Contents/MacOS/Cursor
```

**Linux** —— `~/.local/share/applications/cursor-proxify.desktop`：

```ini
[Desktop Entry]
Name=Cursor (proxify)
Exec=/usr/local/bin/proxify http://127.0.0.1:8080 /usr/share/cursor/cursor
Terminal=false
Type=Application
Icon=cursor
```

---

## 确认真的走了代理

1. 加 `-v` 启动，核对打印出来的内容：

   ```
   [proxify] HTTP_PROXY  = http://127.0.0.1:8080
   [proxify] target     = C:\Program Files\WindowsApps\OpenAI.Codex_...\app\ChatGPT.exe
   [proxify] desktop    = gui, chromium-kernel
   [proxify] detach     = yes
   [proxify] inject     = --proxy-server=http://127.0.0.1:8080 --proxy-bypass-list=localhost;127.0.0.1;::1 --disable-quic --disable-http2
   ```

   `chromium-kernel` 表示识别到了浏览器内核；Cursor / VS Code 还会多一个 `vscode-family`。

2. 打开代理软件的连接日志（Clash 的「连接」页、v2rayN 的 `guiLogs\Vaccess_*.txt`），在软件里发一条消息，日志里应该出现 `chatgpt.com`、`api2.cursor.sh` 之类的连接。

3. 系统设置里的「使用代理服务器」保持关闭。那是全局的，和 proxify 无关。

---

## 常见问题

**启动了，但代理日志里没有流量。**
十有八九是旧实例没退干净（见[上面](#桌面软件指南)）。其次看日志里的出站是不是 `DIRECT`：有 CONNECT 记录但走了直连，是你代理软件的分流规则问题，临时切全局模式验证一下。

**Cursor 窗口正常，Agent / 模型列表连不上。**
Node hook 没注入上。确认启动输出里有 `patched Node entry` 或 `node-hook already in`；Cursor 刚更新过的话重新用 proxify 启动一次。如果输出里是 `WARNING: could not write / patch`，说明安装目录没有写权限（例如装在 `Program Files`）：用管理员身份跑一次 proxify 完成注入，退出 Cursor，之后照常启动即可。

**ChatGPT 语音提示 “Voice chat took too long to start”，重试一次又好了。**
多半是代理节点延迟太高。语音要在几秒内完成信令加十几个来回的媒体握手，节点单程超过 1 秒时，第一次（冷连接）就会超时，重试时连接已经热了所以能成。用这条命令量一下（macOS / Linux 把 `NUL` 换成 `/dev/null`），第二个数（TLS 握手完成）最好在 0.5 秒以内：

```bash
curl -x http://127.0.0.1:8080 -o NUL -s -w "%{time_connect} %{time_appconnect} %{time_starttransfer}\n" https://api.openai.com/v1/models
```

另外要知道：WebRTC 的 UDP 媒体流**进不了 HTTP 代理**，Chromium 会同时尝试 UDP 直连和经代理的 TCP，哪条先通用哪条。

**公司内网的地址不想走代理。**
回环默认已经绕过，`-n` 只需要写额外的：

```bat
proxify.exe -n ".corp.local,10.0.0.0/8" http://127.0.0.1:8080 -app chatgpt
```

它会同时进 `NO_PROXY` 和 `--proxy-bypass-list`。已有的 `NO_PROXY` 环境变量也会被合并进来。

**用 SOCKS5 代理。**

```bat
proxify.exe -s socks5://127.0.0.1:7891 -app chatgpt
```

**程序因为不认识 `--proxy-server` 拒绝启动。**
那它不是 Chromium 内核，去掉 `-g`；若是自动识别误判，加 `--no-flags`（环境变量照设，只是不追加启动参数）。

---

## 命令参考

```
proxify [options] <proxy_url> <command> [args...]
proxify [options] <proxy_url> -app <name> [args...]
```

| 选项 | 说明 |
|---|---|
| `<url>` | HTTP/HTTPS 代理地址，位置参数，如 `http://127.0.0.1:8080` |
| `-s <url>` | SOCKS 代理，如 `socks5://127.0.0.1:1080`。可与 HTTP 代理同时给 |
| `-app <name>` | 启动内置名字对应的软件：`chatgpt`（Windows 商店包 / macOS `.app`），Windows 上 `codex` 为同义词。名字后面的参数原样传给软件。也可写 `-a`、`--app` |
| `appx:<family>` | 写在 `<command>` 的位置（Windows）：按 PackageFamilyName 启动商店 / MSIX 软件，运行时解析带版本号的 `WindowsApps` 路径 |
| `-n <hosts>` | **追加**绕过代理的主机，同时作用于 `NO_PROXY` 和 `--proxy-bypass-list`。回环地址永远包含在内 |
| `-g`, `--gui` | 桌面模式：启动后立即返回，并强制注入浏览器内核参数 |
| `-w`, `--wait` | 等待目标退出并转发退出码（控制台程序的默认行为） |
| `--no-flags` | 不追加 `--proxy-server` 等启动参数，只设环境变量 |
| `--no-hook` | 不修改 Cursor / VS Code 的 JS 入口 |
| `-v` | 启动前打印实际生效的配置 |
| `-h` | 帮助 |

```bash
# 命令行工具
proxify http://127.0.0.1:8080 curl https://example.com
proxify socks5://127.0.0.1:1080 curl https://example.com

# HTTP + SOCKS 同时给
proxify http://127.0.0.1:8080 -s socks5://127.0.0.1:1080 my_app arg1

# 参数里有和 proxify 选项撞名的，用 -- 隔开
proxify http://127.0.0.1:8080 -- my_app -v
```

---

## 工作原理

```
proxify http://proxy:8080  App.exe
   │
   ├─ 环境变量   HTTP_PROXY / HTTPS_PROXY / ALL_PROXY（含小写）
   │             NO_PROXY / no_proxy          = localhost,127.0.0.1,::1 + 已有值 + -n
   │             GLOBAL_AGENT_*               （Node global-agent）
   │             NODE_USE_ENV_PROXY=1         （Node 22+ 的 fetch / undici）
   │             WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS
   │             QTWEBENGINE_CHROMIUM_FLAGS
   │
   ├─ 启动参数   识别到 Electron / CEF / Chromium，或给了 -g 时追加：
   │             --proxy-server=…  --proxy-bypass-list=…
   │             --disable-quic    --disable-http2
   │             （命令行里已经写了的开关不会重复追加）
   │
   ├─ Node hook  识别到 Cursor / VS Code 家族时：
   │             写入 out/proxify-hook.cjs，并在 out/bootstrap-fork.js 开头加载它
   │
   └─ 启动       CreateProcess / execvp ──► App.exe ──► 子进程（继承以上全部）
```

- **为什么关掉 QUIC 和 HTTP/2**：QUIC（HTTP/3）走 UDP，会直接绕过 HTTP CONNECT 代理；关掉 HTTP/2 让 Chromium 退回 HTTP/1.1，经 CONNECT 隧道时兼容性最好。
- **Windows**：读 PE 头判断子系统。控制台程序等待并转发退出码；图形程序以 `DETACHED_PROCESS` 启动后立即返回，且不继承 proxify 的输出管道，所以 `proxify ... | findstr` 或在脚本里捕获输出都不会被卡住。
- **Unix / macOS**：直接 `execvp` 替换自身，内存里不留包装进程。`-g` 时先 `fork` + `setsid()`，终端立刻归还。
- **商店软件**：通过 kernel32 的 `GetPackagesByPackageFamily` / `GetPackagePathByFullName` 查安装目录，再从 `AppxManifest.xml` 取主程序。运行时动态查找这两个函数，不增加任何头文件和链接依赖。

Node hook 的源码是 [`proxify-hook.js`](proxify-hook.js)，由 `_gen_hook_inc.py` 转成 C 字符串 `proxify-hook.inc` 编进可执行文件。开发时把 `proxify-hook.js` 放在 proxify 旁边，会优先使用这份外部文件，改 hook 不用重新编译。

---

## 编译

纯 C99，单文件，零外部依赖。

| 平台 | 命令 |
|---|---|
| Linux / macOS | `gcc -O2 -o proxify proxify.c` |
| Windows (MinGW) | `gcc -O2 -o proxify.exe proxify.c` |
| Windows (MSVC) | `cl /O2 /Fe:proxify.exe proxify.c` |
| Windows (TCC) | `tcc -o proxify.exe proxify.c` |

从 Linux / WSL2 交叉编译 Windows 版：

```bash
sudo apt install -y mingw-w64
x86_64-w64-mingw32-gcc -O2 -o proxify.exe proxify.c
```

改了 `proxify-hook.js` 之后要重新生成内嵌副本：

```bash
python _gen_hook_inc.py
```

推送 `v*` 标签时，GitHub Actions 会自动编译 Windows / macOS / Linux（amd64 + arm64）并发布到 Releases。

---

## 推荐走代理的域名

用规则分流（Clash、Surge 等）时，下面这些域名建议走代理。覆盖 VS Code 扩展市场、GitHub 资源、微软身份认证、Azure 和 Google 账号：

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

ChatGPT / Codex 用到的：`chatgpt.com`、`*.chatgpt.com`、`*.openai.com`、`*.oaistatic.com`、`*.oaiusercontent.com`，以及语音用到的若干 Azure 媒体服务器（以纯 IP 出现，规则里按 IP 段或用全局模式处理）。

---

## 更新记录

**1.4.0**
- 新增 `-app <name>`：`proxify http://127.0.0.1:8080 -app chatgpt` 一行启动 ChatGPT Desktop / Codex。
- 新增 `appx:<PackageFamilyName>` 目标写法，支持 Windows 商店（MSIX）软件，自动跟随更新后变化的安装路径。
- 修复：图形程序脱离启动时不再继承 proxify 的输出管道。此前 `proxify ... | findstr` 或脚本捕获输出会一直卡到目标程序退出。Unix 的 `-g` 同样处理。

**1.3.0**
- 为 Cursor / VS Code 注入 Node 代理 hook，Agent 的 HTTP/2 流量走 HTTP CONNECT / SOCKS5。

**1.1.0**
- 自动识别 Electron / CEF / Chromium 并追加 `--proxy-server` 等参数；为 WebView2 / Qt WebEngine 设置对应环境变量。
- Windows 图形程序启动后不再占住控制台；本机回环默认绕过，`-n` 只追加。
- 跨平台自动构建与发布。

---

## License

[MIT](LICENSE) © 2026 [suifei](https://github.com/suifei)
