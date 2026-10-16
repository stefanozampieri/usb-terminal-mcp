#pragma once
#include <Arduino.h>

// How commands reach the host:
// - keyboard: typed on the USB keyboard into the console session, whose
//   output is mirrored to our serial port with `script -qf /dev/ttyACMx`.
// - serial:   written straight to a login shell running on the serial port
//   (serial-getty@ttyACMx), no keyboard involved.
enum InputMode : uint8_t { INPUT_KEYBOARD = 0, INPUT_SERIAL = 1 };

// Everything the host writes to our USB serial port, kept in a ring buffer
// addressed by absolute byte offsets so readers can hold cursors across
// wraparound. Lives in PSRAM when available.
class OutputLog {
 public:
  bool begin(size_t capacity) {
    buf_ = (uint8_t *)ps_malloc(capacity);
    if (!buf_) {
      capacity = 32 * 1024;
      buf_ = (uint8_t *)malloc(capacity);
    }
    cap_ = buf_ ? capacity : 0;
    return buf_ != nullptr;
  }

  void append(const uint8_t *data, size_t n) {
    if (!cap_) return;
    for (size_t i = 0; i < n; i++) buf_[(end_ + i) % cap_] = data[i];
    end_ += n;
  }

  uint64_t end() const { return end_; }
  uint64_t start() const { return end_ > cap_ ? end_ - cap_ : 0; }
  size_t capacity() const { return cap_; }

  // Bytes in [from, to), clamped to what is still buffered.
  String slice(uint64_t from, uint64_t to) const {
    from = max(from, start());
    to = min(to, end_);
    String s;
    if (to <= from) return s;
    s.reserve(to - from);
    for (uint64_t i = from; i < to; i++) s += char(buf_[i % cap_]);
    return s;
  }

  // Offset of the first occurrence of needle at or after from, or -1.
  int64_t find(const char *needle, uint64_t from) const {
    size_t n = strlen(needle);
    from = max(from, start());
    if (!n || end_ < n) return -1;
    for (uint64_t i = from; i + n <= end_; i++) {
      size_t k = 0;
      while (k < n && buf_[(i + k) % cap_] == uint8_t(needle[k])) k++;
      if (k == n) return int64_t(i);
    }
    return -1;
  }

 private:
  uint8_t *buf_ = nullptr;
  size_t cap_ = 0;
  uint64_t end_ = 0;
};

// Turn raw terminal output into plain text: drop ANSI escape sequences and
// control characters, apply backspaces, and treat a bare \r as "rewrite this
// line" so progress bars collapse to their final state.
inline String cleanTerminal(const String &in) {
  String out;
  out.reserve(in.length());
  size_t lineStart = 0;
  size_t len = in.length();
  for (size_t i = 0; i < len; i++) {
    char c = in[i];
    if (c == 0x1B) {
      char next = i + 1 < len ? in[i + 1] : 0;
      if (next == '[') {  // CSI: ESC [ params final-byte
        i += 2;
        while (i < len && !(in[i] >= 0x40 && in[i] <= 0x7E)) i++;
      } else if (next == ']') {  // OSC: ESC ] ... BEL or ESC backslash
        i += 2;
        while (i < len && in[i] != 0x07 && !(in[i] == 0x1B && i + 1 < len && in[i + 1] == '\\')) i++;
        if (i < len && in[i] == 0x1B) i++;
      } else if (next == '(' || next == ')') {  // charset selection: ESC ( B
        i += 2;
      } else {
        i++;
      }
      continue;
    }
    if (c == '\r') {
      // A run of \r before \n is just a line ending (a tty's onlcr turns the
      // pty's \r\n into \r\r\n); only a \r followed by text rewrites the line.
      size_t j = i;
      while (j < len && in[j] == '\r') j++;
      if (j < len && in[j] == '\n') {
        i = j - 1;
        continue;
      }
      out.remove(lineStart);
      continue;
    }
    if (c == '\b') {
      if (out.length() > lineStart) out.remove(out.length() - 1);
      continue;
    }
    if (c == '\n') {
      out += '\n';
      lineStart = out.length();
      continue;
    }
    if (uint8_t(c) < 0x20 && c != '\t') continue;
    out += c;
  }
  return out;
}
