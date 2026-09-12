/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "menu/menu_item_download_files.h"

#include <limits>

#include "base/base_file_utilities.h"
#include "base/unixtime.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "core/file_utilities.h"
#include "core/mime_type.h"
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

struct GroupedDocument {
	not_null<DocumentData*> document;
	std::vector<FullMsgId> origins;
};

using GroupedDocuments = std::vector<GroupedDocument>;

struct GroupedPhoto {
	not_null<PhotoData*> photo;
	std::vector<FullMsgId> origins;
};

using GroupedPhotos = std::vector<GroupedPhoto>;

constexpr auto kBatchDownloadsPref = "batch_download_files";
constexpr auto kMaxBatchDownloads = 100000;
constexpr auto kMaxBatchDownloadsPayloadSize = 64 * 1024 * 1024;
constexpr auto kMaxBatchDownloadPathSize = 4096;
constexpr auto kBatchDownloadsMagic = quint32(0x4244464C);
constexpr auto kBatchDownloadsVersion = quint32(1);
constexpr auto kBatchDownloadsHeaderSize = quint64(3 * sizeof(quint32));
constexpr auto kBatchDownloadFileSize = quint64(
	sizeof(quint64)
	+ sizeof(qint64)
	+ sizeof(quint32)
	+ 2 * sizeof(quint8));
constexpr auto kBatchDownloadMediaKeySize = quint64(2 * sizeof(quint64));

struct BatchDownloadIdentity {
	FullMsgId id;
	std::optional<MediaKey> mediaKey;
};

std::optional<MediaKey> BatchDownloadMediaKey(
		not_null<DocumentData*> document) {
	return (document->isVideoFile() || document->isVideoMessage())
		? std::make_optional(document->mediaKey())
		: std::nullopt;
}

GroupedDocuments GroupVideoDocuments(Documents documents) {
	auto result = GroupedDocuments();
	result.reserve(documents.size());
	auto mediaKeys = base::flat_map<
		std::pair<Main::Session*, MediaKey>,
		int>();
	for (const auto &[document, origin] : documents) {
		const auto mediaKey = BatchDownloadMediaKey(document);
		if (mediaKey) {
			const auto key = std::make_pair(&document->session(), *mediaKey);
			const auto found = mediaKeys.find(key);
			if (found != mediaKeys.end()) {
				result[found->second].origins.push_back(origin);
				continue;
			}
			mediaKeys.emplace(key, int(result.size()));
		}
		result.push_back({ document, { origin } });
	}
	return result;
}

GroupedPhotos GroupPhotos(Photos photos) {
	auto result = GroupedPhotos();
	result.reserve(photos.size());
	auto keys = base::flat_map<not_null<PhotoData*>, int>();
	for (const auto &[photo, origin] : photos) {
		if (const auto found = keys.find(photo); found != keys.end()) {
			result[found->second].origins.push_back(origin);
		} else {
			keys.emplace(photo, int(result.size()));
			result.push_back({ photo, { origin } });
		}
	}
	return result;
}

bool HasBatchDownloadIdentity(
		const BatchDownloads &downloads,
		const BatchDownloadIdentity &identity) {
	if (!identity.mediaKey) {
		return downloads.contains(identity.id);
	}
	return ranges::any_of(downloads, [&](const auto &entry) {
		return entry.second.mediaKey == identity.mediaKey;
	});
}

BatchDownloads ReadBatchDownloads(not_null<Main::Session*> session) {
	const auto serialized = session->local().readPref<QByteArray>(
		kBatchDownloadsPref);
	if (serialized.isEmpty()) {
		return {};
	}
	if (serialized.size() > kMaxBatchDownloadsPayloadSize) {
		return {};
	}
	auto stream = QDataStream(serialized);
	stream.setVersion(QDataStream::Qt_5_1);
	auto marker = quint32();
	stream >> marker;
	if (stream.status() != QDataStream::Ok) {
		return {};
	}
	auto versioned = false;
	auto count = marker;
	if (marker == kBatchDownloadsMagic) {
		auto version = quint32();
		stream >> version >> count;
		if (stream.status() != QDataStream::Ok
			|| version != kBatchDownloadsVersion) {
			return {};
		}
		versioned = true;
	}
	if (count > kMaxBatchDownloads) {
		return {};
	}
	auto result = BatchDownloads();
	result.reserve(count);
	auto mediaKeys = base::flat_set<MediaKey>();
	for (auto i = quint32(); i != count; ++i) {
		auto peer = quint64();
		auto message = qint64();
		auto path = QString();
		auto completed = quint8();
		stream >> peer >> message >> path >> completed;
		if (stream.status() != QDataStream::Ok
			|| completed > 1
			|| path.size() > kMaxBatchDownloadPathSize) {
			return {};
		}
		auto mediaKey = std::optional<MediaKey>();
		if (versioned) {
			auto hasMediaKey = quint8();
			auto first = quint64();
			auto second = quint64();
			stream >> hasMediaKey;
			if (hasMediaKey == 1) {
				stream >> first >> second;
				mediaKey = MediaKey(first, second);
			} else if (hasMediaKey != 0) {
				return {};
			}
			if (stream.status() != QDataStream::Ok
				|| (mediaKey && !mediaKeys.emplace(*mediaKey).second)) {
				return {};
			}
		}
		const auto inserted = result.emplace(
			FullMsgId(DeserializePeerId(peer), MsgId(message)),
			BatchDownloadFile{
				std::move(path),
				(completed != 0),
				mediaKey,
			});
		if (!inserted.second) {
			return {};
		}
	}
	return stream.atEnd() ? result : BatchDownloads();
}

std::optional<quint64> BatchDownloadFileSerializedSize(
		const BatchDownloadFile &file) {
	if (file.path.size() > kMaxBatchDownloadPathSize) {
		return std::nullopt;
	}
	const auto pathLength = quint64(file.path.size());
	if (pathLength > std::numeric_limits<quint64>::max() / sizeof(QChar)) {
		return std::nullopt;
	}
	const auto pathSize = pathLength * sizeof(QChar);
	const auto mediaKeySize = file.mediaKey
		? kBatchDownloadMediaKeySize
		: 0;
	const auto fixedSize = kBatchDownloadFileSize + mediaKeySize;
	if (pathSize > std::numeric_limits<quint64>::max() - fixedSize) {
		return std::nullopt;
	}
	return fixedSize + pathSize;
}

std::optional<quint64> BatchDownloadsSerializedSize(
		const BatchDownloads &downloads) {
	auto result = kBatchDownloadsHeaderSize;
	for (const auto &entry : downloads) {
		const auto fileSize = BatchDownloadFileSerializedSize(entry.second);
		if (!fileSize
			|| *fileSize > std::numeric_limits<quint64>::max() - result) {
			return std::nullopt;
		}
		result += *fileSize;
	}
	return result;
}

std::optional<QByteArray> SerializeBatchDownloads(
		const BatchDownloads &downloads,
		std::optional<quint64> knownSize = std::nullopt) {
	if (downloads.size() > kMaxBatchDownloads) {
		return std::nullopt;
	}
	const auto expectedSize = knownSize
		? knownSize
		: BatchDownloadsSerializedSize(downloads);
	if (!expectedSize || *expectedSize > kMaxBatchDownloadsPayloadSize) {
		return std::nullopt;
	}
	auto serialized = QByteArray();
	serialized.reserve(int(*expectedSize));
	auto stream = QDataStream(&serialized, QIODevice::WriteOnly);
	stream.setVersion(QDataStream::Qt_5_1);
	stream
		<< kBatchDownloadsMagic
		<< kBatchDownloadsVersion
		<< quint32(downloads.size());
	for (const auto &[id, file] : downloads) {
		stream
			<< SerializePeerId(id.peer)
			<< qint64(id.msg.bare)
			<< file.path
			<< quint8(file.completed ? 1 : 0)
			<< quint8(file.mediaKey ? 1 : 0);
		if (file.mediaKey) {
			stream
				<< quint64(file.mediaKey->first)
				<< quint64(file.mediaKey->second);
		}
	}
	if (stream.status() != QDataStream::Ok
		|| quint64(serialized.size()) != *expectedSize) {
		return std::nullopt;
	}
	return serialized;
}

bool WriteBatchDownloads(
		not_null<Main::Session*> session,
		const BatchDownloads &downloads) {
	auto serialized = SerializeBatchDownloads(downloads);
	if (!serialized) {
		return false;
	}
	session->local().writePref<QByteArray>(
		kBatchDownloadsPref,
		std::move(*serialized));
	return true;
}

bool EnsureBatchDownloadCapacity(
		not_null<Main::Session*> session,
		const std::vector<BatchDownloadIdentity> &identities) {
	auto downloads = ReadBatchDownloads(session);
	for (auto i = downloads.begin(); i != downloads.end();) {
		if (i->second.completed && !QFileInfo::exists(i->second.path)) {
			i = downloads.erase(i);
		} else {
			++i;
		}
	}
	auto additional = 0;
	auto addedIds = base::flat_set<FullMsgId>();
	auto addedMediaKeys = base::flat_set<MediaKey>();
	for (const auto &identity : identities) {
		if (HasBatchDownloadIdentity(downloads, identity)) {
			continue;
		}
		const auto added = identity.mediaKey
			? addedMediaKeys.emplace(*identity.mediaKey).second
			: addedIds.emplace(identity.id).second;
		additional += added ? 1 : 0;
	}
	while (downloads.size() + additional > kMaxBatchDownloads) {
		const auto completed = ranges::find_if(
			downloads,
			[](const auto &entry) { return entry.second.completed; });
		if (completed == downloads.end()) {
			return false;
		}
		downloads.erase(completed);
	}
	return true;
}

bool UpdateBatchDownloadFiles(
		not_null<Main::Session*> session,
		BatchDownloads updates) {
	auto downloads = ReadBatchDownloads(session);
	for (auto &[id, file] : updates) {
		if (file.mediaKey) {
			for (auto i = downloads.begin(); i != downloads.end();) {
				if (i->first != id && i->second.mediaKey == file.mediaKey) {
					i = downloads.erase(i);
				} else {
					++i;
				}
			}
		}
		downloads[id] = std::move(file);
	}
	for (auto i = downloads.begin(); i != downloads.end();) {
		if (i->second.completed && !QFileInfo::exists(i->second.path)) {
			i = downloads.erase(i);
		} else {
			++i;
		}
	}
	auto serializedSize = BatchDownloadsSerializedSize(downloads);
	if (!serializedSize) {
		return false;
	}
	auto count = downloads.size();
	for (auto i = downloads.begin(); i != downloads.end();) {
		if (count <= kMaxBatchDownloads
			&& *serializedSize <= kMaxBatchDownloadsPayloadSize) {
			break;
		} else if (!i->second.completed) {
			++i;
			continue;
		}
		const auto fileSize = BatchDownloadFileSerializedSize(i->second);
		if (!fileSize || *fileSize > *serializedSize) {
			return false;
		}
		*serializedSize -= *fileSize;
		--count;
		i = downloads.erase(i);
	}
	if (count > kMaxBatchDownloads
		|| *serializedSize > kMaxBatchDownloadsPayloadSize) {
		return false;
	}
	auto serialized = SerializeBatchDownloads(downloads, serializedSize);
	if (!serialized) {
		return false;
	}
	session->local().writePref<QByteArray>(
		kBatchDownloadsPref,
		std::move(*serialized));
	return true;
}

bool CompleteBatchDownloadFile(
		not_null<Main::Session*> session,
		FullMsgId id,
		QString path,
		std::optional<MediaKey> mediaKey) {
	auto updates = BatchDownloads();
	updates.emplace(id, BatchDownloadFile{
		std::move(path),
		true,
		mediaKey,
	});
	return UpdateBatchDownloadFiles(session, std::move(updates));
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
	return document->status != FileDownloadFailed
		&& !document->cancelled()
		&& info.isFile()
		&& info.size() == document->size;
}

QString BatchDownloadFileName(not_null<DocumentData*> document) {
	const auto isVideo = document->isVideoFile()
		|| document->isVideoMessage();
	const auto mime = Core::MimeTypeForName(document->mimeString());
	const auto patterns = mime.globPatterns();
	const auto extension = patterns.isEmpty()
		? (isVideo ? u".mp4"_q : QString())
		: QString(patterns.front()).replace('*', QString());
	const auto filename = QFileInfo(document->filename()).fileName();
	if (!filename.isEmpty() && filename != u"."_q && filename != u".."_q) {
		return (isVideo && QFileInfo(filename).suffix().isEmpty())
			? (filename + extension)
			: filename;
	}
	const auto prefix = document->isVideoMessage()
		? u"round"_q
		: document->isVideoFile()
		? u"video"_q
		: document->isAnimation()
		? u"animation"_q
		: u"file"_q;
	return prefix
		+ u"_"_q
		+ QString::number(document->getDC())
		+ u"_"_q
		+ QString::number(document->id)
		+ extension;
}

struct ReservedDownloadPath {
	QString path;
	bool owned = false;
};

QString DownloadPathIdentity(const QString &path) {
	const auto info = QFileInfo(path);
	const auto canonical = info.canonicalFilePath();
	return canonical.isEmpty() ? info.absoluteFilePath() : canonical;
}

ReservedDownloadPath ReserveDownloadPath(
		const QString &name,
		const QString &current,
		const QString &folder) {
	auto allowCurrent = !current.isEmpty();
	while (true) {
		const auto path = filedialogNextFilename(
			name,
			allowCurrent ? current : QString(),
			folder);
		if (path.isEmpty()) {
			return {};
		}
		const auto info = QFileInfo(path);
		const auto canonical = info.canonicalFilePath();
		if (allowCurrent
			&& info.exists()
			&& !canonical.isEmpty()
			&& canonical == QFileInfo(current).canonicalFilePath()) {
			return { path, false };
		}
		auto file = QFile(path);
		if (file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
			return { path, true };
		} else if (!QFileInfo::exists(path)) {
			return {};
		}
		allowCurrent = false;
	}
}

[[nodiscard]] bool Collected(
		HistoryItem *item,
		Documents &documents,
		Photos &photos) {
	if (!item) {
		return false;
	} else if (item->forbidsSaving()) {
		return true;
	} else if (const auto media = item->media()) {
		if (const auto photo = media->photo()) {
			photos.emplace_back(photo, item->fullId());
			return true;
		} else if (const auto document = media->document()) {
			documents.emplace_back(document, item->fullId());
			return true;
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
	const auto groupedDocuments = GroupVideoDocuments(std::move(documents));
	const auto groupedPhotos = GroupPhotos(std::move(photos));
	const auto shouldShowToast = groupedDocuments.empty();
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
	const auto reportDestinations = [=](
			not_null<Main::Session*> owner,
			const std::vector<FullMsgId> &origins,
			const QString &path) {
		if (destination) {
			for (const auto &origin : origins) {
				destination(owner, origin, path);
			}
		}
	};
	const auto reportSaved = [=](
			not_null<Main::Session*> owner,
			const std::vector<FullMsgId> &origins) {
		if (saved) {
			for (const auto &origin : origins) {
				saved(owner, origin);
			}
		}
	};
	const auto reportFailed = [=](
			not_null<Main::Session*> owner,
			const std::vector<FullMsgId> &origins) {
		if (failed) {
			for (const auto &origin : origins) {
				failed(owner, origin);
			}
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
					.text = (groupedPhotos.size() > 1
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
			std::vector<FullMsgId> origins;
			QString name;
			QString path;
			bool pathOwned = false;
			TimeId date = 0;
		};
		auto downloads = std::vector<PhotoDownload>();
		auto pending = base::flat_map<Main::Session*, BatchDownloads>();
		auto blocked = base::flat_set<Main::Session*>();
		if (trackBatchDownload) {
			auto ids = base::flat_map<
				Main::Session*,
				std::vector<BatchDownloadIdentity>>();
			for (const auto &entry : groupedPhotos) {
				for (const auto &origin : entry.origins) {
					ids[&entry.photo->session()].push_back({
						origin,
						std::nullopt,
					});
				}
			}
			for (const auto &[owner, list] : ids) {
				if (!EnsureBatchDownloadCapacity(owner, list)) {
					blocked.emplace(owner);
				}
			}
		}
		for (const auto &entry : groupedPhotos) {
			const auto photo = entry.photo;
			const auto &origins = entry.origins;
			const auto fullId = origins.front();
			const auto owner = &photo->session();
			if (blocked.contains(owner)) {
				reportFailed(owner, origins);
				showFailure();
				continue;
			}
			photo->clearFailed(Data::PhotoSize::Large);
			if (const auto view = photo->createMediaView()) {
				const auto name = u"photo_"_q
					+ QString::number(photo->getDC())
					+ u"_"_q
					+ (photo->id
						? QString::number(photo->id)
						: QString::number(SerializePeerId(fullId.peer))
							+ u"_"_q
							+ QString::number(fullId.msg.bare))
					+ u".jpg"_q;
				auto destinationPath = ReservedDownloadPath();
				if (trackBatchDownload) {
					destinationPath = ReserveDownloadPath(
						name,
						QString(),
						path);
					if (destinationPath.path.isEmpty()) {
						reportFailed(owner, origins);
						showFailure();
						continue;
					}
					for (const auto &origin : origins) {
						pending[owner][origin] = {
							destinationPath.path,
							false,
							std::nullopt,
						};
					}
				}
				const auto photoDate = photo->date();
				const auto item = owner->data().message(fullId);
				downloads.push_back({
					.photo = photo,
					.session = base::make_weak(owner),
					.view = view,
					.origins = origins,
					.name = name,
					.path = destinationPath.path,
					.pathOwned = destinationPath.owned,
					.date = photoDate
						? photoDate
						: (item ? item->date() : TimeId(0)),
				});
			} else {
				reportFailed(&photo->session(), origins);
				showFailure();
			}
		}
		auto rejected = base::flat_set<Main::Session*>();
		for (auto &[owner, files] : pending) {
			if (!UpdateBatchDownloadFiles(owner, std::move(files))) {
				rejected.emplace(owner);
			}
		}
		if (trackBatchDownload) {
			auto accepted = std::vector<PhotoDownload>();
			accepted.reserve(downloads.size());
			for (auto &download : downloads) {
				const auto owner = download.session.get();
				if (!owner || rejected.contains(owner)) {
					if (download.pathOwned) {
						QFile::remove(download.path);
					}
					if (owner) {
						reportFailed(owner, download.origins);
					}
					showFailure();
				} else {
					reportDestinations(owner, download.origins, download.path);
					accepted.push_back(std::move(download));
				}
			}
			downloads = std::move(accepted);
		}
		const auto reportedFailures = std::make_shared<
			base::flat_set<std::pair<Main::Session*, PhotoData*>>>();
		const auto reportFailure = [=](const PhotoDownload &download) {
			const auto owner = download.session.get();
			if (!owner
				|| !reportedFailures->emplace(
					owner,
					download.photo.get()).second) {
				return;
			}
			QFile::remove(download.path);
			reportFailed(owner, download.origins);
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
			auto allSaved = (downloads.size() == groupedPhotos.size());
			auto completed = base::flat_map<Main::Session*, BatchDownloads>();
			auto savedResults = std::vector<
				std::pair<not_null<Main::Session*>, FullMsgId>>();
			auto failedResults = std::vector<
				std::pair<not_null<Main::Session*>, FullMsgId>>();
			for (auto i = 0; i != int(downloads.size()); ++i) {
				const auto &download = downloads[i];
				const auto owner = download.session.get();
				if (!owner) {
					QFile::remove(download.path);
					allSaved = false;
					continue;
				}
				if (download.photo->failed(Data::PhotoSize::Large)
					|| !download.view->loaded()) {
					if (download.photo->failed(Data::PhotoSize::Large)) {
						reportFailure(download);
					} else {
						for (const auto &origin : download.origins) {
							failedResults.emplace_back(owner, origin);
						}
					}
					QFile::remove(download.path);
					allSaved = false;
					continue;
				}
				auto destinationPath = download.path;
				if (destinationPath.isEmpty()) {
					const auto reserved = ReserveDownloadPath(
						download.name,
						QString(),
						path);
					destinationPath = reserved.path;
					if (destinationPath.isEmpty()) {
						for (const auto &origin : download.origins) {
							failedResults.emplace_back(owner, origin);
						}
						allSaved = false;
						continue;
					}
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
					for (const auto &origin : download.origins) {
						completed[owner][origin] = {
							destinationPath,
							true,
							std::nullopt,
						};
						savedResults.emplace_back(owner, origin);
					}
				} else if (!savedToFile) {
					for (const auto &origin : download.origins) {
						failedResults.emplace_back(owner, origin);
					}
				}
				if (savedToFile) {
					lastSavedPath = destinationPath;
				} else {
					QFile::remove(destinationPath);
					allSaved = false;
				}
			}
			for (auto &[owner, files] : completed) {
				const auto updated = UpdateBatchDownloadFiles(
					owner,
					std::move(files));
				Assert(updated);
			}
			if (saved) {
				for (const auto &[owner, id] : savedResults) {
					saved(owner, id);
				}
			}
			for (const auto &[owner, id] : failedResults) {
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
				) | rpl::on_next_done([=](not_null<PhotoData*>) {
					finish();
				}, [=]() mutable {
					endedSessions->emplace(owner);
					finish();
				}, *lifetime);
			}
			for (const auto &download : downloads) {
				download.view->wanted(
					Data::PhotoSize::Large,
					download.origins.front());
			}
			finish();
		}
	};
	const auto saveDocuments = [=](const QString &folderPath) {
		if (folderPath.isEmpty()) {
			for (const auto &entry : groupedDocuments) {
				DocumentSaveClickHandler::SaveAndTrack(
					entry.origins.front(),
					entry.document);
			}
			return;
		}
		struct DocumentDownload {
			not_null<DocumentData*> document;
			not_null<Main::Session*> owner;
			std::vector<FullMsgId> origins;
			QString path;
			std::optional<MediaKey> mediaKey;
			bool pathOwned = false;
		};
		auto downloads = std::vector<DocumentDownload>();
		auto pending = base::flat_map<Main::Session*, BatchDownloads>();
		auto persisted = base::flat_map<Main::Session*, BatchDownloads>();
		auto reservedPaths = base::flat_set<QString>();
		auto blocked = base::flat_set<Main::Session*>();
		auto existing = base::flat_map<Main::Session*, BatchDownloads>();
		if (trackBatchDownload) {
			auto ids = base::flat_map<
				Main::Session*,
				std::vector<BatchDownloadIdentity>>();
			for (const auto &entry : groupedDocuments) {
				const auto document = entry.document;
				ids[&document->session()].push_back({
					entry.origins.front(),
					BatchDownloadMediaKey(document),
				});
			}
			for (const auto &[owner, list] : ids) {
				if (!EnsureBatchDownloadCapacity(owner, list)) {
					blocked.emplace(owner);
				}
			}
		}
		for (const auto &entry : groupedDocuments) {
			const auto document = entry.document;
			const auto origins = entry.origins;
			const auto origin = origins.front();
			const auto owner = &document->session();
			const auto mediaKey = BatchDownloadMediaKey(document);
			if (blocked.contains(owner)) {
				reportFailed(owner, origins);
				showFailure();
				continue;
			}
			if (trackBatchDownload && mediaKey && document->loading()) {
				const auto files = existing.find(owner);
				const auto &sessionFiles = (files != existing.end())
					? files->second
					: existing.emplace(
						owner,
						ReadBatchDownloads(owner)).first->second;
				const auto active = ranges::find_if(
					sessionFiles,
					[&](const auto &entry) {
						return !entry.second.completed
							&& entry.second.mediaKey == mediaKey;
					});
				if (active != sessionFiles.end()) {
					const auto activeId = active->first;
					const auto loadingPath = document->loadingFilePath();
					if (loadingPath.isEmpty()
						|| active->second.path.isEmpty()
						|| DownloadPathIdentity(active->second.path)
							!= DownloadPathIdentity(loadingPath)) {
						reportFailed(owner, origins);
						showFailure();
						continue;
					}
					const auto activePath = active->second.path;
					const auto activeMediaKey = active->second.mediaKey;
					reportDestinations(owner, origins, activePath);
					auto lifetime = std::make_shared<rpl::lifetime>();
					const auto finished = std::make_shared<bool>(false);
					const auto weakOwner = base::make_weak(owner);
					const auto finish = [=] {
						if (document->loading()
							|| std::exchange(*finished, true)) {
							return;
						}
						if (const auto owner = weakOwner.get()) {
							if (DocumentSavedToPath(document, activePath)) {
								if (CompleteBatchDownloadFile(
										owner,
										activeId,
										activePath,
										activeMediaKey)) {
									reportSaved(owner, origins);
								} else {
									reportFailed(owner, origins);
									showFailure();
								}
							} else {
								reportFailed(owner, origins);
								showFailure();
							}
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
								reportFailed(owner, origins);
								showFailure();
							}
						}
						lifetime->destroy();
					}, *lifetime);
					finish();
					continue;
				}
			}
			const auto safeFilename = BatchDownloadFileName(document);
			const auto currentPath = document->filepath(true);
			if (!document->loading()
				&& !currentPath.isEmpty()
				&& !DocumentSavedToPath(document, currentPath)) {
				const auto ownerFiles = persisted.find(owner);
				const auto &sessionFiles = (ownerFiles != persisted.end())
					? ownerFiles->second
					: persisted.emplace(
						owner,
						ReadBatchDownloads(owner)).first->second;
				const auto interrupted = ranges::any_of(
					sessionFiles,
					[&](const auto &file) {
						return !file.second.completed
							&& DownloadPathIdentity(file.second.path)
								== DownloadPathIdentity(currentPath);
					});
				if (interrupted) {
					QFile::remove(currentPath);
				}
			}
			auto reserved = ReserveDownloadPath(
				safeFilename,
				DocumentSavedToPath(document, currentPath)
					? currentPath
					: QString(),
				folderPath);
			while (!reserved.path.isEmpty()
				&& !reservedPaths.emplace(
					DownloadPathIdentity(reserved.path)).second) {
				reserved = ReserveDownloadPath(
					safeFilename,
					QString(),
					folderPath);
			}
			if (reserved.path.isEmpty()) {
				reportFailed(owner, origins);
				showFailure();
				continue;
			}
			if (trackBatchDownload) {
				pending[owner][origin] = {
					reserved.path,
					false,
					mediaKey,
				};
			}
			downloads.push_back({
				document,
				owner,
				origins,
				std::move(reserved.path),
				mediaKey,
				reserved.owned,
			});
		}
		auto rejected = base::flat_set<Main::Session*>();
		for (auto &[owner, files] : pending) {
			if (!UpdateBatchDownloadFiles(owner, std::move(files))) {
				rejected.emplace(owner);
			}
		}
		if (trackBatchDownload) {
			auto accepted = std::vector<DocumentDownload>();
			accepted.reserve(downloads.size());
			for (auto &download : downloads) {
				if (rejected.contains(download.owner)) {
					if (download.pathOwned) {
						QFile::remove(download.path);
					}
					reportFailed(download.owner, download.origins);
					showFailure();
				} else {
					reportDestinations(
						download.owner,
						download.origins,
						download.path);
					accepted.push_back(std::move(download));
				}
			}
			downloads = std::move(accepted);
		}
		for (const auto &download : downloads) {
			const auto document = download.document;
			const auto owner = download.owner;
			const auto &origins = download.origins;
			const auto origin = origins.front();
			const auto &path = download.path;
			const auto mediaKey = download.mediaKey;
			const auto pathOwned = download.pathOwned;
			auto lifetime = std::make_shared<rpl::lifetime>();
			const auto finished = std::make_shared<bool>(false);
			const auto weakOwner = base::make_weak(owner);
			const auto finishFailed = [=](not_null<Main::Session*> owner) {
				if (pathOwned) {
					QFile::remove(path);
				}
				reportFailed(owner, origins);
				showFailure();
			};
			const auto finish = [=] {
				if (document->loading()
					|| std::exchange(*finished, true)) {
					return;
				}
				if (DocumentSavedToPath(document, path)) {
					if (trackBatchDownload) {
						const auto completed = CompleteBatchDownloadFile(
							owner,
							origin,
							path,
							mediaKey);
						if (completed) {
							reportSaved(owner, origins);
						} else if (!completed) {
							finishFailed(owner);
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
					} else if (pathOwned) {
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

bool MarkBatchDownloadFilesCompleted(
		not_null<Main::Session*> session,
	BatchDownloadFiles files) {
	for (auto &[id, file] : files) {
		file.completed = true;
	}
	return UpdateBatchDownloadFiles(session, std::move(files));
}

bool MigrateBatchDownloadFileMediaKey(
		not_null<Main::Session*> session,
		FullMsgId id,
		MediaKey mediaKey) {
	return MigrateBatchDownloadFileMediaKeys(session, { { id, mediaKey } });
}

bool MigrateBatchDownloadFileMediaKeys(
		not_null<Main::Session*> session,
		const std::vector<std::pair<FullMsgId, MediaKey>> &migrations) {
	if (migrations.empty()) {
		return true;
	}
	auto downloads = ReadBatchDownloads(session);
	for (const auto &migration : migrations) {
		if (!downloads.contains(migration.first)) {
			return false;
		}
	}
	for (const auto &[id, mediaKey] : migrations) {
		const auto legacy = downloads.find(id);
		if (legacy == downloads.end()) {
			if (ranges::none_of(downloads, [&](const auto &entry) {
				return entry.second.mediaKey == mediaKey;
			})) {
				return false;
			}
			continue;
		} else if (legacy->second.mediaKey == mediaKey) {
			continue;
		}
		const auto canonical = ranges::find_if(
			downloads,
			[&](const auto &entry) {
				return entry.first != id && entry.second.mediaKey == mediaKey;
			});
		if (canonical != downloads.end()) {
			if (legacy->second.completed && !canonical->second.completed) {
				legacy->second.mediaKey = mediaKey;
				downloads.erase(canonical);
			} else {
				downloads.erase(legacy);
			}
		} else {
			legacy->second.mediaKey = mediaKey;
		}
	}
	return WriteBatchDownloads(session, downloads);
}

void ForgetBatchDownloadFile(
		not_null<Main::Session*> session,
		FullMsgId id) {
	ForgetBatchDownloadFiles(session, { id });
}

void ForgetBatchDownloadFiles(
		not_null<Main::Session*> session,
		const std::vector<FullMsgId> &ids) {
	if (ids.empty()) {
		return;
	}
	auto downloads = ReadBatchDownloads(session);
	auto changed = false;
	for (const auto &id : ids) {
		changed = downloads.remove(id) || changed;
	}
	if (changed) {
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

		if (!Collected(item, docs, photos)) {
			return;
		}
	}
	if (docs.empty() && photos.empty()) {
		return;
	}
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
		if (!Collected(item, docs, photos)) {
			return;
		}
	}
	if (docs.empty() && photos.empty()) {
		return;
	}
	const auto done = [weak = base::make_weak(list)] {
		if (const auto strong = weak.get()) {
			strong->clearSelected();
		}
	};
	AddAction(menu, window, std::move(docs), std::move(photos), done);
}

} // namespace Menu
