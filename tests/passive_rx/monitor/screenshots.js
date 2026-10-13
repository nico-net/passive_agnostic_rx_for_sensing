// Headless screenshots of every dashboard page in light and dark theme (and a phone width).
//   NODE_PATH=$(npm root -g) node screenshots.js http://127.0.0.1:18080 OUT_DIR [run_id] [prefix]
// Uses the pre-installed Chromium (PLAYWRIGHT_BROWSERS_PATH); fails on any page script error.
const { chromium } = require("playwright");
const [base, out, run = "primary", prefix = ""] = process.argv.slice(2);
const pages = ["home", "dl", "ul", "sensing", "cell", "settings", "perf", "spectrum", "history", "logs", "glossary"];
(async () => {
  const browser = await chromium.launch();
  let errors = 0;
  for (const theme of ["light", "dark"]) {
    for (const [w, h, tag] of [[1280, 900, ""], [390, 844, "-phone"]]) {
      if (tag && theme === "dark") continue;
      const ctx = await browser.newContext({ viewport: { width: w, height: h }, colorScheme: theme });
      const p = await ctx.newPage();
      p.on("pageerror", (e) => { errors++; console.error("pageerror", theme, e.message); });
      p.on("console", (m) => { if (m.type() === "error") { errors++; console.error("console", m.text()); } });
      await p.goto(base + "/#home");
      await p.evaluate((r) => { localStorage.setItem("rxdash", JSON.stringify({})); }, run);
      for (const pg of tag ? ["home", "dl", "cell"] : pages) {
        await p.goto(base + "/#" + pg);
        if (run !== "primary") { await p.selectOption("#runSel", run); }
        await p.waitForTimeout(pg === "logs" || pg === "spectrum" ? 1800 : 1200);
        if (pg === "history") {
          for (let i = 0; i < 3; i++) await p.evaluate((k) => { const b = document.querySelectorAll("[data-cmp]")[k]; if (b && !b.checked) b.click(); }, i);
          const g = await p.$("#cmpGo"); if (g) { await g.click(); await p.waitForTimeout(2500); }
        }
        await p.screenshot({ path: `${out}/${prefix}${pg}-${theme}${tag}.png`, fullPage: true });
      }
      await ctx.close();
    }
  }
  await browser.close();
  console.log("screenshots done, errors:", errors);
  process.exit(errors ? 1 : 0);
})();
