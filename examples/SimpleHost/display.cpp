#include "display.h"

bool SimpleDisplay::begin() {
  Wire.begin(D4 /* SDA */, D5 /* SCL */);
  ok_ = display_.begin(SSD1306_SWITCHCAPVCC, kScreenAddr);
  if (ok_) {
    display_.dim(true);
  }
  return ok_;
}

void SimpleDisplay::render(CMRInet::CMRIHost& host,
                           const CMRInet::HostNodeSpec* nodeTable,
                           size_t nodeCount, uint32_t nowMs) {
  if (!ok_) return;
  if (nowMs - lastRenderMs_ < kRefreshMs && lastRenderMs_ != 0) return;
  lastRenderMs_ = nowMs;

  const auto& hs = host.statistics();
  uint32_t nodeErrs[CMRINET_HOST_MAX_NODES] = {};
  uint32_t nodeMisses[CMRINET_HOST_MAX_NODES] = {};
  for (size_t i = 0; i < nodeCount; ++i) {
    CMRInet::RemoteNodeHandle* n = host.node(nodeTable[i].UA);
    if (n != nullptr) {
      nodeErrs[i] = n->statistics().errors;
      nodeMisses[i] = n->statistics().noReplies;
    }
  }
  panel_.sample(nowMs, hs.pollsSent, hs.repliesAccepted, nodeErrs, nodeMisses,
                nodeCount);

  display_.clearDisplay();
  display_.setTextColor(SSD1306_WHITE);

  display_.setTextSize(2);
  display_.setCursor(0, 0);
  display_.print(F("HOST"));
  display_.setTextSize(1);
  char header[16];
  panel_.headerText(header, sizeof(header), nowMs);
  display_.setCursor(60, 4);
  display_.print(header);

  char totals[24];
  panel_.hostTotalsText(totals, sizeof(totals), nowMs);
  display_.setCursor(0, 18);
  display_.print(totals);

  for (size_t i = 0; i < nodeCount; ++i) {
    CMRInet::RemoteNodeHandle* n = host.node(nodeTable[i].UA);
    const bool online =
        (n != nullptr) && (n->state() == CMRInet::RemoteNodeState::kOnline);
    const char* tag =
        (n != nullptr) ? CMRInet::remoteNodeStateTag(n->state()) : "---";
    const uint32_t latMs =
        (n != nullptr) ? n->statistics().lastTurnaroundMs : 0;
    char row[28];
    panel_.nodeRowText(row, sizeof(row), nowMs, i, nodeTable[i].UA, online,
                       tag, latMs);
    display_.setCursor(0, 30 + static_cast<int>(i) * 12);
    display_.print(row);
  }

  flush_.markDirty();
}

void SimpleDisplay::showFatalError(const char* title, const char* detail) {
  if (!ok_) return;
  display_.clearDisplay();
  display_.setTextSize(1);
  display_.setTextColor(SSD1306_WHITE);
  display_.setCursor(0, 0);
  display_.print(title);
  display_.print(detail);
  flush_.markDirty();
  flush_.serviceUntilIdle();
}

void SimpleDisplay::service() {
  if (!ok_) return;
  flush_.service();
}
