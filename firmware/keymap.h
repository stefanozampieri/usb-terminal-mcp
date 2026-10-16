#pragma once
#include <Arduino.h>

// A USB keyboard sends key *positions*, not characters. The host's keyboard
// layout decides which character each position produces, so we need one table
// per layout to know which position (+ modifiers) yields the character we want.

// LAYOUT_US_INTL is US with dead keys (' " ` ~ ^ combine with the next
// letter, e.g. ' + i = í). Same positions as US; typeText follows each dead
// key with a space so it always produces the plain character.
enum Layout : uint8_t { LAYOUT_US = 0, LAYOUT_IT = 1, LAYOUT_US_INTL = 2 };

// HID modifier bits
constexpr uint8_t MOD_LCTRL = 0x01, MOD_LSHIFT = 0x02, MOD_LALT = 0x04, MOD_LGUI = 0x08,
                  MOD_RCTRL = 0x10, MOD_RSHIFT = 0x20, MOD_RALT = 0x40, MOD_RGUI = 0x80;

struct Stroke {
  uint8_t mod;
  uint8_t key;
};

struct NamedKey {
  const char *name;
  uint8_t key;  // HID usage ID, or modifier bits in the modifier table
};

struct TypeResult {
  size_t typed = 0;
  size_t skipped = 0;
  String skippedList;  // first few unsupported characters as U+XXXX
};

struct CharKey {
  uint32_t cp;  // Unicode code point
  uint8_t key;  // HID usage ID
  uint8_t mod;
};

namespace keymap_detail {
constexpr uint8_t S = MOD_LSHIFT;
constexpr uint8_t G = MOD_RALT;  // AltGr

// US ANSI layout
const CharKey US[] = {
  {'\n', 0x28, 0}, {'\t', 0x2B, 0}, {' ', 0x2C, 0},
  {'-', 0x2D, 0}, {'=', 0x2E, 0}, {'[', 0x2F, 0}, {']', 0x30, 0}, {'\\', 0x31, 0},
  {';', 0x33, 0}, {'\'', 0x34, 0}, {'`', 0x35, 0}, {',', 0x36, 0}, {'.', 0x37, 0}, {'/', 0x38, 0},
  {'!', 0x1E, S}, {'@', 0x1F, S}, {'#', 0x20, S}, {'$', 0x21, S}, {'%', 0x22, S},
  {'^', 0x23, S}, {'&', 0x24, S}, {'*', 0x25, S}, {'(', 0x26, S}, {')', 0x27, S},
  {'_', 0x2D, S}, {'+', 0x2E, S}, {'{', 0x2F, S}, {'}', 0x30, S}, {'|', 0x31, S},
  {':', 0x33, S}, {'"', 0x34, S}, {'~', 0x35, S}, {'<', 0x36, S}, {'>', 0x37, S}, {'?', 0x38, S},
};

// Italian ISO layout. ` and ~ use the Linux AltGr positions (Windows' Italian
// layout has no key for them).
const CharKey IT[] = {
  {'\n', 0x28, 0}, {'\t', 0x2B, 0}, {' ', 0x2C, 0},
  {'!', 0x1E, S}, {'"', 0x1F, S}, {0xA3, 0x20, S} /* £ */, {'$', 0x21, S}, {'%', 0x22, S},
  {'&', 0x23, S}, {'/', 0x24, S}, {'(', 0x25, S}, {')', 0x26, S}, {'=', 0x27, S},
  {'\'', 0x2D, 0}, {'?', 0x2D, S}, {'`', 0x2D, G},
  {0xEC, 0x2E, 0} /* ì */, {'^', 0x2E, S}, {'~', 0x2E, G},
  {0xE8, 0x2F, 0} /* è */, {0xE9, 0x2F, S} /* é */, {'[', 0x2F, G}, {'{', 0x2F, G | S},
  {'+', 0x30, 0}, {'*', 0x30, S}, {']', 0x30, G}, {'}', 0x30, G | S},
  {0xF2, 0x33, 0} /* ò */, {0xE7, 0x33, S} /* ç */, {'@', 0x33, G},
  {0xE0, 0x34, 0} /* à */, {0xB0, 0x34, S} /* ° */, {'#', 0x34, G},
  {0xF9, 0x31, 0} /* ù */, {0xA7, 0x31, S} /* § */,
  {'\\', 0x35, 0}, {'|', 0x35, S},
  {',', 0x36, 0}, {';', 0x36, S}, {'.', 0x37, 0}, {':', 0x37, S}, {'-', 0x38, 0}, {'_', 0x38, S},
  {'<', 0x64, 0}, {'>', 0x64, S},
  {0x20AC, 0x08, G} /* € */,
};

template <size_t N>
inline bool lookup(const CharKey (&table)[N], uint32_t cp, Stroke &out) {
  for (const CharKey &e : table) {
    if (e.cp == cp) {
      out = {e.mod, e.key};
      return true;
    }
  }
  return false;
}
}  // namespace keymap_detail

// Map one Unicode code point to a keystroke for the given layout.
// Returns false if the character can't be typed on that layout.
inline bool mapChar(uint32_t cp, Layout layout, Stroke &out) {
  using namespace keymap_detail;
  // Letters and digits sit in the same places on both layouts.
  if (cp >= 'a' && cp <= 'z') { out = {0, uint8_t(0x04 + cp - 'a')}; return true; }
  if (cp >= 'A' && cp <= 'Z') { out = {S, uint8_t(0x04 + cp - 'A')}; return true; }
  if (cp >= '1' && cp <= '9') { out = {0, uint8_t(0x1E + cp - '1')}; return true; }
  if (cp == '0') { out = {0, 0x27}; return true; }
  return layout == LAYOUT_IT ? lookup(IT, cp, out) : lookup(US, cp, out);
}

inline bool isDeadKey(uint32_t cp, Layout layout) {
  return layout == LAYOUT_US_INTL && (cp == '\'' || cp == '"' || cp == '`' || cp == '~' || cp == '^');
}

// Decode the next UTF-8 code point and advance p.
inline uint32_t nextCodepoint(const char *&p) {
  uint8_t c = uint8_t(*p++);
  if (c < 0x80) return c;
  int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
  uint32_t cp = c & (0x3F >> extra);
  while (extra-- > 0 && (uint8_t(*p) & 0xC0) == 0x80) cp = (cp << 6) | (uint8_t(*p++) & 0x3F);
  return cp;
}
