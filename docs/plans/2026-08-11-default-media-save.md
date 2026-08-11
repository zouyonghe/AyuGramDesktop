# Default Media Save Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add a direct `Save` action after `Save As` for media viewer images and videos.

**Architecture:** Reuse `OverlayWidget::downloadMedia`, adding an explicit bypass flag for the per-file path prompt. The new context-menu action will call this direct-save path; existing download callers and `Save As` behavior remain unchanged.

**Tech Stack:** C++17/20, Qt, Telegram Desktop UI menu and settings APIs.

---

### Task 1: Add direct-save context-menu action

**Files:**
- Modify: `Telegram/SourceFiles/media/view/media_view_overlay_widget.cpp:2055-2228, 3290-3300`

**Step 1: Verify existing behavior surface**

Confirm `fillContextMenuActions` currently registers only `Save As`, and
`downloadMedia` redirects to `saveAs` when `askDownloadPath` is enabled.

**Step 2: Implement minimal behavior change**

- Add `bool askPath = true` or equivalent parameter to `downloadMedia`.
- Keep default parameter behavior unchanged for existing callers.
- Guard the `askDownloadPath()` redirect with that parameter.
- Register `Save` immediately after `Save As`, calling direct download with the
  bypass enabled.
- Reuse existing download icon and localization where possible; do not add
  localization or settings fields unless existing generated keys cannot express
  the action.

**Step 3: Inspect diff**

Run:

```bash
git diff --check
git diff -- Telegram/SourceFiles/media/view/media_view_overlay_widget.cpp
```

Expected: only context-menu registration and direct-download prompt routing
change, with no whitespace errors.

**Step 4: Run focused static validation**

Run available repository formatting or lint checks targeting the modified file.
Do not build the project unless explicitly requested.

**Step 5: Review behavior paths**

Verify from source that image, video, and document saves still use configured
download path resolution, while `Save As` still reaches file-dialog APIs.

### Task 2: Final review

**Files:**
- Review: `docs/plans/2026-08-11-default-media-save-design.md`
- Review: `docs/plans/2026-08-11-default-media-save.md`
- Review: `Telegram/SourceFiles/media/view/media_view_overlay_widget.cpp`

**Step 1: Check worktree scope**

Run:

```bash
git status --short
git diff --stat
```

Expected: only design/plan documentation and media viewer implementation are
modified.

**Step 2: Check semantic diff**

Ensure no new serialization field, unrelated menu behavior, or change to
automatic downloads was introduced.
