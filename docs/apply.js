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
      if (PAGES.every(p => have.has("hide_" + p)) && !have.has("mclblob")) out.push("looper with every page hidden: nothing to show");
      if (!have.has("hide_drone") && !have.has("lira")) out.push("the DRONE-8 page needs its engine (lira)");
    }
    return out;
  }

  /* The patched files: {main, extra}. main = BLACKBOX.bin (the stock image + the core: it ends before 0x08100000, the end of flash bank 1, so the stock installer of any unit takes it);
     extra = BBEXTRA.BIN (the machines' code for flash bank 2, which the unit programs into itself from the card) or null when no set needs it. Mirrors patch.py. */
  function apply(stock, data, sets) {
    const c = data.cave, code = unb64(c.code);
    const hasB2 = sets.some(s => c.b2_sets.includes(s));
    const pay = hasB2 ? unb64(c.b2) : null;
    const mainLen = c.code_o + code.length;
    const out = new Uint8Array(hasB2 ? c.b2_o + pay.length : mainLen).fill(0xff);
    out.set(stock);
    out.set(unhex(c.version.hex), c.version.o);
    if (sets.includes("looper")) code[c.host_o] = 1;
    PAGES.forEach((p, i) => { if (sets.includes("hide_" + p)) code[c.hide_o + i] = 1; });
    out.set(code, c.code_o);                           // the cave first: the other sets write into it (guard stubs, the MCL pointers)
    if (hasB2) out.set(pay, c.b2_o);
    for (const s of sets) {
      if (s === "cave") continue;
      for (const w of data.sets[s]) out.set(w.b64 ? unb64(w.b64) : unhex(w.hex), w.o);
    }
    let extra = null;
    if (hasB2) {                                       // the header (one flash word) and the sum of the payload that bank 1 expects
      extra = out.slice(c.b2_o);
      const dv = new DataView(extra.buffer);
      let sum = 0;
      for (let i = 32; i < extra.length; i += 4) sum = (sum + dv.getUint32(i, true)) >>> 0;
      [0x21324242, 1, extra.length, sum, c.b2_itcm_off, c.b2_itcm_len, c.b2_itcm_dst, 0].forEach((w, i) => dv.setUint32(4 * i, w, true));
      new DataView(out.buffer).setUint32(c.sum_o, sum, true);
    }
    return { main: out.slice(0, mainLen), extra };
  }

  /* A zip (stored, no compression) of {name: Uint8Array}: one download holding BLACKBOX.bin and BBEXTRA.BIN. */
  const CRC = (() => { const t = new Uint32Array(256); for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; t[n] = c >>> 0; } return t; })();
  const crc32 = b => { let c = 0xFFFFFFFF; for (let i = 0; i < b.length; i++) c = CRC[(c ^ b[i]) & 255] ^ (c >>> 8); return (c ^ 0xFFFFFFFF) >>> 0; };
  function zip(files) {
    const enc = new TextEncoder(), parts = [], cen = [];
    let off = 0;
    for (const [name, data] of Object.entries(files)) {
      const n = enc.encode(name), crc = crc32(data);
      const lh = new DataView(new ArrayBuffer(30));
      [[0, 0x04034b50, 4], [4, 20, 2], [6, 0, 2], [8, 0, 2], [10, 0, 2], [12, 0x21, 2], [14, crc, 4], [18, data.length, 4], [22, data.length, 4], [26, n.length, 2], [28, 0, 2]]
        .forEach(([o, v, w]) => w === 4 ? lh.setUint32(o, v, true) : lh.setUint16(o, v, true));
      parts.push(new Uint8Array(lh.buffer), n, data);
      const ch = new DataView(new ArrayBuffer(46));
      [[0, 0x02014b50, 4], [4, 20, 2], [6, 20, 2], [8, 0, 2], [10, 0, 2], [12, 0, 2], [14, 0x21, 2], [16, crc, 4], [20, data.length, 4], [24, data.length, 4], [28, n.length, 2], [30, 0, 2], [32, 0, 2], [34, 0, 2], [36, 0, 2], [38, 0, 4], [42, off, 4]]
        .forEach(([o, v, w]) => w === 4 ? ch.setUint32(o, v, true) : ch.setUint16(o, v, true));
      cen.push(new Uint8Array(ch.buffer), n);
      off += 30 + n.length + data.length;
    }
    const csize = cen.reduce((a, b) => a + b.length, 0), end = new DataView(new ArrayBuffer(22));
    end.setUint32(0, 0x06054b50, true); end.setUint16(8, Object.keys(files).length, true); end.setUint16(10, Object.keys(files).length, true);
    end.setUint32(12, csize, true); end.setUint32(16, off, true);
    const all = [...parts, ...cen, new Uint8Array(end.buffer)], out = new Uint8Array(all.reduce((a, b) => a + b.length, 0));
    let at = 0;
    for (const b of all) { out.set(b, at); at += b.length; }
    return out;
  }

  return { resolve, problems, apply, zip, sha256, hex, PAGES };
});
