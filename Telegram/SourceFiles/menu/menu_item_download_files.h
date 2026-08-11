/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class HistoryInner;
class HistoryItem;

namespace Ui {
class PopupMenu;
} // namespace Ui

namespace HistoryView {
class ListWidget;
struct SelectedItem;
} // namespace HistoryView

namespace Window {
class SessionController;
} // namespace Window

namespace Menu {

[[nodiscard]] bool DownloadSelectedFiles(
	not_null<Window::SessionController*> window,
	const std::vector<not_null<HistoryItem*>> &items,
	Fn<void()> callback = nullptr,
	bool forceDefaultPath = false,
	Fn<void(FullMsgId, QString)> destination = nullptr,
	Fn<void(FullMsgId)> saved = nullptr);

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
