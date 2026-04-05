# proxify

> Run any process through a proxy — environment variables inherited by all child and grandchild processes.

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Version](https://img.shields.io/badge/version-1.0-blue.svg)](https://github.com/suifei/proxify/releases/tag/v1.0)
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-lightgrey.svg)]()

Named **`proxify`** — short, semantically clear, in the spirit of `nice`, `nohup`, and `env`. Drop it in `/usr/local/bin` and it becomes a first-class system command.

---

## Features

- Sets **6 proxy environment variables** automatically: `HTTP_PROXY` / `HTTPS_PROXY` / `ALL_PROXY` and their lowercase counterparts — compatible with curl, wget, Go, Node.js, Python requests, and virtually every other HTTP library
- Default `NO_PROXY=localhost,127.0.0.1,::1`; override with `-n`
- **Windows**: uses `CreateProcess`, waits for the child process, and propagates its exit code
- **Unix**: uses `execvp` to replace the current process — zero overhead
- All child and grandchild processes inherit the environment automatically
- Pure C99, ~150 lines, **zero external dependencies**

---

## Build

### Quick start

| Platform | Command |
|---|---|
| Linux / macOS | `gcc -O2 -o proxify proxify.c` |
| Windows (MinGW) | `gcc -O2 -o proxify.exe proxify.c` |
| Windows (MSVC) | `cl /O2 /Fe:proxify.exe proxify.c` |
| Windows (TCC) | `tcc -o proxify.exe proxify.c` |

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
| `-n <hosts>` | Override `NO_PROXY` (default: `localhost,127.0.0.1,::1`) |
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

# Custom NO_PROXY
proxify -n "localhost,10.0.0.0/8" http://127.0.0.1:8080 my_app.exe
```

---

## How it works

```
proxify http://proxy:8080  my_app.exe
   │
   ├─ sets HTTP_PROXY=http://proxy:8080
   ├─ sets HTTPS_PROXY=http://proxy:8080
   ├─ sets ALL_PROXY=http://proxy:8080
   ├─ sets http_proxy / https_proxy / all_proxy (lowercase)
   ├─ sets NO_PROXY=localhost,127.0.0.1,::1
   │
   └─ CreateProcess / execvp ──► my_app.exe  (inherits env)
                                     └──► child processes  (inherit env)
```

On **Windows** the child process is launched via `CreateProcess` with inherited handles. `proxify` waits and forwards the exit code.

On **Unix/macOS** `execvp` replaces the `proxify` process entirely — no wrapper process remains in memory.

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
