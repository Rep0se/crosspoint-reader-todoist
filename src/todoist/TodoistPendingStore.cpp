#include "TodoistPendingStore.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

TodoistPendingStore& TodoistPendingStore::getInstance() {
  static TodoistPendingStore instance;
  return instance;
}

void TodoistPendingStore::addPending(const char* taskId) {
  // Avoid duplicates
  for (const auto& id : pendingIds) {
    if (id == taskId) return;
  }
  pendingIds.emplace_back(taskId);
  saveToFile();
}

void TodoistPendingStore::removePending(const char* taskId) {
  pendingIds.erase(std::remove(pendingIds.begin(), pendingIds.end(), std::string(taskId)), pendingIds.end());
  saveToFile();
}

bool TodoistPendingStore::loadFromFile() {
  FsFile file;
  if (!Storage.openFileForRead("TDPND", STORE_PATH, file)) {
    return false;
  }

  const size_t fileSize = file.fileSize();
  if (fileSize == 0 || fileSize > 4096) {
    file.close();
    return false;
  }

  auto* buf = static_cast<char*>(malloc(fileSize + 1));
  if (!buf) {
    LOG_ERR("TDPND", "malloc failed: %zu bytes", fileSize + 1);
    file.close();
    return false;
  }

  const size_t read = file.read(buf, fileSize);
  buf[read] = '\0';
  file.close();

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, buf);
  free(buf);

  if (err) {
    LOG_ERR("TDPND", "JSON parse error: %s", err.c_str());
    return false;
  }

  pendingIds.clear();
  for (JsonVariant v : doc["pending"].as<JsonArray>()) {
    const char* id = v.as<const char*>();
    if (id) pendingIds.emplace_back(id);
  }

  LOG_DBG("TDPND", "Loaded %d pending task(s)", static_cast<int>(pendingIds.size()));
  return true;
}

bool TodoistPendingStore::saveToFile() const {
  JsonDocument doc;
  JsonArray arr = doc["pending"].to<JsonArray>();
  for (const auto& id : pendingIds) {
    arr.add(id);
  }

  FsFile file;
  if (!Storage.openFileForWrite("TDPND", STORE_PATH, file)) {
    LOG_ERR("TDPND", "Failed to open pending store for write");
    return false;
  }

  serializeJson(doc, file);
  file.close();
  return true;
}
