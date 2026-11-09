#!/usr/bin/env python3
"""生成聚合核对用的 fixture 数据（tests/fixtures/usage-YYYY-MM-DD.json）。

覆盖 2026-10-20 ~ 2026-11-05：跨 3 个自然周（10-19 / 10-26 / 11-02 起）、
跨 2 个自然月（10 月、11 月）；其中 10-25 与 11-01 两天故意不生成文件，
模拟"程序没运行的日子"。格式与程序真实写出的文件完全一致，
且保证不变量：sum(hourlyFg) == fgSeconds、bgSeconds >= fgSeconds。
确定性伪随机（seed 固定），可重复生成。
"""
import json
import os
import random
from datetime import date, timedelta

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures")
os.makedirs(OUT, exist_ok=True)

APPS = [
    # path, name, 每天前台秒的大致范围, 后台额外秒范围
    (r"C:\Program Files\Google\Chrome\Application\chrome.exe", "Google Chrome", (3600, 12600), (600, 3600)),
    (r"C:\Users\me\AppData\Local\Programs\Microsoft VS Code\Code.exe", "Visual Studio Code", (0, 10800), (0, 1200)),
    (r"C:\Program Files\Tencent\WeChat\WeChat.exe", "微信", (600, 3600), (7200, 14400)),
    (r"C:\Program Files\NetEase\CloudMusic\cloudmusic.exe", "网易云音乐", (0, 1800), (3600, 10800)),
    (r"C:\Windows\explorer.exe", "explorer", (300, 1500), (28800, 32400)),
    (r"C:\Program Files\Microsoft Office\root\Office16\WINWORD.EXE", "Microsoft Word", (0, 5400), (0, 900)),
]


def split_hourly(total, rnd):
    """把 fg 秒按时段权重拆进 24 格，保证总和恰好等于 total。"""
    weights = []
    for h in range(24):
        if 8 <= h <= 11:
            w = rnd.uniform(6, 10)
        elif 14 <= h <= 17:
            w = rnd.uniform(5, 9)
        elif 19 <= h <= 22:
            w = rnd.uniform(3, 7)
        elif h in (12, 13, 18):
            w = rnd.uniform(1, 3)
        else:
            w = rnd.uniform(0, 0.4)
        weights.append(w)
    s = sum(weights)
    raw = [total * w / s for w in weights]
    base = [int(x) for x in raw]
    rem = total - sum(base)
    order = sorted(range(24), key=lambda i: raw[i] - base[i], reverse=True)
    for i in range(rem):
        base[order[i % 24]] += 1
    return base


def main():
    rnd = random.Random(20261105)
    skip = {date(2026, 10, 25), date(2026, 11, 1)}  # 模拟没开机的两天
    day = date(2026, 10, 20)
    end = date(2026, 11, 5)
    written = []
    while day <= end:
        if day not in skip:
            apps = []
            for path, name, fg_rng, bg_rng in APPS:
                fg = rnd.randint(*fg_rng)
                if fg == 0 and rnd.random() < 0.7:
                    continue  # 这天没用这个应用
                bg = fg + rnd.randint(*bg_rng)
                apps.append({
                    "path": path,
                    "name": name,
                    "fgSeconds": fg,
                    "bgSeconds": bg,
                    "cpuSeconds": round(rnd.uniform(0, fg * 0.7), 1),
                    "hourlyFg": split_hourly(fg, rnd),
                })
            apps.sort(key=lambda a: -a["fgSeconds"])
            doc = {
                "date": day.isoformat(),
                "idleSeconds": rnd.randint(600, 5400),
                "apps": apps,
            }
            fp = os.path.join(OUT, f"usage-{day.isoformat()}.json")
            with open(fp, "w", encoding="utf-8") as f:
                json.dump(doc, f, ensure_ascii=False, indent=2)
            written.append(day.isoformat())
        day += timedelta(days=1)
    print(f"fixtures: {len(written)} days -> {OUT}")


if __name__ == "__main__":
    main()
