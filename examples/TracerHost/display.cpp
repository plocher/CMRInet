#include "display.h"

#include <string.h>

#include "RemoteNodeHandle.h"  // remoteNodeStateTag

bool TracerDisplay::begin() {
  Wire.begin(D4 /* SDA */, D5 /* SCL */);
  ok_ = display_.begin(SSD1306_SWITCHCAPVCC, kScreenAddr);
  if (ok_) {
    display_.dim(true);
  }
  return ok_;
}

void TracerDisplay::setAnnotation(int lineNumber, const char* text) {
  if (lineNumber == 1) {
    strncpy(line1_, text, sizeof(line1_) - 1);
    line1_[sizeof(line1_) - 1] = '\0';
  } else if (lineNumber == 2) {
    strncpy(line2_, text, sizeof(line2_) - 1);
    line2_[sizeof(line2_) - 1] = '\0';
  }
}

void TracerDisplay::render(CMRInet::CMRIHost& host, uint32_t nowMs) {
  if (!ok_) return;
  if (nowMs - lastRenderMs_ < kRefreshMs && lastRenderMs_ != 0) return;
  lastRenderMs_ = nowMs;

  uint8_t uas[kMaxRows] = {};
  size_t nRows = 0;
  for (unsigned ua = 0; ua <= 127u && nRows < kMaxRows; ++ua) {
    if (host.node(static_cast<uint8_t>(ua)) != nullptr)
      uas[nRows++] = static_cast<uint8_t>(ua);
  }

  const auto& hs = host.statistics();
  uint32_t nodeErrs[kMaxRows] = {};
  uint32_t nodeMisses[kMaxRows] = {};
  for (size_t i = 0; i < nRows; ++i) {
    CMRInet::RemoteNodeHandle* n = host.node(uas[i]);
    if (n != nullptr) {
      nodeErrs[i] = n->statistics().errors;
      nodeMisses[i] = n->statistics().noReplies;
    }
  }
  panel_.sample(nowMs, hs.pollsSent, hs.repliesAccepted, nodeErrs, nodeMisses,
                nRows > 0 ? nRows : 1);

  display_.clearDisplay();
  display_.setTextColor(SSD1306_WHITE);
  display_.setTextSize(2);
  display_.setCursor(0, 0);
  display_.print(F("TRC"));
  display_.setTextSize(1);
  char header[16];
  panel_.headerText(header, sizeof(header), nowMs);
  display_.setCursor(60, 4);
  display_.print(header);

  char totals[24];
  panel_.hostTotalsText(totals, sizeof(totals), nowMs);
  display_.setCursor(0, 18);
  display_.print(totals);

  for (size_t i = 0; i < nRows; ++i) {
    CMRInet::RemoteNodeHandle* node = host.node(uas[i]);
    const bool online =
        (node != nullptr) &&
        (node->state() == CMRInet::RemoteNodeState::kOnline);
    const char* tag =
        (node != nullptr) ? CMRInet::remoteNodeStateTag(node->state()) : "---";
    const uint32_t latMs =
        (node != nullptr) ? node->statistics().lastTurnaroundMs : 0;
    char row[28];
    panel_.nodeRowText(row, sizeof(row), nowMs, i, uas[i], online, tag, latMs);
    display_.setTextSize(1);
    display_.setCursor(0, 30 + static_cast<int>(i) * 10);
    display_.print(row);
  }

  if (line1_[0] != '\0') {
    display_.setCursor(0, 48);
    display_.print(line1_);
  }
  if (line2_[0] != '\0') {
    display_.setCursor(0, 56);
    display_.print(line2_);
  }

  flush_.markDirty();
}

void TracerDisplay::service() {
  if (!ok_) return;
  flush_.service();
}
