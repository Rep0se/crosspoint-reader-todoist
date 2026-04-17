#include "TodoistActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <cstdlib>
#include <cstring>
#include <ctime>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "WifiCredentialStore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/TodoistClient.h"
#include "todoist/TodoistPendingStore.h"

// Vertical padding above/below the text block inside each row
static constexpr int ROW_V_PAD = 5;
// Gap between content line and due-date line within a 2-line row
static constexpr int ROW_LINE_GAP = 3;
// Left/right padding inside the task list area
static constexpr int LIST_SIDE_PADDING = 16;

// Height of a row given whether it has a due-date line.
// lineH should come from renderer.getLineHeight(UI_12_FONT_ID).
static int rowHeightFor(bool hasDue, int lineH) {
  return hasDue ? (2 * lineH + 2 * ROW_V_PAD + ROW_LINE_GAP) : (lineH + 2 * ROW_V_PAD);
}

// Truncate text to fit within availW pixels, appending "..." if needed.
// Uses a static internal buffer — safe because render() is single-threaded.
static void truncateLine(GfxRenderer& renderer, int fontId, int availW, const char* text,
                         char* out, int sz) {
  if (renderer.getTextWidth(fontId, text) <= availW) {
    strncpy(out, text, sz - 1);
    out[sz - 1] = '\0';
    return;
  }
  static char tmp[128];
  const int ellipsisW = renderer.getTextWidth(fontId, "...");
  const int targetW = availW - ellipsisW;
  int len = static_cast<int>(strlen(text));
  // Walk back to last word boundary that fits, falling back to char truncation
  while (len > 0) {
    const int copyLen = std::min(len, static_cast<int>(sizeof(tmp)) - 1);
    memcpy(tmp, text, copyLen);
    tmp[copyLen] = '\0';
    if (renderer.getTextWidth(fontId, tmp) <= targetW) break;
    // Try previous word boundary first
    int prev = len - 1;
    while (prev > 0 && text[prev] != ' ') prev--;
    len = (prev > 0) ? prev : len - 1;
  }
  snprintf(out, sz, "%.*s...", len, text);
}

// Parse SETTINGS.tzOffsetStr ("-12" to "+14", empty = 0) and clamp to valid range.
static int parseTzOffsetHours() {
  const char* s = SETTINGS.tzOffsetStr;
  if (!s || s[0] == '\0') return 0;
  const int v = static_cast<int>(strtol(s, nullptr, 10));
  return (v < -12) ? -12 : (v > 14) ? 14 : v;
}

static constexpr const char* WEEKDAY_ABBR[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static constexpr const char* MONTH_ABBR[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                              "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

// Populate todayDateStr ("YYYY-MM-DD") and titleDateStr ("Fri, Apr 17") from the
// current local time (TZ set via configTime). Returns false if time is not yet set.
static bool computeTodayStrings(char* todayOut, int todaySz, char* titleOut, int titleSz) {
  const time_t now = time(nullptr);
  if (now < 86400) return false;  // NTP not synced yet

  struct tm t;
  localtime_r(&now, &t);

  snprintf(todayOut, todaySz, "%04d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
  snprintf(titleOut, titleSz, "%s, %s %d", WEEKDAY_ABBR[t.tm_wday], MONTH_ABBR[t.tm_mon], t.tm_mday);
  return true;
}

// Given a due date string ("YYYY-MM-DD") and today's ISO string, return a display
// label: "Today", "Yesterday", "Tomorrow", or "Apr 17".
// out must be at least 16 bytes.
static void formatDueLabel(const char* dueDate, const char* todayDateStr, char* out, int sz) {
  if (!dueDate || dueDate[0] == '\0') {
    out[0] = '\0';
    return;
  }

  if (todayDateStr[0] != '\0') {
    if (strcmp(dueDate, todayDateStr) == 0) {
      strncpy(out, "Today", sz - 1);
      out[sz - 1] = '\0';
      return;
    }

    // Compute yesterday and tomorrow by offsetting epoch ±86400 s
    int y = 0, m = 0, d = 0;
    if (sscanf(todayDateStr, "%d-%d-%d", &y, &m, &d) == 3) {
      struct tm base = {};
      base.tm_year = y - 1900;
      base.tm_mon = m - 1;
      base.tm_mday = d;
      base.tm_isdst = -1;
      const time_t baseEpoch = mktime(&base);

      char shifted[11];
      for (int offset : {-1, 1}) {
        struct tm st;
        const time_t se = baseEpoch + offset * 86400;
        localtime_r(&se, &st);
        snprintf(shifted, sizeof(shifted), "%04d-%02d-%02d",
                 st.tm_year + 1900, st.tm_mon + 1, st.tm_mday);
        if (strcmp(dueDate, shifted) == 0) {
          strncpy(out, offset == -1 ? "Yesterday" : "Tomorrow", sz - 1);
          out[sz - 1] = '\0';
          return;
        }
      }
    }
  }

  // Fall back to "Apr 17" style
  int y = 0, m = 0, d = 0;
  if (sscanf(dueDate, "%d-%d-%d", &y, &m, &d) == 3 && m >= 1 && m <= 12) {
    snprintf(out, sz, "%s %d", MONTH_ABBR[m - 1], d);
  } else {
    strncpy(out, dueDate, sz - 1);
    out[sz - 1] = '\0';
  }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void TodoistActivity::onEnter() {
  Activity::onEnter();

  tasks.reserve(TodoistClient::MAX_TASKS);
  TODOIST_PENDING.loadFromFile();

  if (strlen(SETTINGS.todoistApiKey) == 0) {
    state = State::NO_API_KEY;
    requestUpdate();
    return;
  }

  // Cache WiFi credentials so the worker can reconnect if the web server
  // left WiFi in WIFI_OFF mode (CrossPointWebServerActivity::onExit does this).
  reconnectSsid[0] = '\0';
  reconnectPassword[0] = '\0';
  if (WiFi.status() != WL_CONNECTED) {
    RenderLock lock(*this);
    WIFI_STORE.loadFromFile();
    const std::string& lastSsid = WIFI_STORE.getLastConnectedSsid();
    if (!lastSsid.empty()) {
      const auto* cred = WIFI_STORE.findCredential(lastSsid);
      if (cred) {
        strncpy(reconnectSsid, cred->ssid.c_str(), sizeof(reconnectSsid) - 1);
        reconnectSsid[sizeof(reconnectSsid) - 1] = '\0';
        strncpy(reconnectPassword, cred->password.c_str(), sizeof(reconnectPassword) - 1);
        reconnectPassword[sizeof(reconnectPassword) - 1] = '\0';
      }
    }
  }

  workerNoWifi = false;
  todayDateStr[0] = '\0';
  titleDateStr[0] = '\0';
  if (TODOIST_PENDING.hasPending()) {
    state = State::SYNCING_PENDING;
  } else {
    state = State::LOADING;
  }
  requestUpdate();
  startWorker();
}

void TodoistActivity::onExit() {
  if (workerTaskHandle != nullptr) {
    if (!workerDone) {
      vTaskDelete(workerTaskHandle);
    }
    workerTaskHandle = nullptr;
  }

  if (state == State::LOADED && !tasks.empty()) {
    saveSleepBmp();
  }

  Activity::onExit();
}

// ---------------------------------------------------------------------------
// Worker task (runs on FreeRTOS task, NOT the main task)
// ---------------------------------------------------------------------------

void TodoistActivity::workerTrampoline(void* param) {
  auto* self = static_cast<TodoistActivity*>(param);
  self->workerTask();
  vTaskDelete(nullptr);
}

void TodoistActivity::startWorker() {
  workerDone = false;
  workerSuccess = false;
  xTaskCreate(&workerTrampoline, "TodoistWorker", 4096, this, 1, &workerTaskHandle);
}

void TodoistActivity::workerTask() {
  // Reconnect WiFi if needed (web server activity leaves WiFi_OFF on exit)
  if (WiFi.status() != WL_CONNECTED && reconnectSsid[0] != '\0') {
    LOG_DBG("TODO", "WiFi off, attempting reconnect to %s", reconnectSsid);
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    if (reconnectPassword[0] != '\0') {
      WiFi.begin(reconnectSsid, reconnectPassword);
    } else {
      WiFi.begin(reconnectSsid);
    }
    // Wait up to 15 s for association
    for (int i = 0; i < 150 && WiFi.status() != WL_CONNECTED; i++) {
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }

  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR("TODO", "No WiFi after reconnect attempt");
    workerNoWifi = true;
    workerDone = true;
    return;
  }

  // Apply user TZ offset and sync NTP if the clock has not been set yet.
  // Snapshot clock validity BEFORE calling configTime() — configTime() resets
  // the SNTP state and briefly makes time() return 0, so checking after the
  // call would always trigger the NTP wait even when the clock was already set.
  {
    const bool clockWasSet = (time(nullptr) >= 86400);
    const long tzSec = static_cast<long>(parseTzOffsetHours()) * 3600L;
    configTime(tzSec, 0, "pool.ntp.org");
    if (!clockWasSet) {
      LOG_DBG("TODO", "Syncing time via NTP...");
      for (int i = 0; i < 100 && time(nullptr) < 86400; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
      }
      if (time(nullptr) < 86400) {
        LOG_ERR("TODO", "NTP sync timed out after 10s");
      }
    }
  }
  computeTodayStrings(todayDateStr, sizeof(todayDateStr), titleDateStr, sizeof(titleDateStr));
  LOG_DBG("TODO", "Today: %s", todayDateStr);

  bool ok = true;

  // Sync any pending offline completions first
  if (state == State::SYNCING_PENDING) {
    const auto& pending = TODOIST_PENDING.getPending();
    // Copy IDs: pendingIds may be mutated during removePending()
    std::vector<std::string> toSync(pending.begin(), pending.end());
    for (const auto& id : toSync) {
      if (TodoistClient::completeTask(SETTINGS.todoistApiKey, id.c_str())) {
        TODOIST_PENDING.removePending(id.c_str());
      } else {
        LOG_ERR("TODO", "Sync failed for task %s, keeping in queue", id.c_str());
        ok = false;
      }
    }
  }

  // Fetch fresh task list (even if some syncs failed)
  const bool fetched = TodoistClient::fetchTodaysTasks(SETTINGS.todoistApiKey, tasks);
  workerSuccess = fetched;
  workerDone = true;
}

// ---------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------

void TodoistActivity::loop() {
  // Handle worker completion
  if (workerDone && workerTaskHandle != nullptr) {
    workerTaskHandle = nullptr;
    if (workerNoWifi) {
      state = State::NO_WIFI;
    } else {
      state = workerSuccess ? State::LOADED : State::ERROR;
    }
    // Last-chance date computation in case NTP resolved after workerTask returned
    if (todayDateStr[0] == '\0') {
      computeTodayStrings(todayDateStr, sizeof(todayDateStr), titleDateStr, sizeof(titleDateStr));
    }
    selectedIndex = 0;
    scrollOffset = 0;
    requestUpdate();
    return;
  }

  if (state != State::LOADED) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finish();
    }
    return;
  }

  // Task list navigation
  const int count = static_cast<int>(tasks.size());
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentBottom = renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int availH = contentBottom - contentTop;
  const int lineH = renderer.getLineHeight(UI_12_FONT_ID);

  buttonNavigator.onNext([this, count, availH, lineH] {
    if (selectedIndex < count - 1) {
      selectedIndex++;
      adjustScroll(availH, lineH);
      requestUpdate();
    }
  });

  buttonNavigator.onPrevious([this, availH, lineH] {
    if (selectedIndex > 0) {
      selectedIndex--;
      adjustScroll(availH, lineH);
      requestUpdate();
    }
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    markTaskComplete();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
  }
}

void TodoistActivity::adjustScroll(int availH, int lineH) {
  // Scroll up if selection moved above the window
  if (selectedIndex < scrollOffset) {
    scrollOffset = selectedIndex;
    return;
  }
  // Check if selectedIndex is already visible
  int cumH = 0;
  for (int i = scrollOffset; i < static_cast<int>(tasks.size()); i++) {
    const int rh = rowHeightFor(tasks[i].due[0] != '\0', lineH);
    if (cumH + rh > availH) break;
    cumH += rh;
    if (i == selectedIndex) return;  // visible, no adjustment needed
  }
  // Advance scrollOffset until selectedIndex fits
  while (scrollOffset < selectedIndex) {
    scrollOffset++;
    cumH = 0;
    for (int i = scrollOffset; i < static_cast<int>(tasks.size()); i++) {
      const int rh = rowHeightFor(tasks[i].due[0] != '\0', lineH);
      if (cumH + rh > availH) break;
      cumH += rh;
      if (i == selectedIndex) return;
    }
  }
}

void TodoistActivity::markTaskComplete() {
  if (tasks.empty()) return;
  const int idx = selectedIndex;
  if (idx < 0 || idx >= static_cast<int>(tasks.size())) return;

  const char* taskId = tasks[idx].id;

  if (WiFi.status() == WL_CONNECTED) {
    // Complete immediately online
    if (TodoistClient::completeTask(SETTINGS.todoistApiKey, taskId)) {
      tasks.erase(tasks.begin() + idx);
      if (selectedIndex >= static_cast<int>(tasks.size()) && selectedIndex > 0) {
        selectedIndex--;
      }
      requestUpdate();
    } else {
      LOG_ERR("TODO", "Online complete failed, queuing offline");
      TODOIST_PENDING.addPending(taskId);
      tasks.erase(tasks.begin() + idx);
      if (selectedIndex >= static_cast<int>(tasks.size()) && selectedIndex > 0) {
        selectedIndex--;
      }
      requestUpdate();
    }
  } else {
    // Queue for later sync
    TODOIST_PENDING.addPending(taskId);
    tasks.erase(tasks.begin() + idx);
    if (selectedIndex >= static_cast<int>(tasks.size()) && selectedIndex > 0) {
      selectedIndex--;
    }
    requestUpdate();
  }
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void TodoistActivity::render(RenderLock&&) {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  renderer.clearScreen();

  // Build title: "Todoist  ·  Fri, Apr 17" when date is available, else just "Todoist"
  char titleBuf[48];
  if (titleDateStr[0] != '\0') {
    snprintf(titleBuf, sizeof(titleBuf), "%s  ·  %s", tr(STR_TODOIST), titleDateStr);
  } else {
    strncpy(titleBuf, tr(STR_TODOIST), sizeof(titleBuf) - 1);
    titleBuf[sizeof(titleBuf) - 1] = '\0';
  }
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, titleBuf);

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int lineH = renderer.getLineHeight(UI_12_FONT_ID);
  const int availW = pageWidth - LIST_SIDE_PADDING * 2 - 8;

  if (state == State::NO_API_KEY) {
    renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2, tr(STR_TODOIST_NO_API_KEY));
  } else if (state == State::NO_WIFI) {
    renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2, tr(STR_TODOIST_NO_WIFI));
  } else if (state == State::SYNCING_PENDING) {
    GUI.drawPopup(renderer, tr(STR_TODOIST_SYNCING));
  } else if (state == State::LOADING) {
    GUI.drawPopup(renderer, tr(STR_TODOIST_LOADING));
  } else if (state == State::ERROR) {
    renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2, tr(STR_TODOIST_ERROR));
  } else if (state == State::LOADED) {
    if (tasks.empty()) {
      renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2, tr(STR_TODOIST_NO_TASKS));
    } else {
      const int count = static_cast<int>(tasks.size());
      const int textX = LIST_SIDE_PADDING + 4;
      static char contentLine[128];
      char dueLabel[16];
      int rowY = contentTop;

      for (int i = scrollOffset; i < count; i++) {
        formatDueLabel(tasks[i].due, todayDateStr, dueLabel, sizeof(dueLabel));
        const bool hasDue = dueLabel[0] != '\0';
        const int rh = rowHeightFor(hasDue, lineH);

        if (rowY + rh > contentBottom) break;

        const bool selected = (i == selectedIndex);

        // Black fill for selected row (stop 1px short so separator remains visible)
        if (selected) {
          renderer.fillRect(LIST_SIDE_PADDING, rowY, pageWidth - LIST_SIDE_PADDING * 2, rh - 1, true);
        }

        // Content line (truncated to 1 line)
        truncateLine(renderer, UI_12_FONT_ID, availW, tasks[i].content, contentLine, sizeof(contentLine));
        renderer.drawText(UI_12_FONT_ID, textX, rowY + ROW_V_PAD, contentLine, !selected);

        // Due-date line in italic UI_10, if present
        if (hasDue) {
          renderer.drawText(UI_10_FONT_ID, textX, rowY + ROW_V_PAD + lineH + ROW_LINE_GAP, dueLabel,
                            !selected, EpdFontFamily::ITALIC);
        }

        // Separator line at bottom of row
        renderer.drawLine(LIST_SIDE_PADDING, rowY + rh - 1,
                          pageWidth - LIST_SIDE_PADDING, rowY + rh - 1, true);

        rowY += rh;
      }
    }
  }

  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), state == State::LOADED ? tr(STR_DONE) : "", tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

// ---------------------------------------------------------------------------
// BMP save
// ---------------------------------------------------------------------------

void TodoistActivity::saveSleepBmp() const {
  const uint8_t* fb = renderer.getFrameBuffer();
  if (!fb) {
    LOG_ERR("TODO", "No framebuffer to save as sleep BMP");
    return;
  }

  // Use LOGICAL dimensions for the BMP so SleepActivity can display it correctly.
  // The raw framebuffer is in PHYSICAL layout (panelWidth × panelHeight), so we must
  // re-extract pixels by mapping each logical (lx, ly) to its physical address.
  const int logW = renderer.getScreenWidth();
  const int logH = renderer.getScreenHeight();
  const int physW = static_cast<int>(renderer.getDisplayWidth());
  const int physH = static_cast<int>(renderer.getDisplayHeight());
  const int physWBytes = static_cast<int>(renderer.getDisplayWidthBytes());
  const int rowBytes = (logW + 7) / 8;
  const uint32_t pixelDataSize = static_cast<uint32_t>(rowBytes) * static_cast<uint32_t>(logH);
  // Keep width/height aliases for the header-writing code below
  const int width = logW;
  const int height = logH;

  static constexpr uint32_t PIXEL_OFFSET = 14u + 40u + 8u;  // file hdr + info hdr + 2-colour table
  const uint32_t fileSize = PIXEL_OFFSET + pixelDataSize;

  FsFile file;
  if (!Storage.openFileForWrite("TODO", "/sleep.bmp", file)) {
    LOG_ERR("TODO", "Failed to open /sleep.bmp for write");
    return;
  }

  // BITMAPFILEHEADER (14 bytes) — little-endian
  const uint8_t fileHeader[14] = {
      'B',
      'M',
      static_cast<uint8_t>(fileSize),
      static_cast<uint8_t>(fileSize >> 8),
      static_cast<uint8_t>(fileSize >> 16),
      static_cast<uint8_t>(fileSize >> 24),
      0,
      0,
      0,
      0,
      static_cast<uint8_t>(PIXEL_OFFSET),
      static_cast<uint8_t>(PIXEL_OFFSET >> 8),
      static_cast<uint8_t>(PIXEL_OFFSET >> 16),
      static_cast<uint8_t>(PIXEL_OFFSET >> 24),
  };
  file.write(fileHeader, sizeof(fileHeader));

  // BITMAPINFOHEADER (40 bytes) — little-endian, negative height = top-down
  const int32_t negHeight = -height;
  const uint8_t infoHeader[40] = {
      40,  0,  0,  0,  // biSize
      static_cast<uint8_t>(width),        static_cast<uint8_t>(width >> 8),
      static_cast<uint8_t>(width >> 16),  static_cast<uint8_t>(width >> 24),   // biWidth
      static_cast<uint8_t>(negHeight),    static_cast<uint8_t>(negHeight >> 8),
      static_cast<uint8_t>(negHeight >> 16), static_cast<uint8_t>(negHeight >> 24), // biHeight (neg)
      1,   0,                 // biPlanes = 1
      1,   0,                 // biBitCount = 1
      0,   0,   0,   0,       // biCompression = 0 (BI_RGB)
      0,   0,   0,   0,       // biSizeImage = 0 (valid for BI_RGB)
      0,   0,   0,   0,       // biXPelsPerMeter
      0,   0,   0,   0,       // biYPelsPerMeter
      2,   0,   0,   0,       // biClrUsed = 2
      2,   0,   0,   0,       // biClrImportant = 2
  };
  file.write(infoHeader, sizeof(infoHeader));

  // Colour table (8 bytes, BGRA): index 0 = black, index 1 = white
  // Framebuffer: bit=0 → black pixel, bit=1 → white pixel — matches BMP index convention
  const uint8_t colorTable[8] = {
      0x00, 0x00, 0x00, 0x00,  // index 0: black
      0xFF, 0xFF, 0xFF, 0x00,  // index 1: white
  };
  file.write(colorTable, sizeof(colorTable));

  // Pixel data — re-extract in logical order, transforming each logical pixel to
  // its physical framebuffer location using the inverse of rotateCoordinates().
  // This corrects for the 90° rotation applied when the screen is in Portrait mode.
  static uint8_t rowBuf[128];  // max row: 100 bytes (800px) — static to avoid stack pressure
  const GfxRenderer::Orientation orient = renderer.getOrientation();

  for (int ly = 0; ly < logH; ly++) {
    memset(rowBuf, 0, rowBytes);

    for (int lx = 0; lx < logW; lx++) {
      int phyX = 0;
      int phyY = 0;
      switch (orient) {
        case GfxRenderer::Portrait:
          phyX = ly;
          phyY = physH - 1 - lx;
          break;
        case GfxRenderer::PortraitInverted:
          phyX = physW - 1 - ly;
          phyY = lx;
          break;
        case GfxRenderer::LandscapeClockwise:
          phyX = physW - 1 - lx;
          phyY = physH - 1 - ly;
          break;
        case GfxRenderer::LandscapeCounterClockwise:
          phyX = lx;
          phyY = ly;
          break;
      }

      // Read pixel from physical framebuffer (bit=1 → white, bit=0 → black)
      const int physByteIdx = phyY * physWBytes + phyX / 8;
      const bool isWhite = (fb[physByteIdx] >> (7 - (phyX % 8))) & 1;

      // Write to BMP row buffer (same polarity: bit=1 → white)
      if (isWhite) {
        rowBuf[lx / 8] |= static_cast<uint8_t>(1 << (7 - (lx % 8)));
      }
    }

    file.write(rowBuf, rowBytes);
  }

  file.close();
  LOG_INF("TODO", "Saved sleep BMP (%d x %d, %lu bytes)", width, height,
          static_cast<unsigned long>(fileSize));
}
