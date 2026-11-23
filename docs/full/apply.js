/* The patch logic of the web patcher. The same file runs in the page and in tools/verify_web.js, which checks that its output is byte-identical to patch.py's. */
(function (root, factory) {
  if (typeof module === "object" && module.exports) module.exports = factory();
  else root.BBApply = factory();
})(this, function () {
  const unhex = s => { const o = new Uint8Array(s.length / 2); for (let i = 0; i < o.length; i++) o[i] = parseInt(s.substr(2 * i, 2), 16); return o; };
  const unb64 = s => typeof Buffer !== "undefined" ? new Uint8Array(Buffer.from(s, "base64")) : Uint8Array.from(atob(s), c => c.charCodeAt(0));
  const hex = b => [...new Uint8Array(b)].map(x => x.toString(16).padStart(2, "0")).join("");
  const sha256 = async b => hex(await crypto.subtle.digest("SHA-256", b));
  const PAGES = ["looper", "samplr", "trig", "drone"];

  /* parts (some of looper samplr trig drone) + add-ons -> the sets, in the canonical order. Mirrors tools/combo.py from_parts() / resolve(). */
  function resolve(data, parts, addons) {
    const sets = [...data.core];
    for (const p of parts) {
      if (!data.parts[p].available) throw new Error(data.parts[p].title + " is not available in this patcher yet");
      sets.push(...data.parts[p].sets);
    }
    if (parts.length) {
      sets.push(...data.always);                                  // the page host, and saving: every part has them
      for (const p of PAGES) if (!parts.includes(p)) sets.push(data.parts[p].hide);
    }
    for (const a of addons) sets.push(...data.addons[a].sets);
    sets.push("cave");
    const seen = new Set(), todo = [...sets];
    while (todo.length) {
      const s = todo.pop();
      if (seen.has(s)) continue;
      seen.add(s);
      todo.push(...data.requires[s]);
    }
    return data.canon.filter(s => seen.has(s));
  }

  /* what is wrong with a list of sets (what the page greys out); [] = fine. Mirrors tools/combo.py problems(). */
  function problems(data, sets) {
    const have = new Set(sets), out = [];
    for (const s of sets) for (const r of data.requires[s]) if (!have.has(r)) out.push(s + " needs " + r);
    if (have.has("looper")) {
      if (PAGES.every(p => have.has("hide_" + p))) out.push("looper with every page hidden: nothing to show");
      if (!have.has("hide_drone") && !have.has("lira")) out.push("the DRONE-8 page needs its engine (lira)");
    }
    return out;
  }

  function apply(stock, data, sets) {
    const c = data.cave, itcm = unb64(c.itcm);
    const out = new Uint8Array(c.itcm_o + itcm.length).fill(0xff);
    out.set(stock);
    for (const s of sets) {
      if (s === "cave") continue;
      for (const w of data.sets[s]) out.set(unhex(w.hex), w.o);
    }
    out.set(unhex(c.version.hex), c.version.o);
    const code = unb64(c.code);
    if (sets.includes("looper")) code[c.host_o] = 1;
    PAGES.forEach((p, i) => { if (sets.includes("hide_" + p)) code[c.hide_o + i] = 1; });
    out.set(code, c.code_o);
    out.set(itcm, c.itcm_o);
    return out;
  }

  return { resolve, problems, apply, sha256, hex, PAGES };
});
