/*
 * proxify - Launch any process with proxy environment inherited by all children.
 *
 * Usage:
 *   proxify <proxy_url> <command> [args...]
 *   proxify -s <socks_url> <command> [args...]
 *   proxify -g <proxy_url> <desktop_app.exe>
 *   proxify -n <hosts> <proxy_url> <command> [args...]
 *
 * Sets: HTTP_PROXY, HTTPS_PROXY, ALL_PROXY, NO_PROXY (and lowercase variants).
 * Browser-kernel desktop apps also receive --proxy-server / --proxy-bypass-list
 * (and WebView2 / Qt WebEngine equivalents). Loopback is always on the bypass
 * list; -n only adds extra hosts to NO_PROXY and --proxy-bypass-list.
 *
 * Build:
 *   gcc -O2 -o proxify proxify.c        (Linux/macOS)
 *   cl /O2 proxify.c                     (MSVC)
 *   gcc -O2 -o proxify.exe proxify.c     (MinGW)
 */

#ifndef _WIN32
  #define _POSIX_C_SOURCE 200112L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <process.h>
  #include <windows.h>
#else
  #include <limits.h>
  #include <unistd.h>
  #include <sys/stat.h>
  #include <sys/types.h>
#ifndef PATH_MAX
  #define PATH_MAX 4096
#endif
#endif

#define VERSION "1.1.0"
#define DEFAULT_NO_PROXY "localhost,127.0.0.1,::1"

#ifdef _WIN32
  #define PATH_SEP '\\'
  #define EXE_PATH_MAX MAX_PATH
#else
  #define PATH_SEP '/'
  #define EXE_PATH_MAX PATH_MAX
#endif

static void set_env(const char *name, const char *val)
{
#ifdef _WIN32
    SetEnvironmentVariableA(name, val);
    {
        size_t n = strlen(name) + strlen(val) + 2;
        char *buf = (char *)malloc(n);
        if (!buf) return;
        snprintf(buf, n, "%s=%s", name, val);
        /* Some CRTs take ownership of the buffer; do not free. */
        _putenv(buf);
    }
#else
    setenv(name, val, 1);
#endif
}

/* Comma/semicolon NO_PROXY list -> Chromium --proxy-bypass-list (semicolons). */
static void to_bypass_list(const char *in, char *out, size_t outsz)
{
    size_t j = 0;
    int start = 1;

    if (!in || outsz == 0) {
        if (out && outsz) out[0] = '\0';
        return;
    }
    for (; *in && j + 1 < outsz; in++) {
        if (*in == ',' || *in == ';') {
            if (j > 0 && out[j - 1] != ';')
                out[j++] = ';';
            start = 1;
            continue;
        }
        if (start && (*in == ' ' || *in == '\t'))
            continue;
        start = 0;
        out[j++] = *in;
    }
    while (j > 0 && (out[j - 1] == ';' || out[j - 1] == ' ' || out[j - 1] == '\t'))
        j--;
    out[j] = '\0';
}

static int token_eq(const char *a, size_t alen, const char *b, size_t blen)
{
    size_t i;

    if (alen != blen) return 0;
    for (i = 0; i < alen; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + 32);
        if (ca != cb) return 0;
    }
    return 1;
}

static int list_has_token(const char *list, const char *tok, size_t tlen)
{
    const char *p = list;

    if (!list || !tlen) return 0;
    while (*p) {
        const char *start, *end;
        while (*p == ',' || *p == ';' || *p == ' ' || *p == '\t')
            p++;
        if (!*p) break;
        start = p;
        while (*p && *p != ',' && *p != ';')
            p++;
        end = p;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
            end--;
        if (token_eq(start, (size_t)(end - start), tok, tlen))
            return 1;
    }
    return 0;
}

static void append_token(char *out, size_t outsz, const char *tok, size_t tlen)
{
    size_t used;

    if (!tlen || list_has_token(out, tok, tlen))
        return;
    used = strlen(out);
    if (used) {
        if (used + 1 + tlen + 1 > outsz)
            return;
        out[used] = ',';
        memcpy(out + used + 1, tok, tlen);
        out[used + 1 + tlen] = '\0';
    } else if (tlen + 1 <= outsz) {
        memcpy(out, tok, tlen);
        out[tlen] = '\0';
    }
}

static void append_host_list(char *out, size_t outsz, const char *in)
{
    const char *p;

    if (!in) return;
    p = in;
    while (*p) {
        const char *start, *end;
        while (*p == ',' || *p == ';' || *p == ' ' || *p == '\t')
            p++;
        if (!*p) break;
        start = p;
        while (*p && *p != ',' && *p != ';')
            p++;
        end = p;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
            end--;
        append_token(out, outsz, start, (size_t)(end - start));
    }
}

/* Loopback is always included. -n and existing env only add extra hosts. */
static void build_no_proxy(char *out, size_t outsz, const char *cli)
{
    const char *e;

    if (!out || outsz == 0) return;
    out[0] = '\0';
    append_host_list(out, outsz, DEFAULT_NO_PROXY);
    e = getenv("NO_PROXY");
    if (e && e[0])
        append_host_list(out, outsz, e);
    e = getenv("no_proxy");
    if (e && e[0])
        append_host_list(out, outsz, e);
    if (cli && cli[0])
        append_host_list(out, outsz, cli);
}

static const char *pick_proxy_server(const char *http, const char *socks)
{
    return http ? http : socks;
}

static void set_proxy_env(const char *http, const char *socks, const char *no_proxy)
{
    const char *all = http ? http : socks;

    if (http) {
        set_env("HTTP_PROXY",  http);
        set_env("http_proxy",  http);
        set_env("HTTPS_PROXY", http);
        set_env("https_proxy", http);
    }
    if (socks) {
        if (!http) {
            set_env("HTTP_PROXY",  socks);
            set_env("http_proxy",  socks);
            set_env("HTTPS_PROXY", socks);
            set_env("https_proxy", socks);
        }
        all = socks;
    }
    if (all) {
        set_env("ALL_PROXY", all);
        set_env("all_proxy", all);
    }
    set_env("NO_PROXY", no_proxy);
    set_env("no_proxy", no_proxy);
}

/* WebView2 / Qt WebEngine read these instead of (or in addition to) argv. */
static void set_browser_proxy_env(const char *proxy_server, const char *bypass)
{
    char extra[2048];

    if (bypass && bypass[0])
        snprintf(extra, sizeof(extra),
                 "--proxy-server=%s --proxy-bypass-list=%s",
                 proxy_server, bypass);
    else
        snprintf(extra, sizeof(extra), "--proxy-server=%s", proxy_server);

    set_env("WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS", extra);
    set_env("QTWEBENGINE_CHROMIUM_FLAGS", extra);
}

static int file_exists(const char *path)
{
#ifdef _WIN32
    DWORD a = GetFileAttributesA(path);
    return (a != INVALID_FILE_ATTRIBUTES);
#else
    struct stat st;
    return (stat(path, &st) == 0);
#endif
}

static void path_dirname(const char *path, char *out, size_t n)
{
    size_t i, last = (size_t)-1;

    for (i = 0; path[i]; i++) {
        if (path[i] == '/' || path[i] == '\\')
            last = i;
    }
    if (last == (size_t)-1) {
        snprintf(out, n, ".");
        return;
    }
    if (last == 0)
        last = 1;
    if (last >= n)
        last = n - 1;
    memcpy(out, path, last);
    out[last] = '\0';
}

static int path_join(char *out, size_t n, const char *dir, const char *name)
{
    size_t dlen = strlen(dir);
    int sep = (dlen > 0 && dir[dlen - 1] != '/' && dir[dlen - 1] != '\\');
    int wrote;

    if (sep)
        wrote = snprintf(out, n, "%s%c%s", dir, PATH_SEP, name);
    else
        wrote = snprintf(out, n, "%s%s", dir, name);
    return wrote > 0 && (size_t)wrote < n;
}

#ifdef _WIN32
static int resolve_exe(const char *cmd, char *out, DWORD n)
{
    char tmp[MAX_PATH];
    DWORD r;

    if (strpbrk(cmd, "\\/")) {
        r = GetFullPathNameA(cmd, n, out, NULL);
        if (r == 0 || r >= n) return 0;
        if (file_exists(out)) return 1;
        if (!strchr(cmd, '.')) {
            if (snprintf(tmp, sizeof(tmp), "%s.exe", out) < (int)sizeof(tmp) &&
                file_exists(tmp)) {
                snprintf(out, n, "%s", tmp);
                return 1;
            }
        }
        return 0;
    }
    r = SearchPathA(NULL, cmd, ".exe", n, out, NULL);
    return r > 0 && r < n;
}

static int is_gui_exe(const char *path)
{
    HANDLE h;
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS32 nt;
    DWORD nread;

    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    if (!ReadFile(h, &dos, sizeof(dos), &nread, NULL) ||
        nread != sizeof(dos) || dos.e_magic != IMAGE_DOS_SIGNATURE) {
        CloseHandle(h);
        return 0;
    }
    if (SetFilePointer(h, dos.e_lfanew, NULL, FILE_BEGIN) == INVALID_SET_FILE_POINTER) {
        CloseHandle(h);
        return 0;
    }
    if (!ReadFile(h, &nt, sizeof(nt), &nread, NULL) ||
        nread < 4 + sizeof(IMAGE_FILE_HEADER) + 70 ||
        nt.Signature != IMAGE_NT_SIGNATURE) {
        CloseHandle(h);
        return 0;
    }
    CloseHandle(h);
    return nt.OptionalHeader.Subsystem == IMAGE_SUBSYSTEM_WINDOWS_GUI;
}
#else
static int resolve_exe(const char *cmd, char *out, size_t n)
{
    char resolved[PATH_MAX];
    const char *path, *p;

    if (strchr(cmd, '/')) {
        if (realpath(cmd, resolved)) {
            snprintf(out, n, "%s", resolved);
            return 1;
        }
        snprintf(out, n, "%s", cmd);
        return file_exists(out);
    }
    path = getenv("PATH");
    if (!path) return 0;
    p = path;
    while (*p) {
        const char *colon = strchr(p, ':');
        size_t len = colon ? (size_t)(colon - p) : strlen(p);
        char dir[PATH_MAX], cand[PATH_MAX];

        if (len > 0 && len < sizeof(dir)) {
            memcpy(dir, p, len);
            dir[len] = '\0';
            if (path_join(cand, sizeof(cand), dir, cmd) && file_exists(cand)) {
                if (realpath(cand, resolved))
                    snprintf(out, n, "%s", resolved);
                else
                    snprintf(out, n, "%s", cand);
                return 1;
            }
        }
        if (!colon) break;
        p = colon + 1;
    }
    return 0;
}

static int is_gui_exe(const char *path)
{
    (void)path;
    return 0;
}
#endif

static int is_chromium_app(const char *exe)
{
    char dir[EXE_PATH_MAX], cand[EXE_PATH_MAX];
    int i;
#ifdef _WIN32
    static const char *const markers[] = {
        "chrome_elf.dll",
        "chrome.dll",
        "libcef.dll",
        "chrome_100_percent.pak",
        "LICENSES.chromium.html",
        "resources\\app.asar",
        "resources\\electron.asar",
        NULL
    };
#else
    static const char *const markers[] = {
        "chrome-sandbox",
        "chrome_crashpad_handler",
        "libcef.so",
        "libcef.dylib",
        "LICENSES.chromium.html",
        "chrome_100_percent.pak",
        "resources/app.asar",
        "resources/electron.asar",
        NULL
    };
#endif

    path_dirname(exe, dir, sizeof(dir));
    for (i = 0; markers[i]; i++) {
        if (path_join(cand, sizeof(cand), dir, markers[i]) && file_exists(cand))
            return 1;
    }
#ifndef _WIN32
    {
        char contents[EXE_PATH_MAX], fw[EXE_PATH_MAX];
        if (strstr(dir, ".app/Contents/MacOS")) {
            path_dirname(dir, contents, sizeof(contents));
            if (path_join(fw, sizeof(fw), contents, "Frameworks/Electron Framework.framework") &&
                file_exists(fw))
                return 1;
            if (path_join(fw, sizeof(fw), contents, "Frameworks/Chromium Embedded Framework.framework") &&
                file_exists(fw))
                return 1;
        }
    }
#endif
    return 0;
}

static int has_flag_prefix(int argc, char **argv, int start, const char *prefix)
{
    int i;
    size_t n = strlen(prefix);

    for (i = start; i < argc; i++) {
        if (strncmp(argv[i], prefix, n) == 0)
            return 1;
    }
    return 0;
}

static int append_arg(char *cmdline, size_t cap, const char *arg)
{
    size_t used = strlen(cmdline);
    int need_quote = (arg[0] == '\0' || strpbrk(arg, " \t") != NULL);
    size_t add = strlen(arg) + (used ? 1 : 0) + (need_quote ? 2 : 0);

    if (used + add + 1 > cap)
        return -1;
    if (used)
        strcat(cmdline, " ");
    if (need_quote)
        strcat(cmdline, "\"");
    strcat(cmdline, arg);
    if (need_quote)
        strcat(cmdline, "\"");
    return 0;
}

static void usage(void)
{
    fprintf(stderr,
        "proxify v" VERSION " - run any command or desktop app through a proxy\n\n"
        "Usage: proxify [options] <command> [args...]\n\n"
        "Options:\n"
        "  <url>           HTTP/HTTPS proxy (positional, before -s or command)\n"
        "  -s <url>        SOCKS proxy\n"
        "  -n <hosts>      Extra bypass hosts (loopback is always included:\n"
        "                  " DEFAULT_NO_PROXY ")\n"
        "  -g, --gui       Desktop mode: detach and inject browser proxy flags\n"
        "  -w, --wait      Wait for the process (default for console programs)\n"
        "  --no-flags      Do not append --proxy-server / --proxy-bypass-list\n"
        "  -v              Show proxy settings before launching\n"
        "  -h              Show this help\n\n"
        "Desktop / browser-kernel apps (Electron, CEF, Chromium, WebView2, Qt):\n"
        "  Detected automatically. Env vars alone are not enough on Windows;\n"
        "  proxify also passes --proxy-server and --proxy-bypass-list.\n"
        "  -n adds hosts to both NO_PROXY and --proxy-bypass-list.\n\n"
        "Examples:\n"
        "  proxify http://127.0.0.1:8080 curl https://example.com\n"
        "  proxify -n \"10.0.0.0/8,.corp.local\" http://127.0.0.1:8080 app.exe\n"
        "  proxify -g http://127.0.0.1:8081 Antigravity.exe\n");
}

int main(int argc, char *argv[])
{
    const char *http_proxy = NULL;
    const char *socks_proxy = NULL;
    const char *no_proxy_opt = NULL;
    const char *proxy_server;
    int verbose = 0;
    int force_gui = 0;
    int force_wait = 0;
    int no_flags = 0;
    int cmd_start = 0;
    int i;
    int is_gui = 0;
    int is_chrome = 0;
    int inject_argv = 0;
    int detach = 0;
    int resolved_ok = 0;
    char resolved[EXE_PATH_MAX];
    char no_proxy[2048];
    char bypass[1024];
    char proxy_switch[768];
    char bypass_switch[1280];

    if (argc < 2) { usage(); return 1; }

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(); return 0;
        } else if (strcmp(argv[i], "-v") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "-g") == 0 || strcmp(argv[i], "--gui") == 0) {
            force_gui = 1;
        } else if (strcmp(argv[i], "-w") == 0 || strcmp(argv[i], "--wait") == 0) {
            force_wait = 1;
        } else if (strcmp(argv[i], "--no-flags") == 0) {
            no_flags = 1;
        } else if (strcmp(argv[i], "-s") == 0) {
            if (++i >= argc) { fprintf(stderr, "proxify: -s requires an argument\n"); return 1; }
            socks_proxy = argv[i];
        } else if (strcmp(argv[i], "-n") == 0) {
            if (++i >= argc) { fprintf(stderr, "proxify: -n requires an argument\n"); return 1; }
            no_proxy_opt = argv[i];
        } else if (strcmp(argv[i], "--") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "proxify: no command specified\n");
                usage();
                return 1;
            }
            cmd_start = i;
            break;
        } else if (!http_proxy && argv[i][0] != '-' &&
                   (strstr(argv[i], "://") != NULL)) {
            http_proxy = argv[i];
        } else {
            cmd_start = i;
            break;
        }
    }

    if (cmd_start == 0) {
        fprintf(stderr, "proxify: no command specified\n");
        usage();
        return 1;
    }

    if (!http_proxy && !socks_proxy) {
        fprintf(stderr, "proxify: no proxy specified\n");
        usage();
        return 1;
    }

    build_no_proxy(no_proxy, sizeof(no_proxy), no_proxy_opt);
    proxy_server = pick_proxy_server(http_proxy, socks_proxy);
    to_bypass_list(no_proxy, bypass, sizeof(bypass));

    set_proxy_env(http_proxy, socks_proxy, no_proxy);
    set_browser_proxy_env(proxy_server, bypass);

    resolved[0] = '\0';
    resolved_ok = resolve_exe(argv[cmd_start], resolved, sizeof(resolved));
    if (resolved_ok) {
        is_gui = is_gui_exe(resolved);
        is_chrome = is_chromium_app(resolved);
    }
    if (force_gui)
        is_gui = 1;

    detach = force_gui || (is_gui && !force_wait);
    if (force_wait)
        detach = 0;

    inject_argv = !no_flags && (force_gui || is_chrome);
    if (inject_argv) {
        if (!has_flag_prefix(argc, argv, cmd_start, "--proxy-server"))
            snprintf(proxy_switch, sizeof(proxy_switch), "--proxy-server=%s", proxy_server);
        else
            proxy_switch[0] = '\0';
        if (bypass[0] && !has_flag_prefix(argc, argv, cmd_start, "--proxy-bypass-list"))
            snprintf(bypass_switch, sizeof(bypass_switch), "--proxy-bypass-list=%s", bypass);
        else
            bypass_switch[0] = '\0';
        if (!proxy_switch[0] && !bypass_switch[0])
            inject_argv = 0;
    } else {
        proxy_switch[0] = '\0';
        bypass_switch[0] = '\0';
    }

    if (verbose) {
        const char *v;
        v = getenv("HTTP_PROXY");  fprintf(stderr, "[proxify] HTTP_PROXY  = %s\n", v ? v : "(unset)");
        v = getenv("HTTPS_PROXY"); fprintf(stderr, "[proxify] HTTPS_PROXY = %s\n", v ? v : "(unset)");
        v = getenv("ALL_PROXY");   fprintf(stderr, "[proxify] ALL_PROXY   = %s\n", v ? v : "(unset)");
        v = getenv("NO_PROXY");    fprintf(stderr, "[proxify] NO_PROXY    = %s\n", v ? v : "(unset)");
        v = getenv("WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS");
        fprintf(stderr, "[proxify] WEBVIEW2 / Qt flags = %s\n", v ? v : "(unset)");
        fprintf(stderr, "[proxify] target     = %s\n",
                resolved_ok ? resolved : argv[cmd_start]);
        fprintf(stderr, "[proxify] desktop    = %s%s\n",
                is_gui ? "gui" : "console",
                is_chrome ? ", chromium-kernel" : "");
        fprintf(stderr, "[proxify] detach     = %s\n", detach ? "yes" : "no (wait)");
        if (proxy_switch[0] || bypass_switch[0]) {
            fprintf(stderr, "[proxify] inject     =");
            if (proxy_switch[0]) fprintf(stderr, " %s", proxy_switch);
            if (bypass_switch[0]) fprintf(stderr, " %s", bypass_switch);
            fprintf(stderr, "\n");
        } else {
            fprintf(stderr, "[proxify] inject     = (none)\n");
        }
        fprintf(stderr, "[proxify] exec: %s\n", argv[cmd_start]);
    }

#ifdef _WIN32
    {
        char cmdline[32768];
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        DWORD exitcode;
        DWORD flags = 0;

        memset(&si, 0, sizeof(si));
        memset(&pi, 0, sizeof(pi));
        si.cb = sizeof(si);
        cmdline[0] = '\0';

        for (i = cmd_start; i < argc; i++) {
            if (append_arg(cmdline, sizeof(cmdline), argv[i]) != 0) {
                fprintf(stderr, "proxify: command line too long\n");
                return 1;
            }
        }
        if (proxy_switch[0] && append_arg(cmdline, sizeof(cmdline), proxy_switch) != 0) {
            fprintf(stderr, "proxify: command line too long\n");
            return 1;
        }
        if (bypass_switch[0] && append_arg(cmdline, sizeof(cmdline), bypass_switch) != 0) {
            fprintf(stderr, "proxify: command line too long\n");
            return 1;
        }

        if (detach)
            flags |= DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP;

        if (!CreateProcessA(NULL, cmdline, NULL, NULL, TRUE,
                            flags, NULL, NULL, &si, &pi)) {
            fprintf(stderr, "proxify: failed to launch '%s' (error %lu)\n",
                    argv[cmd_start], GetLastError());
            return 127;
        }

        if (!detach) {
            WaitForSingleObject(pi.hProcess, INFINITE);
            GetExitCodeProcess(pi.hProcess, &exitcode);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            return (int)exitcode;
        }
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return 0;
    }
#else
    {
        char *newargv[256];
        int n = 0;
        int extra = 0;

        if (proxy_switch[0]) extra++;
        if (bypass_switch[0]) extra++;
        if (argc - cmd_start + extra + 1 > 256) {
            fprintf(stderr, "proxify: too many arguments\n");
            return 1;
        }
        for (i = cmd_start; i < argc; i++)
            newargv[n++] = argv[i];
        if (proxy_switch[0])
            newargv[n++] = proxy_switch;
        if (bypass_switch[0])
            newargv[n++] = bypass_switch;
        newargv[n] = NULL;

        if (detach) {
            pid_t pid = fork();
            if (pid < 0) {
                fprintf(stderr, "proxify: fork failed: ");
                perror(NULL);
                return 127;
            }
            if (pid > 0)
                return 0;
            setsid();
        }

        execvp(newargv[0], newargv);
        fprintf(stderr, "proxify: failed to exec '%s': ", argv[cmd_start]);
        perror(NULL);
        return 127;
    }
#endif
}
