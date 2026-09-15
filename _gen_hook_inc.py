from pathlib import Path

src = Path(__file__).with_name("proxify-hook.js").read_text(encoding="utf-8")
out = Path(__file__).with_name("proxify-hook.inc")
lines = ["/* Generated from proxify-hook.js. Do not edit. */", "static const char kProxifyHookJs[] ="]
for line in src.splitlines(True):
    esc = (
        line.replace("\\", "\\\\")
        .replace('"', '\\"')
        .replace("\r", "\\r")
        .replace("\n", "\\n")
        .replace("\t", "\\t")
    )
    lines.append('    "' + esc + '"')
lines.append(";")
out.write_text("\n".join(lines) + "\n", encoding="utf-8")
print("wrote", out, "bytes", out.stat().st_size)
