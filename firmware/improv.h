#pragma once
#include <Arduino.h>
#include <functional>
#include <vector>

// Improv Wi-Fi over serial (https://www.improv-wifi.com/serial/). ESP Web
// Tools uses it to send Wi-Fi credentials right after flashing in the browser.
//
// Packet: "IMPROV" | version (1) | type | length | data... | checksum
// checksum = sum of all preceding bytes, mod 256.
namespace improv {

enum State : uint8_t { STATE_AUTHORIZED = 0x02, STATE_PROVISIONING = 0x03, STATE_PROVISIONED = 0x04 };
enum Error : uint8_t { ERROR_NONE = 0x00, ERROR_INVALID_RPC = 0x01, ERROR_UNKNOWN_RPC = 0x02, ERROR_UNABLE_TO_CONNECT = 0x03 };
enum Type : uint8_t { TYPE_CURRENT_STATE = 0x01, TYPE_ERROR_STATE = 0x02, TYPE_RPC = 0x03, TYPE_RPC_RESULT = 0x04 };
enum Command : uint8_t { CMD_WIFI_SETTINGS = 0x01, CMD_GET_STATE = 0x02, CMD_GET_INFO = 0x03, CMD_GET_NETWORKS = 0x04 };

struct Handlers {
  std::function<bool(const String &ssid, const String &pass)> connect;  // blocking; true once connected
  std::function<bool()> provisioned;
  std::function<String()> url;                                         // "next" link after provisioning
  std::function<std::vector<String>()> info;                           // firmware, version, chip, device name
  std::function<std::vector<std::vector<String>>()> networks;          // {ssid, rssi, "YES"/"NO" secured}
};

class Endpoint {
 public:
  Endpoint(Print &out, Handlers &h) : out_(out), h_(h) {}

  // Feed every byte read from the serial port. Returns true while a packet
  // is being parsed, so callers can ignore those bytes for anything else.
  bool feed(uint8_t b) {
    static const char HEADER[] = "IMPROV";
    if (pos_ < 6) {
      if (b == uint8_t(HEADER[pos_])) {
        buf_[pos_++] = b;
      } else {
        pos_ = b == 'I' ? 1 : 0;
        if (pos_) buf_[0] = b;
      }
      return pos_ > 0;
    }
    if (pos_ >= sizeof buf_) {
      pos_ = 0;
      return false;
    }
    buf_[pos_++] = b;
    if (pos_ < 9) return true;                // version, type, length
    size_t total = 9 + buf_[8] + 1;           // header + data + checksum
    if (pos_ < total) return true;
    uint8_t sum = 0;
    for (size_t i = 0; i < total - 1; i++) sum += buf_[i];
    if (sum == buf_[total - 1] && buf_[6] == 1) handle(buf_[7], buf_ + 9, buf_[8]);
    else if (sum != buf_[total - 1]) sendError(ERROR_INVALID_RPC);
    pos_ = 0;
    return true;
  }

 private:
  Print &out_;
  Handlers &h_;
  uint8_t buf_[9 + 255 + 1];
  size_t pos_ = 0;

  void send(Type type, const std::vector<uint8_t> &data) {
    std::vector<uint8_t> p = {'I', 'M', 'P', 'R', 'O', 'V', 1, uint8_t(type), uint8_t(data.size())};
    p.insert(p.end(), data.begin(), data.end());
    uint8_t sum = 0;
    for (uint8_t b : p) sum += b;
    p.push_back(sum);
    out_.write(p.data(), p.size());
    out_.write('\n');  // keeps the packet off the next log line
  }

  void sendState(State s) { send(TYPE_CURRENT_STATE, {uint8_t(s)}); }
  void sendError(Error e) { send(TYPE_ERROR_STATE, {uint8_t(e)}); }

  void sendResult(Command cmd, const std::vector<String> &strings) {
    std::vector<uint8_t> d = {uint8_t(cmd), 0};
    for (const String &s : strings) {
      size_t n = min<size_t>(s.length(), 64);
      d.push_back(uint8_t(n));
      for (size_t i = 0; i < n; i++) d.push_back(uint8_t(s[i]));
    }
    d[1] = uint8_t(d.size() - 2);
    send(TYPE_RPC_RESULT, d);
  }

  void handle(uint8_t type, const uint8_t *data, size_t len) {
    if (type != TYPE_RPC || len < 2 || size_t(data[1]) + 2 > len) return sendError(ERROR_INVALID_RPC);
    uint8_t cmd = data[0];
    const uint8_t *p = data + 2;
    size_t n = data[1];

    switch (cmd) {
      case CMD_WIFI_SETTINGS: {
        if (n < 1 || size_t(p[0]) + 2 > n || size_t(p[0]) + 2 + p[p[0] + 1] > n) return sendError(ERROR_INVALID_RPC);
        String ssid((const char *)p + 1, p[0]);
        String pass((const char *)p + p[0] + 2, p[p[0] + 1]);
        sendError(ERROR_NONE);
        sendState(STATE_PROVISIONING);
        if (h_.connect(ssid, pass)) {
          sendState(STATE_PROVISIONED);
          sendResult(CMD_WIFI_SETTINGS, {h_.url()});
        } else {
          sendError(ERROR_UNABLE_TO_CONNECT);
          sendState(STATE_AUTHORIZED);
        }
        return;
      }
      case CMD_GET_STATE:
        if (h_.provisioned()) {
          sendState(STATE_PROVISIONED);
          sendResult(CMD_GET_STATE, {h_.url()});
        } else {
          sendState(STATE_AUTHORIZED);
        }
        return;
      case CMD_GET_INFO:
        sendResult(CMD_GET_INFO, h_.info());
        return;
      case CMD_GET_NETWORKS:
        for (const auto &net : h_.networks()) sendResult(CMD_GET_NETWORKS, net);
        sendResult(CMD_GET_NETWORKS, {});
        return;
      default:
        sendError(ERROR_UNKNOWN_RPC);
    }
  }
};

}  // namespace improv
