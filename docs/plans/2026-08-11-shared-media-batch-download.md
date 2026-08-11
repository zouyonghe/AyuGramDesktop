# Shared Media Batch Download Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add right-click batch selection and direct batch download to shared photo/video pages.

**Architecture:** Extend existing shared-media selection and top-bar action routing. Reuse existing batch-save code through a callable helper and track requested items in the media list for status painting.

**Tech Stack:** C++, Qt, Telegram Desktop reactive UI and media loading APIs.

---

### Task 1: Expose batch-selection mode

**Files:**
- Modify: `Telegram/SourceFiles/info/media/info_media_widget.cpp`
- Modify: `Telegram/SourceFiles/info/media/info_media_inner_widget.h`
- Modify: `Telegram/SourceFiles/info/media/info_media_inner_widget.cpp`
- Modify: `Telegram/SourceFiles/info/media/info_media_list_widget.h`
- Modify: `Telegram/SourceFiles/info/media/info_media_list_widget.cpp`

Add a checkable top-right menu action for photo/video types. Propagate its
page-local state to the list. In `contextMenuEvent`, toggle selection and accept
the event when enabled; otherwise retain the existing context menu.

### Task 2: Add top-bar download action

**Files:**
- Modify: `Telegram/SourceFiles/info/info_wrap_widget.h`
- Modify: `Telegram/SourceFiles/info/info_top_bar.h`
- Modify: `Telegram/SourceFiles/info/info_top_bar.cpp`
- Modify: `Telegram/SourceFiles/info/info.style`

Add `SelectionAction::Download`, create a download icon button in selection
controls, and route clicks through existing selection-action requests.

### Task 3: Reuse direct batch download

**Files:**
- Modify: `Telegram/SourceFiles/menu/menu_item_download_files.h`
- Modify: `Telegram/SourceFiles/menu/menu_item_download_files.cpp`
- Modify: `Telegram/SourceFiles/info/media/info_media_list_widget.cpp`

Expose the existing selected-media save operation as a callable helper. Allow
the shared-media call to force the configured default path and bypass folder
selection. Resolve selected IDs to history items and start the shared helper.

### Task 4: Paint download status

**Files:**
- Modify: `Telegram/SourceFiles/info/media/info_media_list_widget.h`
- Modify: `Telegram/SourceFiles/info/media/info_media_list_widget.cpp`
- Modify: `Telegram/SourceFiles/info/info.style`
- Modify: `Telegram/Resources/langs/lang.strings`

Track only batch-requested items. Paint scaled status badges over visible
tiles, update while loaders change, and mark completion from save callbacks or
session download completion events.

### Task 5: Verify

Run:

```bash
git diff --check
git status --short
```

Inspect all left-click, right-click, selection clear, direct-path, and status
transition paths. Do not build the project.
