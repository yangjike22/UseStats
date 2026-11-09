#!/usr/bin/env python3
"""独立复算核对：用 Python 按 README 的口径定义重新实现一遍聚合，
与 stats.html 里 JS 聚合函数（经 node 抽取运行）的结果逐项比对。

口径（与页面一致）：
- 日总时长 = 当天所有应用 fgSeconds 之和；bgOnly = max(0, bg - fg)。
- 今日/近7天/近30天视图按日历日界定（含今天），缺文件日按 0 计但不算"有数据"。
- 30 天图表按自然周（周一起）聚合。
- 日均 = 最近 7 个有数据日的均值；周均 = 最近 4 个有数据自然周的周总均值；
  月均 = 最近 3 个有数据自然月的月总均值。取整为 JS Math.round（四舍五入）。

运行：python3 tests/recompute.py   （需要 node 与先运行 gen_fixtures.py）
"""
import json
import math
import os
import subprocess
import sys
from datetime import date, timedelta

HERE = os.path.dirname(os.path.abspath(__file__))
TODAY = date(2026, 11, 5)


def js_round(x):
    return math.floor(x + 0.5)


def monday_of(d):
    return d - timedelta(days=d.weekday())


def day_total(doc):
    return sum(a["fgSeconds"] for a in doc["apps"])


def build_view(days_by_date, mode):
    if mode == "today":
        start = TODAY
    elif mode == "7d":
        start = TODAY - timedelta(days=6)
    else:
        start = TODAY - timedelta(days=29)
    dates = [start + timedelta(days=i) for i in range((TODAY - start).days + 1)]
    total = idle = n_data = 0
    hourly = [0] * 24
    apps = {}
    for ds in dates:
        doc = days_by_date.get(ds.isoformat())
        if not doc:
            continue
        n_data += 1
        total += day_total(doc)
        idle += doc["idleSeconds"]
        for a in doc["apps"]:
            for h in range(24):
                hourly[h] += a["hourlyFg"][h]
            e = apps.setdefault(a["path"], {"name": a["name"], "fg": 0, "bg": 0})
            e["fg"] += a["fgSeconds"]
            e["bg"] += a["bgSeconds"]
            e["name"] = a["name"]
    ranking = [
        {"name": e["name"], "fg": e["fg"], "bgOnly": max(0, e["bg"] - e["fg"]),
         "total": e["fg"] + max(0, e["bg"] - e["fg"])}
        for e in apps.values()
    ]
    ranking.sort(key=lambda r: (-r["fg"], -r["total"], r["name"]))
    peak_sec = max(hourly)
    peak_hour = hourly.index(peak_sec) if peak_sec > 0 else -1
    if mode == "today":
        chart = [{"label": f"{h}:00", "seconds": hourly[h]} for h in range(24)]
    elif mode == "7d":
        chart = [{"label": d.isoformat(),
                  "seconds": day_total(days_by_date[d.isoformat()]) if d.isoformat() in days_by_date else 0}
                 for d in dates]
    else:
        weeks = {}
        for d in dates:
            key = monday_of(d).isoformat()
            weeks[key] = weeks.get(key, 0) + (
                day_total(days_by_date[d.isoformat()]) if d.isoformat() in days_by_date else 0)
        chart = [{"label": k, "seconds": weeks[k]} for k in sorted(weeks)]
    return {
        "total": total, "idle": idle, "daysWithData": n_data,
        "peakHour": peak_hour, "peakSeconds": peak_sec,
        "chart": chart,
        "top3": [{"name": r["name"], "fg": r["fg"], "bgOnly": r["bgOnly"]} for r in ranking[:3]],
        "rankCount": len(ranking[:10]),
    }


def averages(days_by_date):
    totals = sorted(
        ((ds, day_total(doc)) for ds, doc in days_by_date.items()),
        key=lambda x: x[0], reverse=True)
    recent = totals[:7]
    daily = js_round(sum(t for _, t in recent) / len(recent)) if recent else 0
    weeks = {}
    for ds, t in totals:
        weeks[monday_of(date.fromisoformat(ds)).isoformat()] = \
            weeks.get(monday_of(date.fromisoformat(ds)).isoformat(), 0) + t
    wkeys = sorted(weeks, reverse=True)[:4]
    weekly = js_round(sum(weeks[k] for k in wkeys) / len(wkeys)) if wkeys else 0
    months = {}
    for ds, t in totals:
        months[ds[:7]] = months.get(ds[:7], 0) + t
    mkeys = sorted(months, reverse=True)[:3]
    monthly = js_round(sum(months[k] for k in mkeys) / len(mkeys)) if mkeys else 0
    return {
        "daily": {"seconds": daily, "count": len(recent)},
        "weekly": {"seconds": weekly, "count": len(wkeys)},
        "monthly": {"seconds": monthly, "count": len(mkeys)},
    }


def main():
    fix = os.path.join(HERE, "fixtures")
    days_by_date = {}
    for fn in sorted(os.listdir(fix)):
        if fn.startswith("usage-") and fn.endswith(".json"):
            doc = json.load(open(os.path.join(fix, fn), encoding="utf-8"))
            # fixture 不变量自检
            for a in doc["apps"]:
                assert sum(a["hourlyFg"]) == a["fgSeconds"], (fn, a["name"])
                assert a["bgSeconds"] >= a["fgSeconds"], (fn, a["name"])
            days_by_date[doc["date"]] = doc
    assert days_by_date, "no fixtures; run gen_fixtures.py first"

    mine = {
        "today": build_view(days_by_date, "today"),
        "d7": build_view(days_by_date, "7d"),
        "d30": build_view(days_by_date, "30d"),
        "averages": averages(days_by_date),
    }
    js = json.loads(subprocess.run(
        ["node", os.path.join(HERE, "check_agg.mjs")],
        capture_output=True, text=True, check=True).stdout)

    fails = []

    def eq(path, a, b):
        if a != b:
            fails.append(f"{path}: python={a!r} js={b!r}")

    for view in ("today", "d7", "d30"):
        for k in ("total", "idle", "daysWithData", "peakHour", "peakSeconds", "rankCount"):
            eq(f"{view}.{k}", mine[view][k], js[view][k])
        eq(f"{view}.top3", mine[view]["top3"], js[view]["top3"])
        eq(f"{view}.chart", mine[view]["chart"], js[view]["chart"])
    eq("averages", mine["averages"], js["averages"])

    if fails:
        print("MISMATCH:")
        for f in fails:
            print(" -", f)
        sys.exit(1)
    print(f"OK: {len(days_by_date)} fixture days; today total={mine['today']['total']}s, "
          f"daily={mine['averages']['daily']}, weekly={mine['averages']['weekly']}, "
          f"monthly={mine['averages']['monthly']}")
    print("OK: JS 聚合（stats.html）与 Python 独立复算逐项一致")


if __name__ == "__main__":
    main()
