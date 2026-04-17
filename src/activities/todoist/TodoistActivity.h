#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <vector>

#include "../Activity.h"
#include "network/TodoistClient.h"
#include "util/ButtonNavigator.h"

class TodoistActivity final : public Activity {
 public:
  explicit TodoistActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Todoist", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class State { NO_API_KEY, NO_WIFI, SYNCING_PENDING, LOADING, LOADED, ERROR };
  State state = State::NO_API_KEY;

  std::vector<TodoistTask> tasks;
  int selectedIndex = 0;
  int scrollOffset = 0;

  // WiFi reconnect credentials (cached from WifiCredentialStore in onEnter)
  char reconnectSsid[64] = "";
  char reconnectPassword[64] = "";

  // Today's date in ISO format ("YYYY-MM-DD"), populated by worker via NTP.
  // Empty string if NTP sync was not available.
  char todayDateStr[11] = "";
  // Today's date formatted for the title ("Fri, Apr 17"). Empty if unavailable.
  char titleDateStr[20] = "";

  // Worker task (FreeRTOS) for network operations
  TaskHandle_t workerTaskHandle = nullptr;
  volatile bool workerDone = false;
  volatile bool workerSuccess = false;
  volatile bool workerNoWifi = false;

  ButtonNavigator buttonNavigator;

  static void workerTrampoline(void* param);
  void workerTask();

  void startWorker();
  void markTaskComplete();
  void adjustScroll(int availH, int lineH);

  // Write current framebuffer to /sleep.bmp (1-bit monochrome BMP)
  void saveSleepBmp() const;
};
