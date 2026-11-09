#!/usr/bin/env python3
"""把开源字体子集化为 woff2，供 stats.html 离线使用（页面零网络请求）。

字符集 = stats.html 全部出现字符 ∪ GB2312 一级字（3755 常用字，覆盖绝大
多数应用名）∪ 常用符号。稀有字不在子集内时，页面按字体栈回退系统字体，
只是个别字形不同，不缺字成方框的概率极低。
源字体（fonts/src/，均为开源许可）：
- Noto Sans SC（OFL，可变字体 wght 100–900）：界面唯一正文字族，
  固定出 400/500/600/700 四个字重；子集各约 0.7–0.9MB，超页面注入上限，
  走 exe 资源 + 宿主本地供给（stats.cpp 的 FontResourceHandler）。
- IBM Plex Mono Regular/Bold（OFL）：只出现在数字与数据上；子集小，
  以 data URI 直接内嵌页面。
历史产物（Ma Shan Zheng / Caveat 子集）属于已退役的手绘皮肤，不再生成
@font-face；源文件与旧子集保留在 fonts/src、fonts/sub 里备查。
产物：fonts/sub/*.woff2 与 fonts/fonts.css（@font-face 片段，拼进 stats.html）。
"""
import subprocess, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SRC, SUB = HERE / "src", HERE / "sub"
SUB.mkdir(exist_ok=True)

def gb2312_level1():
    chars = set()
    for hi in range(0xB0, 0xD8):
        for lo in range(0xA1, 0xFF):
            try:
                chars.add(bytes([hi, lo]).decode("gb2312"))
            except UnicodeDecodeError:
                pass
    return chars

html = (HERE.parent / "stats.html").read_text(encoding="utf-8")
charset = set(html) | gb2312_level1()
charset |= set("·—…％℃°±×÷≤≥←→↑↓★☆●○◆■▲※、。《》「」『』【】")
charset = {c for c in charset if not c.isspace() or c == " "}
(HERE / "charset.txt").write_text("".join(sorted(charset)), encoding="utf-8")
print("charset size:", len(charset))

def run(cmd):
    print("+", " ".join(cmd[:3]), "...")
    subprocess.run(cmd, check=True)

common = ["--flavor=woff2", "--no-hinting", "--desubroutinize"]
NOTO_WEIGHTS = (400, 500, 600, 700)
for w in NOTO_WEIGHTS:
    static = SUB / f"NotoSansSC-{w}-static.ttf"
    run([sys.executable, "-m", "fontTools.varLib.instancer", str(SRC / "NotoSansSC-var.ttf"),
         f"wght={w}", "-o", str(static)])
    run([sys.executable, "-m", "fontTools.subset", str(static),
         "--text-file=" + str(HERE / "charset.txt"),
         "--output-file=" + str(SUB / f"NotoSansSC-{w}.woff2"),
         "--layout-features=kern", *common])
for w in ("Regular", "Bold"):
    run([sys.executable, "-m", "fontTools.subset", str(SRC / f"IBMPlexMono-{w}.ttf"),
         "--unicodes=U+0020-007E,U+00A0-00FF",
         "--output-file=" + str(SUB / f"IBMPlexMono-{w}.woff2"),
         "--layout-features=", *common])

import base64
def data_face(family, path, weight):
    b64 = base64.b64encode(path.read_bytes()).decode()
    return (f'@font-face{{font-family:"{family}";font-style:normal;'
            f'font-weight:{weight};font-display:swap;'
            f'src:url(data:font/woff2;base64,{b64}) format("woff2");}}')

def url_face(family, filename, weight):
    return (f'@font-face{{font-family:"{family}";font-style:normal;'
            f'font-weight:{weight};font-display:swap;'
            f'src:url("https://usestats.local/fonts/{filename}") format("woff2");}}')

faces = ["/* 内嵌字体（子集化 woff2，构建脚本 fonts/build_fonts.py）：离线可用，页面零网络请求；"
         "Noto Sans SC 走宿主本地供给（见 stats.cpp FontResourceHandler），IBM Plex Mono 为 data URI */"]
for w in NOTO_WEIGHTS:
    faces.append(url_face("Noto Sans SC", f"NotoSansSC-{w}.woff2", str(w)))
faces.append(data_face("IBM Plex Mono", SUB / "IBMPlexMono-Regular.woff2", "400"))
faces.append(data_face("IBM Plex Mono", SUB / "IBMPlexMono-Bold.woff2", "700"))
(HERE / "fonts.css").write_text("\n".join(faces) + "\n", encoding="utf-8")
for p in sorted(SUB.glob("*.woff2")):
    print(p.name, p.stat().st_size)
print("fonts.css written")
