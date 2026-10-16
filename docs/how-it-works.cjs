// Regenerate docs/how-it-works.svg (hand-drawn with rough.js, https://roughjs.com):
//   npm install roughjs@4.6.6
//   download a Caveat woff2 from Google Fonts (OFL), then:
//   node docs/how-it-works.cjs node_modules/roughjs/bundled/rough.cjs.js caveat.woff2 docs/how-it-works.svg
const fs = require("fs");
const [roughPath, fontPath, outPath] = process.argv.slice(2);
const rough = require(require("path").resolve(roughPath));
const gen = rough.generator();

const W = 1200, TOP = 85, H = 520;

// Colors are tokens mapped to CSS classes.
const C = { ink: "#000001", blue: "#000002", orange: "#000003", green: "#000004", soft: "#000005", muted: "#000006" };
const CLASS = Object.fromEntries(Object.entries(C).map(([k, v]) => [v, k]));

const parts = [];
let seed = 1;

function draw(drawable, extraAttrs = "") {
  for (const p of gen.toPaths(drawable)) {
    const strokeCls = CLASS[p.stroke];
    const fillCls = p.fill && p.fill !== "none" ? CLASS[p.fill] : null;
    const cls = [strokeCls && `s-${strokeCls}`, fillCls && `f-${fillCls}`].filter(Boolean).join(" ");
    parts.push(`<path d="${p.d}" class="${cls}" stroke-width="${p.strokeWidth}"${fillCls ? "" : ' fill="none"'}${extraAttrs}/>`);
  }
}

const o = (extra = {}) => ({ stroke: C.ink, strokeWidth: 2.2, roughness: 1.4, bowing: 1.2, seed: seed++, ...extra });

function text(x, y, str, { size = 24, cls = "ink", anchor = "middle", weight = 600 } = {}) {
  const esc = str.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
  parts.push(`<text x="${x}" y="${y}" font-size="${size}" font-weight="${weight}" text-anchor="${anchor}" class="t-${cls}">${esc}</text>`);
}

function arrowHead(tip, from, color, width = 2.6) {
  const a = Math.atan2(tip[1] - from[1], tip[0] - from[0]);
  const len = 18, spread = 0.45;
  const l = [tip[0] - len * Math.cos(a - spread), tip[1] - len * Math.sin(a - spread)];
  const r = [tip[0] - len * Math.cos(a + spread), tip[1] - len * Math.sin(a + spread)];
  draw(gen.linearPath([l, tip, r], o({ stroke: color, strokeWidth: width })));
}

// Curved arrow through a bulge point.
function arrow(from, via, to, color) {
  draw(gen.curve([from, via, to], o({ stroke: color, strokeWidth: 2.6 })));
  arrowHead(to, via, color);
}

// Wi-Fi symbol: a dot and three arcs opening upwards.
function wifi(cx, cy, color) {
  draw(gen.circle(cx, cy, 11, o({ stroke: color, fill: color, fillStyle: "solid", roughness: 0.6 })));
  for (const r of [16, 30, 44]) {
    draw(gen.arc(cx, cy, r * 2, r * 2, -Math.PI * 0.78, -Math.PI * 0.22, false, o({ stroke: color, strokeWidth: 3, roughness: 0.9 })));
  }
}

// The USB "trident": trunk with a dot at the base and an arrow tip, one branch
// ending in a circle, the other in a square.
function usbSymbol(cx, cy, color) {
  const s = o({ stroke: color, strokeWidth: 2.6, roughness: 0.7 });
  draw(gen.line(cx - 34, cy, cx + 26, cy, s));
  draw(gen.circle(cx - 36, cy, 11, o({ stroke: color, fill: color, fillStyle: "solid", roughness: 0.5 })));
  draw(gen.polygon([[cx + 24, cy - 8], [cx + 40, cy], [cx + 24, cy + 8]], o({ stroke: color, fill: color, fillStyle: "solid", roughness: 0.5 })));
  draw(gen.linearPath([[cx - 18, cy], [cx - 6, cy - 14], [cx + 8, cy - 14]], s));
  draw(gen.circle(cx + 12, cy - 14, 9, o({ stroke: color, strokeWidth: 2.4, roughness: 0.5 })));
  draw(gen.linearPath([[cx - 4, cy], [cx + 8, cy + 14], [cx + 16, cy + 14]], s));
  draw(gen.rectangle(cx + 16, cy + 10, 9, 9, o({ stroke: color, fill: color, fillStyle: "solid", roughness: 0.5 })));
}

// ---- AI agent: a laptop
draw(gen.rectangle(50, 120, 240, 150, o({ fill: C.blue, fillStyle: "hachure", hachureGap: 9, fillWeight: 1 })));
draw(gen.rectangle(62, 132, 216, 126, o({ fill: C.soft, fillStyle: "solid", roughness: 0.6 })));
draw(gen.polygon([[30, 270], [310, 270], [334, 298], [6, 298]], o()));
text(170, 168, "coding agent", { size: 30, weight: 700 });
text(170, 205, 'run_command("df -h")', { size: 24, cls: "blue" });
text(170, 240, "→ exit 0 · 37% used", { size: 24, cls: "orange" });
text(170, 342, "AI agent", { size: 36, weight: 700 });
text(170, 370, "Claude Code, Codex… any MCP client", { size: 22, cls: "muted", weight: 500 });

// ---- Wi-Fi link between the agent and the ESP32
arrow([318, 175], [412, 138], [500, 205], C.blue);
text(410, 120, "MCP over Wi-Fi", { size: 26, cls: "blue", weight: 700 });
wifi(412, 262, C.blue);
arrow([500, 282], [412, 322], [326, 262], C.orange);
text(415, 350, "output + exit code", { size: 24, cls: "orange", weight: 700 });

// ---- ESP32-S3 board
draw(gen.rectangle(510, 190, 180, 110, o({ fill: C.green, fillStyle: "hachure", hachureGap: 8, fillWeight: 1.2 })));
draw(gen.rectangle(565, 215, 64, 60, o({ fill: C.ink, fillStyle: "cross-hatch", hachureGap: 6 })));
for (let i = 0; i < 5; i++) {
  draw(gen.line(575 + i * 11, 205, 575 + i * 11, 215, o({ strokeWidth: 1.6, roughness: 0.8 })));
  draw(gen.line(575 + i * 11, 275, 575 + i * 11, 285, o({ strokeWidth: 1.6, roughness: 0.8 })));
}
draw(gen.linearPath([[522, 205], [532, 205], [532, 225], [542, 225], [542, 205], [552, 205]], o({ strokeWidth: 1.8 })));
draw(gen.rectangle(684, 236, 20, 26, o({ fill: C.ink, fillStyle: "solid" })));
text(600, 345, "ESP32-S3", { size: 36, weight: 700 });
text(600, 373, "~€8 · tiny MCP server", { size: 22, cls: "muted", weight: 500 });

// ---- USB cable from the ESP32 to the server
draw(gen.curve([[704, 249], [760, 246], [815, 268], [856, 272]], o({ strokeWidth: 7, roughness: 0.5, bowing: 0.5 })));
draw(gen.rectangle(850, 262, 26, 20, o({ fill: C.ink, fillStyle: "solid", roughness: 0.6 })));
text(788, 168, "USB keyboard →", { size: 24, cls: "blue", weight: 700 });
text(788, 190, "types the command", { size: 21, cls: "blue", weight: 500 });
usbSymbol(786, 222, C.ink);
text(788, 312, "← USB serial", { size: 24, cls: "orange", weight: 700 });
text(788, 334, "output comes back", { size: 21, cls: "orange", weight: 500 });

// ---- Headless server, with an optional screen on top
draw(gen.rectangle(876, 232, 296, 88, o({ fill: C.soft, fillStyle: "solid", roughness: 1 })));
draw(gen.rectangle(876, 232, 296, 88, o()));
for (let row = 0; row < 2; row++) {
  for (let i = 0; i < 4; i++) {
    draw(gen.rectangle(892 + i * 40, 248 + row * 30, 32, 18, o({ strokeWidth: 1.6, roughness: 0.9 })));
    draw(gen.line(898 + i * 40, 257 + row * 30, 916 + i * 40, 257 + row * 30, o({ strokeWidth: 1.4, roughness: 0.6 })));
  }
}
for (let i = 0; i < 4; i++) draw(gen.line(1062, 254 + i * 9, 1130, 254 + i * 9, o({ stroke: C.muted, strokeWidth: 1.6, roughness: 0.8 })));
draw(gen.circle(1150, 256, 10, o({ stroke: C.green, fill: C.green, fillStyle: "solid", roughness: 0.4 })));
draw(gen.circle(1150, 278, 10, o({ stroke: C.green, fill: C.green, fillStyle: "solid", roughness: 0.4 })));
draw(gen.circle(1150, 302, 14, o({ strokeWidth: 1.8, roughness: 0.6 })));

const dashed = ' stroke-dasharray="9 7"';
draw(gen.rectangle(950, 100, 172, 110, o({ stroke: C.muted, roughness: 0.9 })), dashed);
draw(gen.line(1036, 210, 1036, 232, o({ stroke: C.muted, strokeWidth: 2.4, roughness: 0.5 })), dashed);
text(966, 138, "$ df -h", { size: 24, anchor: "start", cls: "blue" });
text(966, 166, "/dev/sda1  37%", { size: 22, anchor: "start", weight: 500 });
text(1036, 196, "screen optional", { size: 19, cls: "muted", weight: 500 });

text(1024, 362, "Your server / PC", { size: 36, weight: 700 });
text(1024, 390, "headless is fine · BIOS · login · shell", { size: 22, cls: "muted", weight: 500 });

// ---- Caption
draw(gen.line(330, 438, 870, 438, o({ stroke: C.blue, strokeWidth: 1.6, roughness: 2 })));
text(600, 474, "One USB cable. Nothing to install on the target.", { size: 34, weight: 700 });
text(600, 505, "Works where SSH can't: no network, broken drivers, boot menus.", { size: 22, cls: "muted", weight: 500 });

const font = fs.readFileSync(fontPath).toString("base64");
const svg = `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 ${TOP} ${W} ${H - TOP}" width="${W}" height="${H - TOP}" role="img" aria-label="An AI coding agent sends a command over Wi-Fi (MCP) to an ESP32-S3. The ESP32 is plugged into a server with a USB cable: it types the command as a USB keyboard, and the output comes back over USB serial to the agent with the exit code.">
<style>
@font-face{font-family:"Caveat";src:url(data:font/woff2;base64,${font}) format("woff2");font-weight:500 700}
text{white-space:pre;font-family:Caveat,"Segoe Print","Comic Sans MS",cursive}
path{stroke-linecap:round;stroke-linejoin:round}
.s-ink{stroke:#1e1e1e}.s-blue{stroke:#1971c2}.s-orange{stroke:#e8590c}.s-green{stroke:#2f9e44}.s-soft{stroke:none}.s-muted{stroke:#868e96}
.f-ink{fill:#1e1e1e}.f-blue{fill:#1971c2}.f-orange{fill:#e8590c}.f-green{fill:#2f9e44}.f-soft{fill:#f1f3f5}.f-muted{fill:#868e96}
.t-ink{fill:#1e1e1e}.t-blue{fill:#1971c2}.t-orange{fill:#e8590c}.t-muted{fill:#6b6b6b}
</style>
<rect x="0" y="${TOP}" width="${W}" height="${H - TOP}" fill="#ffffff"/>
${parts.join("\n")}
</svg>
`;
fs.writeFileSync(outPath, svg);
console.log(`wrote ${outPath} (${(svg.length / 1024).toFixed(0)} KB, ${parts.length} elements)`);
