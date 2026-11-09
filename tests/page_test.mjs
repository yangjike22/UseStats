// 页面初始化逻辑测试：把 stats.html 里的真实脚本（头部错误收集 + 主脚本）
// 在最小 DOM 桩上跑起来，分别喂不同形态的数据/设置，断言：
//  - 初始化全程不抛异常；
//  - 正常数据能渲染出总量与排行榜行；
//  - 空数据集进入明确的「暂无数据」状态（#noData 可见、平均值显示 —），而不是空白或崩溃；
//  - 畸形数据（缺 apps 字段、缺 hourlyFg、idle 极大）不抛异常；
//  - 三页（总览/图表/设置）可编程切换且 hidden 状态正确；
//  - 图表页在 15 天 fixture 下渲染出趋势 SVG/环图与时段、星期柱图；
//  - 设置对象经 applySettingsObject → collectSettings 往返不丢字段，非法值回退。
//  - 主题切换只在设置页构建（总览无换肤入口）；
//  - 排行行使用统一 grid 模板（名字列 minmax(120px,360px) 优先、条带弹性 minmax(60px,1fr)；下限压低防高 DPI 窄 CSS 视口下占比列被挤出行外）；
//  - 图表页口径说明位于 2×2 网格之后的页脚位置；
//  - 图表三态：30 天仅 2 天有文件时，无文件日渲染为空槽位（不是零柱）、
//    有文件零时长日渲染为基线小刻，标题旁有覆盖标注且低覆盖提示可见。
// 运行：node tests/page_test.mjs
import { readFileSync, readdirSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import vm from "node:vm";

const here = dirname(fileURLToPath(import.meta.url));
const html = readFileSync(join(here, "..", "stats.html"), "utf8");

// 抽出全部无 src 的 <script> 块（头部错误收集、数据占位、主脚本）
const blocks = [];
for (const m of html.matchAll(/<script>([\s\S]*?)<\/script>/g)) blocks.push(m[1]);
if (blocks.length < 3) { console.error("expected >=3 inline script blocks, got", blocks.length); process.exit(1); }
const headBlock = blocks[0];
const mainBlock = blocks[blocks.length - 1];
if (!mainBlock.includes("aggBuildView")) { console.error("main script block not found"); process.exit(1); }

// ---------- 最小 DOM 桩 ----------
class El {
  constructor(tag) {
    this.tagName = (tag || "div").toUpperCase();
    this.children = [];
    this.style = {};
    this.dataset = {};
    this.hidden = false;
    this.className = "";
    this.checked = false;
    this._text = "";
    this._html = "";
    this._listeners = {};
    this._attrs = {};
    this.firstChild = { textContent: "" };
    const set = new Set();
    this.classList = {
      toggle: (c, force) => { if (force === undefined) { set.has(c) ? set.delete(c) : set.add(c); } else { force ? set.add(c) : set.delete(c); } },
      add: (c) => set.add(c),
      contains: (c) => set.has(c),
    };
  }
  set textContent(v) { this._text = String(v); }
  get textContent() { return this._text; }
  set innerHTML(v) { this._html = String(v); this.children = []; }
  get innerHTML() { return this._html; }
  appendChild(c) { this.children.push(c); return c; }
  addEventListener(type, fn) { this._listeners[type] = fn; }
  dispatch(type, ev) { if (this._listeners[type]) this._listeners[type](ev || {}); }
  setAttribute(k, v) { this._attrs[k] = String(v); }
  getAttribute(k) { return this._attrs[k]; }
  querySelector() { return new El("span"); }
  closest(sel) {
    if (sel === "button" && this.tagName === "BUTTON") return this;
    return null;
  }
}

function makeLocalStorage() {
  const m = new Map();
  return {
    getItem: (k) => (m.has(k) ? m.get(k) : null),
    setItem: (k, v) => m.set(k, String(v)),
    removeItem: (k) => m.delete(k),
  };
}

function runScenario(name, usageData, settingsData) {
  const registry = new Map();
  const win = {
    USAGE_DATA: undefined,
    USESTATS_SETTINGS: undefined,
    chrome: undefined,
    __pageLog: [],
    localStorage: makeLocalStorage(),
    addEventListener() {},
    setInterval: () => 0,
    clearInterval: () => {},
  };
  win.window = win;
  const documentStub = {
    documentElement: new El("html"),
    getElementById(id) {
      if (!registry.has(id)) registry.set(id, new El("div"));
      return registry.get(id);
    },
    createElement: (tag) => new El(tag),
    querySelectorAll: () => [],
  };
  win.document = documentStub;
  const ctx = vm.createContext(Object.assign(win, { document: documentStub, console }));
  let threw = null;
  try {
    vm.runInContext(headBlock, ctx, { filename: "head.js" });
    win.USAGE_DATA = usageData; // 等价于宿主把数据内嵌进页面
    win.USESTATS_SETTINGS = settingsData; // 等价于宿主把 settings.json 内嵌进页面
    vm.runInContext(mainBlock, ctx, { filename: "main.js" });
  } catch (e) {
    threw = e;
  }
  // 在同一上下文里跑一段探针代码（可调用页面脚本里的全局函数），返回其结果
  const probe = (code) => vm.runInContext(code, ctx, { filename: "probe.js" });
  return { registry, threw, logs: win.__pageLog.slice(), probe };
}

// ---------- 静态无障碍语义检查（直接查 HTML 源码的语义骨架） ----------
const staticChecks = [
  [(h) => (h.split('role="tablist"').length - 1) >= 2, "两个 tablist（页面导航 + 时间范围）"],
  [(h) => h.includes('role="tab"') && h.includes('aria-selected="true"') && h.includes('aria-controls="page-overview"'), "页签 role=tab + aria-selected + aria-controls"],
  [(h) => h.includes("<main>") && h.includes('role="tabpanel"'), "main 地标 + tabpanel"],
  [(h) => h.includes('aria-label="后台统计"') && h.includes('aria-label="开机自启"'), "开关 input 有可访问名"],
  [(h) => h.includes('role="group"') && h.includes('role="alert"'), "分段控件 group + 诊断条 alert"],
  [(h) => h.includes("prefers-reduced-motion") && h.includes(":focus-visible"), "减少动态偏好 + 可见焦点环"],
  [(h) => h.includes("--fs-display:32px") && h.includes("--fs-stat:28px") && h.includes("--fs-section:20px"), "字阶 token 已落地"],
  [(h) => !h.includes('content:"+"'), "装饰性 + 角标已剥除"],
  [(h) => !h.includes('id="themeBar"'), "总览无主题切换入口（主题切换只在设置页）"],
  [(h) => /\.rrow\{[^}]*display:grid/.test(h) && /\.rrow\{[^}]*grid-template-columns:\s*minmax\(120px, 360px\) minmax\(60px, 1fr\)/.test(h), "排行行 grid 模板：名字列优先 minmax(120px,360px)、条带弹性 minmax(60px,1fr)"],
  [(h) => h.indexOf('id="chartsIntro"') > h.indexOf('class="chart-grid"'), "图表页口径说明在 2×2 网格之后（页脚位置）"],
];
let staticFailed = 0;
for (const [fn, label] of staticChecks) {
  if (fn(html)) console.log("OK   静态语义：" + label);
  else { staticFailed++; console.log("FAIL 静态语义：" + label); }
}
if (staticFailed) { console.error(staticFailed + " static a11y check(s) failed"); process.exit(1); }

// ---------- 数据场景 ----------
function loadFixtures() {
  const fixDir = join(here, "fixtures");
  return readdirSync(fixDir)
    .filter((f) => /^usage-\d{4}-\d{2}-\d{2}\.json$/.test(f))
    .sort()
    .map((f) => JSON.parse(readFileSync(join(fixDir, f), "utf8")));
}
const TODAY = "2026-11-05";
const fixtures = loadFixtures();

const scenarios = [
  {
    name: "正常 15 天 fixture",
    data: { today: TODAY, days: fixtures },
    check({ registry, threw, logs }) {
      const errs = [];
      if (threw) errs.push("threw: " + threw.message);
      if (!logs.some((l) => l.startsWith("render-ok"))) errs.push("no render-ok log: " + JSON.stringify(logs));
      const total = registry.get("blkTotal");
      if (!total || !total._text || total._text === "—") errs.push("blkTotal not filled: " + (total && total._text));
      const rank = registry.get("rank");
      if (!rank || rank.children.length === 0) errs.push("rank has no rows");
      const chart = registry.get("chart");
      if (!chart || chart.children.length !== 24) errs.push("chart cols != 24: " + (chart && chart.children.length));
      const nd = registry.get("noData");
      if (nd && nd.hidden === false) errs.push("noData should stay hidden");
      const avg = registry.get("avgDaily");
      if (!avg || avg._text === "—" || !avg._text) errs.push("avgDaily not filled");
      return errs;
    },
  },
  {
    name: "空数据集（新目录、还没有任何记录）",
    data: { today: TODAY, days: [] },
    check({ registry, threw, logs }) {
      const errs = [];
      if (threw) errs.push("threw: " + threw.message);
      if (!logs.some((l) => l.startsWith("render-ok"))) errs.push("no render-ok log");
      const nd = registry.get("noData");
      if (!nd || nd.hidden !== false) errs.push("noData block not shown");
      for (const id of ["avgDaily", "avgWeekly", "avgMonthly"]) {
        const el = registry.get(id);
        if (!el || el._text !== "—") errs.push(id + " should be —, got " + (el && el._text));
      }
      const empty = registry.get("emptyMsg");
      if (!empty || empty.hidden !== false) errs.push("emptyMsg not shown for empty ranking");
      return errs;
    },
  },
  {
    name: "只有 1 天数据",
    data: { today: TODAY, days: fixtures.slice(-1) },
    check({ registry, threw }) {
      const errs = [];
      if (threw) errs.push("threw: " + threw.message);
      const total = registry.get("blkTotal");
      if (!total || !total._text || total._text === "—") errs.push("blkTotal not filled");
      const avg = registry.get("avgDaily");
      if (!avg || avg._text === "—") errs.push("avgDaily should have a value");
      const label = registry.get("avgDailyLabel");
      if (!label || !label._text.includes("近 1 天")) errs.push("avgDailyLabel wrong: " + (label && label._text));
      return errs;
    },
  },
  {
    name: "idle 极大值 + 应用字段畸形",
    data: {
      today: TODAY,
      days: [
        { date: TODAY, idleSeconds: 4000000000, apps: [] },
        { date: "2026-11-04" }, // 缺 apps / idleSeconds
        {
          date: "2026-11-03", idleSeconds: 60,
          apps: [
            { path: "C:\\x.exe", name: "X" }, // 缺 fg/bg/hourlyFg
            { path: "C:\\y.exe", name: "Y", fgSeconds: 120, bgSeconds: 30, hourlyFg: null },
            null, // 混入 null 条目
          ],
        },
      ],
    },
    check({ registry, threw }) {
      const errs = [];
      if (threw) errs.push("threw: " + threw.message);
      const nd = registry.get("noData");
      if (nd && nd.hidden === false) errs.push("noData should stay hidden (days exist)");
      return errs;
    },
  },
  {
    name: "主题切换只在设置页构建（总览无换肤入口）",
    data: { today: TODAY, days: fixtures },
    check({ registry, threw }) {
      const errs = [];
      if (threw) errs.push("threw: " + threw.message);
      const bar = registry.get("themeBar");
      if (bar && bar.children.length !== 0) {
        errs.push("overview themeBar should not be built anymore, got " + bar.children.length + " buttons");
      }
      const barSet = registry.get("themeBarSet");
      if (!barSet || barSet.children.length !== 4) {
        errs.push("themeBarSet (settings page) should have 4 theme buttons, got " + (barSet && barSet.children.length));
      }
      return errs;
    },
  },
  {
    name: "三页切换（总览/图表/设置）状态正确",
    data: { today: TODAY, days: fixtures },
    check({ threw, probe }) {
      if (threw) return ["threw: " + threw.message];
      return probe(`(function () {
        const errs = [];
        const vis = (id) => document.getElementById(id).hidden === false;
        if (!vis("page-overview")) errs.push("overview should start visible");
        switchPage("charts");
        if (!vis("page-charts")) errs.push("charts not visible after switch");
        if (vis("page-overview") || vis("page-settings")) errs.push("other pages should be hidden");
        switchPage("settings");
        if (!vis("page-settings")) errs.push("settings not visible after switch");
        switchPage("overview");
        if (!vis("page-overview")) errs.push("overview not visible after switch back");
        switchPage("bogus");
        if (!vis("page-overview")) errs.push("bogus page should fall back to overview");
        return errs;
      })()`);
    },
  },
  {
    name: "图表页在 15 天 fixture 下渲染出元素",
    data: { today: TODAY, days: fixtures },
    check({ registry, threw }) {
      const errs = [];
      if (threw) errs.push("threw: " + threw.message);
      const trend = registry.get("trendChart");
      const th = trend ? trend._html : "";
      if (!th.includes("<svg")) errs.push("trend svg missing");
      const rects = (th.match(/<rect/g) || []).length;
      if (rects < 30) errs.push("trend bars < 30: " + rects);
      if (!th.includes("<polyline")) errs.push("trend moving-average polyline missing");
      const hour = registry.get("hourChart");
      if (!hour || hour.children.length !== 24) errs.push("hourChart cols != 24: " + (hour && hour.children.length));
      const week = registry.get("weekChart");
      if (!week || week.children.length !== 7) errs.push("weekChart cols != 7: " + (week && week.children.length));
      const share = registry.get("shareChart");
      const sh = share ? share._html : "";
      if (!sh.includes("<circle")) errs.push("share donut circles missing");
      const legend = registry.get("shareLegend");
      if (!legend || !legend._html.includes("lg-name")) errs.push("share legend missing");
      const note = registry.get("trendNote");
      if (!note || !note._text.includes("7 日平均") || !note._text.includes("空槽＝未记录")) errs.push("trendNote wrong: " + (note && note._text));
      return errs;
    },
  },
  {
    name: "设置读写往返不丢字段 + 非法值回退",
    data: { today: TODAY, days: fixtures },
    check({ threw, probe }) {
      if (threw) return ["threw: " + threw.message];
      return probe(`(function () {
        const errs = [];
        applySettingsObject({ idleSeconds: 300, backgroundStats: false, retentionDays: 90, theme: "dark", autoStart: false, wifiAssist: false, wifiAutoConnect: false, mediaNoIdle: false, focusGrace: false });
        const c = collectSettings();
        if (c.idleSeconds !== 300) errs.push("idle lost: " + c.idleSeconds);
        if (c.backgroundStats !== false) errs.push("backgroundStats lost");
        if (c.retentionDays !== 90) errs.push("retention lost: " + c.retentionDays);
        if (c.theme !== "dark") errs.push("theme lost: " + c.theme);
        if (c.autoStart !== false) errs.push("autoStart lost");
        if (c.wifiAssist !== false) errs.push("wifiAssist lost");
        if (c.wifiAutoConnect !== false) errs.push("wifiAutoConnect lost");
        if (c.mediaNoIdle !== false) errs.push("mediaNoIdle lost");
        if (c.focusGrace !== false) errs.push("focusGrace lost");
        applySettingsObject({ idleSeconds: 42, retentionDays: 7 });
        const c2 = collectSettings();
        if (c2.idleSeconds !== 180) errs.push("bad idle not defaulted: " + c2.idleSeconds);
        if (c2.retentionDays !== 0) errs.push("bad retention not defaulted: " + c2.retentionDays);
        // 非法 theme 被忽略（保持当前主题），不许把主题打回默认或抛错
        applySettingsObject({ theme: "neon" });
        if (collectSettings().theme !== "dark") errs.push("bad theme should be ignored, got " + collectSettings().theme);
        return errs;
      })()`);
    },
  },
  {
    name: "宿主注入的 settings 被正确采用",
    data: { today: TODAY, days: fixtures },
    settings: { idleSeconds: 600, backgroundStats: false, retentionDays: 365, theme: "red", autoStart: false, wifiAssist: false, wifiAutoConnect: true, mediaNoIdle: false, focusGrace: true, dataDir: "D:\\usestat", dataDays: 15 },
    check({ registry, threw, probe }) {
      const errs = [];
      if (threw) errs.push("threw: " + threw.message);
      else {
        const sub = probe(`(function () {
          const errs = [];
          const c = collectSettings();
          if (c.idleSeconds !== 600) errs.push("injected idle not used: " + c.idleSeconds);
          if (c.backgroundStats !== false) errs.push("injected bg not used");
          if (c.retentionDays !== 365) errs.push("injected retention not used: " + c.retentionDays);
          if (c.theme !== "red") errs.push("injected theme not used: " + c.theme);
          if (c.autoStart !== false) errs.push("injected autoStart not used");
          if (c.wifiAssist !== false) errs.push("injected wifiAssist not used");
          if (c.wifiAutoConnect !== true) errs.push("injected wifiAutoConnect not used");
          if (c.mediaNoIdle !== false) errs.push("injected mediaNoIdle not used");
          if (c.focusGrace !== true) errs.push("injected focusGrace not used");
          return errs;
        })()`);
        errs.push(...sub);
      }
      const dd = registry.get("dataDirText");
      if (!dd || dd._text !== "D:\\usestat") errs.push("dataDir not shown: " + (dd && dd._text));
      const dn = registry.get("dataDaysText");
      if (!dn || dn._text !== "15") errs.push("dataDays not shown: " + (dn && dn._text));
      const bg = registry.get("setBgInput");
      if (!bg || bg.checked !== false) errs.push("bg switch not synced");
      const au = registry.get("setAutoInput");
      if (!au || au.checked !== false) errs.push("autoStart switch not synced");
      const wf = registry.get("setWifiInput");
      if (!wf || wf.checked !== false) errs.push("wifiAssist switch not synced");
      const wa = registry.get("setWifiAutoInput");
      if (!wa || wa.checked !== true) errs.push("wifiAutoConnect switch not synced");
      const md = registry.get("setMediaInput");
      if (!md || md.checked !== false) errs.push("mediaNoIdle switch not synced");
      const gr = registry.get("setGraceInput");
      if (!gr || gr.checked !== true) errs.push("focusGrace switch not synced");
      return errs;
    },
  },
  {
    name: "无障碍：图表文字摘要 + 排行文本替代 + 控件 aria 状态",
    data: { today: TODAY, days: fixtures },
    check({ registry, threw }) {
      const errs = [];
      if (threw) errs.push("threw: " + threw.message);
      for (const id of ["chart", "hourChart", "weekChart"]) {
        const el = registry.get(id);
        if (!el || el.getAttribute("role") !== "img") errs.push(id + " missing role=img");
        else if (!el.getAttribute("aria-label")) errs.push(id + " missing aria-label summary");
      }
      const trend = registry.get("trendChart");
      if (!trend || !trend._html.includes('aria-label="近 30 天每日总时长趋势，最高'))
        errs.push("trend svg aria-label lacks summary");
      const share = registry.get("shareChart");
      if (!share || !share._html.includes("占比最高"))
        errs.push("share svg aria-label lacks summary");
      const rank = registry.get("rank");
      const row0 = rank && rank.children[0];
      if (!row0 || !row0._html.includes('aria-hidden="true"')) errs.push("rank blocks not aria-hidden");
      if (!row0 || !row0._html.includes('class="vh"')) errs.push("rank row lacks screen-reader text");
      if (!row0 || !row0._html.includes('class="r-id"')) errs.push("rank row identity group (.r-id grid cell) missing");
      const idle = registry.get("setIdle");
      if (!idle || idle.children.some((b) => b.getAttribute("aria-pressed") == null))
        errs.push("seg buttons missing aria-pressed");
      else if (idle.children.filter((b) => b.getAttribute("aria-pressed") === "true").length !== 1)
        errs.push("seg should have exactly one pressed button");
      const tb = registry.get("themeBarSet");
      if (!tb || tb.children.some((b) => b.getAttribute("aria-pressed") == null))
        errs.push("theme buttons missing aria-pressed");
      else if (tb.children.filter((b) => b.getAttribute("aria-pressed") === "true").length !== 1)
        errs.push("theme bar should have exactly one pressed button");
      return errs;
    },
  },
  {
    name: "图表三态：30 天仅 2 天有文件（1 天有数据 + 1 天空文件）",
    data: {
      today: TODAY,
      days: [
        { date: "2026-11-04", idleSeconds: 0, apps: [] }, // 有文件但前台总时长为 0（真零）
        fixtures[fixtures.length - 1], // 2026-11-05，有真实数据
      ],
    },
    check({ registry, threw }) {
      const errs = [];
      if (threw) errs.push("threw: " + threw.message);
      const trend = registry.get("trendChart");
      const th = trend ? trend._html : "";
      const count = (re) => (th.match(re) || []).length;
      if (count(/data-state="missing"/g) !== 28)
        errs.push("missing slots should be 28, got " + count(/data-state="missing"/g));
      if (count(/data-state="zero"/g) !== 1)
        errs.push("zero ticks should be 1, got " + count(/data-state="zero"/g));
      if (count(/data-state="data"/g) !== 1)
        errs.push("data bars should be 1, got " + count(/data-state="data"/g));
      const cov = registry.get("trendCov");
      if (!cov || !cov._text.includes("已记录 2 / 30 天"))
        errs.push("trendCov wrong: " + (cov && cov._text));
      const hint = registry.get("trendHint");
      if (!hint || hint.hidden !== false) errs.push("trendHint should be visible when daysPresent < 3");
      const week = registry.get("weekChart");
      const slots = week ? week.children.filter((c) => c._html.includes("未记录")).length : 0;
      if (slots !== 5) errs.push("weekChart missing slots should be 5, got " + slots);
      const intro = registry.get("chartsIntro");
      if (!intro || !intro._text.includes("已记录 2 / 30 天"))
        errs.push("chartsIntro lacks coverage: " + (intro && intro._text));
      return errs;
    },
  },
  {
    name: "USAGE_DATA 整体缺失（占位符未替换的极端情况）",
    data: undefined,
    check({ registry, threw }) {
      const errs = [];
      if (threw) errs.push("threw: " + threw.message);
      const nd = registry.get("noData");
      if (!nd || nd.hidden !== false) errs.push("noData block not shown");
      return errs;
    },
  },
];

let failed = 0;
for (const s of scenarios) {
  const res = runScenario(s.name, s.data, s.settings);
  let errs;
  try {
    errs = s.check(res);
  } catch (e) {
    errs = ["check itself threw: " + e.message];
  }
  if (errs.length) {
    failed++;
    console.log("FAIL " + s.name);
    for (const e of errs) console.log("  - " + e);
    if (res.threw) console.log("  stack: " + res.threw.stack.split("\n")[1]);
  } else {
    console.log("OK   " + s.name);
  }
}
if (failed) { console.error(failed + " scenario(s) failed"); process.exit(1); }
console.log("OK: 页面初始化、三页切换、图表渲染与设置往返在各场景下均不抛异常且状态正确");
