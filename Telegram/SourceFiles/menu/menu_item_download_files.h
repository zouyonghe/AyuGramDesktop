/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/data_types.h"

class HistoryInner;
class HistoryItem;

namespace Ui {
class PopupMenu;
} // namespace Ui

namespace HistoryView {
class ListWidget;
struct SelectedItem;
} // namespace HistoryView

namespace Main {
class Session;
} // namespace Main

namespace Window {
class SessionController;
} // namespace Window

namespace Menu {

struct BatchDownloadFile {
	QString path;
	bool completed = false;
	std::optional<MediaKey> mediaKey;
};

using BatchDownloadFiles = base::flat_map<FullMsgId, BatchDownloadFile>;

[[nodiscard]] BatchDownloadFiles BatchDownloadFilesFor(
	not_null<Main::Session*> session);
[[nodiscard]] bool MigrateBatchDownloadFileMediaKey(
	not_null<Main::Session*> session,
	FullMsgId id,
	MediaKey mediaKey);
[[nodiscard]] bool MigrateBatchDownloadFileMediaKeys(
	not_null<Main::Session*> session,
	const std::vector<std::pair<FullMsgId, MediaKey>> &migrations);
void ForgetBatchDownloadFile(
	not_null<Main::Session*> session,
	FullMsgId id);
void ForgetBatchDownloadFiles(
	not_null<Main::Session*> session,
	const std::vector<FullMsgId> &ids);

[[nodiscard]] bool DownloadSelectedFiles(
	not_null<Window::SessionController*> window,
	const std::vector<not_null<HistoryItem*>> &items,
	Fn<void()> callback = nullptr,
	bool forceDefaultPath = false,
	Fn<void(not_null<Main::Session*>, FullMsgId, QString)> destination = nullptr,
	Fn<void(not_null<Main::Session*>, FullMsgId)> saved = nullptr,
	Fn<void(not_null<Main::Session*>, FullMsgId)> failed = nullptr);

void AddDownloadFilesAction(
	not_null<Ui::PopupMenu*> menu,
	not_null<Window::SessionController*> window,
	const std::vector<HistoryView::SelectedItem> &selectedItems,
	not_null<HistoryView::ListWidget*> list);

void AddDownloadFilesAction(
	not_null<Ui::PopupMenu*> menu,
	not_null<Window::SessionController*> window,
	const std::vector<not_null<HistoryItem*>> &items,
	not_null<HistoryInner*> list);

} // namespace Menu
