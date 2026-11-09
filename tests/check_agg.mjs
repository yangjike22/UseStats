// 从 stats.html 抽出 // ==AGG-BEGIN== ~ // ==AGG-END== 的纯聚合函数，
// 用 tests/fixtures 的数据跑出结果 JSON（供 recompute.py 独立复算后比对）。
import { readFileSync, readdirSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const html = readFileSync(join(here, "..", "stats.html"), "utf8");
const m = html.match(/\/\/ ==AGG-BEGIN==([\s\S]*?)\/\/ ==AGG-END==/);
if (!m) { console.error("AGG markers not found"); process.exit(1); }
const agg = new Function(m[1] + "\nreturn { aggBuildView, aggComputeAverages };")();

const fixDir = join(here, "fixtures");
const days = readdirSync(fixDir)
  .filter((f) => /^usage-\d{4}-\d{2}-\d{2}\.json$/.test(f))
  .sort()
  .map((f) => JSON.parse(readFileSync(join(fixDir, f), "utf8")));

const TODAY = "2026-11-05"; // fixture 的最后一天
const data = { today: TODAY, days };
const slim = (v) => ({
  total: v.total, idle: v.idle, daysWithData: v.daysWithData,
  peakHour: v.peakHour, peakSeconds: v.peakSeconds,
  chart: v.chart, top3: v.ranking.slice(0, 3).map((r) => ({ name: r.name, fg: r.fg, bgOnly: r.bgOnly })),
  rankCount: v.ranking.length,
});
const out = {
  today: slim(agg.aggBuildView(data, "today")),
  d7: slim(agg.aggBuildView(data, "7d")),
  d30: slim(agg.aggBuildView(data, "30d")),
  averages: agg.aggComputeAverages(data),
};
console.log(JSON.stringify(out, null, 1));
