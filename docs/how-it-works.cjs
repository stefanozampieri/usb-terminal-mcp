// Regenerate docs/how-it-works.svg (hand-drawn with rough.js, https://roughjs.com):
//   npm install roughjs@4.6.6
//   download a Caveat woff2 from Google Fonts (OFL), then:
//   node docs/how-it-works.cjs node_modules/roughjs/bundled/rough.cjs.js caveat.woff2 docs/how-it-works.svg
const fs = require("fs");
const [roughPath, fontPath, outPath] = process.argv.slice(2);
const rough = require(require("path").resolve(roughPath));
const gen = rough.generator();

const W = 1200, H = 520;

// Colors are tokens mapped to CSS classes so the SVG follows light/dark mode.
const C = { ink: "#000001", blue: "#000002", orange: "#000003", green: "#000004", soft: "#000005" };
const CLASS = Object.fromEntries(Object.entries(C).map(([k, v]) => [v, k]));

const parts = [];
let seed = 1;

function draw(drawable) {
  for (const p of gen.toPaths(drawable)) {
    const strokeCls = CLASS[p.stroke];
    const fillCls = p.fill && p.fill !== "none" ? CLASS[p.fill] : null;
    const cls = [strokeCls && `s-${strokeCls}`, fillCls && `f-${fillCls}`].filter(Boolean).join(" ");
    parts.push(`<path d="${p.d}" class="${cls}" stroke-width="${p.strokeWidth}" fill="${fillCls ? "" : "none"}"/>`.replace(' fill=""', ""));
  }
}

const o = (extra = {}) => ({ stroke: C.ink, strokeWidth: 2.2, roughness: 1.4, bowing: 1.2, seed: seed++, ...extra });

function text(x, y, str, { size = 24, cls = "ink", anchor = "middle", weight = 600 } = {}) {
  const esc = str.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
  parts.push(`<text x="${x}" y="${y}" font-size="${size}" font-weight="${weight}" text-anchor="${anchor}" class="t-${cls}">${esc}</text>`);
}

// Curved arrow through a bulge point, with a sketchy head at the end.
function arrow(from, via, to, color) {
  draw(gen.curve([from, via, to], o({ stroke: color, strokeWidth: 2.6 })));
  const a = Math.atan2(to[1] - via[1], to[0] - via[0]);
  const len = 18, spread = 0.45;
  const l = [to[0] - len * Math.cos(a - spread), to[1] - len * Math.sin(a - spread)];
  const r = [to[0] - len * Math.cos(a + spread), to[1] - len * Math.sin(a + spread)];
  draw(gen.linearPath([l, to, r], o({ stroke: color, strokeWidth: 2.6 })));
}

// ---- AI agent: a laptop
draw(gen.rectangle(50, 120, 240, 150, o({ fill: C.blue, fillStyle: "hachure", hachureGap: 9, fillWeight: 1 })));
draw(gen.rectangle(62, 132, 216, 126, o({ fill: C.soft, fillStyle: "solid", roughness: 0.6 })));
draw(gen.polygon([[30, 270], [310, 270], [334, 298], [6, 298]], o()));
text(170, 168, "Claude Code", { size: 30, weight: 700 });
text(170, 205, 'run_command("df -h")', { size: 24, cls: "blue" });
text(170, 240, "→ exit 0 · 37% used", { size: 24, cls: "orange" });
text(170, 342, "AI agent", { size: 36, weight: 700 });
text(170, 370, "Claude Code or any MCP client", { size: 22, cls: "muted", weight: 500 });

// ---- ESP32-S3 board
draw(gen.rectangle(510, 190, 180, 110, o({ fill: C.green, fillStyle: "hachure", hachureGap: 8, fillWeight: 1.2 })));
draw(gen.rectangle(565, 215, 64, 60, o({ fill: C.ink, fillStyle: "cross-hatch", hachureGap: 6 })));
for (let i = 0; i < 5; i++) {
  draw(gen.line(575 + i * 11, 205, 575 + i * 11, 215, o({ strokeWidth: 1.6, roughness: 0.8 })));
  draw(gen.line(575 + i * 11, 275, 575 + i * 11, 285, o({ strokeWidth: 1.6, roughness: 0.8 })));
}
draw(gen.linearPath([[522, 205], [532, 205], [532, 225], [542, 225], [542, 205], [552, 205]], o({ strokeWidth: 1.8 })));
draw(gen.rectangle(686, 225, 18, 22, o({ fill: C.ink, fillStyle: "solid" })));
draw(gen.rectangle(686, 258, 18, 22, o({ fill: C.ink, fillStyle: "solid" })));
for (const r of [22, 38, 54]) draw(gen.arc(600, 178, r * 2, r * 2, -Math.PI * 0.78, -Math.PI * 0.22, false, o({ stroke: C.blue, strokeWidth: 2.4 })));
text(600, 345, "ESP32-S3", { size: 36, weight: 700 });
text(600, 373, "~€8 · tiny MCP server", { size: 22, cls: "muted", weight: 500 });

// ---- Target computer: a monitor
draw(gen.rectangle(880, 100, 280, 190, o()));
draw(gen.rectangle(892, 112, 256, 166, o({ fill: C.soft, fillStyle: "solid", roughness: 0.6 })));
draw(gen.line(1020, 290, 1020, 318, o({ strokeWidth: 3 })));
draw(gen.line(965, 320, 1075, 320, o({ strokeWidth: 3 })));
text(905, 148, "$ df -h", { size: 24, anchor: "start", cls: "blue" });
text(905, 180, "Filesystem   Size  Use%", { size: 22, anchor: "start", weight: 500 });
text(905, 208, "/dev/sda1    936G   37%", { size: 22, anchor: "start", weight: 500 });
text(905, 248, "$ _", { size: 24, anchor: "start" });
text(1020, 360, "Your server / PC", { size: 36, weight: 700 });
text(1020, 388, "BIOS · login · shell · no network needed", { size: 22, cls: "muted", weight: 500 });

// ---- Arrows
arrow([318, 175], [412, 150], [500, 210], C.blue);
text(410, 132, "MCP over Wi-Fi", { size: 26, cls: "blue", weight: 700 });

arrow([712, 232], [792, 214], [868, 202], C.blue);
text(790, 164, "USB keyboard", { size: 24, cls: "blue", weight: 700 });
text(790, 186, "types the command", { size: 21, cls: "blue", weight: 500 });

arrow([868, 262], [792, 284], [712, 268], C.orange);
text(790, 312, "USB serial", { size: 24, cls: "orange", weight: 700 });
text(790, 334, "output comes back", { size: 21, cls: "orange", weight: 500 });

arrow([500, 282], [412, 318], [326, 262], C.orange);
text(415, 346, "output + exit code", { size: 24, cls: "orange", weight: 700 });

// ---- Caption
draw(gen.line(330, 438, 870, 438, o({ stroke: C.blue, strokeWidth: 1.6, roughness: 2 })));
text(600, 474, "One USB cable. Nothing to install on the target.", { size: 34, weight: 700 });
text(600, 505, "Works where SSH can't: no network, broken drivers, boot menus.", { size: 22, cls: "muted", weight: 500 });

const font = fs.readFileSync(fontPath).toString("base64");
const svg = `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 85 ${W} ${H - 85}" width="${W}" height="${H - 85}" role="img" aria-label="An AI agent sends a command over Wi-Fi (MCP) to an ESP32-S3, which types it on the target computer as a USB keyboard; the output comes back over USB serial and returns to the agent with the exit code.">
<style>
@font-face{font-family:"Caveat";src:url(data:font/woff2;base64,${font}) format("woff2");font-weight:500 700}
text{white-space:pre;font-family:Caveat,"Segoe Print","Comic Sans MS",cursive}
path{stroke-linecap:round;stroke-linejoin:round}
.s-ink{stroke:#1e1e1e}.s-blue{stroke:#1971c2}.s-orange{stroke:#e8590c}.s-green{stroke:#2f9e44}.s-soft{stroke:none}
.f-ink{fill:#1e1e1e}.f-blue{fill:#1971c2}.f-orange{fill:#e8590c}.f-green{fill:#2f9e44}.f-soft{fill:#f1f3f5}
.t-ink{fill:#1e1e1e}.t-blue{fill:#1971c2}.t-orange{fill:#e8590c}.t-muted{fill:#6b6b6b}
@media (prefers-color-scheme:dark){
.s-ink{stroke:#e9ecef}.s-blue{stroke:#74c0fc}.s-orange{stroke:#ffa94d}.s-green{stroke:#69db7c}
.f-ink{fill:#e9ecef}.f-blue{fill:#74c0fc}.f-orange{fill:#ffa94d}.f-green{fill:#69db7c}.f-soft{fill:#25262b}
.t-ink{fill:#e9ecef}.t-blue{fill:#74c0fc}.t-orange{fill:#ffa94d}.t-muted{fill:#a6a7ab}}
</style>
${parts.join("\n")}
</svg>
`;
fs.writeFileSync(outPath, svg);
console.log(`wrote ${outPath} (${(svg.length / 1024).toFixed(0)} KB, ${parts.length} elements)`);
