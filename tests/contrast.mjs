// 对比度验算：从 stats.html 的 <style> 抽出四套主题的 CSS 变量，
// 再抽出 JS THEMES 的头像调色板，按 WCAG 相对亮度公式逐对计算对比度。
// 门槛：正文/控件文字 ≥4.5:1；大字、图形、焦点环 ≥3:1；头像字母（15px 粗体）≥4.5:1。
// 运行：node tests/contrast.mjs（任一对不达标以非零码退出，并打印最紧的若干对）
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const html = readFileSync(join(here, "..", "stats.html"), "utf8");
const style = html.match(/<style>([\s\S]*?)<\/style>/)[1];

function parseVars(block) {
  const vars = {};
  for (const m of block.matchAll(/(--[\w-]+)\s*:\s*([^;]+);/g)) vars[m[1]] = m[2].trim();
  return vars;
}
const rootBlock = style.match(/:root\{([\s\S]*?)\n  \}/)[1];
const base = parseVars(rootBlock);
const themes = { classic: { ...base } };
for (const m of style.matchAll(/:root\[data-theme="(\w+)"\]\{([\s\S]*?)\n  \}/g)) {
  themes[m[1]] = { ...base, ...parseVars(m[2]) };
}

// JS 侧头像调色板（顺序与 THEMES 定义一致：classic/soft/dark/red）
const avatarSets = [...html.matchAll(/avatars:\s*\[([^\]]+)\]/g)]
  .map((m) => [...m[1].matchAll(/#[0-9A-Fa-f]{6}/g)].map((x) => x[0]));
const avatarInks = [...html.matchAll(/avatarInk:\s*"(#[0-9A-Fa-f]{6})"/g)].map((m) => m[1]);
const themeOrder = ["classic", "soft", "dark", "red"];

function lum(hex) {
  const c = hex.replace("#", "");
  const f = (v) => {
    v = parseInt(v, 16) / 255;
    return v <= 0.03928 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4);
  };
  return 0.2126 * f(c.slice(0, 2)) + 0.7152 * f(c.slice(2, 4)) + 0.0722 * f(c.slice(4, 6));
}
function ratio(a, b) {
  const [l1, l2] = [lum(a), lum(b)].sort((x, y) => y - x);
  return (l1 + 0.05) / (l2 + 0.05);
}

const SURFACES = ["bg", "panel", "panel2", "track"];
const pairs = []; // [fg 描述, 取值函数, min]
for (const t of ["ink", "sub", "gray"])
  for (const s of SURFACES) pairs.push([`${t} / ${s}`, (v) => [v["--" + t], v["--" + s]], 4.5]);
pairs.push(["a1 / panel", (v) => [v["--a1"], v["--panel"]], 4.5]);
pairs.push(["a1 / panel2（仅大字/图形用例）", (v) => [v["--a1"], v["--panel2"]], 3.0]);
pairs.push(["a1-deep / panel2", (v) => [v["--a1-deep"], v["--panel2"]], 4.5]);
pairs.push(["a1 / bg（焦点环）", (v) => [v["--a1"], v["--bg"]], 3.0]);
pairs.push(["a1-ink / a1", (v) => [v["--a1-ink"], v["--a1"]], 4.5]);
pairs.push(["a2-ink / a2", (v) => [v["--a2-ink"], v["--a2"]], 4.5]);
pairs.push(["a2 / panel", (v) => [v["--a2"], v["--panel"]], 3.0]);
pairs.push(["a2 / panel2", (v) => [v["--a2"], v["--panel2"]], 3.0]);
pairs.push(["a3 / panel", (v) => [v["--a3"], v["--panel"]], 3.0]);
pairs.push(["a3 / panel2", (v) => [v["--a3"], v["--panel2"]], 3.0]);
pairs.push(["a2-soft / panel", (v) => [v["--a2-soft"], v["--panel"]], 3.0]);
pairs.push(["a2-soft / panel2", (v) => [v["--a2-soft"], v["--panel2"]], 3.0]);
pairs.push(["a2-soft / track", (v) => [v["--a2-soft"], v["--track"]], 3.0]);
pairs.push(["a1-deep / a1-soft", (v) => [v["--a1-deep"], v["--a1-soft"]], 4.5]);
pairs.push(["ink / a1-soft", (v) => [v["--ink"], v["--a1-soft"]], 4.5]);
pairs.push(["#FFFFFF / danger", (v) => ["#FFFFFF", v["--danger"]], 4.5]);
// 图表数据系列色（--data-*，与强调/状态色分工独立）：图形门槛 ≥3:1
pairs.push(["data-fg / panel", (v) => [v["--data-fg"], v["--panel"]], 3.0]);
pairs.push(["data-fg / panel2", (v) => [v["--data-fg"], v["--panel2"]], 3.0]);
pairs.push(["data-bg / panel", (v) => [v["--data-bg"], v["--panel"]], 3.0]);
pairs.push(["data-bg / panel2", (v) => [v["--data-bg"], v["--panel2"]], 3.0]);
pairs.push(["data-bg / track", (v) => [v["--data-bg"], v["--track"]], 3.0]);
pairs.push(["data-ma / panel", (v) => [v["--data-ma"], v["--panel"]], 3.0]);
pairs.push(["data-ma / panel2", (v) => [v["--data-ma"], v["--panel2"]], 3.0]);

let failed = 0;
const all = [];
for (const name of themeOrder) {
  const v = themes[name];
  if (!v) { console.log("FAIL 缺主题 " + name); failed++; continue; }
  for (const [label, pick, min] of pairs) {
    const [fg, bg] = pick(v);
    if (!fg || !bg || !/^#[0-9A-Fa-f]{6}$/.test(fg) || !/^#[0-9A-Fa-f]{6}$/.test(bg)) {
      console.log(`FAIL ${name} ${label}: 颜色缺失或非 hex（${fg} / ${bg}）`);
      failed++;
      continue;
    }
    const r = ratio(fg, bg);
    all.push({ name, label, r, min });
    if (r < min) { failed++; console.log(`FAIL ${name} ${label} = ${r.toFixed(2)} < ${min}`); }
  }
  // 头像：字母色 × 该主题调色板每一色
  const pal = avatarSets[themeOrder.indexOf(name)] || [];
  const inkC = avatarInks[themeOrder.indexOf(name)];
  pal.forEach((c, i) => {
    const r = ratio(inkC, c);
    all.push({ name, label: `头像字 / 调色板[${i}] ${c}`, r, min: 4.5 });
    if (r < 4.5) { failed++; console.log(`FAIL ${name} 头像字 / ${c} = ${r.toFixed(2)} < 4.5`); }
  });
}

all.sort((a, b) => a.r / a.min - b.r / b.min);
console.log(`共验算 ${all.length} 对；最紧的 8 对：`);
for (const x of all.slice(0, 8))
  console.log(`  ${x.name} ${x.label} = ${x.r.toFixed(2)}（门槛 ${x.min}）`);
if (failed) { console.error(failed + " 对未达标"); process.exit(1); }
console.log("OK: 四套主题全部组合对比度 ≥ WCAG AA");
