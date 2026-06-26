#pragma once

#include <string>
#include <vector>

// Persists task IDs that were marked complete while offline.
// On next open with WiFi, TodoistActivity syncs these before fetching fresh tasks.
class TodoistPendingStore {
 public:
  static TodoistPendingStore& getInstance();

  TodoistPendingStore(const TodoistPendingStore&) = delete;
  TodoistPendingStore& operator=(const TodoistPendingStore&) = delete;

  // Add a task ID to the pending queue and persist immediately.
  void addPending(const char* taskId);

  // Remove a task ID after successful sync and persist immediately.
  void removePending(const char* taskId);

  const std::vector<std::string>& getPending() const { return pendingIds; }
  bool hasPending() const { return !pendingIds.empty(); }

  bool loadFromFile();
  bool saveToFile() const;

 private:
  static constexpr char STORE_PATH[] = "/.crosspoint/todoist_pending.json";

  std::vector<std::string> pendingIds;

  TodoistPendingStore() = default;
};

#define TODOIST_PENDING TodoistPendingStore::getInstance()
