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
using BatchDownloads = base::flat_map<FullMsgId, BatchDownloadFile>;

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
		result.emplace(
			FullMsgId(DeserializePeerId(peer), MsgId(message)),
			BatchDownloadFile{ std::move(path), (completed != 0) });
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
		for (const auto &[id, file] : downloads) {
			stream
				<< SerializePeerId(id.peer)
				<< qint64(id.msg.bare)
				<< file.path
				<< quint8(file.completed ? 1 : 0);
		}
	}
	session->local().writePref<QByteArray>(
		kBatchDownloadsPref,
		std::move(serialized));
}

void SetBatchDownloadFile(
		not_null<Main::Session*> session,
		FullMsgId id,
		QString path,
		bool completed) {
	auto downloads = ReadBatchDownloads(session);
	downloads[id] = { std::move(path), completed };
	WriteBatchDownloads(session, downloads);
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
		Fn<void(not_null<Main::Session*>, FullMsgId)> saved) {
	const auto shouldShowToast = documents.empty();
	const auto trackBatchDownload = (destination || saved);

	const auto weak = base::make_weak(controller);
	const auto saveImages = [=](const QString &folderPath) {
		const auto controller = weak.get();
		if (!controller) {
			return;
		}
		const auto session = &controller->session();
		const auto downloadPath = folderPath.isEmpty()
			? Core::App().settings().downloadPath()
			: folderPath;
		const auto path = downloadPath.isEmpty()
			? File::DefaultDownloadPath(session)
			: (downloadPath == FileDialog::Tmp())
			? session->local().tempDirectory()
			: downloadPath;
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
			if (const auto view = photo->createMediaView()) {
				view->wanted(Data::PhotoSize::Large, fullId);
				const auto photoDate = photo->date();
				const auto item = session->data().message(fullId);
				downloads.push_back({
					.photo = photo,
					.session = base::make_weak(&photo->session()),
					.view = view,
					.id = fullId,
					.date = photoDate
						? photoDate
						: (item ? item->date() : TimeId(0)),
				});
			}
		}

		const auto finalCheck = [=] {
			for (const auto &download : downloads) {
				if (!download.session.get()) {
					return std::optional<bool>();
				} else if (download.photo->loading()) {
					return std::optional<bool>(false);
				}
			}
			return std::optional<bool>(true);
		};

		const auto saveToFiles = [=] {
			const auto fullPath = [&](int i) {
				return filedialogDefaultName(
					u"photo_"_q + QString::number(i),
					u".jpg"_q,
					path);
			};
			auto lastPath = QString();
			for (auto i = 0; i < downloads.size(); i++) {
				lastPath = fullPath(i + 1);
				const auto &download = downloads[i];
				const auto owner = download.session.get();
				if (!owner) {
					continue;
				}
				if (trackBatchDownload) {
					SetBatchDownloadFile(owner, download.id, lastPath, false);
				}
				if (destination) {
					destination(owner, download.id, lastPath);
				}
				const auto savedToFile = download.view->saveToFile(lastPath);
				if (savedToFile && download.date > 0) {
					auto f = QFile(lastPath);
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
					SetBatchDownloadFile(
						owner,
						download.id,
						lastPath,
						true);
					if (saved) {
						saved(owner, download.id);
					}
				} else if (!savedToFile && trackBatchDownload) {
					ForgetBatchDownloadFile(owner, download.id);
				}
			}
			if (showToast) {
				showToast(lastPath);
			}
		};

		if (finalCheck().value_or(false)) {
			saveToFiles();
		} else {
			auto lifetime = std::make_shared<rpl::lifetime>();
			for (const auto &download : downloads) {
				const auto owner = download.session.get();
				if (!owner) {
					continue;
				}
				owner->data().photoLoadProgress(
				) | rpl::on_next([=](not_null<PhotoData*>) mutable {
					const auto ready = finalCheck();
					if (!ready) {
						base::take(lifetime)->destroy();
					} else if (*ready) {
						saveToFiles();
						base::take(lifetime)->destroy();
					}
				}, *lifetime);
			}
		}
	};
	const auto saveDocuments = [=](const QString &folderPath) {
		for (const auto &[document, origin] : documents) {
			if (!folderPath.isEmpty()) {
				const auto owner = &document->session();
				const auto path = filedialogNextFilename(
					document->filename(),
					document->filepath(true),
					folderPath);
				if (path.isEmpty()) {
					continue;
				}
				if (destination) {
					destination(owner, origin, path);
				}
				if (trackBatchDownload) {
					SetBatchDownloadFile(owner, origin, path, false);
				}
				document->save(origin, path);
				if (document->loading() && trackBatchDownload) {
					auto lifetime = std::make_shared<rpl::lifetime>();
					document->owner().documentLoadProgress(
					) | rpl::on_next([=](not_null<DocumentData*> changed) mutable {
						if (changed != document) {
							return;
						}
						if (document->loading()) {
							return;
						}
						if (document->filepath(true) == path
							&& QFileInfo::exists(path)) {
							SetBatchDownloadFile(owner, origin, path, true);
							if (saved) {
								saved(owner, origin);
							}
						} else {
							ForgetBatchDownloadFile(owner, origin);
						}
						base::take(lifetime)->destroy();
					}, *lifetime);
				} else if (QFileInfo::exists(path) && trackBatchDownload) {
					SetBatchDownloadFile(owner, origin, path, true);
					if (saved) {
						saved(owner, origin);
					}
				} else if (trackBatchDownload) {
					ForgetBatchDownloadFile(owner, origin);
				}
			} else {
				DocumentSaveClickHandler::SaveAndTrack(origin, document);
			}
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
			const auto configured = Core::App().settings().downloadPath();
			const auto path = configured.isEmpty()
				? File::DefaultDownloadPath(session)
				: (configured == FileDialog::Tmp())
				? session->local().tempDirectory()
				: configured;
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
		Fn<void(not_null<Main::Session*>, FullMsgId)> saved) {
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
	PrepareDownloadAction(
		window,
		std::move(documents),
		std::move(photos),
		std::move(callback),
		forceDefaultPath,
		std::move(destination),
		std::move(saved))();
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
