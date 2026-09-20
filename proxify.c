/*
 * proxify - Launch any process with proxy environment inherited by all children.
 *
 * Usage:
 *   proxify <proxy_url> <command> [args...]
 *   proxify -s <socks_url> <command> [args...]
 *   proxify -g <proxy_url> <desktop_app.exe>
 *   proxify <proxy_url> -app chatgpt
 *   proxify <proxy_url> /Applications/Cursor.app      (macOS bundle)
 *   proxify -n <hosts> <proxy_url> <command> [args...]
 *
 * Sets: HTTP_PROXY, HTTPS_PROXY, ALL_PROXY, NO_PROXY (and lowercase variants).
 * Browser-kernel desktop apps also receive --proxy-server / --proxy-bypass-list
 * / --disable-quic (and WebView2 / Qt WebEngine equivalents). Loopback is always
 * on the bypass list; -n only adds extra hosts to NO_PROXY and --proxy-bypass-list.
 *
 * Build:
 *   gcc -O2 -o proxify proxify.c        (Linux/macOS)
 *   cl /O2 proxify.c                     (MSVC)
 *   gcc -O2 -o proxify.exe proxify.c     (MinGW)
 */

#ifndef _WIN32
  #define _POSIX_C_SOURCE 200112L
#endif
#ifdef __APPLE__
  #define _DARWIN_C_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #ifndef _WIN32_WINNT
    #define _WIN32_WINNT 0x0601
  #endif
  #include <process.h>
  #include <windows.h>
  #include <tlhelp32.h>
#else
  #include <limits.h>
  #include <fcntl.h>
  #include <unistd.h>
  #include <dirent.h>
  #include <sys/stat.h>
  #include <sys/types.h>
#ifndef PATH_MAX
  #define PATH_MAX 4096
#endif
#ifdef __APPLE__
  #include <libproc.h>
#endif
#endif

#define VERSION "1.5.0"
#define DEFAULT_NO_PROXY "localhost,127.0.0.1,::1"
#define HOOK_MARKER "PROXIFY_NODE_HOOK"

#include "proxify-hook.inc"

static const char kEsmHookPrepend[] =
    "/*PROXIFY_NODE_HOOK*/\n"
    "import{createRequire as __pxCR}from'node:module';\n"
    "try{__pxCR(import.meta.url)('./proxify-hook.cjs')}catch(e){}\n";

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
        /* global-agent (Node) and similar libraries. */
        set_env("GLOBAL_AGENT_HTTP_PROXY", all);
        set_env("GLOBAL_AGENT_HTTPS_PROXY", all);
    }
    set_env("NO_PROXY", no_proxy);
    set_env("no_proxy", no_proxy);
    if (all)
        set_env("GLOBAL_AGENT_NO_PROXY", no_proxy);
    /* Node 22+ undici/fetch honors HTTP_PROXY only when this is set. */
    set_env("NODE_USE_ENV_PROXY", "1");
}

/* WebView2 / Qt WebEngine read these instead of (or in addition to) argv. */
static void set_browser_proxy_env(const char *proxy_server, const char *bypass)
{
    char extra[2048];

    if (bypass && bypass[0])
        snprintf(extra, sizeof(extra),
                 "--proxy-server=%s --proxy-bypass-list=%s --disable-quic --disable-http2",
                 proxy_server, bypass);
    else
        snprintf(extra, sizeof(extra),
                 "--proxy-server=%s --disable-quic --disable-http2", proxy_server);

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
#ifdef __APPLE__
    /* The main binary of a .app bundle is a GUI program. */
    return strstr(path, ".app/Contents/MacOS/") != NULL;
#else
    (void)path;
    return 0;
#endif
}
#endif

#ifdef _WIN32
/* Store (MSIX) apps live in a versioned WindowsApps folder that changes on
 * every update. "appx:<PackageFamilyName>" resolves the current install dir and
 * the main executable from AppxManifest.xml. The kernel32 exports are looked up
 * at run time so old SDKs / TCC still build. */
typedef LONG (WINAPI *pkgs_by_family_fn)(PCWSTR, UINT32 *, PWSTR *, UINT32 *, WCHAR *);
typedef LONG (WINAPI *pkg_path_fn)(PCWSTR, UINT32 *, PWSTR);

static int resolve_appx(const char *family, char *out, size_t n)
{
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    pkgs_by_family_fn get_pkgs;
    pkg_path_fn get_path;
    WCHAR wfamily[256], names[2048], wroot[MAX_PATH];
    PWSTR full[16];
    UINT32 count = 16, len = 2048, plen = MAX_PATH;
    char root[MAX_PATH], manifest[MAX_PATH], rel[MAX_PATH], xml[65536];
    const char *p, *q;
    FILE *f;
    size_t got, i;

    if (!k32)
        return 0;
    get_pkgs = (pkgs_by_family_fn)(void (*)(void))
        GetProcAddress(k32, "GetPackagesByPackageFamily");
    get_path = (pkg_path_fn)(void (*)(void))
        GetProcAddress(k32, "GetPackagePathByFullName");
    if (!get_pkgs || !get_path)
        return 0;
    if (!MultiByteToWideChar(CP_ACP, 0, family, -1, wfamily, 256))
        return 0;
    if (get_pkgs(wfamily, &count, full, &len, names) != ERROR_SUCCESS || count == 0)
        return 0;
    if (get_path(full[0], &plen, wroot) != ERROR_SUCCESS)
        return 0;
    if (!WideCharToMultiByte(CP_ACP, 0, wroot, -1, root, sizeof(root), NULL, NULL))
        return 0;
    if (!path_join(manifest, sizeof(manifest), root, "AppxManifest.xml"))
        return 0;

    f = fopen(manifest, "rb");
    if (!f)
        return 0;
    got = fread(xml, 1, sizeof(xml) - 1, f);
    fclose(f);
    xml[got] = '\0';
    p = strstr(xml, "<Application ");
    if (p)
        p = strstr(p, "Executable=\"");
    if (!p)
        return 0;
    p += 12;
    q = strchr(p, '"');
    if (!q || (size_t)(q - p) >= sizeof(rel))
        return 0;
    for (i = 0; p + i < q; i++)
        rel[i] = (p[i] == '/') ? '\\' : p[i];
    rel[i] = '\0';
    return path_join(out, n, root, rel) && file_exists(out);
}
#endif

#ifdef __APPLE__
/* "Foo.app" -> "Foo.app/Contents/MacOS/<CFBundleExecutable>". Launching through
 * `open` would drop our environment and flags, so the main binary is needed. */
static int resolve_bundle(const char *bundle, char *out, size_t n)
{
    char base[PATH_MAX], plist[PATH_MAX], rel[PATH_MAX];
    char *xml;
    const char *p, *q, *name;
    struct stat st;
    size_t len = strlen(bundle), got;
    int ok = 0;
    FILE *f;

    while (len > 1 && bundle[len - 1] == '/')
        len--;
    if (len < 5 || len >= sizeof(base) || strncmp(bundle + len - 4, ".app", 4) != 0)
        return 0;
    memcpy(base, bundle, len);
    base[len] = '\0';
    if (stat(base, &st) != 0 || !S_ISDIR(st.st_mode))
        return 0;

    if (path_join(plist, sizeof(plist), base, "Contents/Info.plist") &&
        stat(plist, &st) == 0 && st.st_size > 0 && st.st_size < (1 << 22) &&
        (f = fopen(plist, "rb")) != NULL) {
        xml = (char *)malloc((size_t)st.st_size + 1);
        got = xml ? fread(xml, 1, (size_t)st.st_size, f) : 0;
        fclose(f);
        if (xml) {
            xml[got] = '\0';
            p = strstr(xml, "<key>CFBundleExecutable</key>");
            if (p)
                p = strstr(p, "<string>");
            q = p ? strstr(p, "</string>") : NULL;
            ok = q && snprintf(rel, sizeof(rel), "Contents/MacOS/%.*s",
                               (int)(q - (p + 8)), p + 8) < (int)sizeof(rel) &&
                 path_join(out, n, base, rel) && file_exists(out);
            free(xml);
            if (ok)
                return 1;
        }
    }
    /* Binary plist or no key: the executable is usually named after the bundle. */
    name = base;
    for (p = base; *p; p++) {
        if (*p == '/')
            name = p + 1;
    }
    if (snprintf(rel, sizeof(rel), "Contents/MacOS/%.*s",
                 (int)(strlen(name) - 4), name) >= (int)sizeof(rel))
        return 0;
    return path_join(out, n, base, rel) && file_exists(out);
}

/* "app:ChatGPT|Codex" -> first of those bundles found in /Applications or
 * ~/Applications. */
static int resolve_app_name(const char *names, char *out, size_t n)
{
    const char *home = getenv("HOME");
    const char *p = names;

    while (*p) {
        const char *bar = strchr(p, '|');
        int len = bar ? (int)(bar - p) : (int)strlen(p);
        char bundle[PATH_MAX];

        if (len > 0) {
            if (snprintf(bundle, sizeof(bundle), "/Applications/%.*s.app", len, p) <
                    (int)sizeof(bundle) && resolve_bundle(bundle, out, n))
                return 1;
            if (home && snprintf(bundle, sizeof(bundle), "%s/Applications/%.*s.app",
                                 home, len, p) < (int)sizeof(bundle) &&
                resolve_bundle(bundle, out, n))
                return 1;
        }
        if (!bar) break;
        p = bar + 1;
    }
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
        "resources\\app\\product.json",
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
        "resources/app/product.json",
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
            if (path_join(fw, sizeof(fw), contents, "Resources/app.asar") &&
                file_exists(fw))
                return 1;
            /* Apps rename the framework ("Codex Framework.framework" in
             * ChatGPT), so look for Chromium's resources inside any of them. */
            if (path_join(fw, sizeof(fw), contents, "Frameworks")) {
                DIR *d = opendir(fw);
                struct dirent *e;
                int found = 0;

                while (d && !found && (e = readdir(d)) != NULL) {
                    size_t len = strlen(e->d_name);
                    if (len <= 10 || strcmp(e->d_name + len - 10, ".framework") != 0)
                        continue;
                    if (snprintf(cand, sizeof(cand), "%s/%s/Resources/chrome_100_percent.pak",
                                 fw, e->d_name) < (int)sizeof(cand) && file_exists(cand))
                        found = 1;
                }
                if (d)
                    closedir(d);
                if (found)
                    return 1;
            }
        }
    }
#endif
    return 0;
}

static int is_vscode_family(const char *exe)
{
    char dir[EXE_PATH_MAX], cand[EXE_PATH_MAX];

    path_dirname(exe, dir, sizeof(dir));
#ifdef _WIN32
    if (path_join(cand, sizeof(cand), dir, "resources\\app\\product.json") &&
        file_exists(cand))
        return 1;
#else
    if (path_join(cand, sizeof(cand), dir, "resources/app/product.json") &&
        file_exists(cand))
        return 1;
    if (strstr(dir, ".app/Contents/MacOS")) {
        char contents[EXE_PATH_MAX];
        path_dirname(dir, contents, sizeof(contents));
        if (path_join(cand, sizeof(cand), contents, "Resources/app/product.json") &&
            file_exists(cand))
            return 1;
    }
#endif
    return 0;
}

#if defined(_WIN32) || defined(__APPLE__)
static const char *path_basename(const char *path)
{
    const char *base = path, *p;

    for (p = path; *p; p++) {
        if (*p == '/' || *p == '\\')
            base = p + 1;
    }
    return base;
}
#endif

#ifdef _WIN32
static DWORD find_running_exe(const char *exe)
{
    HANDLE snap;
    PROCESSENTRY32 pe;
    DWORD self = GetCurrentProcessId();
    DWORD found = 0;

    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return 0;
    pe.dwSize = sizeof(pe);
    if (!Process32First(snap, &pe)) {
        CloseHandle(snap);
        return 0;
    }
    do {
        HANDLE h;
        char path[MAX_PATH];
        DWORD n = MAX_PATH;

        if (pe.th32ProcessID == self)
            continue;
        h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!h)
            continue;
        if (QueryFullProcessImageNameA(h, 0, path, &n) &&
            _stricmp(path, exe) == 0)
            found = pe.th32ProcessID;
        CloseHandle(h);
        if (found)
            break;
    } while (Process32Next(snap, &pe));
    CloseHandle(snap);
    return found;
}
#elif defined(__APPLE__)
static pid_t find_running_exe(const char *exe)
{
    static pid_t pids[8192];
    char path[PROC_PIDPATHINFO_MAXSIZE];
    pid_t self = getpid();
    int n, i;

    n = proc_listallpids(pids, (int)sizeof(pids));
    for (i = 0; i < n; i++) {
        if (pids[i] <= 0 || pids[i] == self)
            continue;
        if (proc_pidpath(pids[i], path, sizeof(path)) > 0 && strcmp(path, exe) == 0)
            return pids[i];
    }
    return 0;
}
#endif

static int write_bytes(const char *path, const char *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    size_t w;

    if (!f)
        return 0;
    w = fwrite(data, 1, len, f);
    fclose(f);
    return w == len;
}

static int looks_like_esm(const char *head)
{
    return strstr(head, "import.meta") != NULL ||
           strstr(head, "export function") != NULL ||
           strstr(head, "export default") != NULL ||
           strstr(head, "export const") != NULL;
}

static void slash_path(const char *in, char *out, size_t n)
{
    size_t j = 0;

    for (; *in && j + 1 < n; in++)
        out[j++] = (*in == '\\') ? '/' : *in;
    out[j] = '\0';
}

static size_t hook_prefix_len(const char *data, size_t len)
{
    size_t i = 0;

    if (len < 21 || strncmp(data, "/*" HOOK_MARKER "*/", 21) != 0)
        return 0;
    while (i < len) {
        size_t start = i;
        char line[768];
        size_t n;

        while (i < len && data[i] != '\n')
            i++;
        if (i < len)
            i++;
        n = i - start;
        if (n >= sizeof(line))
            n = sizeof(line) - 1;
        memcpy(line, data + start, n);
        line[n] = '\0';
        if (strstr(line, HOOK_MARKER) || strstr(line, "__pxCR") ||
            strstr(line, "proxify-hook"))
            continue;
        return start;
    }
    return i;
}

static int apply_hook_snippet(const char *path, const char *abs_hook_slash, int verbose)
{
    FILE *f;
    char *data, *out;
    size_t total, skip, plen, rest;
    long pos;
    char cjs_prefix[EXE_PATH_MAX + 160];
    const char *prefix;
    const char *head;

    f = fopen(path, "rb");
    if (!f)
        return -1;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    pos = ftell(f);
    if (pos < 0) {
        fclose(f);
        return -1;
    }
    total = (size_t)pos;
    data = (char *)malloc(total + 1);
    if (!data) {
        fclose(f);
        return -1;
    }
    if (fseek(f, 0, SEEK_SET) != 0 || fread(data, 1, total, f) != total) {
        free(data);
        fclose(f);
        return -1;
    }
    fclose(f);
    data[total] = '\0';
    skip = hook_prefix_len(data, total);
    head = data + skip;
    if (looks_like_esm(head) && strstr(path, "bootstrap-fork") != NULL)
        prefix = kEsmHookPrepend;
    else {
        snprintf(cjs_prefix, sizeof(cjs_prefix),
                 "/*PROXIFY_NODE_HOOK*/try{require(\"%s\")}catch(e){}\n",
                 abs_hook_slash);
        prefix = cjs_prefix;
    }
    plen = strlen(prefix);
    rest = total - skip;
    if (skip == plen && strncmp(data, prefix, plen) == 0) {
        free(data);
        if (verbose)
            fprintf(stderr, "[proxify] node-hook already in %s\n", path);
        return 0;
    }
    out = (char *)malloc(plen + rest);
    if (!out) {
        free(data);
        return -1;
    }
    memcpy(out, prefix, plen);
    if (rest)
        memcpy(out + plen, data + skip, rest);
    if (!write_bytes(path, out, plen + rest)) {
        free(out);
        free(data);
        return -1;
    }
    free(out);
    free(data);
    return 1;
}

static int strip_hook_snippet(const char *path, int verbose)
{
    FILE *f;
    char *data;
    size_t total, skip;
    long pos;

    f = fopen(path, "rb");
    if (!f)
        return -1;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    pos = ftell(f);
    if (pos < 0) {
        fclose(f);
        return -1;
    }
    total = (size_t)pos;
    data = (char *)malloc(total + 1);
    if (!data) {
        fclose(f);
        return -1;
    }
    if (fseek(f, 0, SEEK_SET) != 0 || fread(data, 1, total, f) != total) {
        free(data);
        fclose(f);
        return -1;
    }
    fclose(f);
    data[total] = '\0';
    skip = hook_prefix_len(data, total);
    if (skip == 0) {
        free(data);
        return 0;
    }
    if (!write_bytes(path, data + skip, total - skip)) {
        free(data);
        return -1;
    }
    free(data);
    (void)verbose;
    return 1;
}

static int copy_file(const char *src, const char *dst)
{
    FILE *in, *out;
    char buf[8192];
    size_t n;

    in = fopen(src, "rb");
    if (!in)
        return 0;
    out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return 0;
    }
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            fclose(in);
            fclose(out);
            return 0;
        }
    }
    fclose(in);
    fclose(out);
    return 1;
}

static int sibling_hook_js(char *out, size_t n, const char *argv0)
{
    char self[EXE_PATH_MAX], dir[EXE_PATH_MAX];

#ifdef _WIN32
    {
        DWORD r = GetModuleFileNameA(NULL, self, (DWORD)sizeof(self));
        if (r == 0 || r >= sizeof(self))
            snprintf(self, sizeof(self), "%s", argv0 ? argv0 : "");
    }
#elif defined(__linux__)
    {
        ssize_t r = readlink("/proc/self/exe", self, sizeof(self) - 1);
        if (r > 0)
            self[r] = '\0';
        else
            snprintf(self, sizeof(self), "%s", argv0 ? argv0 : "");
    }
#else
    snprintf(self, sizeof(self), "%s", argv0 ? argv0 : "");
#endif
    path_dirname(self, dir, sizeof(dir));
    return path_join(out, n, dir, "proxify-hook.js") && file_exists(out);
}

static int vscode_app_dir(const char *exe, char *out, size_t n)
{
    char dir[EXE_PATH_MAX], cand[EXE_PATH_MAX];

    path_dirname(exe, dir, sizeof(dir));
#ifdef _WIN32
    if (path_join(cand, sizeof(cand), dir, "resources\\app") && file_exists(cand)) {
        snprintf(out, n, "%s", cand);
        return 1;
    }
#else
    if (path_join(cand, sizeof(cand), dir, "resources/app") && file_exists(cand)) {
        snprintf(out, n, "%s", cand);
        return 1;
    }
    if (strstr(dir, ".app/Contents/MacOS")) {
        char contents[EXE_PATH_MAX];
        path_dirname(dir, contents, sizeof(contents));
        if (path_join(cand, sizeof(cand), contents, "Resources/app") &&
            file_exists(cand)) {
            snprintf(out, n, "%s", cand);
            return 1;
        }
    }
#endif
    return 0;
}

static int inject_vscode_node_hook(const char *exe, const char *argv0, int verbose)
{
    char app[EXE_PATH_MAX], hook[EXE_PATH_MAX], cand[EXE_PATH_MAX];
    char sibling[EXE_PATH_MAX], hook_slash[EXE_PATH_MAX];
    int i, patched = 0, failed = 0;
#ifdef _WIN32
    /* Only bootstrap-fork.js: Cursor verifies extension hashes (always-local
     * dist/main.js). Patching those files disables Connect transport. */
    static const char *const targets[] = {
        "out\\bootstrap-fork.js",
        NULL
    };
    static const char *const revert_targets[] = {
        "extensions\\cursor-always-local\\dist\\main.js",
        "extensions\\cursor-agent-worker\\dist\\main.js",
        "extensions\\cursor-agent-host\\dist\\main.js",
        NULL
    };
#else
    static const char *const targets[] = {
        "out/bootstrap-fork.js",
        NULL
    };
    static const char *const revert_targets[] = {
        "extensions/cursor-always-local/dist/main.js",
        "extensions/cursor-agent-worker/dist/main.js",
        "extensions/cursor-agent-host/dist/main.js",
        NULL
    };
#endif

    if (!vscode_app_dir(exe, app, sizeof(app)))
        return 0;
#ifdef _WIN32
    if (!path_join(hook, sizeof(hook), app, "out\\proxify-hook.cjs"))
        return 0;
#else
    if (!path_join(hook, sizeof(hook), app, "out/proxify-hook.cjs"))
        return 0;
#endif
    if (sibling_hook_js(sibling, sizeof(sibling), argv0)) {
        if (!copy_file(sibling, hook)) {
            fprintf(stderr, "[proxify] WARNING: could not copy Node proxy hook\n");
            return 0;
        }
    } else if (!write_bytes(hook, kProxifyHookJs, strlen(kProxifyHookJs))) {
        fprintf(stderr, "[proxify] WARNING: could not write Node proxy hook\n");
        return 0;
    }
    slash_path(hook, hook_slash, sizeof(hook_slash));
    set_env("PROXIFY_NODE_HOOK", hook);
    if (verbose)
        set_env("PROXIFY_HOOK_DEBUG", "1");

    for (i = 0; revert_targets[i]; i++) {
        int pr;
        if (!path_join(cand, sizeof(cand), app, revert_targets[i]) || !file_exists(cand))
            continue;
        pr = strip_hook_snippet(cand, verbose);
        if (pr > 0)
            fprintf(stderr, "[proxify] restored extension file %s\n", cand);
        else if (pr < 0)
            fprintf(stderr, "[proxify] WARNING: could not restore %s\n", cand);
    }

    for (i = 0; targets[i]; i++) {
        int pr;
        if (!path_join(cand, sizeof(cand), app, targets[i]) || !file_exists(cand))
            continue;
        pr = apply_hook_snippet(cand, hook_slash, verbose);
        if (pr > 0) {
            patched++;
            fprintf(stderr, "[proxify] patched Node entry %s\n", cand);
        } else if (pr < 0) {
            failed++;
            fprintf(stderr, "[proxify] WARNING: could not patch %s\n", cand);
        }
    }
    fprintf(stderr, "[proxify] node-hook   = %s\n", hook);
    return patched;
}

/* -app <name>: short names for apps whose real target nobody can remember. */
static const struct { const char *name; const char *target; } kApps[] = {
#ifdef _WIN32
    { "chatgpt", "appx:OpenAI.Codex_2p2nqsd0c76g0" },
    { "codex",   "appx:OpenAI.Codex_2p2nqsd0c76g0" },
#elif defined(__APPLE__)
    /* Same Electron package as on Windows (com.openai.codex); older installs
     * carry it as Codex.app. */
    { "chatgpt", "app:ChatGPT|Codex" },
    { "codex",   "app:Codex|ChatGPT" },
#endif
    { NULL, NULL }
};

static const char *app_target(const char *name)
{
    int i;

    for (i = 0; kApps[i].name; i++) {
        if (token_eq(name, strlen(name), kApps[i].name, strlen(kApps[i].name)))
            return kApps[i].target;
    }
    return NULL;
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

#ifdef _WIN32
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
#endif

static void usage(void)
{
    fprintf(stderr,
        "proxify v" VERSION " - run any command or desktop app through a proxy\n\n"
        "Usage: proxify [options] <command> [args...]\n\n"
        "Options:\n"
        "  <url>           HTTP/HTTPS proxy (positional, before -s or command)\n"
        "  -s <url>        SOCKS proxy\n"
        "  -app <name>     Launch a known app instead of <command>: chatgpt, codex\n"
        "  -n <hosts>      Extra bypass hosts (loopback is always included:\n"
        "                  " DEFAULT_NO_PROXY ")\n"
        "  -g, --gui       Desktop mode: detach and inject browser proxy flags\n"
        "  -w, --wait      Wait for the process (default for console programs)\n"
        "  --no-flags      Do not append Chromium proxy flags (--proxy-server, --disable-http2, ...)\n"
        "  --no-hook       Do not patch Cursor/VS Code JS to proxy Node http2\n"
        "  -v              Show proxy settings before launching\n"
        "  -h              Show this help\n\n"
        "Desktop / browser-kernel apps (Electron, CEF, Chromium, WebView2, Qt):\n"
        "  Detected automatically. Env vars alone are not enough on Windows;\n"
        "  proxify also passes --proxy-server, --proxy-bypass-list, --disable-quic, --disable-http2.\n"
        "  Cursor / VS Code: also injects a Node hook so Agent HTTP/2 uses CONNECT.\n"
        "  Fully Quit the app first (single-instance otherwise ignores new flags).\n"
        "  Windows Store apps: use appx:<PackageFamilyName> as the command, e.g.\n"
        "  appx:OpenAI.Codex_2p2nqsd0c76g0 (ChatGPT). Survives app updates.\n"
        "  macOS: the command may be a bundle (/Applications/Cursor.app) or\n"
        "  app:<Name> (app:Cursor). Never `open -a`: it drops env and flags.\n\n"
        "Examples:\n"
        "  proxify http://127.0.0.1:8080 curl https://example.com\n"
        "  proxify -n \"10.0.0.0/8,.corp.local\" http://127.0.0.1:8080 app.exe\n"
        "  proxify -g http://127.0.0.1:8081 Antigravity.exe\n"
        "  proxify http://127.0.0.1:8080 -app chatgpt\n");
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
    int no_hook = 0;
    int cmd_start = 0;
    int i;
    int is_gui = 0;
    int is_chrome = 0;
    int is_vscode = 0;
    int inject_argv = 0;
    int detach = 0;
    int resolved_ok = 0;
    int from_app = 0;
    char resolved[EXE_PATH_MAX];
    char workdir[EXE_PATH_MAX];
    char no_proxy[2048];
    char bypass[1024];
    char proxy_switch[768];
    char bypass_switch[1280];
    char quic_switch[32];
    char http2_switch[32];

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
        } else if (strcmp(argv[i], "--no-hook") == 0) {
            no_hook = 1;
        } else if (strcmp(argv[i], "-s") == 0) {
            if (++i >= argc) { fprintf(stderr, "proxify: -s requires an argument\n"); return 1; }
            socks_proxy = argv[i];
        } else if (strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "-app") == 0 ||
                   strcmp(argv[i], "--app") == 0) {
            const char *target;
            int k;
            if (++i >= argc) { fprintf(stderr, "proxify: -app requires a name\n"); return 1; }
            target = app_target(argv[i]);
            if (!target) {
                fprintf(stderr, "proxify: unknown app '%s'. Known:", argv[i]);
                for (k = 0; kApps[k].name; k++)
                    fprintf(stderr, " %s", kApps[k].name);
                fprintf(stderr, "\n");
                return 1;
            }
            /* The name becomes the command; anything after it goes to the app. */
            argv[i] = (char *)target;
            from_app = 1;
            cmd_start = i;
            break;
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

#ifdef _WIN32
    if (strncmp(argv[cmd_start], "appx:", 5) == 0) {
        static char appx_exe[MAX_PATH];
        if (!resolve_appx(argv[cmd_start] + 5, appx_exe, sizeof(appx_exe))) {
            fprintf(stderr, "proxify: Store package '%s' not found "
                    "(PowerShell: Get-AppxPackage | select PackageFamilyName)\n",
                    argv[cmd_start] + 5);
            return 127;
        }
        argv[cmd_start] = appx_exe;
    }
#elif defined(__APPLE__)
    {
        static char bundle_exe[PATH_MAX];
        if (strncmp(argv[cmd_start], "app:", 4) == 0) {
            if (!resolve_app_name(argv[cmd_start] + 4, bundle_exe, sizeof(bundle_exe))) {
                fprintf(stderr, "proxify: app '%s' not found in /Applications "
                        "or ~/Applications\n", argv[cmd_start] + 4);
                return 127;
            }
            argv[cmd_start] = bundle_exe;
        } else if (resolve_bundle(argv[cmd_start], bundle_exe, sizeof(bundle_exe))) {
            argv[cmd_start] = bundle_exe;
        }
    }
#endif

    resolved[0] = '\0';
    workdir[0] = '\0';
    resolved_ok = resolve_exe(argv[cmd_start], resolved, sizeof(resolved));
    if (resolved_ok) {
        is_gui = is_gui_exe(resolved);
        is_chrome = is_chromium_app(resolved);
        is_vscode = is_vscode_family(resolved);
        if (is_vscode)
            is_chrome = 1;
        path_dirname(resolved, workdir, sizeof(workdir));
        if (is_vscode && !no_hook)
            inject_vscode_node_hook(resolved, argv[0], verbose);
    }
    if (force_gui)
        is_gui = 1;

    detach = force_gui || (is_gui && !force_wait);
    if (force_wait)
        detach = 0;

    inject_argv = !no_flags && (force_gui || is_chrome);
    proxy_switch[0] = '\0';
    bypass_switch[0] = '\0';
    quic_switch[0] = '\0';
    http2_switch[0] = '\0';
    if (inject_argv) {
        if (!has_flag_prefix(argc, argv, cmd_start, "--proxy-server"))
            snprintf(proxy_switch, sizeof(proxy_switch), "--proxy-server=%s", proxy_server);
        if (bypass[0] && !has_flag_prefix(argc, argv, cmd_start, "--proxy-bypass-list"))
            snprintf(bypass_switch, sizeof(bypass_switch), "--proxy-bypass-list=%s", bypass);
        if (!has_flag_prefix(argc, argv, cmd_start, "--disable-quic"))
            snprintf(quic_switch, sizeof(quic_switch), "--disable-quic");
        if (!has_flag_prefix(argc, argv, cmd_start, "--disable-http2"))
            snprintf(http2_switch, sizeof(http2_switch), "--disable-http2");
        if (!proxy_switch[0] && !bypass_switch[0] && !quic_switch[0] && !http2_switch[0])
            inject_argv = 0;
    }

    if (verbose) {
        const char *v;
        v = getenv("HTTP_PROXY");  fprintf(stderr, "[proxify] HTTP_PROXY  = %s\n", v ? v : "(unset)");
        v = getenv("HTTPS_PROXY"); fprintf(stderr, "[proxify] HTTPS_PROXY = %s\n", v ? v : "(unset)");
        v = getenv("ALL_PROXY");   fprintf(stderr, "[proxify] ALL_PROXY   = %s\n", v ? v : "(unset)");
        v = getenv("NO_PROXY");    fprintf(stderr, "[proxify] NO_PROXY    = %s\n", v ? v : "(unset)");
        v = getenv("NODE_USE_ENV_PROXY");
        fprintf(stderr, "[proxify] NODE_USE_ENV_PROXY = %s\n", v ? v : "(unset)");
        v = getenv("WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS");
        fprintf(stderr, "[proxify] WEBVIEW2 / Qt flags = %s\n", v ? v : "(unset)");
        fprintf(stderr, "[proxify] target     = %s\n",
                resolved_ok ? resolved : argv[cmd_start]);
        fprintf(stderr, "[proxify] desktop    = %s%s%s\n",
                is_gui ? "gui" : "console",
                is_chrome ? ", chromium-kernel" : "",
                is_vscode ? ", vscode-family" : "");
        fprintf(stderr, "[proxify] detach     = %s\n", detach ? "yes" : "no (wait)");
        if (proxy_switch[0] || bypass_switch[0] || quic_switch[0] || http2_switch[0]) {
            fprintf(stderr, "[proxify] inject     =");
            if (proxy_switch[0]) fprintf(stderr, " %s", proxy_switch);
            if (bypass_switch[0]) fprintf(stderr, " %s", bypass_switch);
            if (quic_switch[0]) fprintf(stderr, " %s", quic_switch);
            if (http2_switch[0]) fprintf(stderr, " %s", http2_switch);
            fprintf(stderr, "\n");
        } else {
            fprintf(stderr, "[proxify] inject     = (none)\n");
        }
        fprintf(stderr, "[proxify] exec: %s\n", argv[cmd_start]);
    }

#if defined(_WIN32) || defined(__APPLE__)
    if (resolved_ok && (is_gui || is_chrome || is_vscode)) {
        unsigned long running = (unsigned long)find_running_exe(resolved);
        if (running) {
            fprintf(stderr,
                "[proxify] WARNING: %s is already running (pid %lu).\n"
                "[proxify] Electron/VS Code reuse the first instance and ignore new proxy flags.\n",
                path_basename(resolved), running);
#ifdef _WIN32
            fprintf(stderr,
                "[proxify] Fully Quit from the tray, or: taskkill /F /IM %s\n",
                path_basename(resolved));
#else
            fprintf(stderr,
                "[proxify] Fully Quit it first (Cmd+Q; closing the window is not enough),\n"
                "[proxify] or: kill %lu\n", running);
#endif
        }
    }
#endif
#ifdef __APPLE__
    if (from_app && resolved_ok && !is_chrome) {
        fprintf(stderr,
            "[proxify] note: %s is not the Electron build (old native ChatGPT?).\n"
            "[proxify]       Native macOS apps follow the system proxy and ignore these\n"
            "[proxify]       settings. Update ChatGPT to the current version.\n",
            resolved);
    }
#else
    (void)from_app;
#endif
    if (is_vscode && no_hook) {
        fprintf(stderr,
            "[proxify] note: --no-hook set. Cursor Agent HTTP/2 will not use the proxy\n"
            "[proxify]       unless settings.json has http.proxy and disableHttp2.\n");
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
        if (quic_switch[0] && append_arg(cmdline, sizeof(cmdline), quic_switch) != 0) {
            fprintf(stderr, "proxify: command line too long\n");
            return 1;
        }
        if (http2_switch[0] && append_arg(cmdline, sizeof(cmdline), http2_switch) != 0) {
            fprintf(stderr, "proxify: command line too long\n");
            return 1;
        }

        if (detach)
            flags |= DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP;

        /* A detached app must not inherit our stdout/stderr: it would hold the
         * pipe open and hang callers such as "proxify ... | findstr". */
        if (!CreateProcessA(NULL, cmdline, NULL, NULL, detach ? FALSE : TRUE,
                            flags, NULL,
                            workdir[0] ? workdir : NULL, &si, &pi)) {
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
        if (quic_switch[0]) extra++;
        if (http2_switch[0]) extra++;
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
        if (quic_switch[0])
            newargv[n++] = quic_switch;
        if (http2_switch[0])
            newargv[n++] = http2_switch;
        newargv[n] = NULL;

        if (workdir[0] && chdir(workdir) != 0) {
            fprintf(stderr, "proxify: chdir '%s': ", workdir);
            perror(NULL);
        }

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
            /* Same reason as on Windows: do not keep the caller's pipe open. */
            {
                int fd = open("/dev/null", O_RDWR);
                if (fd >= 0) {
                    dup2(fd, 0);
                    dup2(fd, 1);
                    dup2(fd, 2);
                    if (fd > 2)
                        close(fd);
                }
            }
        }

        execvp(newargv[0], newargv);
        fprintf(stderr, "proxify: failed to exec '%s': ", argv[cmd_start]);
        perror(NULL);
        return 127;
    }
#endif
}
