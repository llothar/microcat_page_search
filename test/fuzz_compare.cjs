// For each indexed page: does Chrome's DOM text (MarkIt's walk) equal the indexed text?
// Usage: node fuzz_compare.cjs <dir> <texts.json> <fail list> -> soundness summary
const { chromium } = require('playwright-core');
const fs = require('fs'), path = require('path');
const [dir, textsFile, failFile] = process.argv.slice(2);
const texts = JSON.parse(fs.readFileSync(textsFile, 'utf8'));
const failing = new Set(fs.readFileSync(failFile, 'utf8').split('\n').filter(Boolean));
(async () => {
  const browser = await chromium.launch({ executablePath: process.env.CHROME || '/usr/bin/google-chrome', args: ['--no-sandbox'] });
  const page = await browser.newPage();
  const count = { pass_same: 0, pass_diff: 0, fail_same: 0, fail_diff: 0 };
  const unsound = [];
  for (const t of texts) {
    await page.goto('file://' + path.resolve(dir, t.path));
    const dom = await page.evaluate(() => {
      const root = document.querySelector('main') || document.body;
      const w = document.createTreeWalker(root, NodeFilter.SHOW_TEXT);
      let s = '';
      for (let n; (n = w.nextNode());) {
        const p = n.parentElement;
        if (!['[data-index-skip]', 'script', 'style'].some(sel => p.closest(sel))) s += n.textContent;
      }
      return s;
    });
    const same = dom === t.text, pass = !failing.has(t.path);
    count[(pass ? 'pass_' : 'fail_') + (same ? 'same' : 'diff')]++;
    if (pass && !same) {
      let i = 0; while (dom[i] === t.text[i]) i++;
      unsound.push(`${t.path} at ${i}: DOM ${JSON.stringify(dom.slice(i, i + 30))} index ${JSON.stringify(t.text.slice(i, i + 30))}`);
    }
  }
  console.log(JSON.stringify(count));
  unsound.forEach(u => console.log('UNSOUND ' + u));
  await browser.close();
})();
