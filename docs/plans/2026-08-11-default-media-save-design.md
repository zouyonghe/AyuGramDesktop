# Default Media Save

## Goal

Let users save images and videos from the media viewer without choosing a
directory each time, while reusing the existing download path setting.

## Behavior

- Keep existing `Save As` context-menu action and directory picker behavior.
- Add `Save` immediately after `Save As` in the media viewer context menu.
- `Save` writes directly to the configured download path.
- `Save` bypasses the per-file path prompt.
- Existing default, temporary, and custom download path choices remain the
  source of the destination directory.
- Existing unique filename generation, download tracking, and save toast remain
  in use.

## Implementation

Extend media viewer's existing direct-download entry point with an explicit
option to bypass the `askDownloadPath` setting. The new context-menu action
passes that option; all other callers retain current behavior. No new setting,
serialization field, localization key, or storage migration is needed.

## Error Handling

Use current direct-download behavior when destination path is unavailable or
media is not loaded. Existing loading and failure behavior stays unchanged.

## Verification

- Inspect context-menu construction to confirm ordering and callbacks.
- Verify both image and video actions use configured download path.
- Verify `Save As` still opens file picker.
- Run focused formatting/static checks available without building the project.
