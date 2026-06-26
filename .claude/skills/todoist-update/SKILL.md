---
name: todoist-update
description: Rebase the local `todoist` feature branch on top of an updated `master` (e.g. after pulling a new upstream CrossPoint release), resolve the recurring conflicts, fix build-time API drift, build, flash for the user to test, then squash to one commit and force-push the fork. Use whenever the user wants to update/rebase/refresh the todoist branch onto new master, sync the feature with a new CrossPoint release, or "bring todoist up to date".
---

# Update the Todoist branch from master

CrossPoint ships a new upstream release roughly monthly. Each time, the single
Todoist feature commit has to be replayed on top of the new `master`. The
conflicts are predictable: the Todoist commit *adds* to insertion points
(string tables, enums, switch/case lists, the home menu) that upstream also
edits, and upstream sometimes *renames or replaces* a type the Todoist code
uses. This skill captures the whole loop.

Companion doc: **`.claude/features/todoist-integration.md`** holds the
per-file "Must Survive Rebases" verification checklist. Use it as the source of
truth for *what* must be present; this skill is *how* to get there.

Do not bug-fix the feature here — this is maintenance only. If a real defect
surfaces, finish the rebase first, then handle it as a separate change.

## 0. Preflight

```bash
git fetch origin                       # or upstream, whichever tracks CrossPoint
git switch todoist
git log --oneline master..todoist      # confirm exactly the Todoist commit(s)
git status --short                     # tree must be clean (untracked is fine)
git branch todoist-backup-prerebase todoist   # safety net; delete at the end
```

Note the remote `todoist` SHA — you need it for the `--force-with-lease` at the
end: `git rev-parse origin/todoist`.

## 1. Rebase

```bash
git rebase master
```

Expect conflicts. Resolve them with the principle below, then
`git add <files>` and `GIT_EDITOR=true git rebase --continue`.

## 2. Resolving the recurring conflicts

Almost every conflict is a both-sides-added clash at an insertion point. The
rule is **keep both sides** — master's change *and* the Todoist addition — not
one or the other. Specific files seen every cycle:

- **`lib/I18n/translations/english.yaml`** — string table. Keep master's new
  `STR_*` *and* the Todoist `STR_TODOIST*`, `STR_TZ_OFFSET*`, `STR_CAT_ADDONS`,
  `STR_CUSTOM_COVER` keys. No duplicate keys (the i18n generator will choke).
- **`src/CrossPointSettings.h`** — the `SLEEP_SCREEN_MODE` enum. Both sides add
  a value at the same slot. **Renumber so values stay unique and
  `SLEEP_SCREEN_MODE_COUNT` stays last.** The Todoist `CUSTOM_COVER` index is
  *not fixed* — it shifts as upstream adds modes. (Last cycle: master added
  `QUICK_RESUME = 6`, so Todoist became `CUSTOM_COVER = 7`.)
- **`src/SettingsList.h`** — the sleep-screen `Enum(...)` option list maps
  strings to enum values **by position**, so its order must match the enum
  exactly. Append the Todoist option after master's. The Addons settings block
  (`todoistApiKey` obfuscated, `todoistSleepHint` label, `tzOffsetStr`,
  `tzOffsetHint`) usually auto-merges — verify it survived.
- **`src/components/themes/BaseTheme.h`** — the `UIIcon` enum. Keep master's
  additions and append `Todoist`.
- **`src/components/themes/lyra/LyraTheme.cpp`** — the `iconForName` switch.
  Keep both new `case`s; confirm `#include "components/icons/todoist.h"` is
  present.
- **`src/activities/boot_sleep/SleepActivity.h`** — keep master's members and
  the Todoist `tryRenderCustomBmp()` / `renderCustomCoverSleepScreen()` decls.
- **`src/network/html/SettingsPage.html`** — keep master's markup changes
  (e.g. row `id`) *inside* the Todoist `label`-type branch.
- **Home menu** (`src/activities/home/HomeActivity.cpp` + `.h`,
  `src/activities/ActivityManager.h`) — the hardest. Upstream owns the
  `HomeMenuItem` enum + `menuItemToIndex` / `indexToMenuItem` helpers and the
  dispatch. Todoist must be a first-class menu item:
  - Add `TODOIST` to `enum class HomeMenuItem`.
  - Make both helpers Todoist-aware (extra `bool hasTodoist` param, Todoist
    first — matching the order `render()` builds the menu in).
  - Dispatch through `indexToMenuItem(..., hasTodoistApiKey)` with a
    `case HomeMenuItem::TODOIST: onTodoistOpen();`.
  - The order the helpers encode **must** match the visible order in
    `render()` (Todoist inserted at front, after Continue-Reading when present).

After resolving, run the verification checklist in
`.claude/features/todoist-integration.md` (it includes integration points that
often auto-merge silently, e.g. `goToTodoist()` and the `SettingType::LABEL`
handling in `CrossPointWebServer.cpp`). If the indices in that doc no longer
match (they drift as upstream adds enum values), update the doc.

## 3. Sync the submodule

Master usually bumps the SDK pointer. After the rebase:

```bash
git submodule update --init --recursive
git status --short          # open-x4-sdk should no longer show as modified
```

Skipping this builds against a stale SDK and produces confusing errors.

## 4. Build, and fix API drift

macOS uses the PlatformIO penv binary, never Homebrew's (see
`.claude/build-environment.md`):

```bash
~/.platformio/penv/bin/pio run
```

Two failure classes recur:

- **JPEGDEC patch "does not apply cleanly".** The lib in `.pio/libdeps` is in a
  half-patched state. Wipe it and rebuild — it re-fetches pristine and the
  pre-build script (`scripts/patch_jpegdec.py`) patches cleanly:
  ```bash
  rm -rf .pio/libdeps/default/JPEGDEC && ~/.platformio/penv/bin/pio run
  ```
- **`'X' was not declared` in Todoist files.** Upstream refactored a type the
  Todoist code uses. Last cycle SD storage moved from raw SdFat `FsFile` to the
  pimpl `HalFile` wrapper (`lib/hal/HalStorage.h`), so `TodoistActivity.cpp` and
  `TodoistPendingStore.cpp` needed `FsFile` → `HalFile` (same method names:
  `write`/`read`/`fileSize`/`close`, and `HalFile : public Print` keeps
  `serializeJson(doc, file)` working). When this happens, find the new
  abstraction and port to it — **do not reach around the HAL.**

Iterate build → fix → build until clean. Sanity-check RAM/Flash in the size
summary (RAM well under the ~320 KB DRAM ceiling).

## 5. Flash and let the user test

```bash
~/.platformio/penv/bin/pio device list           # find the Espressif port (VID 303A)
~/.platformio/penv/bin/pio run -t upload --upload-port <PORT>
```

The ESP32-C3 USB-CDC port drops on reset/sleep; if upload fails with the port
missing, ask the user to wake/boot the device, then re-detect and retry.

Ask the user to verify on-device (this is human scope — you can't):
- Todoist activity opens, lists today's tasks, mark-complete works.
- Home menu items each open the right screen (you touched the dispatch).
- Custom sleep screen still renders (the `HalFile` port touches BMP save).

**Do not squash or push until the user confirms the test passes.**

## 6. Squash to one commit and force-push

Once confirmed, collapse everything since master into a single feature commit
(non-interactive — interactive rebase is unavailable in this environment):

```bash
git reset --soft master
git commit            # one clean "feat: Todoist integration ..." message
```

Force-push the fork, asserting the remote hasn't moved since preflight:

```bash
git push --force-with-lease=todoist:<remote-sha-from-preflight> origin todoist
```

`--force-with-lease` (not bare `--force`) refuses the push if someone else
updated the remote in the meantime.

## 7. Clean up

```bash
git branch -D todoist-backup-prerebase     # reflog still recovers if needed
```

## Self-review

- [ ] `git log --oneline master..todoist` shows exactly one commit.
- [ ] Every item in `.claude/features/todoist-integration.md`'s rebase
      checklist is present.
- [ ] No leftover conflict markers; no duplicate i18n keys.
- [ ] Sleep-screen enum values unique, `..._COUNT` last; option-list order in
      `SettingsList.h` matches the enum.
- [ ] Submodule synced; build clean on `~/.platformio/penv/bin/pio run`.
- [ ] User confirmed the on-device test before the force-push.
- [ ] Pushed with `--force-with-lease`; backup branch deleted.
