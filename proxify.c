/*
 * proxify - Launch any process with proxy environment inherited by all children.
 *
 * Usage:
 *   proxify <proxy_url> <command> [args...]
 *   proxify -s <socks_url> <command> [args...]
 *   proxify <proxy_url> -s <socks_url> <command> [args...]
 *
 * Examples:
 *   proxify http://127.0.0.1:8080 curl https://example.com
 *   proxify socks5://127.0.0.1:1080 wget https://example.com
 *   proxify http://127.0.0.1:8080 -s socks5://127.0.0.1:1080 my_app.exe
 *
 * Sets: HTTP_PROXY, HTTPS_PROXY, ALL_PROXY, NO_PROXY (and lowercase variants)
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
  #include <unistd.h>
  #include <sys/wait.h>
#endif

#define VERSION "1.0.0"

static void set_env(const char *name, const char *val)
{
#ifdef _WIN32
    SetEnvironmentVariableA(name, val);
    /* Also push into CRT env for _spawnvp */
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s=%s", name, val);
    _putenv(buf);
#else
    setenv(name, val, 1);
#endif
}

static void set_proxy_env(const char *http, const char *socks)
{
    const char *all = http ? http : socks;

    if (http) {
        set_env("HTTP_PROXY",  http);
        set_env("http_proxy",  http);
        set_env("HTTPS_PROXY", http);
        set_env("https_proxy", http);
    }
    if (socks) {
        /* SOCKS overrides ALL_PROXY; if no http given, also set HTTP(S)_PROXY */
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
    /* Respect existing NO_PROXY, or set empty */
    if (!getenv("NO_PROXY")) {
        set_env("NO_PROXY",  "localhost,127.0.0.1,::1");
        set_env("no_proxy",  "localhost,127.0.0.1,::1");
    }
}

static void usage(void)
{
    fprintf(stderr,
        "proxify v" VERSION " - run any command through a proxy\n\n"
        "Usage: proxify [options] <command> [args...]\n\n"
        "Options:\n"
        "  <url>         HTTP/HTTPS proxy (positional, before -s or command)\n"
        "  -s <url>      SOCKS proxy\n"
        "  -n <hosts>    NO_PROXY hosts (default: localhost,127.0.0.1,::1)\n"
        "  -v            Show proxy settings before launching\n"
        "  -h            Show this help\n\n"
        "Examples:\n"
        "  proxify http://127.0.0.1:8080 my_app.exe\n"
        "  proxify socks5://127.0.0.1:1080 curl https://example.com\n"
        "  proxify http://127.0.0.1:8080 -s socks5://127.0.0.1:1080 app arg1\n");
}

int main(int argc, char *argv[])
{
    const char *http_proxy = NULL;
    const char *socks_proxy = NULL;
    const char *no_proxy = NULL;
    int verbose = 0;
    int cmd_start = 0;
    int i;

    if (argc < 2) { usage(); return 1; }

    /* Parse options */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(); return 0;
        } else if (strcmp(argv[i], "-v") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "-s") == 0) {
            if (++i >= argc) { fprintf(stderr, "proxify: -s requires an argument\n"); return 1; }
            socks_proxy = argv[i];
        } else if (strcmp(argv[i], "-n") == 0) {
            if (++i >= argc) { fprintf(stderr, "proxify: -n requires an argument\n"); return 1; }
            no_proxy = argv[i];
        } else if (!http_proxy && argv[i][0] != '-' &&
                   (strstr(argv[i], "://") != NULL)) {
            /* First bare URL-like arg => http proxy */
            http_proxy = argv[i];
        } else {
            /* This is the command */
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

    /* Set environment */
    set_proxy_env(http_proxy, socks_proxy);
    if (no_proxy) {
        set_env("NO_PROXY", no_proxy);
        set_env("no_proxy", no_proxy);
    }

    if (verbose) {
        const char *v;
        v = getenv("HTTP_PROXY");  fprintf(stderr, "[proxify] HTTP_PROXY  = %s\n", v ? v : "(unset)");
        v = getenv("HTTPS_PROXY"); fprintf(stderr, "[proxify] HTTPS_PROXY = %s\n", v ? v : "(unset)");
        v = getenv("ALL_PROXY");   fprintf(stderr, "[proxify] ALL_PROXY   = %s\n", v ? v : "(unset)");
        v = getenv("NO_PROXY");    fprintf(stderr, "[proxify] NO_PROXY    = %s\n", v ? v : "(unset)");
        fprintf(stderr, "[proxify] exec: %s\n", argv[cmd_start]);
    }

    /* Launch the command */
#ifdef _WIN32
    {
        /* Build command line string for CreateProcess */
        char cmdline[32768] = {0};
        STARTUPINFOA si = {0};
        PROCESS_INFORMATION pi = {0};
        DWORD exitcode;

        si.cb = sizeof(si);

        for (i = cmd_start; i < argc; i++) {
            /* Quote args containing spaces */
            int has_space = (strchr(argv[i], ' ') != NULL);
            if (i > cmd_start) strcat(cmdline, " ");
            if (has_space) strcat(cmdline, "\"");
            strcat(cmdline, argv[i]);
            if (has_space) strcat(cmdline, "\"");
        }

        if (!CreateProcessA(NULL, cmdline, NULL, NULL, TRUE,
                            0, NULL, NULL, &si, &pi)) {
            fprintf(stderr, "proxify: failed to launch '%s' (error %lu)\n",
                    argv[cmd_start], GetLastError());
            return 127;
        }

        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &exitcode);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return (int)exitcode;
    }
#else
    {
        /* On Unix, just execvp - replaces current process, env is inherited */
        execvp(argv[cmd_start], &argv[cmd_start]);
        /* If we get here, exec failed */
        fprintf(stderr, "proxify: failed to exec '%s': ", argv[cmd_start]);
        perror(NULL);
        return 127;
    }
#endif
}
