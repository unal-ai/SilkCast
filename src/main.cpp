#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#include "capture_v4l2.hpp"
#include "encoder_h264.hpp"
#include "httplib.h"
#include "mp4_frag.hpp"
#include "types.hpp"
#include "yuv_convert.hpp"
#ifdef __APPLE__
std::vector<std::string> list_avfoundation_devices();
#endif
#ifdef __linux__
#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#endif

using namespace std::chrono_literals;

class SessionManager {
public:
  explicit SessionManager(int idle_timeout_seconds)
      : idle_timeout_seconds_(idle_timeout_seconds),
        reaper_thread_([this] { reap_loop(); }) {}
  ~SessionManager() {
    stop_reaper_ = true;
    if (reaper_thread_.joinable())
      reaper_thread_.join();
  }

  std::shared_ptr<Session> get_or_create(const std::string &device_id,
                                         const CaptureParams &params) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = sessions_.find(device_id);
    if (it != sessions_.end()) {
      return it->second;
    }
    auto session = std::make_shared<Session>();
    session->device_id = device_id;
    session->params = params;
    session->capture = std::make_shared<CaptureV4L2>();
    sessions_[device_id] = session;
    return session;
  }

  void touch(const std::string &device_id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = sessions_.find(device_id);
    if (it != sessions_.end()) {
      it->second->last_accessed = std::chrono::steady_clock::now();
    }
  }

  void release_if_idle(const std::string &device_id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = sessions_.find(device_id);
    if (it != sessions_.end()) {
      if (it->second->client_count.load() == 0) {
        if (it->second->capture)
          it->second->capture->stop();
        sessions_.erase(it);
      }
    }
  }

  std::vector<std::string> list_devices() const {
    std::vector<std::string> devices;
#ifdef __APPLE__
    devices = list_avfoundation_devices();
#else
    for (const auto &entry : std::filesystem::directory_iterator("/dev")) {
      const auto name = entry.path().filename().string();
      if (name.rfind("video", 0) == 0) {
#ifdef __linux__
        int fd = open(entry.path().c_str(), O_RDWR | O_NONBLOCK, 0);
        if (fd >= 0) {
          v4l2_capability cap;
          if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
            __u32 caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS)
                             ? cap.device_caps
                             : cap.capabilities;
            if (caps & V4L2_CAP_VIDEO_CAPTURE) {
              devices.push_back(name);
            }
          }
          close(fd);
        }
#else
        devices.push_back(name);
#endif
      }
    }
#endif
    if (devices.empty()) {
      devices.push_back("video0"); // fallback hint
    }
    std::sort(devices.begin(), devices.end());
    return devices;
  }

  std::optional<std::shared_ptr<Session>> find(const std::string &device_id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = sessions_.find(device_id);
    if (it == sessions_.end())
      return std::nullopt;
    return it->second;
  }

private:
  void reap_loop() {
    while (!stop_reaper_) {
      {
        std::lock_guard<std::mutex> lock(mu_);
        const auto now = std::chrono::steady_clock::now();
        for (auto it = sessions_.begin(); it != sessions_.end();) {
          auto &sess = it->second;
          auto idle_for = std::chrono::duration_cast<std::chrono::seconds>(
                              now - sess->last_accessed)
                              .count();
          if (sess->client_count.load() == 0 &&
              idle_for > idle_timeout_seconds_) {
            if (sess->capture)
              sess->capture->stop();
            it = sessions_.erase(it);
          } else {
            ++it;
          }
        }
      }
      std::this_thread::sleep_for(10s);
    }
  }

  mutable std::mutex mu_;
  std::unordered_map<std::string, std::shared_ptr<Session>> sessions_;
  std::thread reaper_thread_;
  std::atomic<bool> stop_reaper_{false};
  const int idle_timeout_seconds_;
};

// Minimal 1x1 white JPEG (valid) for placeholder MJPEG stream.
static const unsigned char kTinyJpeg[] = {
    0xFF, 0xD8, 0xFF, 0xDB, 0x00, 0x43, 0x00, 0x03, 0x02, 0x02, 0x03, 0x02,
    0x02, 0x03, 0x03, 0x03, 0x03, 0x04, 0x03, 0x03, 0x04, 0x05, 0x08, 0x05,
    0x05, 0x04, 0x04, 0x05, 0x0A, 0x07, 0x07, 0x06, 0x08, 0x0C, 0x0A, 0x0C,
    0x0C, 0x0B, 0x0A, 0x0B, 0x0B, 0x0D, 0x0E, 0x12, 0x10, 0x0D, 0x0E, 0x11,
    0x0E, 0x0B, 0x0B, 0x10, 0x16, 0x10, 0x11, 0x13, 0x14, 0x15, 0x15, 0x15,
    0x0C, 0x0F, 0x17, 0x18, 0x16, 0x14, 0x18, 0x12, 0x14, 0x15, 0x14, 0xFF,
    0xC0, 0x00, 0x11, 0x08, 0x00, 0x01, 0x00, 0x01, 0x03, 0x01, 0x11, 0x00,
    0x02, 0x11, 0x01, 0x03, 0x11, 0x01, 0xFF, 0xC4, 0x00, 0x14, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xFF, 0xC4, 0x00, 0x14, 0x10, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xFF, 0xDA, 0x00, 0x0C, 0x03, 0x01, 0x00, 0x02, 0x11, 0x03,
    0x11, 0x00, 0x3F, 0x00, 0xFF, 0xD9};

static const char kIndexHtml[] = R"HTML(
<!doctype html>
<html lang="en">
  <head>
    <meta charset="utf-8"/>
    <meta name="viewport" content="width=device-width,initial-scale=1"/>
    <title>SilkCast Link Builder</title>
    <style>
      :root {
        --bg: #0c0f14;
        --card: #141a22;
        --muted: #8ca1b3;
        --text: #e9f0f6;
        --accent: #4dd0a6;
        --accent-2: #6aa6ff;
        --border: #233041;
      }
      * { box-sizing: border-box; }
      body {
        margin: 0;
        font-family: ui-sans-serif, system-ui, -apple-system, Segoe UI, sans-serif;
        background: radial-gradient(1200px 600px at 10% -10%, #1b2430 0%, #0c0f14 45%);
        color: var(--text);
      }
      .wrap { max-width: 1080px; margin: 32px auto; padding: 0 20px 40px; }
      header { display: flex; align-items: center; justify-content: space-between; gap: 16px; }
      h1 { margin: 0; font-size: 28px; letter-spacing: 0.4px; }
      .pill { padding: 6px 10px; border: 1px solid var(--border); border-radius: 999px; color: var(--muted); font-size: 12px; }
      .grid { display: grid; grid-template-columns: 1.2fr 1fr; gap: 18px; margin-top: 18px; }
      .card { background: var(--card); border: 1px solid var(--border); border-radius: 14px; padding: 16px; }
      .card h2 { margin: 0 0 12px; font-size: 16px; }
      .tabs { display: flex; flex-wrap: wrap; gap: 8px; }
      .tabs button {
        border: 1px solid var(--border);
        background: transparent;
        color: var(--text);
        padding: 8px 12px;
        border-radius: 10px;
        cursor: pointer;
        font-size: 13px;
      }
      .tabs button.active { border-color: var(--accent); color: var(--accent); }
      .row { display: flex; gap: 8px; align-items: center; margin-bottom: 10px; }
      label { font-size: 13px; color: var(--muted); }
      input, select, button.action {
        background: #0f141b;
        color: var(--text);
        border: 1px solid var(--border);
        border-radius: 10px;
        padding: 8px 10px;
        font-size: 13px;
        width: 100%;
      }
      input[type="checkbox"] { width: auto; }
      .param {
        display: grid;
        grid-template-columns: 24px 1fr 1fr;
        gap: 8px;
        align-items: center;
        padding: 6px 0;
      }
      .param .name { font-size: 12px; color: var(--muted); }
      .param input[type="number"], .param input[type="text"], .param select { width: 100%; }
      .muted { color: var(--muted); font-size: 12px; }
      .output { display: flex; gap: 8px; align-items: center; margin-top: 8px; }
      .output input { flex: 1; }
      .action { cursor: pointer; }
      .action.primary { border-color: var(--accent); color: var(--accent); }
      .action.secondary { border-color: var(--accent-2); color: var(--accent-2); }
      .qr { display: flex; flex-direction: column; align-items: center; gap: 8px; }
      canvas { background: #fff; padding: 10px; border-radius: 12px; }
      .chip { display: inline-flex; align-items: center; gap: 6px; padding: 6px 10px; border: 1px solid var(--border); border-radius: 10px; margin-top: 6px; }
      @media (max-width: 900px) {
        .grid { grid-template-columns: 1fr; }
      }
    </style>
  </head>
  <body>
    <div class="wrap">
      <header>
        <div>
          <h1>SilkCast Link Builder</h1>
          <div class="muted">Assemble stream links and share them instantly.</div>
        </div>
        <div class="pill">Just GET</div>
      </header>

      <div class="grid">
        <div class="card">
          <h2>Endpoint</h2>
          <div class="tabs" id="endpointTabs">
            <button data-endpoint="live" class="active">/stream/live</button>
            <button data-endpoint="udp">/stream/udp</button>
            <button data-endpoint="stats">/stream/{id}/stats</button>
            <button data-endpoint="list">/device/list</button>
          </div>

          <div style="margin-top: 14px;">
            <div class="row">
              <label style="min-width: 70px;">Device</label>
              <select id="deviceSelect"></select>
              <button class="action secondary" id="refreshDevices">Refresh</button>
            </div>
            <div class="row">
              <label style="min-width: 70px;">Device ID</label>
              <input id="deviceInput" value="video0" placeholder="video0 or camera name"/>
            </div>
            <div class="muted" id="deviceHint">Pick a device to update the link.</div>
          </div>

          <div id="captureParams" style="margin-top: 16px;">
            <h2>Capture Params</h2>
            <div class="param">
              <input type="checkbox" id="use_w" checked/>
              <div class="name">w</div>
              <input type="number" id="w" value="1280" min="1"/>
            </div>
            <div class="param">
              <input type="checkbox" id="use_h" checked/>
              <div class="name">h</div>
              <input type="number" id="h" value="720" min="1"/>
            </div>
            <div class="param">
              <input type="checkbox" id="use_fps" checked/>
              <div class="name">fps</div>
              <input type="number" id="fps" value="30" min="1"/>
            </div>
            <div class="param">
              <input type="checkbox" id="use_bitrate" checked/>
              <div class="name">bitrate</div>
              <input type="number" id="bitrate" value="256" min="1"/>
            </div>
            <div class="param">
              <input type="checkbox" id="use_gop" checked/>
              <div class="name">gop</div>
              <input type="number" id="gop" value="30" min="1"/>
            </div>
            <div class="param">
              <input type="checkbox" id="use_codec" checked/>
              <div class="name">codec</div>
              <select id="codec">
                <option value="mjpeg">mjpeg</option>
                <option value="h264">h264</option>
              </select>
            </div>
            <div class="param">
              <input type="checkbox" id="use_latency" checked/>
              <div class="name">latency</div>
              <select id="latency">
                <option value="view">view</option>
                <option value="low">low</option>
                <option value="ultra">ultra</option>
              </select>
            </div>
            <div class="param">
              <input type="checkbox" id="use_container" checked/>
              <div class="name">container</div>
              <select id="container">
                <option value="raw">raw</option>
                <option value="mp4">mp4</option>
              </select>
            </div>
          </div>

          <div id="udpParams" style="margin-top: 16px;">
            <h2>UDP Params</h2>
            <div class="param">
              <div></div>
              <div class="name">target</div>
              <input type="text" id="target" value="127.0.0.1"/>
            </div>
            <div class="param">
              <div></div>
              <div class="name">port</div>
              <input type="number" id="port" value="5000" min="1"/>
            </div>
            <div class="param">
              <input type="checkbox" id="use_duration" checked/>
              <div class="name">duration</div>
              <input type="number" id="duration" value="10" min="1"/>
            </div>
          </div>
        </div>

        <div class="card">
          <h2>Shareable Link</h2>
          <div class="output">
            <input id="outputUrl" readonly/>
            <button class="action primary" id="copyBtn">Copy</button>
            <button class="action secondary" id="openBtn">Open</button>
          </div>
          <div class="chip muted" id="methodHint">GET</div>

          <div style="margin-top: 18px;">
            <h2>QR Code</h2>
            <div class="qr">
              <canvas id="qr"></canvas>
              <div class="muted" id="qrHint">QR updates when the link changes.</div>
            </div>
          </div>
        </div>
      </div>
    </div>

    <script>
      /* Minimal QR generator (MIT) adapted from qrcode-generator by Kazuhiko Arase */
      var qrcode = function() {
        function QR8bitByte(data) {
          this.mode = 1;
          this.data = data;
          this.parsed = [];
          for (var i = 0; i < this.data.length; i++) {
            this.parsed.push(this.data.charCodeAt(i));
          }
          this.getLength = function() { return this.parsed.length; };
          this.write = function(buffer) {
            for (var i = 0; i < this.parsed.length; i++) {
              buffer.put(this.parsed[i], 8);
            }
          };
        }
        var QRUtil = {
          PATTERN_POSITION_TABLE: [
            [], [6, 18], [6, 22], [6, 26], [6, 30], [6, 34], [6, 22, 38],
            [6, 24, 42], [6, 26, 46], [6, 28, 50], [6, 30, 54]
          ],
          G15: (1 << 10) | (1 << 8) | (1 << 5) | (1 << 4) | (1 << 2) | (1 << 1) | (1 << 0),
          G18: (1 << 12) | (1 << 11) | (1 << 10) | (1 << 9) | (1 << 8) | (1 << 5) | (1 << 2) | (1 << 0),
          G15_MASK: (1 << 14) | (1 << 12) | (1 << 10) | (1 << 4) | (1 << 1),
          getBCHTypeInfo: function(data) {
            var d = data << 10;
            while (QRUtil.getBCHDigit(d) - QRUtil.getBCHDigit(QRUtil.G15) >= 0) {
              d ^= (QRUtil.G15 << (QRUtil.getBCHDigit(d) - QRUtil.getBCHDigit(QRUtil.G15)));
            }
            return ((data << 10) | d) ^ QRUtil.G15_MASK;
          },
          getBCHDigit: function(data) {
            var digit = 0;
            while (data != 0) {
              digit++;
              data >>>= 1;
            }
            return digit;
          },
          getPatternPosition: function(typeNumber) {
            return QRUtil.PATTERN_POSITION_TABLE[typeNumber - 1];
          },
          getMask: function(maskPattern, i, j) {
            switch (maskPattern) {
              case 0: return (i + j) % 2 == 0;
              case 1: return i % 2 == 0;
              case 2: return j % 3 == 0;
              case 3: return (i + j) % 3 == 0;
              case 4: return (Math.floor(i / 2) + Math.floor(j / 3)) % 2 == 0;
              case 5: return ((i * j) % 2) + ((i * j) % 3) == 0;
              case 6: return ((((i * j) % 2) + ((i * j) % 3)) % 2) == 0;
              case 7: return ((((i + j) % 2) + ((i * j) % 3)) % 2) == 0;
              default: return false;
            }
          },
          getErrorCorrectPolynomial: function(errorCorrectLength) {
            var a = [1];
            for (var i = 0; i < errorCorrectLength; i++) {
              a = QRUtil.polynomialMultiply(a, [1, QRUtil.gexp(i)]);
            }
            return a;
          },
          polynomialMultiply: function(a, b) {
            var num = new Array(a.length + b.length - 1);
            for (var i = 0; i < num.length; i++) num[i] = 0;
            for (var i = 0; i < a.length; i++) {
              for (var j = 0; j < b.length; j++) {
                num[i + j] ^= QRUtil.gexp(QRUtil.glog(a[i]) + QRUtil.glog(b[j]));
              }
            }
            return num;
          },
          glog: function(n) {
            if (n < 1) throw new Error("glog");
            return QRUtil.LOG_TABLE[n];
          },
          gexp: function(n) {
            while (n < 0) n += 255;
            while (n >= 256) n -= 255;
            return QRUtil.EXP_TABLE[n];
          }
        };
        QRUtil.EXP_TABLE = new Array(256);
        QRUtil.LOG_TABLE = new Array(256);
        for (var i = 0; i < 8; i++) QRUtil.EXP_TABLE[i] = 1 << i;
        for (var i = 8; i < 256; i++) QRUtil.EXP_TABLE[i] =
          QRUtil.EXP_TABLE[i - 4] ^ QRUtil.EXP_TABLE[i - 5] ^
          QRUtil.EXP_TABLE[i - 6] ^ QRUtil.EXP_TABLE[i - 8];
        for (var i = 0; i < 255; i++) QRUtil.LOG_TABLE[QRUtil.EXP_TABLE[i]] = i;

        function QRBitBuffer() {
          this.buffer = [];
          this.length = 0;
          this.put = function(num, length) {
            for (var i = 0; i < length; i++) {
              this.putBit(((num >>> (length - i - 1)) & 1) == 1);
            }
          };
          this.get = function(index) {
            var bufIndex = Math.floor(index / 8);
            return ((this.buffer[bufIndex] >>> (7 - index % 8)) & 1) == 1;
          };
          this.putBit = function(bit) {
            var bufIndex = Math.floor(this.length / 8);
            if (this.buffer.length <= bufIndex) {
              this.buffer.push(0);
            }
            if (bit) {
              this.buffer[bufIndex] |= (0x80 >>> (this.length % 8));
            }
            this.length++;
          };
        }

        function QRPolynomial(num, shift) {
          var offset = 0;
          while (offset < num.length && num[offset] == 0) offset++;
          this.num = new Array(num.length - offset + shift);
          for (var i = 0; i < num.length - offset; i++) {
            this.num[i] = num[i + offset];
          }
        }
        QRPolynomial.prototype.get = function(index) { return this.num[index]; };
        QRPolynomial.prototype.getLength = function() { return this.num.length; };
        QRPolynomial.prototype.multiply = function(e) {
          var num = new Array(this.getLength() + e.getLength() - 1);
          for (var i = 0; i < num.length; i++) num[i] = 0;
          for (var i = 0; i < this.getLength(); i++) {
            for (var j = 0; j < e.getLength(); j++) {
              num[i + j] ^= QRUtil.gexp(QRUtil.glog(this.get(i)) + QRUtil.glog(e.get(j)));
            }
          }
          return new QRPolynomial(num, 0);
        };
        QRPolynomial.prototype.mod = function(e) {
          if (this.getLength() - e.getLength() < 0) return this;
          var ratio = QRUtil.glog(this.get(0)) - QRUtil.glog(e.get(0));
          var num = this.num.slice();
          for (var i = 0; i < e.getLength(); i++) {
            num[i] ^= QRUtil.gexp(QRUtil.glog(e.get(i)) + ratio);
          }
          return new QRPolynomial(num, 0).mod(e);
        };

        function QRRSBlock(totalCount, dataCount) {
          this.totalCount = totalCount;
          this.dataCount = dataCount;
        }
        QRRSBlock.getRSBlocks = function(typeNumber, errorCorrectLevel) {
          var RS_BLOCK_TABLE = [
            // type 1
            [1, 26, 19], [1, 26, 16], [1, 26, 13], [1, 26, 9],
            // type 2
            [1, 44, 34], [1, 44, 28], [1, 44, 22], [1, 44, 16],
            // type 3
            [1, 70, 55], [1, 70, 44], [2, 35, 17], [2, 35, 13],
            // type 4
            [1, 100, 80], [2, 50, 32], [2, 50, 24], [4, 25, 9],
            // type 5
            [1, 134, 108], [2, 67, 43], [2, 33, 15], [2, 33, 11],
            // type 6
            [2, 86, 68], [4, 43, 27], [4, 43, 19], [4, 43, 15],
            // type 7
            [2, 98, 78], [4, 49, 31], [2, 32, 14], [4, 39, 13],
            // type 8
            [2, 121, 97], [2, 60, 38], [4, 40, 18], [4, 40, 14],
            // type 9
            [2, 146, 116], [3, 58, 36], [4, 36, 16], [4, 36, 12],
            // type 10
            [2, 86, 68], [4, 69, 43], [6, 43, 19], [6, 43, 15]
          ];
          var offset = (typeNumber - 1) * 4 + errorCorrectLevel;
          var rs = RS_BLOCK_TABLE[offset];
          var list = [];
          for (var i = 0; i < rs[0]; i++) {
            list.push(new QRRSBlock(rs[1], rs[2]));
          }
          return list;
        };

        function QRCode(typeNumber, errorCorrectLevel) {
          this.typeNumber = typeNumber;
          this.errorCorrectLevel = errorCorrectLevel;
          this.modules = null;
          this.moduleCount = 0;
          this.dataCache = null;
          this.dataList = [];

          this.addData = function(data) {
            this.dataList.push(new QR8bitByte(data));
            this.dataCache = null;
          };
          this.isDark = function(row, col) {
            if (row < 0 || this.moduleCount <= row || col < 0 || this.moduleCount <= col)
              return false;
            return this.modules[row][col];
          };
          this.getModuleCount = function() { return this.moduleCount; };
          this.make = function() {
            if (this.typeNumber < 1) {
              this.typeNumber = 1;
              for (var type = 1; type < 11; type++) {
                var rsBlocks = QRRSBlock.getRSBlocks(type, this.errorCorrectLevel);
                var buffer = new QRBitBuffer();
                for (var i = 0; i < this.dataList.length; i++) {
                  var data = this.dataList[i];
                  buffer.put(data.mode, 4);
                  buffer.put(data.getLength(), 8);
                  data.write(buffer);
                }
                var totalDataCount = 0;
                for (var i = 0; i < rsBlocks.length; i++) {
                  totalDataCount += rsBlocks[i].dataCount;
                }
                if (buffer.length <= totalDataCount * 8) {
                  this.typeNumber = type;
                  break;
                }
              }
            }
            this.makeImpl(false, this.getBestMaskPattern());
          };
          this.makeImpl = function(test, maskPattern) {
            this.moduleCount = this.typeNumber * 4 + 17;
            this.modules = new Array(this.moduleCount);
            for (var row = 0; row < this.moduleCount; row++) {
              this.modules[row] = new Array(this.moduleCount);
              for (var col = 0; col < this.moduleCount; col++) {
                this.modules[row][col] = null;
              }
            }
            this.setupPositionProbePattern(0, 0);
            this.setupPositionProbePattern(this.moduleCount - 7, 0);
            this.setupPositionProbePattern(0, this.moduleCount - 7);
            this.setupPositionAdjustPattern();
            this.setupTimingPattern();
            this.setupTypeInfo(test, maskPattern);
            if (this.typeNumber >= 7) this.setupTypeNumber(test);
            if (this.dataCache == null) {
              this.dataCache = QRCode.createData(this.typeNumber, this.errorCorrectLevel, this.dataList);
            }
            this.mapData(this.dataCache, maskPattern);
          };
          this.setupPositionProbePattern = function(row, col) {
            for (var r = -1; r <= 7; r++) {
              if (row + r <= -1 || this.moduleCount <= row + r) continue;
              for (var c = -1; c <= 7; c++) {
                if (col + c <= -1 || this.moduleCount <= col + c) continue;
                if ((0 <= r && r <= 6 && (c == 0 || c == 6)) ||
                    (0 <= c && c <= 6 && (r == 0 || r == 6)) ||
                    (2 <= r && r <= 4 && 2 <= c && c <= 4)) {
                  this.modules[row + r][col + c] = true;
                } else {
                  this.modules[row + r][col + c] = false;
                }
              }
            }
          };
          this.getBestMaskPattern = function() {
            var minLostPoint = 0;
            var pattern = 0;
            for (var i = 0; i < 8; i++) {
              this.makeImpl(true, i);
              var lostPoint = QRCode.getLostPoint(this);
              if (i == 0 || minLostPoint > lostPoint) {
                minLostPoint = lostPoint;
                pattern = i;
              }
            }
            return pattern;
          };
          this.setupTimingPattern = function() {
            for (var i = 8; i < this.moduleCount - 8; i++) {
              if (this.modules[i][6] != null) continue;
              this.modules[i][6] = (i % 2 == 0);
              if (this.modules[6][i] != null) continue;
              this.modules[6][i] = (i % 2 == 0);
            }
          };
          this.setupPositionAdjustPattern = function() {
            var pos = QRUtil.getPatternPosition(this.typeNumber);
            for (var i = 0; i < pos.length; i++) {
              for (var j = 0; j < pos.length; j++) {
                var row = pos[i];
                var col = pos[j];
                if (this.modules[row][col] != null) continue;
                for (var r = -2; r <= 2; r++) {
                  for (var c = -2; c <= 2; c++) {
                    if (r == -2 || r == 2 || c == -2 || c == 2 || (r == 0 && c == 0)) {
                      this.modules[row + r][col + c] = true;
                    } else {
                      this.modules[row + r][col + c] = false;
                    }
                  }
                }
              }
            }
          };
          this.setupTypeNumber = function(test) {
            var bits = QRUtil.getBCHTypeInfo(this.typeNumber);
            for (var i = 0; i < 18; i++) {
              var mod = (!test && ((bits >> i) & 1) == 1);
              this.modules[Math.floor(i / 3)][i % 3 + this.moduleCount - 8 - 3] = mod;
              this.modules[i % 3 + this.moduleCount - 8 - 3][Math.floor(i / 3)] = mod;
            }
          };
          this.setupTypeInfo = function(test, maskPattern) {
            var data = (this.errorCorrectLevel << 3) | maskPattern;
            var bits = QRUtil.getBCHTypeInfo(data);
            for (var i = 0; i < 15; i++) {
              var mod = (!test && ((bits >> i) & 1) == 1);
              if (i < 6) {
                this.modules[i][8] = mod;
              } else if (i < 8) {
                this.modules[i + 1][8] = mod;
              } else {
                this.modules[this.moduleCount - 15 + i][8] = mod;
              }
            }
            for (var i = 0; i < 15; i++) {
              var mod = (!test && ((bits >> i) & 1) == 1);
              if (i < 8) {
                this.modules[8][this.moduleCount - i - 1] = mod;
              } else if (i < 9) {
                this.modules[8][15 - i - 1 + 1] = mod;
              } else {
                this.modules[8][15 - i - 1] = mod;
              }
            }
            this.modules[this.moduleCount - 8][8] = (!test);
          };
          this.mapData = function(data, maskPattern) {
            var inc = -1;
            var row = this.moduleCount - 1;
            var bitIndex = 7;
            var byteIndex = 0;
            for (var col = this.moduleCount - 1; col > 0; col -= 2) {
              if (col == 6) col--;
              while (true) {
                for (var c = 0; c < 2; c++) {
                  if (this.modules[row][col - c] == null) {
                    var dark = false;
                    if (byteIndex < data.length) {
                      dark = (((data[byteIndex] >>> bitIndex) & 1) == 1);
                    }
                    var mask = QRUtil.getMask(maskPattern, row, col - c);
                    if (mask) dark = !dark;
                    this.modules[row][col - c] = dark;
                    bitIndex--;
                    if (bitIndex == -1) {
                      byteIndex++;
                      bitIndex = 7;
                    }
                  }
                }
                row += inc;
                if (row < 0 || this.moduleCount <= row) {
                  row -= inc;
                  inc = -inc;
                  break;
                }
              }
            }
          };
        }

        QRCode.createData = function(typeNumber, errorCorrectLevel, dataList) {
          var rsBlocks = QRRSBlock.getRSBlocks(typeNumber, errorCorrectLevel);
          var buffer = new QRBitBuffer();
          for (var i = 0; i < dataList.length; i++) {
            var data = dataList[i];
            buffer.put(data.mode, 4);
            buffer.put(data.getLength(), 8);
            data.write(buffer);
          }
          var totalDataCount = 0;
          for (var i = 0; i < rsBlocks.length; i++) {
            totalDataCount += rsBlocks[i].dataCount;
          }
          if (buffer.length > totalDataCount * 8) {
            throw new Error("code length overflow");
          }
          if (buffer.length + 4 <= totalDataCount * 8) buffer.put(0, 4);
          while (buffer.length % 8 != 0) buffer.putBit(false);
          while (true) {
            if (buffer.length >= totalDataCount * 8) break;
            buffer.put(0xec, 8);
            if (buffer.length >= totalDataCount * 8) break;
            buffer.put(0x11, 8);
          }
          return QRCode.createBytes(buffer, rsBlocks);
        };

        QRCode.createBytes = function(buffer, rsBlocks) {
          var offset = 0;
          var maxDcCount = 0;
          var maxEcCount = 0;
          var dcdata = new Array(rsBlocks.length);
          var ecdata = new Array(rsBlocks.length);
          for (var r = 0; r < rsBlocks.length; r++) {
            var dcCount = rsBlocks[r].dataCount;
            var ecCount = rsBlocks[r].totalCount - dcCount;
            maxDcCount = Math.max(maxDcCount, dcCount);
            maxEcCount = Math.max(maxEcCount, ecCount);
            dcdata[r] = new Array(dcCount);
            for (var i = 0; i < dcdata[r].length; i++) {
              dcdata[r][i] = 0xff & buffer.buffer[i + offset];
            }
            offset += dcCount;
            var rsPoly = QRUtil.getErrorCorrectPolynomial(ecCount);
            var rawPoly = new QRPolynomial(dcdata[r], rsPoly.getLength() - 1);
            var modPoly = rawPoly.mod(rsPoly);
            ecdata[r] = new Array(rsPoly.getLength() - 1);
            for (var i = 0; i < ecdata[r].length; i++) {
              var modIndex = i + modPoly.getLength() - ecdata[r].length;
              ecdata[r][i] = (modIndex >= 0) ? modPoly.get(modIndex) : 0;
            }
          }
          var totalCodeCount = 0;
          for (var i = 0; i < rsBlocks.length; i++) {
            totalCodeCount += rsBlocks[i].totalCount;
          }
          var data = new Array(totalCodeCount);
          var index = 0;
          for (var i = 0; i < maxDcCount; i++) {
            for (var r = 0; r < rsBlocks.length; r++) {
              if (i < dcdata[r].length) data[index++] = dcdata[r][i];
            }
          }
          for (var i = 0; i < maxEcCount; i++) {
            for (var r = 0; r < rsBlocks.length; r++) {
              if (i < ecdata[r].length) data[index++] = ecdata[r][i];
            }
          }
          return data;
        };

        QRCode.getLostPoint = function(qrCode) {
          var moduleCount = qrCode.getModuleCount();
          var lostPoint = 0;
          for (var row = 0; row < moduleCount; row++) {
            for (var col = 0; col < moduleCount; col++) {
              var sameCount = 0;
              var dark = qrCode.isDark(row, col);
              for (var r = -1; r <= 1; r++) {
                if (row + r < 0 || moduleCount <= row + r) continue;
                for (var c = -1; c <= 1; c++) {
                  if (col + c < 0 || moduleCount <= col + c) continue;
                  if (r == 0 && c == 0) continue;
                  if (dark == qrCode.isDark(row + r, col + c)) sameCount++;
                }
              }
              if (sameCount > 5) lostPoint += (3 + sameCount - 5);
            }
          }
          for (var row = 0; row < moduleCount - 1; row++) {
            for (var col = 0; col < moduleCount - 1; col++) {
              var count = 0;
              if (qrCode.isDark(row, col)) count++;
              if (qrCode.isDark(row + 1, col)) count++;
              if (qrCode.isDark(row, col + 1)) count++;
              if (qrCode.isDark(row + 1, col + 1)) count++;
              if (count == 0 || count == 4) lostPoint += 3;
            }
          }
          for (var row = 0; row < moduleCount; row++) {
            for (var col = 0; col < moduleCount - 6; col++) {
              if (qrCode.isDark(row, col) &&
                  !qrCode.isDark(row, col + 1) &&
                  qrCode.isDark(row, col + 2) &&
                  qrCode.isDark(row, col + 3) &&
                  qrCode.isDark(row, col + 4) &&
                  !qrCode.isDark(row, col + 5) &&
                  qrCode.isDark(row, col + 6)) {
                lostPoint += 40;
              }
            }
          }
          for (var col = 0; col < moduleCount; col++) {
            for (var row = 0; row < moduleCount - 6; row++) {
              if (qrCode.isDark(row, col) &&
                  !qrCode.isDark(row + 1, col) &&
                  qrCode.isDark(row + 2, col) &&
                  qrCode.isDark(row + 3, col) &&
                  qrCode.isDark(row + 4, col) &&
                  !qrCode.isDark(row + 5, col) &&
                  qrCode.isDark(row + 6, col)) {
                lostPoint += 40;
              }
            }
          }
          var darkCount = 0;
          for (var col = 0; col < moduleCount; col++) {
            for (var row = 0; row < moduleCount; row++) {
              if (qrCode.isDark(row, col)) darkCount++;
            }
          }
          var ratio = Math.abs(100 * darkCount / moduleCount / moduleCount - 50) / 5;
          lostPoint += ratio * 10;
          return lostPoint;
        };

        return function(typeNumber, errorCorrectLevel) {
          return new QRCode(typeNumber, errorCorrectLevel);
        };
      }();
    </script>

    <script>
      const endpointButtons = Array.from(document.querySelectorAll('#endpointTabs button'));
      const deviceSelect = document.getElementById('deviceSelect');
      const deviceInput = document.getElementById('deviceInput');
      const refreshBtn = document.getElementById('refreshDevices');
      const outputUrl = document.getElementById('outputUrl');
      const copyBtn = document.getElementById('copyBtn');
      const openBtn = document.getElementById('openBtn');
      const methodHint = document.getElementById('methodHint');
      const captureParams = document.getElementById('captureParams');
      const udpParams = document.getElementById('udpParams');
      const qrHint = document.getElementById('qrHint');

      const paramKeys = [
        { key: 'w', input: 'w', use: 'use_w' },
        { key: 'h', input: 'h', use: 'use_h' },
        { key: 'fps', input: 'fps', use: 'use_fps' },
        { key: 'bitrate', input: 'bitrate', use: 'use_bitrate' },
        { key: 'gop', input: 'gop', use: 'use_gop' },
        { key: 'codec', input: 'codec', use: 'use_codec' },
        { key: 'latency', input: 'latency', use: 'use_latency' },
        { key: 'container', input: 'container', use: 'use_container' }
      ];

      const udpExtras = [
        { key: 'duration', input: 'duration', use: 'use_duration' }
      ];

      let endpoint = 'live';

      function updateEndpointUI() {
        endpointButtons.forEach(btn => {
          btn.classList.toggle('active', btn.dataset.endpoint === endpoint);
        });
        captureParams.style.display = (endpoint === 'live' || endpoint === 'udp') ? 'block' : 'none';
        udpParams.style.display = (endpoint === 'udp') ? 'block' : 'none';
        methodHint.textContent = endpoint === 'udp' ? 'GET (UDP trigger)' : 'GET';
        updateUrl();
      }

      function addQueryParam(params, key, value) {
        if (value === null || value === undefined || value === '') return;
        params.push(encodeURIComponent(key) + '=' + encodeURIComponent(value));
      }

      function buildUrl() {
        const origin = window.location.origin;
        const device = deviceInput.value.trim() || 'video0';
        if (endpoint === 'list') return origin + '/device/list';
        if (endpoint === 'stats') return origin + '/stream/' + encodeURIComponent(device) + '/stats';
        let path = endpoint === 'udp'
          ? '/stream/udp/' + encodeURIComponent(device)
          : '/stream/live/' + encodeURIComponent(device);
        const params = [];
        if (endpoint === 'udp') {
          addQueryParam(params, 'target', document.getElementById('target').value.trim());
          addQueryParam(params, 'port', document.getElementById('port').value.trim());
          const useDuration = document.getElementById('use_duration');
          if (useDuration && useDuration.checked) {
            addQueryParam(params, 'duration', document.getElementById('duration').value.trim());
          }
        }
        paramKeys.forEach(p => {
          const use = document.getElementById(p.use);
          const input = document.getElementById(p.input);
          if (use && use.checked) {
            addQueryParam(params, p.key, input.value.trim());
          }
        });
        return origin + path + (params.length ? '?' + params.join('&') : '');
      }

      function renderQR(text) {
        const canvas = document.getElementById('qr');
        const ctx = canvas.getContext('2d');
        if (!text) {
          ctx.clearRect(0, 0, canvas.width, canvas.height);
          return;
        }
        try {
          const qr = qrcode(0, 1);
          qr.addData(text);
          qr.make();
          const size = qr.getModuleCount();
          const scale = 4;
          canvas.width = canvas.height = size * scale;
          ctx.fillStyle = '#fff';
          ctx.fillRect(0, 0, canvas.width, canvas.height);
          ctx.fillStyle = '#000';
          for (let r = 0; r < size; r++) {
            for (let c = 0; c < size; c++) {
              if (qr.isDark(r, c)) {
                ctx.fillRect(c * scale, r * scale, scale, scale);
              }
            }
          }
          if (qrHint) qrHint.textContent = 'QR updates when the link changes.';
        } catch (e) {
          canvas.width = canvas.height = 140;
          ctx.fillStyle = '#fff';
          ctx.fillRect(0, 0, canvas.width, canvas.height);
          ctx.fillStyle = '#000';
          ctx.font = '12px sans-serif';
          ctx.fillText('QR too long', 20, 70);
          if (qrHint) qrHint.textContent = 'URL too long for this QR encoder.';
        }
      }

      function updateUrl() {
        const url = buildUrl();
        outputUrl.value = url;
        renderQR(url);
      }

      endpointButtons.forEach(btn => {
        btn.addEventListener('click', () => {
          endpoint = btn.dataset.endpoint;
          updateEndpointUI();
        });
      });

      deviceSelect.addEventListener('change', () => {
        if (deviceSelect.value) {
          deviceInput.value = deviceSelect.value;
          updateUrl();
        }
      });
      deviceInput.addEventListener('input', updateUrl);

      function wireParam(useId, inputId) {
        const use = document.getElementById(useId);
        const input = document.getElementById(inputId);
        if (!use || !input) return;
        use.addEventListener('change', updateUrl);
        input.addEventListener('input', () => {
          if (input.value !== '') use.checked = true;
          updateUrl();
        });
      }
      paramKeys.forEach(p => wireParam(p.use, p.input));
      udpExtras.forEach(p => wireParam(p.use, p.input));
      wireParam('use_duration', 'duration');
      document.getElementById('target').addEventListener('input', updateUrl);
      document.getElementById('port').addEventListener('input', updateUrl);

      copyBtn.addEventListener('click', async () => {
        try {
          await navigator.clipboard.writeText(outputUrl.value);
          copyBtn.textContent = 'Copied';
          setTimeout(() => (copyBtn.textContent = 'Copy'), 1200);
        } catch (e) {
          outputUrl.select();
          document.execCommand('copy');
        }
      });
      openBtn.addEventListener('click', () => {
        window.open(outputUrl.value, '_blank');
      });

      async function refreshDevices() {
        try {
          const resp = await fetch('/device/list');
          const list = await resp.json();
          deviceSelect.innerHTML = '';
          list.forEach((id) => {
            const opt = document.createElement('option');
            opt.value = id;
            opt.textContent = id;
            deviceSelect.appendChild(opt);
          });
          if (list.length) {
            deviceSelect.value = list[0];
            deviceInput.value = list[0];
          }
        } catch (e) {
          deviceSelect.innerHTML = '';
          const opt = document.createElement('option');
          opt.value = '';
          opt.textContent = 'manual';
          deviceSelect.appendChild(opt);
        }
        updateUrl();
      }

      refreshBtn.addEventListener('click', refreshDevices);
      refreshDevices();
      updateEndpointUI();
    </script>
  </body>
</html>
)HTML";

std::string json_array(const std::vector<std::string> &items) {
  std::string out = "[";
  for (size_t i = 0; i < items.size(); ++i) {
    out += "\"" + items[i] + "\"";
    if (i + 1 < items.size())
      out += ",";
  }
  out += "]";
  return out;
}

std::string build_error_json(const std::string &msg,
                             const std::string &details = "") {
  std::string out = "{\"error\":\"" + msg + "\"";
  if (!details.empty())
    out += ",\"details\":\"" + details + "\"";
  out += "}";
  return out;
}

// Convert Annex-B NAL stream to length-prefixed (AVCC) single-sample buffer.
std::vector<uint8_t> annexb_to_avcc(const std::string &annexb) {
  std::vector<uint8_t> out;
  size_t i = 0;
  auto len = annexb.size();
  while (i + 3 < len) {
    // find start code
    if (!(annexb[i] == 0 && annexb[i + 1] == 0 &&
          ((annexb[i + 2] == 1) ||
           (annexb[i + 2] == 0 && i + 3 < len && annexb[i + 3] == 1)))) {
      ++i;
      continue;
    }
    size_t sc_size = (annexb[i + 2] == 1) ? 3 : 4;
    i += sc_size;
    size_t start = i;
    while (i + 3 < len &&
           !(annexb[i] == 0 && annexb[i + 1] == 0 &&
             ((annexb[i + 2] == 1) ||
              (annexb[i + 2] == 0 && i + 3 < len && annexb[i + 3] == 1)))) {
      ++i;
    }
    size_t nalsize = i - start;
    uint32_t n = static_cast<uint32_t>(nalsize);
    out.push_back((n >> 24) & 0xFF);
    out.push_back((n >> 16) & 0xFF);
    out.push_back((n >> 8) & 0xFF);
    out.push_back(n & 0xFF);
    out.insert(out.end(), annexb.begin() + start,
               annexb.begin() + start + nalsize);
  }
  return out;
}

// Extract SPS/PPS from Annex-B sample (first IDR)
void extract_sps_pps(const std::string &annexb, std::vector<uint8_t> &sps,
                     std::vector<uint8_t> &pps) {
  size_t i = 0;
  auto len = annexb.size();
  while (i + 4 < len) {
    if (!(annexb[i] == 0 && annexb[i + 1] == 0 &&
          ((annexb[i + 2] == 1) ||
           (annexb[i + 2] == 0 && annexb[i + 3] == 1)))) {
      ++i;
      continue;
    }
    size_t sc_size = (annexb[i + 2] == 1) ? 3 : 4;
    i += sc_size;
    size_t start = i;
    while (i + 4 < len && !(annexb[i] == 0 && annexb[i + 1] == 0 &&
                            ((annexb[i + 2] == 1) ||
                             (annexb[i + 2] == 0 && annexb[i + 3] == 1)))) {
      ++i;
    }
    size_t nalsize = i - start;
    if (nalsize == 0)
      continue;
    uint8_t nal_type = annexb[start] & 0x1F;
    if (nal_type == 7 && sps.empty()) {
      sps.assign(annexb.begin() + start, annexb.begin() + start + nalsize);
    } else if (nal_type == 8 && pps.empty()) {
      pps.assign(annexb.begin() + start, annexb.begin() + start + nalsize);
    }
    if (!sps.empty() && !pps.empty())
      break;
  }
}

CaptureParams parse_params(const httplib::Request &req) {
  CaptureParams p;
  if (req.has_param("w"))
    p.width = std::stoi(req.get_param_value("w"));
  if (req.has_param("h"))
    p.height = std::stoi(req.get_param_value("h"));
  if (req.has_param("fps"))
    p.fps = std::stoi(req.get_param_value("fps"));
  if (req.has_param("bitrate"))
    p.bitrate_kbps = std::stoi(req.get_param_value("bitrate"));
  if (req.has_param("gop"))
    p.gop = std::stoi(req.get_param_value("gop"));
  if (req.has_param("codec"))
    p.codec = req.get_param_value("codec");
  if (req.has_param("latency"))
    p.latency = req.get_param_value("latency");
  if (req.has_param("container"))
    p.container = req.get_param_value("container");
  return p;
}

void add_effective_headers(httplib::Response &res, const EffectiveParams &eff) {
  const auto &a = eff.actual;
  res.set_header("Effective-Params",
                 "codec=" + a.codec + ";w=" + std::to_string(a.width) +
                     ";h=" + std::to_string(a.height) +
                     ";fps=" + std::to_string(a.fps) +
                     ";bitrate=" + std::to_string(a.bitrate_kbps) +
                     ";gop=" + std::to_string(a.gop) + ";latency=" + a.latency);
}

void serve_mjpeg_placeholder(const CaptureParams &p, httplib::Response &res,
                             std::shared_ptr<Session> session,
                             std::function<void(bool)> on_done) {
  const auto boundary = "frame";
  res.set_header("Connection", "close");
  res.set_chunked_content_provider(
      "multipart/x-mixed-replace; boundary=" + std::string(boundary),
      [p, boundary, session](size_t, httplib::DataSink &sink) mutable {
        const int frame_interval_ms = std::max(1, 1000 / std::max(1, p.fps));
        std::string prefix =
            "--" + std::string(boundary) +
            "\r\nContent-Type: image/jpeg\r\nContent-Length: " +
            std::to_string(sizeof(kTinyJpeg)) + "\r\n\r\n";
        for (;;) {
          if (!sink.write(prefix.data(), prefix.size()))
            return false;
          if (!sink.write(reinterpret_cast<const char *>(kTinyJpeg),
                          sizeof(kTinyJpeg)))
            return false;
          if (!sink.write("\r\n", 2))
            return false;
          session->frames_sent.fetch_add(1);
          session->bytes_sent.fetch_add(prefix.size() + sizeof(kTinyJpeg) + 2);
          session->last_accessed = std::chrono::steady_clock::now();
          std::this_thread::sleep_for(
              std::chrono::milliseconds(frame_interval_ms));
        }
        return true;
      },
      on_done);
}

void serve_mjpeg_live(const CaptureParams &p, httplib::Response &res,
                      std::shared_ptr<Session> session,
                      std::function<void(bool)> on_done) {
  const auto boundary = "frame";
  res.set_header("Connection", "close");
  res.set_chunked_content_provider(
      "multipart/x-mixed-replace; boundary=" + std::string(boundary),
      [p, boundary, session](size_t, httplib::DataSink &sink) mutable {
        const int frame_interval_ms = std::max(1, 1000 / std::max(1, p.fps));
        std::string prefix;
        std::string frame;
        for (;;) {
          if (!session->capture || !session->capture->running()) {
            std::this_thread::sleep_for(20ms);
            continue;
          }
          if (session->capture->pixel_format() != PixelFormat::MJPEG ||
              !session->capture->latest_frame(frame)) {
            std::this_thread::sleep_for(10ms);
            continue;
          }
          prefix = "--" + std::string(boundary) +
                   "\r\nContent-Type: image/jpeg\r\nContent-Length: " +
                   std::to_string(frame.size()) + "\r\n\r\n";
          if (!sink.write(prefix.data(), prefix.size()))
            return false;
          if (!sink.write(frame.data(), frame.size()))
            return false;
          if (!sink.write("\r\n", 2))
            return false;
          session->frames_sent.fetch_add(1);
          session->bytes_sent.fetch_add(prefix.size() + frame.size() + 2);
          session->last_accessed = std::chrono::steady_clock::now();
          std::this_thread::sleep_for(
              std::chrono::milliseconds(frame_interval_ms));
        }
        return true;
      },
      on_done);
}

void serve_h264_live(const CaptureParams &p, httplib::Response &res,
                     std::shared_ptr<Session> session,
                     std::function<void(bool)> on_done) {
#ifdef HAS_OPENH264
  res.set_header("Connection", "close");
  res.set_header("Content-Type", "video/H264");
  res.set_chunked_content_provider(
      "video/H264",
      [p, session](size_t, httplib::DataSink &sink) mutable {
        if (!session->encoder) {
          session->encoder = std::make_shared<H264Encoder>();
          if (!session->encoder->init(session->params)) {
            return false;
          }
          session->encoder->force_idr();
        }
        const int y_size = p.width * p.height;
        const int uv_size = (p.width / 2) * (p.height / 2);
        std::string frame;
        std::string yuv;
        yuv.resize(y_size + 2 * uv_size);
        uint8_t *y = reinterpret_cast<uint8_t *>(yuv.data());
        uint8_t *u = y + y_size;
        uint8_t *v = u + uv_size;

        const int frame_interval_ms = std::max(1, 1000 / std::max(1, p.fps));
        bool first = true;
        for (;;) {
          if (!session->capture || !session->capture->running()) {
            std::this_thread::sleep_for(20ms);
            continue;
          }
          PixelFormat fmt = session->capture->pixel_format();
          if ((fmt != PixelFormat::YUYV && fmt != PixelFormat::NV12) ||
              !session->capture->latest_frame(frame)) {
            std::this_thread::sleep_for(10ms);
            continue;
          }
          if (fmt == PixelFormat::YUYV) {
            yuyv_to_i420(reinterpret_cast<const uint8_t *>(frame.data()),
                         p.width, p.height, y, u, v);
          } else {
            const uint8_t *src_y =
                reinterpret_cast<const uint8_t *>(frame.data());
            const uint8_t *src_uv = src_y + (p.width * p.height);
            nv12_to_i420(src_y, src_uv, p.width, p.height, p.width, p.width, y,
                         u, v);
          }
          if (first) {
            session->encoder->force_idr();
            first = false;
          }
          std::string nal;
          if (!session->encoder->encode_i420(y, u, v, nal)) {
            std::this_thread::sleep_for(5ms);
            continue;
          }
          if (!nal.empty()) {
            // Annex B start code 00 00 00 01
            static const char start_code[] = {0, 0, 0, 1};
            if (!sink.write(start_code, 4))
              return false;
            if (!sink.write(nal.data(), nal.size()))
              return false;
            session->frames_sent.fetch_add(1);
            session->bytes_sent.fetch_add(4 + nal.size());
            session->last_accessed = std::chrono::steady_clock::now();
          }
          std::this_thread::sleep_for(
              std::chrono::milliseconds(frame_interval_ms));
        }
        return true;
      },
      on_done);
#else
  (void)p;
  // (void)session;
  res.status = 503;
  res.set_content(build_error_json("h264_unavailable", "OpenH264 not enabled"),
                  "application/json");
  on_done(false);
#endif
}

void serve_fmp4_live(const CaptureParams &p, httplib::Response &res,
                     std::shared_ptr<Session> session,
                     std::function<void(bool)> on_done) {
#ifdef HAS_OPENH264
  res.set_header("Connection", "close");
  res.set_header("Content-Type", "video/mp4");
  res.set_header("Cache-Control", "no-store");
  res.set_header("Access-Control-Allow-Origin", "*");
  const uint32_t sample_duration = p.fps > 0 ? (90000 / p.fps) : 6000;

  res.set_chunked_content_provider(
      "video/mp4",
      [p, session, sample_duration](size_t, httplib::DataSink &sink) mutable {
        if (!session->encoder) {
          session->encoder = std::make_shared<H264Encoder>();
          if (!session->encoder->init(session->params))
            return false;
          session->encoder->force_idr();
        }
        const int y_size = p.width * p.height;
        const int uv_size = (p.width / 2) * (p.height / 2);
        std::string frame;
        std::string yuv;
        yuv.resize(y_size + 2 * uv_size);
        uint8_t *y = reinterpret_cast<uint8_t *>(yuv.data());
        uint8_t *u = y + y_size;
        uint8_t *v = u + uv_size;
        bool sent_init = false;
        Mp4Fragmenter *mux = nullptr;
        std::unique_ptr<Mp4Fragmenter> mux_guard;
        uint64_t decode_time = 0;
        while (true) {
          if (!session->capture || !session->capture->running()) {
            std::this_thread::sleep_for(10ms);
            continue;
          }
          if (!session->capture->latest_frame(frame)) {
            std::this_thread::sleep_for(5ms);
            continue;
          }
          PixelFormat fmt = session->capture->pixel_format();
          if (fmt != PixelFormat::YUYV && fmt != PixelFormat::NV12) {
            std::this_thread::sleep_for(5ms);
            continue;
          }
          if (fmt == PixelFormat::YUYV) {
            yuyv_to_i420(reinterpret_cast<const uint8_t *>(frame.data()),
                         p.width, p.height, y, u, v);
          } else {
            const uint8_t *src_y =
                reinterpret_cast<const uint8_t *>(frame.data());
            const uint8_t *src_uv = src_y + (p.width * p.height);
            nv12_to_i420(src_y, src_uv, p.width, p.height, p.width, p.width, y,
                         u, v);
          }
          std::string nal_annexb;
          if (!session->encoder->encode_i420(y, u, v, nal_annexb)) {
            std::this_thread::sleep_for(5ms);
            continue;
          }
          if (session->sps.empty() || session->pps.empty()) {
            extract_sps_pps(nal_annexb, session->sps, session->pps);
            if (!session->sps.empty() && !session->pps.empty()) {
              mux_guard = std::make_unique<Mp4Fragmenter>(
                  p.width, p.height, p.fps, session->sps, session->pps);
              mux = mux_guard.get();
            }
          }
          if (!mux) {
            continue;
          }
          if (!sent_init) {
            auto init_seg = mux->build_init_segment();
            if (!sink.write(init_seg.data(), init_seg.size()))
              return false;
            sent_init = true;
          }
          auto avcc = annexb_to_avcc(nal_annexb);
          bool keyframe = !nal_annexb.empty() && ((nal_annexb[4] & 0x1F) == 5);
          auto frag = mux->build_fragment(avcc, session->seqno++, decode_time,
                                          sample_duration, keyframe);
          decode_time += sample_duration;
          if (!sink.write(frag.data(), frag.size()))
            return false;

          session->frames_sent.fetch_add(1);
          session->bytes_sent.fetch_add(frag.size());
          session->last_accessed = std::chrono::steady_clock::now();
          std::this_thread::sleep_for(
              std::chrono::milliseconds(1000 / std::max(1, p.fps)));
        }
        return true;
      },
      on_done);
#else
  (void)p;
  // (void)session;
  res.status = 503;
  res.set_content(build_error_json("h264_unavailable", "OpenH264 not enabled"),
                  "application/json");
  on_done(false);
#endif
}

int main(int argc, char *argv[]) {
  struct Config {
    std::string addr = "0.0.0.0";
    int port = 8080;
    int idle_timeout = 10;
    std::string default_codec = "mjpeg";
  } cfg;

  for (int i = 1; i < argc; ++i) {
    std::string arg(argv[i]);
    if (arg == "--addr" && i + 1 < argc) {
      cfg.addr = argv[++i];
    } else if (arg == "--port" && i + 1 < argc) {
      cfg.port = std::stoi(argv[++i]);
    } else if (arg == "--idle-timeout" && i + 1 < argc) {
      cfg.idle_timeout = std::stoi(argv[++i]);
    } else if (arg == "--codec" && i + 1 < argc) {
      cfg.default_codec = argv[++i];
    } else if (arg == "--help" || arg == "-h") {
      std::cout << "SilkCast\n"
                << "  --addr <ip>          Bind address (default 0.0.0.0)\n"
                << "  --port <port>        Bind port   (default 8080)\n"
                << "  --idle-timeout <s>   Idle seconds before closing device "
                   "(default 10)\n"
                << "  --codec <mjpeg|h264> Default codec if not specified "
                   "(default mjpeg)\n";
      return 0;
    }
  }

  SessionManager sessions(cfg.idle_timeout);
  httplib::Server svr;

  // NOTE: WebSocket endpoint disabled - cpp-httplib FetchContent version lacks
  // WebSocket support. Use HTTP streaming endpoints (/stream/live/{device})
  // instead. svr.set_ws_endpoint(...) removed for compatibility.
  svr.Get(R"(/)", [](const httplib::Request &, httplib::Response &res) {
    res.set_header("Content-Type", "text/html; charset=utf-8");
    res.status = 200;
    res.set_content(kIndexHtml, "text/html");
  });

  svr.Get(R"(/device/list)",
          [&sessions](const httplib::Request &, httplib::Response &res) {
            auto devices = sessions.list_devices();
            res.set_header("Content-Type", "application/json");
            res.status = 200;
            res.set_content(json_array(devices), "application/json");
          });

  svr.Get(R"(/stream/([^/]+)/stats)", [&sessions](const httplib::Request &req,
                                                  httplib::Response &res) {
    std::string device_id = req.matches[1].str();
    auto session_opt = sessions.find(device_id);
    if (!session_opt) {
      res.status = 404;
      res.set_content(
          build_error_json("not_found", "device " + std::string(device_id)),
          "application/json");
      return;
    }
    auto session = *session_opt;
    sessions.touch(device_id);
    const auto now = std::chrono::steady_clock::now();
    double uptime =
        std::chrono::duration_cast<std::chrono::seconds>(now - session->started)
            .count();
    if (uptime < 0.001)
      uptime = 0.001;
    double fps = session->frames_sent.load() / uptime;
    double bitrate_kbps = (session->bytes_sent.load() * 8.0 / 1000.0) / uptime;

    res.status = 200;
    res.set_content("{"
                    "\"device\":\"" +
                        session->device_id +
                        "\","
                        "\"fps\":" +
                        std::to_string(session->params.fps) +
                        ","
                        "\"bitrate_kbps\":" +
                        std::to_string(session->params.bitrate_kbps) +
                        ","
                        "\"active_clients\":" +
                        std::to_string(session->client_count.load()) +
                        ","
                        "\"fps_out\":" +
                        std::to_string(fps) +
                        ","
                        "\"bitrate_out_kbps\":" +
                        std::to_string(bitrate_kbps) +
                        ","
                        "\"frames_sent\":" +
                        std::to_string(session->frames_sent.load()) +
                        ","
                        "\"bytes_sent\":" +
                        std::to_string(session->bytes_sent.load()) + "}",
                    "application/json");
  });

  svr.Get(R"(/stream/live/([^/]+))", [&sessions](const httplib::Request &req,
                                                 httplib::Response &res) {
    std::string device_id = req.matches[1].str();
    auto params = parse_params(req);
    if (params.codec.empty())
      params.codec = "mjpeg";
    if (params.container.empty())
      params.container = "raw";

    // On-demand session creation.
    auto session = sessions.get_or_create(device_id, params);
    session->client_count.fetch_add(1);
    session->last_accessed = std::chrono::steady_clock::now();

    EffectiveParams eff{params, session->params};
    add_effective_headers(res, eff);

    // If request codec/container mismatch first-comer, respond 409 with
    // Effective-Params.
    if (params.codec != session->params.codec ||
        params.container != session->params.container) {
      res.status = 409;
      res.set_content(
          build_error_json("conflict", "params locked by first requester"),
          "application/json");
      session->client_count.fetch_sub(1);
      return;
    }

    // Callback for cleanup when stream ends
    auto on_done = [device_id, &sessions](bool) {
      auto session_opt = sessions.find(device_id);
      if (session_opt) {
        (*session_opt)->client_count.fetch_sub(1);
        sessions.release_if_idle(device_id);
      }
    };

    // Start capture if needed (first requester defines params).
    if (!session->capture->running()) {
      if (!session->capture->start(device_id, session->params)) {
        res.status = 503;
        res.set_content(
            build_error_json("device_unavailable", "failed to open camera"),
            "application/json");
        session->client_count.fetch_sub(1);
        return;
      }
      session->pixel_format = session->capture->pixel_format();
#ifdef __APPLE__
      session->params.width = session->capture->width();
      session->params.height = session->capture->height();
#endif
      session->started = std::chrono::steady_clock::now();
      session->frames_sent = 0;
      session->bytes_sent = 0;
    }

    EffectiveParams eff_actual{params, session->params};
    add_effective_headers(res, eff_actual);

    if (params.codec == "mjpeg") {
      serve_mjpeg_live(session->params, res, session, on_done);
    } else if (params.codec == "h264") {
      if (params.container == "mp4") {
        serve_fmp4_live(session->params, res, session, on_done);
      } else {
        serve_h264_live(session->params, res, session, on_done);
      }
    } else {
      res.status = 400;
      res.set_content(build_error_json("bad_request", "unsupported codec"),
                      "application/json");
      // For failure case, we must decrement immediately as on_done won't be
      // called
      session->client_count.fetch_sub(1);
      sessions.release_if_idle(device_id);
    }
  });

  svr.Get(R"(/stream/udp/([^/]+))", [&sessions](const httplib::Request &req,
                                                httplib::Response &res) {
#ifdef __linux__
    std::string device_id = req.matches[1].str();
    if (!req.has_param("target") || !req.has_param("port")) {
      res.status = 400;
      res.set_content(
          build_error_json("bad_request", "target and port are required"),
          "application/json");
      return;
    }
    const std::string target = req.get_param_value("target");
    int port = std::stoi(req.get_param_value("port"));
    int duration_sec = req.has_param("duration")
                           ? std::stoi(req.get_param_value("duration"))
                           : 10;
    auto params = parse_params(req);
    if (params.codec.empty())
      params.codec = "h264"; // UDP favors H.264

    auto session = sessions.get_or_create(device_id, params);
    session->client_count.fetch_add(1);
    session->last_accessed = std::chrono::steady_clock::now();

    if (!session->capture->running()) {
      if (!session->capture->start(device_id, session->params)) {
        res.status = 503;
        res.set_content(
            build_error_json("device_unavailable", "failed to open camera"),
            "application/json");
        session->client_count.fetch_sub(1);
        return;
      }
      session->started = std::chrono::steady_clock::now();
      session->frames_sent = 0;
      session->bytes_sent = 0;
    }

    std::thread([session, params, target, port, duration_sec, &sessions]() {
      int sock = socket(AF_INET, SOCK_DGRAM, 0);
      if (sock < 0) {
        session->client_count.fetch_sub(1);
        sessions.release_if_idle(session->device_id);
        return;
      }
      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(port);
      if (inet_pton(AF_INET, target.c_str(), &addr.sin_addr) != 1) {
        close(sock);
        session->client_count.fetch_sub(1);
        sessions.release_if_idle(session->device_id);
        return;
      }

      std::string frame;
      std::string yuv;
      const int y_size = params.width * params.height;
      const int uv_size = (params.width / 2) * (params.height / 2);
      yuv.resize(y_size + 2 * uv_size);
      uint8_t *y = reinterpret_cast<uint8_t *>(yuv.data());
      uint8_t *u = y + y_size;
      uint8_t *v = u + uv_size;
      bool first = true;
      auto start = std::chrono::steady_clock::now();
      const int frame_interval_ms = std::max(1, 1000 / std::max(1, params.fps));
      const size_t mtu = 1400;

      while (true) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();
        if (elapsed >= duration_sec)
          break;
        if (!session->capture || !session->capture->running()) {
          std::this_thread::sleep_for(10ms);
          continue;
        }
        if (!session->capture->latest_frame(frame)) {
          std::this_thread::sleep_for(5ms);
          continue;
        }
        if (params.codec == "mjpeg") {
          size_t offset = 0;
          while (offset < frame.size()) {
            size_t chunk = std::min(mtu, frame.size() - offset);
            sendto(sock, frame.data() + offset, chunk, 0,
                   reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
            offset += chunk;
          }
          session->frames_sent.fetch_add(1);
          session->bytes_sent.fetch_add(frame.size());
        } else if (params.codec == "h264") {
#ifdef HAS_OPENH264
          if (!session->encoder) {
            session->encoder = std::make_shared<H264Encoder>();
            if (!session->encoder->init(session->params))
              break;
            session->encoder->force_idr();
            first = false;
          }
          PixelFormat fmt = session->capture->pixel_format();
          if (fmt != PixelFormat::YUYV && fmt != PixelFormat::NV12) {
            std::this_thread::sleep_for(5ms);
            continue;
          }
          if (fmt == PixelFormat::YUYV) {
            yuyv_to_i420(reinterpret_cast<const uint8_t *>(frame.data()),
                         params.width, params.height, y, u, v);
          } else {
            const uint8_t *src_y =
                reinterpret_cast<const uint8_t *>(frame.data());
            const uint8_t *src_uv = src_y + (params.width * params.height);
            nv12_to_i420(src_y, src_uv, params.width, params.height,
                         params.width, params.width, y, u, v);
          }
          if (first) {
            session->encoder->force_idr();
            first = false;
          }
          std::string nal;
          if (!session->encoder->encode_i420(y, u, v, nal)) {
            std::this_thread::sleep_for(5ms);
            continue;
          }
          static const char start_code[] = {0x00, 0x00, 0x00, 0x01};
          std::string packet;
          packet.reserve(sizeof(start_code) + nal.size());
          packet.append(start_code, sizeof(start_code));
          packet.append(nal);
          sendto(sock, packet.data(), packet.size(), 0,
                 reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
          session->frames_sent.fetch_add(1);
          session->bytes_sent.fetch_add(packet.size());
#else
          break;
#endif
        } else {
          break;
        }
        session->last_accessed = std::chrono::steady_clock::now();
        std::this_thread::sleep_for(
            std::chrono::milliseconds(frame_interval_ms));
      }
      close(sock);
      session->client_count.fetch_sub(1);
      sessions.release_if_idle(session->device_id);
    }).detach();

    res.status = 200;
    res.set_content("{\"status\":\"udp_stream_started\"}", "application/json");
#else
    (void)req;
    res.status = 503;
    res.set_content(build_error_json("udp_unavailable", "UDP sender supported on Linux only"), "application/json");
#endif
  });

  svr.set_error_handler([](const httplib::Request &, httplib::Response &res) {
    if (res.status == 404) {
      res.set_content(build_error_json("not_found"), "application/json");
    } else {
      res.set_content(build_error_json("error"), "application/json");
    }
  });

  std::cout << "SilkCast server listening on " << cfg.addr << ":" << cfg.port
            << " (idle-timeout=" << cfg.idle_timeout << "s)" << std::endl;
  svr.listen(cfg.addr.c_str(), cfg.port);
  return 0;
}
