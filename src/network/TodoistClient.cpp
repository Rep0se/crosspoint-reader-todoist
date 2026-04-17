#include "TodoistClient.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Logging.h>
#include <NetworkClientSecure.h>
#include <WiFi.h>

#include <cstring>
#include <memory>

constexpr char TodoistClient::TASKS_URL[];
constexpr char TodoistClient::COMPLETE_URL_BASE[];

bool TodoistClient::fetchTodaysTasks(const char* apiKey, std::vector<TodoistTask>& outTasks) {
  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR("TODO", "fetchTodaysTasks: no WiFi");
    return false;
  }

  auto* secureClient = new NetworkClientSecure();
  secureClient->setInsecure();
  std::unique_ptr<NetworkClientSecure> client(secureClient);

  HTTPClient http;
  http.setTimeout(15000);
  http.begin(*client, TASKS_URL);

  char authHeader[160];
  snprintf(authHeader, sizeof(authHeader), "Bearer %s", apiKey);
  http.addHeader("Authorization", authHeader);
  http.addHeader("User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    LOG_ERR("TODO", "Fetch failed: HTTP %d", code);
    http.end();
    return false;
  }

  // Use a filter document to extract only id, content, and due.string, minimising heap use.
  // API v1 wraps results: {"results": [...], "next_cursor": null}
  JsonDocument filter;
  filter["results"][0]["id"] = true;
  filter["results"][0]["content"] = true;
  filter["results"][0]["due"]["date"] = true;

  JsonDocument doc;
  const DeserializationError err =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();

  if (err) {
    LOG_ERR("TODO", "JSON parse error: %s", err.c_str());
    return false;
  }

  outTasks.clear();
  for (JsonObject task : doc["results"].as<JsonArray>()) {
    if (static_cast<int>(outTasks.size()) >= MAX_TASKS) break;

    const char* id = task["id"].as<const char*>();
    const char* content = task["content"].as<const char*>();
    if (!id || !content) continue;

    TodoistTask t;
    strncpy(t.id, id, sizeof(t.id) - 1);
    t.id[sizeof(t.id) - 1] = '\0';
    strncpy(t.content, content, sizeof(t.content) - 1);
    t.content[sizeof(t.content) - 1] = '\0';

    const char* dueDate = task["due"]["date"].as<const char*>();
    if (dueDate) {
      strncpy(t.due, dueDate, sizeof(t.due) - 1);
      t.due[sizeof(t.due) - 1] = '\0';
    } else {
      t.due[0] = '\0';
    }

    outTasks.push_back(t);
  }

  LOG_INF("TODO", "Fetched %d task(s)", static_cast<int>(outTasks.size()));
  return true;
}

bool TodoistClient::completeTask(const char* apiKey, const char* taskId) {
  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR("TODO", "completeTask: no WiFi");
    return false;
  }

  auto* secureClient = new NetworkClientSecure();
  secureClient->setInsecure();
  std::unique_ptr<NetworkClientSecure> client(secureClient);

  HTTPClient http;
  char url[96];
  snprintf(url, sizeof(url), "%s%s/close", COMPLETE_URL_BASE, taskId);
  http.begin(*client, url);

  char authHeader[160];
  snprintf(authHeader, sizeof(authHeader), "Bearer %s", apiKey);
  http.addHeader("Authorization", authHeader);
  http.addHeader("Content-Length", "0");
  http.addHeader("User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);

  const int code = http.POST("");
  http.end();

  if (code == 204) {
    LOG_INF("TODO", "Task %s marked complete", taskId);
    return true;
  }
  LOG_ERR("TODO", "Complete failed: HTTP %d for task %s", code, taskId);
  return false;
}
