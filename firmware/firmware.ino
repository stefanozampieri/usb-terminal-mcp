// ESP32-S3 terminal for AI agents: a USB keyboard plus a USB serial port,
// driven by a tiny MCP server over Wi-Fi.
//
// USB-OTG port  -> the computer to control. It sees a plain USB keyboard and
//                  a serial port (/dev/ttyACMx) that carries terminal output.
// USB-UART port -> optional, for flashing, Wi-Fi setup and serial logs
//
// MCP endpoint: POST http://<ip>/mcp  (Streamable HTTP, JSON responses only)
// Auth:         Authorization: Bearer <token generated on first boot>
//
// Setup: Wi-Fi comes from the browser installer (Improv serial), from the
// "usb-terminal-XXXX" setup network the device opens when it has no working
// Wi-Fi, or from an optional secrets.h baked in at compile time.

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <vector>
#include <esp_mac.h>
#include "USB.h"
#include "USBHIDKeyboard.h"
#include "USBCDC.h"
#if __has_include("secrets.h")
#include "secrets.h"  // optional compile-time Wi-Fi/token; wins over saved settings
#endif
#include "keymap.h"
#include "terminal.h"
#include "improv.h"

static const char *FW_VERSION = "3.0.0";
static const char *FW_NAME = "USB Terminal MCP";
static const size_t MAX_TEXT = 5000;            // characters per typed/sent call
static const size_t LOG_CAPACITY = 1024 * 1024; // terminal output kept in PSRAM
static const size_t MAX_RETURN = 12000;         // characters of output per tool result
static const int BOOT_BUTTON = 0;               // GPIO0, the BOOT button on most S3 boards
static const uint32_t STA_GIVE_UP_MS = 30000;   // open the setup network after this long offline
static const uint32_t SETUP_WINDOW_MS = 10 * 60 * 1000;  // after provisioning
static const uint32_t BUTTON_WINDOW_MS = 2 * 60 * 1000;  // after a BOOT press

USBHIDKeyboard Keyboard;
WebServer server(80);
DNSServer dns;
Preferences prefs;
OutputLog outputLog;
USBCDC TermSerial;  // the serial port the host sees as /dev/ttyACMx

String deviceName;  // usb-terminal-xxxx: hostname, setup network and mDNS name
String wifiSsid, wifiPass;
String mcpToken;
bool apActive = false;           // the usb-terminal-XXXX setup network is up
uint32_t lastOnlineMs = 0;       // last time the station had an IP
uint32_t setupWindowUntil = 0;   // setup page shows the token and accepts changes until then

Layout layout = LAYOUT_US;
InputMode inputMode = INPUT_KEYBOARD;
uint16_t keyDelayMs = 8;  // key hold time and gap between keystrokes
volatile bool usbMounted = false;
volatile bool hostPortOpen = false;  // a program on the host has our serial port open (DTR)

uint64_t readCursor = 0;  // terminal_read continues from here
String pendingId;         // end marker of a command still running after run_command timed out

// ---------------------------------------------------------------- serial port

// Move whatever the host wrote to the serial port into the log. Called from
// every wait so the host never blocks on a full USB buffer.
static void pumpSerial() {
  uint8_t tmp[512];
  while (TermSerial.available() > 0) {
    size_t n = TermSerial.read(tmp, sizeof tmp);
    if (!n) break;
    outputLog.append(tmp, n);
  }
}

static void waitMs(uint32_t ms) {
  uint32_t start = millis();
  do {
    pumpSerial();
    delay(1);
  } while (millis() - start < ms);
}

// Serial input goes to a tty, where Enter is \r.
static void serialSend(const char *text) {
  for (const char *p = text; *p; p++) {
    char c = *p == '\n' ? '\r' : *p;
    TermSerial.write(uint8_t(c));
  }
  TermSerial.flush();
}

// ---------------------------------------------------------------- keyboard

static void releaseAll() {
  KeyReport r = {};
  Keyboard.sendReport(&r);
}

// Press modifiers, then the key, then release everything. Sending modifiers a
// moment before the key keeps slow hosts (BIOS, consoles) from missing Shift.
static void sendStroke(uint8_t mod, uint8_t key, uint16_t holdMs) {
  KeyReport r = {};
  r.modifiers = mod;
  if (mod) {
    Keyboard.sendReport(&r);
    waitMs(holdMs / 2 + 1);
  }
  r.keys[0] = key;
  Keyboard.sendReport(&r);
  waitMs(holdMs);
  releaseAll();
  waitMs(holdMs);
}

static TypeResult typeText(const char *text, uint16_t delayMs) {
  TypeResult res;
  for (const char *p = text; *p;) {
    uint32_t cp = nextCodepoint(p);
    if (cp == '\r') continue;
    Stroke s;
    if (mapChar(cp, layout, s)) {
      sendStroke(s.mod, s.key, delayMs);
      if (isDeadKey(cp, layout)) sendStroke(0, 0x2C, delayMs);  // dead key + space = plain char
      res.typed++;
    } else {
      if (res.skipped < 10) {
        char buf[12];
        snprintf(buf, sizeof buf, "%sU+%04X", res.skipped ? ", " : "", (unsigned)cp);
        res.skippedList += buf;
      }
      res.skipped++;
    }
  }
  return res;
}

static const NamedKey NAMED_KEYS[] = {
  {"enter", 0x28}, {"return", 0x28}, {"esc", 0x29}, {"escape", 0x29}, {"backspace", 0x2A},
  {"tab", 0x2B}, {"space", 0x2C}, {"capslock", 0x39},
  {"f1", 0x3A}, {"f2", 0x3B}, {"f3", 0x3C}, {"f4", 0x3D}, {"f5", 0x3E}, {"f6", 0x3F},
  {"f7", 0x40}, {"f8", 0x41}, {"f9", 0x42}, {"f10", 0x43}, {"f11", 0x44}, {"f12", 0x45},
  {"printscreen", 0x46}, {"prtsc", 0x46}, {"sysrq", 0x46}, {"scrolllock", 0x47}, {"pause", 0x48},
  {"insert", 0x49}, {"ins", 0x49}, {"home", 0x4A}, {"pageup", 0x4B}, {"pgup", 0x4B},
  {"delete", 0x4C}, {"del", 0x4C}, {"end", 0x4D}, {"pagedown", 0x4E}, {"pgdn", 0x4E},
  {"right", 0x4F}, {"left", 0x50}, {"down", 0x51}, {"up", 0x52}, {"menu", 0x65},
};

static const NamedKey MODIFIERS[] = {
  {"ctrl", MOD_LCTRL}, {"control", MOD_LCTRL}, {"lctrl", MOD_LCTRL}, {"rctrl", MOD_RCTRL},
  {"shift", MOD_LSHIFT}, {"lshift", MOD_LSHIFT}, {"rshift", MOD_RSHIFT},
  {"alt", MOD_LALT}, {"lalt", MOD_LALT}, {"altgr", MOD_RALT}, {"ralt", MOD_RALT},
  {"win", MOD_LGUI}, {"super", MOD_LGUI}, {"meta", MOD_LGUI}, {"gui", MOD_LGUI}, {"cmd", MOD_LGUI},
};

static bool findIn(const NamedKey *table, size_t n, const String &name, uint8_t &out) {
  for (size_t i = 0; i < n; i++) {
    if (name == table[i].name) {
      out = table[i].key;
      return true;
    }
  }
  return false;
}

// Parse one combo like "ctrl+alt+f2", "enter", "ctrl++" or "win".
static bool parseCombo(String combo, uint8_t &mod, uint8_t &key, String &err) {
  mod = 0;
  key = 0;
  std::vector<String> parts;
  // A trailing '+' is the plus key itself ("ctrl++", "+").
  bool plusKey = combo.endsWith("+");
  if (plusKey) combo.remove(combo.length() - 1);
  int start = 0;
  while (start <= (int)combo.length()) {
    int plus = combo.indexOf('+', start);
    if (plus < 0) plus = combo.length();
    String part = combo.substring(start, plus);
    if (part.length()) parts.push_back(part);
    start = plus + 1;
  }
  if (plusKey) parts.push_back("+");
  if (parts.empty()) {
    err = "empty key combo";
    return false;
  }

  for (size_t i = 0; i < parts.size(); i++) {
    String name = parts[i];
    bool last = i == parts.size() - 1;
    String lower = name;
    lower.toLowerCase();
    uint8_t m;
    if (findIn(MODIFIERS, sizeof MODIFIERS / sizeof *MODIFIERS, lower, m)) {
      mod |= m;  // a lone modifier (e.g. "win") is allowed as the last part
      continue;
    }
    if (!last) {
      err = "unknown modifier '" + name + "'";
      return false;
    }
    if (findIn(NAMED_KEYS, sizeof NAMED_KEYS / sizeof *NAMED_KEYS, lower, key)) return true;
    // Otherwise a single character, typed through the current layout.
    // Letters are lowercased so "ctrl+C" means Ctrl+C, not Ctrl+Shift+C.
    const char *p = name.c_str();
    uint32_t cp = nextCodepoint(p);
    if (*p) {
      err = "unknown key '" + name + "'";
      return false;
    }
    if (cp >= 'A' && cp <= 'Z') cp += 'a' - 'A';
    Stroke s;
    if (!mapChar(cp, layout, s)) {
      err = "key '" + name + "' not available on this layout";
      return false;
    }
    mod |= s.mod;
    key = s.key;
  }
  return true;
}

// Space-separated combos, each optionally repeated with "*N" (e.g. "down*5").
// Everything is parsed before anything is pressed, so a typo presses nothing.
static bool pressKeys(const String &spec, String &err, int &count) {
  struct Step {
    uint8_t mod, key;
    int repeat;
  };
  std::vector<Step> steps;
  int i = 0;
  while (i < (int)spec.length()) {
    while (i < (int)spec.length() && isspace((unsigned char)spec[i])) i++;
    int j = i;
    while (j < (int)spec.length() && !isspace((unsigned char)spec[j])) j++;
    if (j > i) {
      String tok = spec.substring(i, j);
      int repeat = 1;
      int star = tok.lastIndexOf('*');
      if (star > 0 && star < (int)tok.length() - 1) {
        repeat = tok.substring(star + 1).toInt();
        if (repeat < 1 || repeat > 100) {
          err = "repeat count must be 1-100 in '" + tok + "'";
          return false;
        }
        tok = tok.substring(0, star);
      }
      Step s{0, 0, repeat};
      if (!parseCombo(tok, s.mod, s.key, err)) return false;
      steps.push_back(s);
    }
    i = j;
  }
  if (steps.empty()) {
    err = "no keys given";
    return false;
  }
  // Combos are held a little longer than typed text; some hosts need it for
  // things like Ctrl+Alt+F2.
  uint16_t hold = max<uint16_t>(keyDelayMs, 30);
  count = 0;
  for (const Step &s : steps) {
    for (int r = 0; r < s.repeat; r++) {
      sendStroke(s.mod, s.key, hold);
      count++;
    }
  }
  return true;
}

// ---------------------------------------------------------------- terminal

// Send text to the host's shell through the current input mode.
static void sendInput(const char *text) {
  if (inputMode == INPUT_SERIAL) serialSend(text);
  else typeText(text, keyDelayMs);
}

// Wrapping commands in { ...\n} keeps multi-line commands and heredocs intact.
// The printf format splits the marker so the echoed command line never
// contains the marker itself, only the real output does.
static String wrapCommand(const String &cmd, const String &id) {
  return "{ " + cmd + "\n}; printf '\\n__MCPEND_%s_%s__\\n' " + id + " $?\n";
}

// Find the end marker of command `id` at or after `from`. Returns its offset
// (or -1) and fills in the exit code and the offset just past the marker.
static int64_t findMarker(const String &id, uint64_t from, int &exitCode, uint64_t &after) {
  String prefix = "__MCPEND_" + id + "_";
  int64_t pos = outputLog.find(prefix.c_str(), from);
  if (pos < 0) return -1;
  uint64_t digits = pos + prefix.length();
  String tail = outputLog.slice(digits, digits + 8);
  int close = tail.indexOf("__");
  if (close < 0) return -1;  // marker not fully received yet
  exitCode = tail.substring(0, close).toInt();
  after = digits + close + 2;
  return pos;
}

// Clean output in [from, to), keeping the tail if it's too long. If echoId is
// given, drop the echoed command lines (everything up to "<id> $?").
static String collectOutput(uint64_t from, uint64_t to, const String &echoId, bool &truncated) {
  truncated = false;
  uint64_t rawLimit = MAX_RETURN * 3;  // raw output shrinks once escapes are stripped
  if (from < outputLog.start()) {
    from = outputLog.start();
    truncated = true;
  }
  if (to > from + rawLimit) {
    from = to - rawLimit;
    truncated = true;
  }
  String text = cleanTerminal(outputLog.slice(from, to));
  if (echoId.length()) {
    int e = text.indexOf(echoId + " $?");
    if (e >= 0) {
      int nl = text.indexOf('\n', e);
      text = nl >= 0 ? text.substring(nl + 1) : "";
    }
  }
  text.trim();
  if (text.length() > MAX_RETURN) {
    text = text.substring(text.length() - MAX_RETURN);
    truncated = true;
  }
  return text;
}

static String newId() {
  char buf[9];
  snprintf(buf, sizeof buf, "%08lx", (unsigned long)esp_random());
  return buf;
}

static String runCommand(const String &cmd, uint32_t timeoutS, bool &isError) {
  isError = false;
  pumpSerial();
  String id = newId();
  uint64_t start = outputLog.end();
  sendInput(wrapCommand(cmd, id).c_str());

  int exitCode = 0;
  uint64_t after = 0;
  int64_t marker = -1;
  uint64_t scanFrom = start;
  uint32_t deadline = millis() + timeoutS * 1000;
  while ((int32_t)(millis() - deadline) < 0) {
    waitMs(20);
    marker = findMarker(id, scanFrom, exitCode, after);
    if (marker >= 0) break;
    // Only rescan the tail; the marker can straddle the previous end.
    uint64_t end = outputLog.end();
    scanFrom = end > start + 32 ? max(start, end - 32) : start;
  }

  bool truncated;
  if (marker >= 0) {
    String out = collectOutput(start, marker, id, truncated);
    readCursor = after;
    pendingId = "";
    String res = "exit code: " + String(exitCode) + "\n";
    if (truncated) res += "(output truncated, showing the end; pipe through head/tail/grep for less)\n";
    return res + out;
  }

  // Still running (or waiting for input, e.g. a sudo password or a y/n prompt).
  uint64_t end = outputLog.end();
  String out = collectOutput(start, end, id, truncated);
  readCursor = end;
  pendingId = id;
  if (end == start) {
    isError = true;
    return "No terminal output received within " + String(timeoutS) +
           "s. The output channel is probably not attached: in keyboard mode call attach_terminal "
           "(the console must be at a shell prompt); in serial mode check the serial-getty session.";
  }
  return "Command still running after " + String(timeoutS) +
         "s (it may be waiting for input). Use terminal_read to keep reading, terminal_send to answer a "
         "prompt, or press_keys 'ctrl+c' / terminal_send '\\u0003' to interrupt. Output so far:\n" + out;
}

static String terminalRead(uint32_t waitMsMax) {
  uint32_t start = millis();
  uint64_t lastEnd = outputLog.end();
  uint32_t lastChange = millis();
  int exitCode = 0;
  uint64_t after = 0;
  int64_t marker = -1;
  while (millis() - start < waitMsMax) {
    waitMs(20);
    if (pendingId.length() && (marker = findMarker(pendingId, readCursor, exitCode, after)) >= 0) break;
    uint64_t end = outputLog.end();
    if (end != lastEnd) {
      lastEnd = end;
      lastChange = millis();
    } else if (end > readCursor && millis() - lastChange > 400) {
      break;  // got output and it has gone quiet
    }
  }

  bool truncated;
  String res;
  if (marker >= 0) {
    res = collectOutput(readCursor, marker, "", truncated);
    res += "\n[command finished, exit code " + String(exitCode) + "]";
    pendingId = "";
    readCursor = after;
  } else {
    uint64_t end = outputLog.end();
    res = collectOutput(readCursor, end, "", truncated);
    readCursor = end;
    if (!res.length()) res = "(no new output)";
  }
  if (truncated) res = "(output truncated, showing the end)\n" + res;
  return res;
}

// Type the one-liner that mirrors the console session to our serial port.
static String attachTerminal() {
  pumpSerial();
  uint64_t start = outputLog.end();
  typeText(
    "export PAGER=cat SYSTEMD_PAGER=cat GIT_PAGER=cat; "
    "D=$(ls /dev/serial/by-id/*USB_Terminal* | head -n1); "
    "sudo chown $USER \"$D\" && script -qf \"$D\"\n",
    keyDelayMs);
  uint32_t t0 = millis();
  while (millis() - t0 < 30000 && outputLog.end() == start) waitMs(50);
  if (outputLog.end() == start) {
    return "Typed the attach command but no output arrived within 30s. sudo is probably waiting for "
           "the password: the user has to type it on the machine. Call terminal_read or run_command "
           "'true' afterwards to check.";
  }
  readCursor = outputLog.end();
  pendingId = "";
  return "Terminal attached: the console session is now mirrored to the ESP32. Use run_command. "
         "Type 'exit' (run_command is fine) to detach.";
}

// ---------------------------------------------------------------- settings

static const char *layoutName(Layout l) {
  return l == LAYOUT_IT ? "it" : l == LAYOUT_US_INTL ? "us-intl" : "us";
}

static const char *inputName(InputMode m) { return m == INPUT_SERIAL ? "serial" : "keyboard"; }

static void loadSettings() {
  prefs.begin("kbd", true);
  layout = Layout(prefs.getUChar("layout", LAYOUT_US));
  inputMode = InputMode(prefs.getUChar("input", INPUT_KEYBOARD));
  keyDelayMs = prefs.getUShort("delay", 8);
  prefs.end();
}

static void saveSettings() {
  prefs.begin("kbd", false);
  prefs.putUChar("layout", layout);
  prefs.putUChar("input", inputMode);
  prefs.putUShort("delay", keyDelayMs);
  prefs.end();
}

static String statusText() {
  String s;
  s += "firmware: " + String(FW_VERSION) + "\n";
  s += "usb_host_connected: " + String(usbMounted ? "yes" : "no") + "\n";
  s += "host_serial_port_open: " + String(hostPortOpen ? "yes" : "no") + "\n";
  s += "input_mode: " + String(inputName(inputMode)) + "\n";
  s += "layout: " + String(layoutName(layout)) + "\n";
  s += "key_delay_ms: " + String(keyDelayMs) + "\n";
  s += "terminal_bytes_received: " + String((unsigned long)outputLog.end()) + "\n";
  s += "command_still_running: " + String(pendingId.length() ? "yes" : "no") + "\n";
  s += "device_name: " + deviceName + "\n";
  s += "wifi_ssid: " + wifiSsid + "\n";
  s += "wifi_ip: " + WiFi.localIP().toString() + "\n";
  s += "setup_network_active: " + String(apActive ? "yes" : "no") + "\n";
  s += "wifi_rssi_dbm: " + String(WiFi.RSSI()) + "\n";
  s += "uptime_s: " + String(millis() / 1000);
  return s;
}

// ---------------------------------------------------------------- MCP

static const char *SERVER_INSTRUCTIONS =
  "This server is a terminal on another computer, reached through a USB device plugged into it: "
  "a keyboard plus a serial port that carries terminal output back. "
  "Normal flow: get_status, then attach_terminal once (needs the console at a shell prompt; the "
  "user types the sudo password on the machine if asked), then run_command for everything; it "
  "returns the output and exit code. Commands needing input (passwords, y/n prompts) return "
  "'still running': answer with terminal_send and continue with terminal_read. Prefer "
  "non-interactive flags (apt -y, --no-pager). Never type passwords yourself; the user types them "
  "on the machine. type_text/press_keys drive the raw keyboard and work anywhere (BIOS, boot menu, "
  "login prompt) but return no output. The host's keyboard layout must match the configured "
  "layout; use us-intl if ' or \" merge with the next letter.";

static const char *TOOLS_JSON = R"JSON([
{
  "name": "run_command",
  "description": "Run a shell command on the attached computer and return its output and exit code. Works once the terminal is attached (attach_terminal in keyboard mode, or a serial-getty shell in serial mode). Multi-line commands and heredocs are fine. If the command is still running at the timeout (long job, or waiting for input like a password or y/n prompt) you get the output so far; continue with terminal_read / terminal_send. Avoid full-screen programs (vim, less, top); use non-interactive equivalents.",
  "inputSchema": {
    "type": "object",
    "properties": {
      "command": {"type": "string", "description": "Shell command(s) to run."},
      "timeout_s": {"type": "integer", "minimum": 1, "maximum": 600, "description": "How long to wait for completion. Default 30."}
    },
    "required": ["command"]
  }
},
{
  "name": "terminal_read",
  "description": "Read new terminal output since the last read, e.g. to follow a command that run_command reported as still running. Waits up to wait_ms for output (returns early when output goes quiet or the pending command finishes).",
  "inputSchema": {
    "type": "object",
    "properties": {
      "wait_ms": {"type": "integer", "minimum": 0, "maximum": 120000, "description": "Default 2000."}
    }
  }
},
{
  "name": "terminal_send",
  "description": "Send raw input to the terminal without waiting for a result, e.g. 'y\\n' to answer a prompt. \\n is Enter. In serial mode control characters work too ('\\u0003' = Ctrl+C, '\\u0004' = Ctrl+D); in keyboard mode use press_keys for those. Read the result with terminal_read.",
  "inputSchema": {
    "type": "object",
    "properties": {
      "text": {"type": "string"}
    },
    "required": ["text"]
  }
},
{
  "name": "attach_terminal",
  "description": "Keyboard mode only. Types a one-line command on the console that mirrors the session to the ESP32 serial port (script -qf /dev/serial/by-id/...USB_Terminal...), after which run_command returns output. The console must be at a logged-in shell prompt. Uses sudo once to get access to the serial port; the user types the password on the machine if asked.",
  "inputSchema": {"type": "object", "properties": {}}
},
{
  "name": "type_text",
  "description": "Type text on the raw USB keyboard, exactly as given. Works anywhere (BIOS, boot menu, login prompt) but returns no output. A newline (\\n) presses Enter and a tab (\\t) presses Tab; nothing is pressed at the end unless the text ends with \\n. Up to 5000 characters per call. Characters the current layout can't type are skipped and reported.",
  "inputSchema": {
    "type": "object",
    "properties": {
      "text": {"type": "string", "description": "Text to type."},
      "key_delay_ms": {"type": "integer", "minimum": 2, "maximum": 200, "description": "Optional per-call override of the delay between keystrokes. Raise it if the host drops characters."}
    },
    "required": ["text"]
  }
},
{
  "name": "press_keys",
  "description": "Press keys or shortcuts on the raw USB keyboard. Space-separated sequence of combos, each with optional *N repeat. Modifiers: ctrl, shift, alt, altgr, win. Named keys: enter, esc, tab, space, backspace, delete, insert, home, end, pageup, pagedown, up, down, left, right, f1-f12, printscreen, menu. Any single character also works. Examples: 'ctrl+c', 'ctrl+alt+f2', 'alt+sysrq+b', 'down*3 enter', 'ctrl+d'.",
  "inputSchema": {
    "type": "object",
    "properties": {
      "keys": {"type": "string", "description": "Key combos, e.g. 'ctrl+alt+delete' or 'down*3 enter'. Use type_text for words."}
    },
    "required": ["keys"]
  }
},
{
  "name": "configure",
  "description": "Change settings (saved across reboots). layout must match the keyboard layout configured on the attached computer; use us-intl when ' or \" merge with the next letter (e.g. 'i shows as í). input_mode: keyboard (commands are typed; output mirrored via attach_terminal) or serial (commands written to a login shell on the serial port, set up with serial-getty@ttyACM0).",
  "inputSchema": {
    "type": "object",
    "properties": {
      "layout": {"type": "string", "enum": ["us", "us-intl", "it"]},
      "input_mode": {"type": "string", "enum": ["keyboard", "serial"]},
      "key_delay_ms": {"type": "integer", "minimum": 2, "maximum": 200}
    }
  }
},
{
  "name": "get_status",
  "description": "Report whether the device is connected to a computer, whether the host has the serial port open (terminal attached), input mode, layout, typing speed and Wi-Fi status.",
  "inputSchema": {"type": "object", "properties": {}}
}
])JSON";

static void toolResult(JsonObject result, const String &text, bool isError = false) {
  JsonObject c = result["content"].to<JsonArray>().add<JsonObject>();
  c["type"] = "text";
  c["text"] = text;
  result["isError"] = isError;
}

static const char *NO_HOST_WARNING = " WARNING: no USB host detected; keystrokes probably went nowhere.";

static void callTool(JsonVariantConst params, JsonObject result) {
  String name = params["name"] | "";
  JsonVariantConst args = params["arguments"];

  if (name == "run_command") {
    String cmd = args["command"] | "";
    if (!cmd.length()) return toolResult(result, "missing 'command'", true);
    if (cmd.length() > MAX_TEXT) return toolResult(result, "command too long (max 5000 bytes); write a script file in steps", true);
    uint32_t timeoutS = constrain(args["timeout_s"] | 30, 1, 600);
    bool isError;
    String out = runCommand(cmd, timeoutS, isError);
    return toolResult(result, out, isError);
  }

  if (name == "terminal_read") {
    uint32_t wait = constrain(args["wait_ms"] | 2000, 0, 120000);
    return toolResult(result, terminalRead(wait));
  }

  if (name == "terminal_send") {
    const char *text = args["text"] | (const char *)nullptr;
    if (!text) return toolResult(result, "missing 'text'", true);
    if (strlen(text) > MAX_TEXT) return toolResult(result, "text too long (max 5000 bytes per call)", true);
    sendInput(text);
    return toolResult(result, String("Sent via ") + inputName(inputMode) + ". Use terminal_read for the result.");
  }

  if (name == "attach_terminal") {
    if (inputMode != INPUT_KEYBOARD) return toolResult(result, "attach_terminal is for keyboard mode; in serial mode the shell already runs on the serial port", true);
    if (!usbMounted) return toolResult(result, "no USB host detected", true);
    return toolResult(result, attachTerminal());
  }

  if (name == "type_text") {
    const char *text = args["text"] | (const char *)nullptr;
    if (!text) return toolResult(result, "missing 'text'", true);
    if (strlen(text) > MAX_TEXT) return toolResult(result, "text too long (max 5000 bytes per call); split it", true);
    uint16_t d = constrain(args["key_delay_ms"] | (int)keyDelayMs, 2, 200);
    TypeResult r = typeText(text, d);
    String msg = "Typed " + String(r.typed) + " characters.";
    if (r.skipped) msg += " Skipped " + String(r.skipped) + " characters not available on layout '" + layoutName(layout) + "': " + r.skippedList;
    if (!usbMounted) msg += NO_HOST_WARNING;
    return toolResult(result, msg, r.typed == 0 && r.skipped > 0);
  }

  if (name == "press_keys") {
    String keys = args["keys"] | "";
    String err;
    int count = 0;
    if (!pressKeys(keys, err, count)) return toolResult(result, "Nothing pressed: " + err, true);
    String msg = "Pressed " + String(count) + " key combo(s).";
    if (!usbMounted) msg += NO_HOST_WARNING;
    return toolResult(result, msg);
  }

  if (name == "configure") {
    if (args["layout"].is<const char *>()) {
      String l = args["layout"].as<String>();
      if (l == "us") layout = LAYOUT_US;
      else if (l == "it") layout = LAYOUT_IT;
      else if (l == "us-intl") layout = LAYOUT_US_INTL;
      else return toolResult(result, "unknown layout '" + l + "' (use us, us-intl or it)", true);
    }
    if (args["input_mode"].is<const char *>()) {
      String m = args["input_mode"].as<String>();
      if (m == "keyboard") inputMode = INPUT_KEYBOARD;
      else if (m == "serial") inputMode = INPUT_SERIAL;
      else return toolResult(result, "unknown input_mode '" + m + "' (use keyboard or serial)", true);
    }
    if (args["key_delay_ms"].is<int>()) keyDelayMs = constrain(args["key_delay_ms"].as<int>(), 2, 200);
    saveSettings();
    return toolResult(result, statusText());
  }

  if (name == "get_status") return toolResult(result, statusText());

  toolResult(result, "unknown tool '" + name + "'", true);
}

static void sendJson(int code, JsonDocument &doc) {
  String out;
  serializeJson(doc, out);
  server.send(code, "application/json", out);
}

static void sendRpcError(JsonVariantConst id, int code, const char *message) {
  JsonDocument res;
  res["jsonrpc"] = "2.0";
  res["id"] = id;
  res["error"]["code"] = code;
  res["error"]["message"] = message;
  sendJson(200, res);
}

static bool authorized() {
  return mcpToken.length() && server.header("Authorization") == "Bearer " + mcpToken;
}

static void handleMcpPost() {
  if (!authorized()) {
    server.send(401, "application/json", "{\"error\":\"unauthorized\"}");
    return;
  }
  JsonDocument req;
  if (deserializeJson(req, server.arg("plain"))) return sendRpcError(JsonVariantConst(), -32700, "parse error");
  if (!req.is<JsonObject>()) return sendRpcError(JsonVariantConst(), -32600, "batch requests not supported");

  // Notifications and client responses carry no id: acknowledge without a body.
  if (req["id"].isNull()) {
    server.send(202);
    return;
  }

  String method = req["method"] | "";
  JsonDocument res;
  res["jsonrpc"] = "2.0";
  res["id"] = req["id"];

  if (method == "initialize") {
    JsonObject r = res["result"].to<JsonObject>();
    r["protocolVersion"] = req["params"]["protocolVersion"] | "2025-06-18";
    r["capabilities"]["tools"].to<JsonObject>();
    r["serverInfo"]["name"] = "usb-terminal";
    r["serverInfo"]["version"] = FW_VERSION;
    r["instructions"] = SERVER_INSTRUCTIONS;
  } else if (method == "ping") {
    res["result"].to<JsonObject>();
  } else if (method == "tools/list") {
    JsonDocument tools;
    deserializeJson(tools, TOOLS_JSON);
    res["result"]["tools"] = tools;
  } else if (method == "tools/call") {
    Serial.printf("[mcp] tools/call %s\n", (const char *)(req["params"]["name"] | "?"));
    callTool(req["params"], res["result"].to<JsonObject>());
  } else {
    return sendRpcError(req["id"], -32601, "method not found");
  }
  sendJson(200, res);
}

// ---------------------------------------------------------------- setup

static void usbEvent(void *, esp_event_base_t base, int32_t id, void *) {
  if (base != ARDUINO_USB_EVENTS) return;
  switch (id) {
    case ARDUINO_USB_STARTED_EVENT:
    case ARDUINO_USB_RESUME_EVENT: usbMounted = true; break;
    case ARDUINO_USB_STOPPED_EVENT:
    case ARDUINO_USB_SUSPEND_EVENT: usbMounted = false; break;
  }
}

static void cdcEvent(void *, esp_event_base_t base, int32_t id, void *data) {
  if (base != ARDUINO_USB_CDC_EVENTS) return;
  if (id == ARDUINO_USB_CDC_LINE_STATE_EVENT) {
    hostPortOpen = ((arduino_usb_cdc_event_data_t *)data)->line_state.dtr;
  } else if (id == ARDUINO_USB_CDC_DISCONNECTED_EVENT) {
    hostPortOpen = false;
  }
}

static void wifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    Serial.printf("[wifi] connected, IP %s  ->  http://%s/mcp  (setup page: http://%s/setup)\n",
                  WiFi.localIP().toString().c_str(), WiFi.localIP().toString().c_str(),
                  WiFi.localIP().toString().c_str());
  } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED && !apActive) {
    Serial.printf("[wifi] disconnected (reason %u), retrying...\n", info.wifi_sta_disconnected.reason);
  }
}

// ---------------------------------------------------------------- Wi-Fi and token

static bool online() { return WiFi.status() == WL_CONNECTED; }

static bool setupWindowOpen() { return setupWindowUntil && (int32_t)(setupWindowUntil - millis()) > 0; }

// Changing Wi-Fi reveals the token afterwards, so it needs physical presence
// (BOOT press or Improv over the UART cable). The only exception is a board
// that was never configured, so first setup works from a phone.
static bool canChangeWifi() { return setupWindowOpen() || (apActive && !wifiSsid.length()); }

static String claudeCommand() {
  return "claude mcp add --transport http usb-terminal http://" + WiFi.localIP().toString() +
         "/mcp --header \"Authorization: Bearer " + mcpToken + "\"";
}

// Unlock the setup page (token + Wi-Fi changes) for a while and print the
// connection details on the UART log, which only someone at the device sees.
static void openSetupWindow(uint32_t ms) {
  setupWindowUntil = (millis() + ms) | 1;
  IPAddress ip = online() ? WiFi.localIP() : WiFi.softAPIP();
  Serial.printf("[setup] setup page unlocked for %lu s: http://%s/setup\n", (unsigned long)(ms / 1000), ip.toString().c_str());
  if (online()) Serial.println("[setup] " + claudeCommand());
}

static void loadNetworkConfig() {
  prefs.begin("kbd", true);
  wifiSsid = prefs.getString("ssid", "");
  wifiPass = prefs.getString("pass", "");
  mcpToken = prefs.getString("token", "");
  prefs.end();
#ifdef WIFI_SSID
  if (strlen(WIFI_SSID) && strcmp(WIFI_SSID, "YOUR_WIFI_NAME") != 0) {
    wifiSsid = WIFI_SSID;
    wifiPass = WIFI_PASS;
  }
#endif
#ifdef MCP_TOKEN
  if (strlen(MCP_TOKEN) && strcmp(MCP_TOKEN, "CHANGE_ME") != 0) mcpToken = MCP_TOKEN;
#endif
}

static void saveWifi() {
  prefs.begin("kbd", false);
  prefs.putString("ssid", wifiSsid);
  prefs.putString("pass", wifiPass);
  prefs.end();
}

// Call once the radio is on: the hardware RNG is only truly random then.
static void ensureToken() {
  if (mcpToken.length()) return;
  uint8_t b[24];
  esp_fill_random(b, sizeof b);
  char hex[2 * sizeof b + 1];
  for (size_t i = 0; i < sizeof b; i++) snprintf(hex + 2 * i, 3, "%02x", b[i]);
  mcpToken = hex;
  prefs.begin("kbd", false);
  prefs.putString("token", mcpToken);
  prefs.end();
  Serial.println("[setup] generated a new access token");
}

static void factoryReset() {
  Serial.println("[setup] factory reset: erasing Wi-Fi, token and settings");
  prefs.begin("kbd", false);
  prefs.clear();
  prefs.end();
  delay(200);
  ESP.restart();
}

static void startSetupNetwork() {
  if (apActive) return;
  WiFi.setAutoReconnect(false);  // station retries make the setup network hop channels
  if (!online()) WiFi.disconnect();
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(deviceName.c_str());
  dns.start(53, "*", WiFi.softAPIP());
  apActive = true;
  Serial.printf("[setup] no Wi-Fi: join the '%s' network and open http://%s/setup\n",
                deviceName.c_str(), WiFi.softAPIP().toString().c_str());
}

static void stopSetupNetwork() {
  if (!apActive) return;
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  apActive = false;
  Serial.println("[setup] Wi-Fi connected, setup network closed");
}

// Try credentials for up to timeoutMs; the setup network stays up meanwhile.
static bool tryConnect(const String &ssid, const String &pass, uint32_t timeoutMs) {
  Serial.printf("[wifi] connecting to '%s'...\n", ssid.c_str());
  WiFi.disconnect();
  WiFi.begin(ssid.c_str(), pass.c_str());
  uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs && !online()) {
    waitMs(100);
    if (apActive) dns.processNextRequest();
  }
  return online();
}

// Used by both Improv and the setup page. On success the new network is
// saved and the setup page is unlocked so it can show the token.
static bool provisionWifi(const String &ssid, const String &pass) {
  if (tryConnect(ssid, pass, 20000)) {
    wifiSsid = ssid;
    wifiPass = pass;
    saveWifi();
    lastOnlineMs = millis();
    openSetupWindow(SETUP_WINDOW_MS);
    return true;
  }
  Serial.printf("[wifi] could not connect to '%s'\n", ssid.c_str());
  if (wifiSsid.length()) WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  return false;
}

static void networkLoop() {
  if (online()) lastOnlineMs = millis();
  if (apActive) dns.processNextRequest();
  bool offlineTooLong = !online() && millis() - lastOnlineMs > STA_GIVE_UP_MS;
  if (!apActive && (!wifiSsid.length() || offlineTooLong)) startSetupNetwork();

  static uint32_t lastCheck = 0, lastRetry = 0;
  if (apActive && millis() - lastCheck > 5000) {
    lastCheck = millis();
    bool clients = WiFi.softAPgetStationNum() > 0;
    if (online() && !clients) stopSetupNetwork();
    // Retry the saved network now and then, but not while someone is on the setup page.
    else if (!online() && wifiSsid.length() && !clients && millis() - lastRetry > 60000) {
      lastRetry = millis();
      WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
    }
  }
}

// Short press: show the token on the setup page for 2 minutes.
// Hold 5 seconds: factory reset.
static void buttonLoop() {
  static uint32_t downSince = 0;
  bool down = digitalRead(BOOT_BUTTON) == LOW;
  if (down && !downSince) downSince = millis() | 1;
  if (down && millis() - downSince > 5000) factoryReset();
  if (!down && downSince) {
    if (millis() - downSince > 50) openSetupWindow(BUTTON_WINDOW_MS);
    downSince = 0;
  }
}

// ---------------------------------------------------------------- Improv

static std::vector<std::vector<String>> scanNetworks() {
  std::vector<std::vector<String>> nets;
  int n = WiFi.scanNetworks();
  for (int i = 0; i < n && nets.size() < 20; i++) {
    if (!WiFi.SSID(i).length()) continue;
    nets.push_back({WiFi.SSID(i), String(WiFi.RSSI(i)), WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "NO" : "YES"});
  }
  WiFi.scanDelete();
  return nets;
}

improv::Handlers improvHandlers = {
  provisionWifi,
  [] { return online(); },
  [] { return "http://" + WiFi.localIP().toString() + "/setup"; },
  [] { return std::vector<String>{FW_NAME, FW_VERSION, "ESP32-S3", deviceName}; },
  scanNetworks,
};
improv::Endpoint improvUart(Serial, improvHandlers);

// ---------------------------------------------------------------- setup page

static String htmlEscape(const String &s) {
  String o;
  for (char c : s) {
    if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '&') o += "&amp;";
    else if (c == '"') o += "&quot;";
    else if (c == '\'') o += "&#39;";
    else o += c;
  }
  return o;
}

static String pageShell(const String &body) {
  return "<!doctype html><html><head><meta charset=utf-8>"
         "<meta name=viewport content='width=device-width,initial-scale=1'><title>USB Terminal setup</title>"
         "<style>body{font-family:system-ui,sans-serif;max-width:640px;margin:2em auto;padding:0 16px;line-height:1.5}"
         "code,pre{background:#eee;border-radius:4px;padding:2px 4px}pre{padding:10px;white-space:pre-wrap;word-break:break-all}"
         "input,button{font-size:1em;padding:8px;margin:4px 0;width:100%;box-sizing:border-box}.muted{color:#666}"
         "@media(prefers-color-scheme:dark){body{background:#111;color:#ddd}code,pre{background:#222}.muted{color:#999}}"
         "</style></head><body><h1>USB Terminal</h1>" + body + "</body></html>";
}

static String agentSection() {
  String ip = WiFi.localIP().toString();
  if (!setupWindowOpen())
    return "<p>Press the <b>BOOT</b> button on the device, then reload this page: the access token is shown for 2 minutes.</p>";
  if (!online()) return "<p>Connect the device to Wi-Fi first; the access details appear here afterwards.</p>";
  return "<p>Claude Code:</p><pre>" + htmlEscape(claudeCommand()) + "</pre>"
         "<p>Other MCP clients: URL <code>http://" + ip + "/mcp</code>, header <code>Authorization: Bearer " +
         mcpToken + "</code></p><p class=muted>Also reachable as " + deviceName +
         ".local. Reserve this IP in your router so it doesn't change. The token is hidden again in a few minutes; "
         "press BOOT to show it again.</p>";
}

static void handleSetupPage() {
  String b = "<p class=muted>" + deviceName + " &middot; firmware " + FW_VERSION + "</p><h2>Wi-Fi</h2><p>";
  if (online()) b += "Connected to <b>" + htmlEscape(wifiSsid) + "</b> as <code>" + WiFi.localIP().toString() + "</code>.";
  else if (wifiSsid.length()) b += "Not connected (saved network: <b>" + htmlEscape(wifiSsid) + "</b>).";
  else b += "Not configured yet.";
  b += "</p>";
  if (!canChangeWifi()) {
    b += "<p class=muted>Press the BOOT button on the device, then reload, to change the Wi-Fi.</p>";
  } else {
    b += "<form method=post action=/setup/wifi><input name=ssid list=nets placeholder='Network name' required>"
         "<datalist id=nets>";
    for (const auto &net : scanNetworks()) b += "<option value=\"" + htmlEscape(net[0]) + "\">";
    b += "</datalist><input name=pass type=password placeholder=Password><button>Connect</button></form>"
         "<p class=muted>2.4 GHz networks only.</p>";
  }
  b += "<h2>Connect your AI agent</h2>" + agentSection();
  b += "<h2>Reset</h2><p class=muted>Hold BOOT for 5 seconds to erase Wi-Fi, token and settings.</p>";
  server.send(200, "text/html", pageShell(b));
}

static void handleSetupWifi() {
  if (!canChangeWifi()) {
    server.send(403, "text/html", pageShell("<p>Locked. Press the BOOT button on the device, then try again.</p>"));
    return;
  }
  String ssid = server.arg("ssid");
  if (!ssid.length()) {
    server.send(400, "text/html", pageShell("<p>Missing network name. <a href=/setup>Back</a></p>"));
    return;
  }
  if (!provisionWifi(ssid, server.arg("pass"))) {
    server.send(200, "text/html", pageShell("<p>Could not connect to <b>" + htmlEscape(ssid) +
                                            "</b>. Check the password and that it's a 2.4 GHz network.</p><p><a href=/setup>Back</a></p>"));
    return;
  }
  String ip = WiFi.localIP().toString();
  server.send(200, "text/html", pageShell("<p>Connected to <b>" + htmlEscape(ssid) + "</b>. The device is now at <code>" + ip +
                                          "</code>.</p><h2>Connect your AI agent</h2>" + agentSection() +
                                          "<p>Back on your normal network, this page is at <a href='http://" + ip + "/setup'>http://" + ip + "/setup</a>.</p>"));
}

// ---------------------------------------------------------------- main

void setup() {
  Serial.begin(115200);  // UART0 -> the board's USB-UART port
  pinMode(BOOT_BUTTON, INPUT_PULLUP);
  loadSettings();
  loadNetworkConfig();
  bool logOk = outputLog.begin(LOG_CAPACITY);

  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char name[24];
  snprintf(name, sizeof name, "usb-terminal-%02x%02x", mac[4], mac[5]);
  deviceName = name;

  USB.onEvent(usbEvent);
  USB.manufacturerName("usb-terminal-mcp");
  USB.productName("USB Terminal");
  Keyboard.begin();
  TermSerial.onEvent(cdcEvent);
  TermSerial.setRxBufferSize(8192);
  TermSerial.begin();
  USB.begin();

  WiFi.onEvent(wifiEvent);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(deviceName.c_str());
  WiFi.setSleep(false);  // lower latency for incoming requests
  WiFi.setAutoReconnect(true);
  ensureToken();
  if (wifiSsid.length()) WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  MDNS.begin(deviceName.c_str());
  MDNS.addService("http", "tcp", 80);

  const char *headers[] = {"Authorization"};
  server.collectHeaders(headers, 1);
  server.on("/mcp", HTTP_POST, handleMcpPost);
  server.on("/mcp", HTTP_GET, [] { server.send(405, "text/plain", "SSE stream not supported"); });
  server.on("/", HTTP_GET, [] {
    server.sendHeader("Location", "/setup");
    server.send(302);
  });
  server.on("/setup", HTTP_GET, handleSetupPage);
  server.on("/setup/wifi", HTTP_POST, handleSetupWifi);
  // On the setup network every unknown URL leads to the setup page, which
  // makes phones open it as a captive portal.
  server.onNotFound([] {
    if (!apActive) return server.send(404, "text/plain", "not found");
    server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/setup");
    server.send(302);
  });
  server.begin();

  Serial.printf("\n[boot] %s %s (%s), layout %s, input %s, terminal log %u KB%s\n", FW_NAME, FW_VERSION,
                deviceName.c_str(), layoutName(layout), inputName(inputMode), (unsigned)(outputLog.capacity() / 1024),
                logOk ? "" : " (ALLOC FAILED)");
  if (wifiSsid.length()) Serial.printf("[wifi] connecting to '%s'...\n", wifiSsid.c_str());
  Serial.println("[setup] press BOOT to show the access token, hold 5 s to factory reset");
}

void loop() {
  server.handleClient();
  pumpSerial();
  networkLoop();
  buttonLoop();

  // UART: Improv packets from the browser installer; 'i' prints status.
  while (Serial.available()) {
    uint8_t b = Serial.read();
    if (!improvUart.feed(b) && b == 'i') Serial.println(statusText());
  }
}
