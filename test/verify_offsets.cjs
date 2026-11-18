// Verifies in headless Chrome that the indexed text equals the DOM text MarkIt walks, and that
// MarkIt's markRanges() with the returned ranges selects exactly the indexed words.
// Usage: CHROME=/path/to/chrome node verify_offsets.cjs <dir with texts.json, results.json, pages> <@markitjs/core dist/index.js>
// texts.json: page_index_tool texts <index>; results.json: array of page_index_tool query outputs.
const { chromium } = require('playwright-core');
const fs = require('fs'), path = require('path');
const dir = process.argv[2], markitCode = fs.readFileSync(process.argv[3], 'utf8');
const texts = JSON.parse(fs.readFileSync(path.join(dir, 'texts.json'), 'utf8'));
const results = JSON.parse(fs.readFileSync(path.join(dir, 'results.json'), 'utf8'));
const exclude = ['[data-fs-skip]', 'script', 'style'];
(async () => {
  const browser = await chromium.launch({ executablePath: process.env.CHROME || '/usr/bin/google-chrome', args: ['--no-sandbox'] });
  const page = await browser.newPage();
  let same = 0; const diffs = [];
  for (const t of texts) {
    await page.goto('file://' + path.resolve(dir, t.path));
    const dom = await page.evaluate((exclude) => {
      const root = document.querySelector('main') || document.body;
      const w = document.createTreeWalker(root, NodeFilter.SHOW_TEXT);
      let s = '';
      for (let n; (n = w.nextNode());) {
        const p = n.parentElement;
        if (!exclude.some(sel => p.matches(sel) || p.closest(sel))) s += n.textContent;
      }
      return s;
    }, exclude);
    if (dom === t.text) { same++; continue; }
    let i = 0; while (i < dom.length && dom[i] === t.text[i]) i++;
    diffs.push(`${t.path}: first difference at ${i}: DOM ${JSON.stringify(dom.slice(i, i + 25))} index ${JSON.stringify(t.text.slice(i, i + 25))}`);
  }
  console.log(`text check: ${same} of ${texts.length} pages identical to the DOM text`);
  diffs.forEach(d => console.log('  ' + d));

  const byPath = Object.fromEntries(texts.map(t => [t.path, t.text]));
  let checked = 0, wrong = 0, renderer = '';
  const samples = [];
  for (const q of results) {
    for (const r of q.results.slice(0, 5)) {
      if (!r.ranges.length) continue;
      await page.goto('file://' + path.resolve(dir, r.path));
      await page.addScriptTag({ type: 'module', content: markitCode + '\nwindow.__markit = markit;' });
      await page.waitForFunction(() => window.__markit);
      const got = await page.evaluate(([ranges, exclude]) => {
        const m = window.__markit(document.querySelector('main'));
        m.markRanges(ranges.map(([start, length]) => ({ start, length })), { exclude });
        return { matches: m.getMatches().map(x => x.text), api: typeof CSS !== 'undefined' && 'highlights' in CSS };
      }, [r.ranges, exclude]);
      renderer = got.api ? 'CSS Custom Highlight API' : 'DOM';
      const text = byPath[r.path];
      r.ranges.forEach(([s, l], i) => {
        checked++;
        const expect = text.substr(s, l);
        const before = text[s - 1] || ' ', after = text[s + l] || ' ';
        const whole = !/[\p{L}\p{N}]/u.test(before) && !/[\p{L}\p{N}]/u.test(after);
        if (got.matches[i] !== expect || !whole || !expect) { wrong++; if (wrong < 10) console.log(`  mismatch ${r.path} [${s},${l}] markit=${JSON.stringify(got.matches[i])} index=${JSON.stringify(expect)} context=${JSON.stringify(text.substr(s - 5, l + 10))}`); }
        else if (samples.length < 12 && i < 2) samples.push(expect);
      });
    }
  }
  console.log(`range check (${renderer}): ${checked - wrong} of ${checked} ranges select the indexed word exactly`);
  console.log('  sample words:', samples.join(', '));
  await browser.close();
})();
