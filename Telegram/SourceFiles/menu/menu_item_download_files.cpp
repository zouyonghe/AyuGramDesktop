/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "menu/menu_item_download_files.h"

#include "base/base_file_utilities.h"
#include "base/unixtime.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "core/file_utilities.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_file_click_handler.h"
#include "data/data_peer_id.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_session.h"
#include "history/history_inner_widget.h"
#include "history/history_item.h"
#include "history/view/history_view_list_widget.h" // HistoryView::SelectedItem.
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "mainwindow.h"
#include "storage/storage_account.h"
#include "ui/text/text_utilities.h"
#include "ui/toast/toast.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_session_controller.h"
#include "window/window_controller.h"
#include "styles/style_chat_helpers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_widgets.h"

namespace Menu {
namespace {

using Documents = std::vector<std::pair<not_null<DocumentData*>, FullMsgId>>;
using Photos = std::vector<std::pair<not_null<PhotoData*>, FullMsgId>>;
using BatchDownloads = BatchDownloadFiles;

constexpr auto kBatchDownloadsPref = "batch_download_files";
constexpr auto kMaxBatchDownloads = 100000;

BatchDownloads ReadBatchDownloads(not_null<Main::Session*> session) {
	const auto serialized = session->local().readPref<QByteArray>(
		kBatchDownloadsPref);
	if (serialized.isEmpty()) {
		return {};
	}
	auto stream = QDataStream(serialized);
	stream.setVersion(QDataStream::Qt_5_1);
	auto count = quint32();
	stream >> count;
	if (count > kMaxBatchDownloads) {
		return {};
	}
	auto result = BatchDownloads();
	result.reserve(count);
	for (auto i = quint32(); i != count; ++i) {
		auto peer = quint64();
		auto message = qint64();
		auto path = QString();
		auto completed = quint8();
		stream >> peer >> message >> path >> completed;
		if (stream.status() != QDataStream::Ok) {
			return {};
		}
		if (completed) {
			result.emplace(
				FullMsgId(DeserializePeerId(peer), MsgId(message)),
				std::move(path));
		}
	}
	return result;
}

void WriteBatchDownloads(
		not_null<Main::Session*> session,
		const BatchDownloads &downloads) {
	auto serialized = QByteArray();
	{
		auto stream = QDataStream(&serialized, QIODevice::WriteOnly);
		stream.setVersion(QDataStream::Qt_5_1);
		stream << quint32(downloads.size());
		for (const auto &[id, path] : downloads) {
			stream
				<< SerializePeerId(id.peer)
				<< qint64(id.msg.bare)
				<< path
				<< quint8(1);
		}
	}
	session->local().writePref<QByteArray>(
		kBatchDownloadsPref,
		std::move(serialized));
}

void CompleteBatchDownloadFiles(
		not_null<Main::Session*> session,
		BatchDownloads completed) {
	auto downloads = ReadBatchDownloads(session);
	for (auto &[id, path] : completed) {
		downloads[id] = std::move(path);
	}
	WriteBatchDownloads(session, downloads);
}

void CompleteBatchDownloadFile(
		not_null<Main::Session*> session,
		FullMsgId id,
		QString path) {
	auto updates = BatchDownloads();
	updates.emplace(id, std::move(path));
	CompleteBatchDownloadFiles(session, std::move(updates));
}

QString DefaultDownloadPath(not_null<Main::Session*> session) {
	const auto configured = Core::App().settings().downloadPath();
	return configured.isEmpty()
		? File::DefaultDownloadPath(session)
		: (configured == FileDialog::Tmp())
		? session->local().tempDirectory()
		: configured;
}

bool DocumentSavedToPath(
		not_null<DocumentData*> document,
		const QString &path) {
	const auto info = QFileInfo(path);
	return info.exists()
		&& info.size() == document->size;
}

QString NumberedFilename(const QString &name, int index) {
	if (index == 1) {
		return name;
	}
	const auto extensionIndex = name.lastIndexOf('.');
	const auto prefix = (extensionIndex >= 0)
		? name.mid(0, extensionIndex)
		: name;
	const auto extension = (extensionIndex >= 0)
		? name.mid(extensionIndex)
		: QString();
	return prefix + u" (%1)"_q.arg(index) + extension;
}

[[nodiscard]] bool Added(
		HistoryItem *item,
		Documents &documents,
		Photos &photos) {
	if (item && !item->forbidsForward()) {
		if (const auto media = item->media()) {
			if (const auto photo = media->photo()) {
				photos.emplace_back(photo, item->fullId());
				return true;
			} else if (const auto document = media->document()) {
				documents.emplace_back(document, item->fullId());
				return true;
			}
		}
	}
	return false;
}

Fn<void()> PrepareDownloadAction(
		not_null<Window::SessionController*> controller,
		Documents &&documents,
		Photos &&photos,
		Fn<void()> callback,
		bool forceDefaultPath,
		Fn<void(not_null<Main::Session*>, FullMsgId, QString)> destination,
		Fn<void(not_null<Main::Session*>, FullMsgId)> saved,
		Fn<void(not_null<Main::Session*>, FullMsgId)> failed) {
	const auto shouldShowToast = documents.empty();
	const auto trackBatchDownload = (destination || saved || failed);

	const auto weak = base::make_weak(controller);
	const auto failureShown = std::make_shared<bool>(false);
	const auto showFailure = [=] {
		if (std::exchange(*failureShown, true)) {
			return;
		}
		if (const auto controller = weak.get()) {
			controller->showToast(tr::ayu_MediaDownloadFailedToast(tr::now));
		}
	};
	const auto saveImages = [=](const QString &folderPath) {
		const auto controller = weak.get();
		if (!controller) {
			return;
		}
		const auto session = &controller->session();
		const auto path = folderPath.isEmpty()
			? DefaultDownloadPath(session)
			: folderPath;
		if (path.isEmpty()) {
			return;
		}
		QDir().mkpath(path);

		const auto showToast = !shouldShowToast
			? Fn<void(const QString &)>(nullptr)
			: [=](const QString &lastPath) {
				const auto controller = weak.get();
				if (!controller) {
					return;
				}
				const auto filter = [lastPath](const auto ...) {
					File::ShowInFolder(lastPath);
					return false;
				};
				controller->showToast({
					.text = (photos.size() > 1
							? tr::lng_mediaview_saved_images_to
							: tr::lng_mediaview_saved_to)(
						tr::now,
						lt_downloads,
						tr::link(
							tr::lng_mediaview_downloads(tr::now),
							"internal:show_saved_message"),
						tr::marked),
					.filter = filter,
					.iconLottie = u"toast/save_to_gallery"_q,
					.iconLottieSize = st::toastLottieIconSize,
					.st = &st::defaultToast,
				});
			};

		struct PhotoDownload {
			not_null<PhotoData*> photo;
			base::weak_ptr<Main::Session> session;
			std::shared_ptr<Data::PhotoMedia> view;
			FullMsgId id;
			TimeId date = 0;
		};
		auto downloads = std::vector<PhotoDownload>();
		for (const auto &[photo, fullId] : photos) {
			photo->clearFailed(Data::PhotoSize::Large);
			if (const auto view = photo->createMediaView()) {
				const auto photoDate = photo->date();
				const auto item = photo->session().data().message(fullId);
				downloads.push_back({
					.photo = photo,
					.session = base::make_weak(&photo->session()),
					.view = view,
					.id = fullId,
					.date = photoDate
						? photoDate
						: (item ? item->date() : TimeId(0)),
				});
			} else {
				if (trackBatchDownload) {
					ForgetBatchDownloadFile(&photo->session(), fullId);
				}
				if (failed) {
					failed(&photo->session(), fullId);
				}
				showFailure();
			}
		}
		const auto reportedFailures = std::make_shared<
			base::flat_set<std::pair<Main::Session*, FullMsgId>>>();
		const auto reportFailure = [=](const PhotoDownload &download) {
			const auto owner = download.session.get();
			if (!owner
				|| !reportedFailures->emplace(owner, download.id).second) {
				return;
			}
			if (trackBatchDownload) {
				ForgetBatchDownloadFile(owner, download.id);
			}
			if (failed) {
				failed(owner, download.id);
			}
			showFailure();
		};
		const auto endedSessions = std::make_shared<
			base::flat_set<Main::Session*>>();
		const auto finalCheck = [=] {
			for (const auto &download : downloads) {
				const auto owner = download.session.get();
				if (owner
					&& !endedSessions->contains(owner)
					&& !download.photo->failed(Data::PhotoSize::Large)
					&& !download.view->loaded()) {
					return false;
				}
			}
			return true;
		};

		const auto saveToFiles = [=] {
			auto lastSavedPath = QString();
			auto allSaved = (downloads.size() == photos.size());
			auto completed = base::flat_map<Main::Session*, BatchDownloads>();
			auto savedResults = std::vector<
				std::pair<not_null<Main::Session*>, FullMsgId>>();
			auto failedResults = std::vector<
				std::pair<not_null<Main::Session*>, FullMsgId>>();
			for (auto i = 0; i != int(downloads.size()); ++i) {
				const auto &download = downloads[i];
				const auto owner = download.session.get();
				if (!owner) {
					allSaved = false;
					continue;
				}
				if (download.photo->failed(Data::PhotoSize::Large)
					|| !download.view->loaded()) {
					if (download.photo->failed(Data::PhotoSize::Large)) {
						reportFailure(download);
					} else {
						failedResults.emplace_back(owner, download.id);
					}
					allSaved = false;
					continue;
				}
				const auto name = u"photo_"_q
					+ QString::number(i + 1)
					+ u".jpg"_q;
				const auto destinationPath = filedialogNextFilename(
					name,
					QString(),
					path);
				if (destinationPath.isEmpty()) {
					failedResults.emplace_back(owner, download.id);
					allSaved = false;
					continue;
				}
				if (destination) {
					destination(owner, download.id, destinationPath);
				}
				const auto savedToFile = download.view->saveToFile(
					destinationPath);
				if (savedToFile && download.date > 0) {
					auto f = QFile(destinationPath);
					if (f.open(QIODevice::ReadWrite)) {
						const auto when = base::unixtime::parse(download.date);
						f.setFileTime(
							when,
							QFileDevice::FileModificationTime);
						f.setFileTime(
							when,
							QFileDevice::FileAccessTime);
					}
				}
				if (savedToFile && trackBatchDownload) {
					completed[owner][download.id] = destinationPath;
					savedResults.emplace_back(owner, download.id);
				} else if (!savedToFile) {
					failedResults.emplace_back(owner, download.id);
				}
				if (savedToFile) {
					lastSavedPath = destinationPath;
				} else {
					QFile::remove(destinationPath);
					allSaved = false;
				}
			}
			for (auto &[owner, files] : completed) {
				CompleteBatchDownloadFiles(owner, std::move(files));
			}
			if (saved) {
				for (const auto &[owner, id] : savedResults) {
					saved(owner, id);
				}
			}
			for (const auto &[owner, id] : failedResults) {
				if (trackBatchDownload) {
					ForgetBatchDownloadFile(owner, id);
				}
				if (failed) {
					failed(owner, id);
				}
			}
			if (showToast && allSaved && !lastSavedPath.isEmpty()) {
				showToast(lastSavedPath);
			} else if (!allSaved) {
				showFailure();
			}
		};

		if (finalCheck()) {
			saveToFiles();
		} else {
			auto lifetime = std::make_shared<rpl::lifetime>();
			const auto finished = std::make_shared<bool>(false);
			const auto finish = [=] {
				if (finalCheck() && !std::exchange(*finished, true)) {
					saveToFiles();
					lifetime->destroy();
				}
			};
			for (const auto &download : downloads) {
				const auto owner = download.session.get();
				if (!owner) {
					continue;
				}
				owner->data().photoLoadProgress(
				) | rpl::on_next_done([=](not_null<PhotoData*> changed) mutable {
					if (changed->failed(Data::PhotoSize::Large)) {
						for (const auto &download : downloads) {
							if (download.photo == changed) {
								reportFailure(download);
							}
						}
					}
					finish();
				}, [=]() mutable {
					endedSessions->emplace(owner);
					finish();
				}, *lifetime);
			}
			for (const auto &download : downloads) {
				download.view->wanted(Data::PhotoSize::Large, download.id);
				if (download.photo->failed(Data::PhotoSize::Large)) {
					reportFailure(download);
				}
			}
			finish();
		}
	};
	const auto saveDocuments = [=](const QString &folderPath) {
		if (folderPath.isEmpty()) {
			for (const auto &[document, origin] : documents) {
				DocumentSaveClickHandler::SaveAndTrack(origin, document);
			}
			return;
		}
		auto nameCounts = base::flat_map<QString, int>();
		auto reservedPaths = base::flat_set<QString>();
		for (const auto &[document, origin] : documents) {
			const auto owner = &document->session();
			const auto filename = QFileInfo(
				document->filename()).fileName();
			const auto safeFilename = (filename.isEmpty()
				|| filename == u"."_q
				|| filename == u".."_q)
				? u"file"_q
				: filename;
			auto path = QString();
			do {
				const auto downloadName = NumberedFilename(
					safeFilename,
					++nameCounts[safeFilename]);
				path = filedialogNextFilename(
					downloadName,
					document->filepath(true),
					folderPath);
			} while (!path.isEmpty()
				&& !reservedPaths.emplace(path.toCaseFolded()).second);
			if (path.isEmpty()) {
				if (failed) {
					failed(owner, origin);
				}
				showFailure();
				continue;
			}
			if (destination) {
				destination(owner, origin, path);
			}
			auto lifetime = std::make_shared<rpl::lifetime>();
			const auto finished = std::make_shared<bool>(false);
			const auto weakOwner = base::make_weak(owner);
			const auto finishFailed = [=](not_null<Main::Session*> owner) {
				QFile::remove(path);
				if (trackBatchDownload) {
					ForgetBatchDownloadFile(owner, origin);
					if (failed) {
						failed(owner, origin);
					}
				}
				showFailure();
			};
			const auto finish = [=] {
				if (document->loading()
					|| std::exchange(*finished, true)) {
					return;
				}
				if (DocumentSavedToPath(document, path)) {
					if (trackBatchDownload) {
						CompleteBatchDownloadFile(owner, origin, path);
						if (saved) {
							saved(owner, origin);
						}
					}
				} else {
					finishFailed(owner);
				}
				lifetime->destroy();
			};
			document->owner().documentLoadProgress(
			) | rpl::on_next_done([=](not_null<DocumentData*> changed) {
				if (changed == document) {
					finish();
				}
			}, [=] {
				if (!std::exchange(*finished, true)) {
					if (const auto owner = weakOwner.get()) {
						finishFailed(owner);
					} else {
						QFile::remove(path);
					}
				}
				lifetime->destroy();
			}, *lifetime);
			document->save(origin, path);
			finish();
		}
	};

	return [=] {
		const auto save = [=](const QString &folderPath) {
			saveImages(folderPath);
			saveDocuments(folderPath);
			if (callback) {
				callback();
			}
		};
		const auto controller = weak.get();
		if (!controller) {
			return;
		}
		if (!forceDefaultPath && Core::App().settings().askDownloadPath()) {
			const auto initialPath = [] {
				const auto path = Core::App().settings().downloadPath();
				if (!path.isEmpty() && path != FileDialog::Tmp()) {
					return path.left(path.size()
						- (path.endsWith('/') ? 1 : 0));
				}
				return QString();
			}();
			const auto handleFolder = [=](const QString &result) {
				if (!result.isEmpty()) {
					const auto folderPath = result.endsWith('/')
						? result
						: (result + '/');
					save(folderPath);
				}
			};
			FileDialog::GetFolder(
				controller->window().widget().get(),
				tr::lng_download_path_choose(tr::now),
				initialPath,
				handleFolder);
		} else if (forceDefaultPath) {
			const auto session = &controller->session();
			const auto path = DefaultDownloadPath(session);
			if (!path.isEmpty()) {
				save(path.endsWith('/') ? path : (path + '/'));
			}
		} else {
			save(QString());
		}
	};
}

void AddAction(
		not_null<Ui::PopupMenu*> menu,
		not_null<Window::SessionController*> controller,
		Documents &&documents,
		Photos &&photos,
		Fn<void()> callback) {
	const auto text = documents.empty()
		? tr::lng_context_save_images_selected(tr::now)
		: tr::lng_context_save_documents_selected(tr::now);
	const auto icon = documents.empty()
		? &st::menuIconSaveImage
		: &st::menuIconDownload;
	menu->addAction(
		text,
		PrepareDownloadAction(
			controller,
			std::move(documents),
			std::move(photos),
			std::move(callback),
			false,
			nullptr,
			nullptr,
			nullptr),
		icon);
}

} // namespace

BatchDownloadFiles BatchDownloadFilesFor(
		not_null<Main::Session*> session) {
	return ReadBatchDownloads(session);
}

void ForgetBatchDownloadFile(
		not_null<Main::Session*> session,
		FullMsgId id) {
	auto downloads = ReadBatchDownloads(session);
	if (downloads.remove(id)) {
		WriteBatchDownloads(session, downloads);
	}
}

bool DownloadSelectedFiles(
		not_null<Window::SessionController*> window,
		const std::vector<not_null<HistoryItem*>> &items,
		Fn<void()> callback,
		bool forceDefaultPath,
		Fn<void(not_null<Main::Session*>, FullMsgId, QString)> destination,
		Fn<void(not_null<Main::Session*>, FullMsgId)> saved,
		Fn<void(not_null<Main::Session*>, FullMsgId)> failed) {
	auto documents = Documents();
	auto photos = Photos();
	for (const auto &item : items) {
		if (!Added(item, documents, photos)) {
			return false;
		}
	}
	if (items.empty()) {
		return false;
	}
	if (forceDefaultPath) {
		const auto path = DefaultDownloadPath(&window->session());
		if (path.isEmpty() || !QDir().mkpath(path)) {
			window->showToast(tr::lng_download_path_failed(tr::now));
			return false;
		}
	}
	PrepareDownloadAction(
		window,
		std::move(documents),
		std::move(photos),
		std::move(callback),
		forceDefaultPath,
		std::move(destination),
		std::move(saved),
		std::move(failed))();
	return true;
}

void AddDownloadFilesAction(
		not_null<Ui::PopupMenu*> menu,
		not_null<Window::SessionController*> window,
		const std::vector<HistoryView::SelectedItem> &selectedItems,
		not_null<HistoryView::ListWidget*> list) {
	if (selectedItems.empty()) {
		return;
	}
	auto docs = Documents();
	auto photos = Photos();
	for (const auto &selectedItem : selectedItems) {
		const auto &id = selectedItem.msgId;
		const auto item = window->session().data().message(id);

		if (!Added(item, docs, photos)) {
			return;
		}
	}
	std::sort(docs.begin(), docs.end(), [](const auto &a, const auto &b) {
		return a.second < b.second;
	});
	std::sort(photos.begin(), photos.end(), [](const auto &a, const auto &b) {
		return a.second < b.second;
	});
	const auto done = [weak = base::make_weak(list)] {
		if (const auto strong = weak.get()) {
			strong->cancelSelection();
		}
	};
	AddAction(menu, window, std::move(docs), std::move(photos), done);
}

void AddDownloadFilesAction(
		not_null<Ui::PopupMenu*> menu,
		not_null<Window::SessionController*> window,
		const std::vector<not_null<HistoryItem*>> &items,
		not_null<HistoryInner*> list) {
	if (items.empty()) {
		return;
	}
	auto docs = Documents();
	auto photos = Photos();
	for (const auto &item : items) {
		if (!Added(item, docs, photos)) {
			return;
		}
	}
	std::sort(docs.begin(), docs.end(), [](const auto &a, const auto &b) {
		return a.second < b.second;
	});
	std::sort(photos.begin(), photos.end(), [](const auto &a, const auto &b) {
		return a.second < b.second;
	});
	const auto done = [weak = base::make_weak(list)] {
		if (const auto strong = weak.get()) {
			strong->clearSelected();
		}
	};
	AddAction(menu, window, std::move(docs), std::move(photos), done);
}

} // namespace Menu
