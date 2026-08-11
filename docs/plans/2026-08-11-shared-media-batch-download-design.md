# Shared Media Batch Download

## Goal

Add an opt-in batch-selection mode to chat and channel photo/video pages, with
direct batch download and per-item download status.

## Behavior

- Add a checkable `Batch selection` action to the photo/video page top-right
  menu.
- Keep the mode local to the current media page; do not persist it globally.
- While enabled, left-click keeps opening or playing media.
- While enabled, right-click toggles the item selection and does not open the
  context menu.
- Selected items use the existing shared-media selection visuals and count.
- Add a download button to the selection toolbar.
- Batch download always uses the configured default download directory and
  never opens a directory picker.
- Downloaded items remain selected until the user clears the selection.

## Download Status

Only items requested through the batch download action show a status badge:

- `Waiting` before transfer starts.
- `Downloading` while media data is loading.
- `Downloaded` after the file is saved.
- Items not requested for batch download show no badge.

Status changes repaint the affected media tiles. Existing media loaders and
session downloader completion notifications remain the source of transfer
state.

## Architecture

Reuse `Info::Media::ListWidget` selection storage and add a page-local
batch-selection flag. Route a new `SelectionAction::Download` from
`Info::TopBar` to `ListWidget`. Extract the existing batch media save operation
from `Menu::AddDownloadFilesAction` so both the chat selection menu and shared
media toolbar use one implementation, with shared media forcing the default
path.

## Verification

- Confirm normal left-click media activation remains unchanged.
- Confirm right-click toggles selection only while batch mode is enabled.
- Confirm disabling batch mode clears selection and restores context menus.
- Confirm download action never invokes folder selection.
- Confirm status badges only appear for requested items and transition through
  waiting, downloading, and downloaded states.
- Run formatting and static diff checks without building the project.
