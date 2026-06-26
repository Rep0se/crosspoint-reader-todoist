#pragma once

#include <vector>

struct TodoistTask {
  char id[24];       // Todoist task ID (numeric string)
  char content[128]; // Task label, truncated if longer
  char due[16];      // due.date from API ("YYYY-MM-DD"), "" if none
};

// Stateless HTTP client for the Todoist unified API v1.
// All methods require an active WiFi connection (WiFi.status() == WL_CONNECTED).
class TodoistClient {
 public:
  static constexpr int MAX_TASKS = 50;

  // Fetch today's incomplete tasks into outTasks.
  // outTasks must be pre-reserved. Returns false on network or parse error.
  static bool fetchTodaysTasks(const char* apiKey, std::vector<TodoistTask>& outTasks);

  // Mark a task complete via POST /close. Returns false on error.
  static bool completeTask(const char* apiKey, const char* taskId);

 private:
  // v1 dropped the `filter` param from /tasks; the today filter lives on a dedicated
  // endpoint and uses `query`, not `filter`. Plain /tasks?filter=today returns ALL
  // active tasks unfiltered.
  static constexpr char TASKS_URL[] = "https://api.todoist.com/api/v1/tasks/filter?query=today";
  static constexpr char COMPLETE_URL_BASE[] = "https://api.todoist.com/api/v1/tasks/";
};
