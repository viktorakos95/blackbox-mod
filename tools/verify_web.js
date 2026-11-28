// usage: node tools/verify_web.js <cases.json>   cases = [{parts, addons, sets, sha}], from tools/verify_web.py.
// Runs the page's own logic (docs/apply.js) on the stock image and compares the set list and the SHA-256 with patch.py's.
const fs = require("fs"), path = require("path");
const root = path.join(__dirname, "..");
const A = require(path.join(root, "docs", "apply.js"));
const data = JSON.parse(fs.readFileSync(path.join(root, "docs", "data.json")));
const stock = new Uint8Array(fs.readFileSync(path.join(root, "firmware", "BLACKBOX-3.1.9.bin")));
const cases = JSON.parse(fs.readFileSync(process.argv[2]));
(async () => {
  if ((await A.sha256(stock)) !== data.stock_sha256) throw new Error("stock hash");
  let bad = 0;
  for (const c of cases) {
    const sets = A.resolve(data, c.parts, c.addons);
    const same = JSON.stringify(sets) === JSON.stringify(c.sets);
    const pr = A.problems(data, sets);
    const r = A.apply(stock, data, sets);
    const sha = (await A.sha256(r.main)) + (r.extra ? "+" + (await A.sha256(r.extra)) : "");
    const ok = same && sha === c.sha && pr.length === 0;
    if (!ok) { bad++; console.log("MISMATCH", JSON.stringify({ parts: c.parts, addons: c.addons }), same ? "" : "sets differ: " + sets.join(" "), sha === c.sha ? "" : "sha differs", pr.join("; ")); }
  }
  console.log(`${cases.length} combinations, ${bad} mismatch(es)`);
  process.exit(bad ? 1 : 0);
})();
