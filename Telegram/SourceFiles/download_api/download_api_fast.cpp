/*
This file is part of AyuGram Desktop,
the unofficial fork of Telegram Desktop with a built-in download API.

For license and copyright information please follow this link:
https://github.com/AyuGram/AyuGramDesktop/blob/master/LEGAL
*/
#include "download_api/download_api_fast.h"

#include "export/data/export_data_types.h"
#include "export/output/export_output_result.h"

#include "logs.h"
#include "mtproto/mtp_instance.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>

#include <algorithm>
#include <cmath>

namespace DownloadApi {
namespace {

constexpr auto kFileChunkSize = 8 * 128 * 1024;
constexpr auto kMessagesLimit = 100;
constexpr auto kDialogsLimit = 100;
constexpr auto kMaxDialogsPages = 5;

bool MatchDialogsPeer(
		const Export::Data::DialogInfo &info,
		uint64 wanted) {
	if (!wanted || wanted == info.peerId.value) {
		return wanted != 0;
	}
	const auto input = info.input;
	if (input.type() == mtpc_inputPeerChannel) {
		return input.c_inputPeerChannel().vchannel_id().v == wanted;
	} else if (input.type() == mtpc_inputPeerChat) {
		return input.c_inputPeerChat().vchat_id().v == wanted;
	} else if (input.type() == mtpc_inputPeerUser) {
		return input.c_inputPeerUser().vuser_id().v == wanted;
	}
	return false;
}

} // namespace

FastController::FastController(QPointer<MTP::Instance> mtproto)
: _wrapped(std::move(mtproto)) {
}

FastController::~FastController() {
}

void FastController::start(JobOptions options) {
	_wrapped.with([=](Implementation &unwrapped) {
		unwrapped.start(std::move(options));
	});
}

void FastController::cancel() {
	_wrapped.with([=](Implementation &unwrapped) {
		unwrapped.cancel();
	});
}

rpl::producer<JobState> FastController::state() const {
	return _wrapped.producer_on_main([=](const Implementation &unwrapped) {
		return unwrapped.state();
	});
}

FastControllerObject::FastControllerObject(
		crl::weak_on_queue<FastControllerObject> weak,
		QPointer<MTP::Instance> mtproto)
: _mtp(mtproto, weak.runner())
, _state(JobState{}) {
}

rpl::producer<JobState> FastControllerObject::state() const {
	return rpl::single(
		_state
	) | rpl::then(
		_stateChanges.events()
	);
}

void FastControllerObject::setState(JobState &&state) {
	_state = std::move(state);
	_stateChanges.fire_copy(_state);
}

void FastControllerObject::addDebug(const QString &line) {
	_state.debug = (u"|"_q + line + _state.debug).left(1200);
}

void FastControllerObject::emitState() {
	auto state = JobState();
	state.status = JobState::Status::Running;
	state.startedAt = _startedAt;
	state.dialogName = _chatName;
	state.dialogIndex = 0;
	state.dialogCount = 1;
	state.itemIndex = _state.filesFinished;
	state.itemCount = _state.filesTotal;
	state.bytesLoaded = _state.bytesLoadedTotal;
	state.bytesTotal = _state.bytesTotalAll;
	state.bytesLoadedTotal = _state.bytesLoadedTotal;
	state.bytesTotalAll = _state.bytesTotalAll;
	state.filesFinished = _state.filesFinished;
	state.filesTotal = _state.filesTotal;
	const auto now = QDateTime::currentMSecsSinceEpoch();
	if (_lastAt > 0) {
		const auto dtMs = std::max<qint64>(1, now - _lastAt);
		const auto dtBytes = std::max<qint64>(
			0,
			_state.bytesLoadedTotal - _lastBytes);
		const auto dtFiles = std::max<qint64>(
			0,
			_state.filesFinished - _lastFiles);
		const auto instant = 1000.0 * dtBytes / dtMs;
		state.bytesPerSecond = (_lastBps == 0)
			? instant
			: (_lastBps * 0.75 + instant * 0.25);
		state.filesPerSecond = (state.bytesPerSecond > 0)
			? 1000.0 * dtFiles / dtMs
			: 0;
		const auto remain = _state.bytesTotalAll - _state.bytesLoadedTotal;
		state.etaSeconds = (state.bytesPerSecond > 0
			&& remain > 0)
			? int32(std::min<double>(remain / state.bytesPerSecond, 86400.0 * 7))
			: 0;
	}
	_lastAt = now;
	_lastBytes = _state.bytesLoadedTotal;
	_lastFiles = _state.filesFinished;
	_lastBps = state.bytesPerSecond;
	_lastFilesBps = state.filesPerSecond;
	state.debug = _state.debug;
	setState(std::move(state));
}

void FastControllerObject::fail(const QString &error) {
	if (_finished || _cancelled) {
		return;
	}
	_cancelled = true;
	auto state = JobState();
	state.status = JobState::Status::Error;
	state.error = error;
	state.startedAt = _startedAt;
	setState(std::move(state));
}

bool FastControllerObject::stopped() const {
	return _cancelled || _finished;
}

void FastControllerObject::start(JobOptions options) {
	_options = std::move(options);
	_startedAt = QDateTime::currentMSecsSinceEpoch();
	if (_options.topicRootId) {
		_relativePath = u"chats/chat_"_q
			+ QString::number(qint64(_options.peerId))
			+ u"/topic_"_q
			+ QString::number(_options.topicRootId)
			+ u"/"_q;
	}
	auto state = JobState();
	state.status = JobState::Status::Running;
	state.startedAt = _startedAt;
	setState(std::move(state));
	requestSelf();
}

void FastControllerObject::cancel() {
	if (_cancelled || _finished) {
		return;
	}
	_cancelled = true;
	auto state = JobState();
	state.status = JobState::Status::Cancelled;
	state.startedAt = _startedAt;
	setState(std::move(state));
}

void FastControllerObject::requestSelf() {
	_mtp.request(MTPusers_GetUsers(
		MTP_vector<MTPInputUser>(1, MTP_inputUserSelf())
	)).done([=](const MTPVector<MTPUser> &result) {
		if (stopped()) {
			return;
		}
		for (const auto &user : result.v) {
			user.match([&](const MTPDuser &data) {
				if (data.is_self()) {
					_selfId = data.vid().v;
				}
			}, [&](const MTPDuserEmpty&) {
			});
		}
		if (_selfId) {
			requestDialogsPage(
				0,
				0,
				MTP_inputPeerEmpty());
		}
	}).fail([=](const MTP::Error &error) {
		fail(error.type());
	}).send();
}

void FastControllerObject::requestDialogsPage(
		int offsetId,
		int offsetDate,
		MTPInputPeer offsetPeer) {
	_mtp.request(MTPmessages_GetDialogs(
		MTP_flags(0),
		MTPint(), // folder_id
		MTP_int(offsetDate),
		MTP_int(offsetId),
		offsetPeer,
		MTP_int(kDialogsLimit),
		MTP_long(0)
	)).done([=](const MTPmessages_Dialogs &result) {
		if (stopped()) {
			return;
		}
		dialogsPageDone(result);
	}).fail([=](const MTP::Error &error) {
		fail(error.type());
	}).send();
}

void FastControllerObject::dialogsPageDone(
		const MTPmessages_Dialogs &result) {
	const auto info = Export::Data::ParseDialogsInfo(result);
	for (const auto &dialog : info.chats) {
		if (MatchDialogsPeer(dialog, _options.peerId)) {
			_peer = dialog.input;
			_peerId = dialog.peerId;
			_chatName = QString::fromUtf8(dialog.name);
			addDebug(u"peer="_q + _chatName);
			if (_options.topicRootId) {
				startTopic();
			} else if (_options.messageId) {
				startMessage();
			} else {
				startMessages();
			}
			return;
		}
	}
	if (info.chats.empty()
		|| _dialogsPages >= kMaxDialogsPages) {
		fail(u"No chat matches peer_id in dialogs"_q);
		return;
	}
	const auto last = info.chats.back();
	++_dialogsPages;
	requestDialogsPage(
		last.topMessageId,
		last.topMessageDate,
		last.input);
}

void FastControllerObject::startMessages() {
	_nextOffsetId = 0;
	requestHistoryPage();
}

void FastControllerObject::requestHistoryPage() {
	_mtp.request(MTPmessages_GetHistory(
		_peer,
		MTP_int(_nextOffsetId),
		MTP_int(0), // offset_date
		MTP_int(0), // add_offset
		MTP_int(kMessagesLimit),
		MTP_int(0), // max_id
		MTP_int(0), // min_id
		MTP_long(0) // hash
	)).done([=](const MTPmessages_Messages &result) {
		if (stopped()) {
			return;
		}
		historyPageDone(result);
	}).fail([=](const MTP::Error &error) {
		fail(error.type());
	}).send();
}

void FastControllerObject::historyPageDone(
		const MTPmessages_Messages &result) {
	handleMessagesPage(result);
	if (_nextOffsetId > 0) {
		requestHistoryPage();
	} else {
		finishIfDone();
	}
}

void FastControllerObject::startMessage() {
	_mtp.request(MTPmessages_GetMessages(
		MTP_vector<MTPInputMessage>(
			1,
			MTP_inputMessageID(MTP_int(_options.messageId)))
	)).done([=](const MTPmessages_Messages &result) {
		if (stopped()) {
			return;
		}
		handleMessagesPage(result);
		if (!stopped()) {
			finishIfDone();
		}
	}).fail([=](const MTP::Error &error) {
		fail(error.type());
	}).send();
}

void FastControllerObject::startTopic() {
	_mtp.request(MTPchannels_GetMessages(
		MTP_inputChannel(
			_peer.c_inputPeerChannel().vchannel_id(),
			_peer.c_inputPeerChannel().vaccess_hash()),
		MTP_vector<MTPInputMessage>(
			1,
			MTP_inputMessageID(MTP_int(_options.topicRootId)))
	)).done([=](const MTPmessages_Messages &result) {
		if (stopped()) {
			return;
		}
		handleMessagesPage(result);
		if (!stopped()) {
			requestRepliesPage(0);
		}
	}).fail([=](const MTP::Error &error) {
		fail(error.type());
	}).send();
}

void FastControllerObject::requestRepliesPage(int offsetId) {
	_mtp.request(MTPmessages_GetReplies(
		_peer,
		MTP_int(_options.topicRootId),
		MTP_int(offsetId),
		MTP_int(0), // offset_date
		MTP_int(0), // add_offset
		MTP_int(kMessagesLimit),
		MTP_int(0), // max_id
		MTP_int(0), // min_id
		MTP_long(0) // hash
	)).done([=](const MTPmessages_Messages &result) {
		if (stopped()) {
			return;
		}
		repliesPageDone(result);
	}).fail([=](const MTP::Error &error) {
		fail(error.type());
	}).send();
}

void FastControllerObject::repliesPageDone(
		const MTPmessages_Messages &result) {
	handleMessagesPage(result);
	if (stopped()) {
		return;
	}
	if (_nextOffsetId > 0) {
		requestRepliesPage(_nextOffsetId);
	} else {
		finishIfDone();
	}
}

void FastControllerObject::handleMessagesPage(
		const MTPmessages_Messages &result) {
	auto slice = result.match([&](
			const MTPDmessages_messagesNotModified &) {
		return Export::Data::MessagesSlice();
	}, [&](const auto &data) {
		auto context = Export::Data::ParseMediaContext();
		context.selfPeerId = peerFromUser(UserId(qlonglong(_selfId)));
		return Export::Data::ParseMessagesSlice(
			context,
			data.vmessages(),
			data.vusers(),
			data.vchats(),
			_relativePath);
	});
	Q_UNUSED(addDebug(u"msgs="_q + QString::number(int(slice.list.size()))));
	if (slice.list.empty()) {
		_nextOffsetId = 0;
	} else {
		_nextOffsetId = slice.list.back().id;
	}
	collectFiles(std::move(slice));
}

void FastControllerObject::collectFiles(
		Export::Data::MessagesSlice &&slice) {
	auto settings = Export::Settings();
	settings.singlePeerFrom = _options.from;
	settings.singlePeerTill = _options.till;
	settings.media.sizeLimit = _options.sizeLimit;
	for (const auto &message : slice.list) {
		if (Export::Data::SkipMessageByDate(message, settings)) {
			continue;
		}
		const auto file = message.file();
		if (!file.location
			|| file.skipReason != Export::Data::File::SkipReason::None) {
			continue;
		}
		if (_options.sizeLimit > 0 && file.size > _options.sizeLimit) {
			continue;
		}
		if (file.size <= 0) {
			continue;
		}
		auto active = ActiveFile();
		active.location = file.location;
		active.size = file.size;
		active.path = fileName(file);
		_pendingFiles.push_back(std::move(active));
		_state.bytesTotalAll += file.size;
		++_state.filesTotal;
	}
	Q_UNUSED(slice);
	addDebug(u"page files="_q + QString::number(_state.filesTotal));
	emitState();
	pumpFiles();
}

QString FastControllerObject::mediaRelativePath() const {
	return _relativePath + u"media/"_q;
}

QString FastControllerObject::fileName(const Export::Data::File &file) const {
	const auto suggested = file.suggestedPath.trimmed();
	const auto name = suggested.isEmpty()
		? u"file_"_q + QString::number(file.size)
		: suggested;
	return mediaRelativePath() + name;
}

void FastControllerObject::pumpFiles() {
	if (stopped()) {
		return;
	}
	if (_pumping) {
		_pumpingAgain = true;
		return;
	}
	_pumping = true;
	do {
		_pumpingAgain = false;
		while (int(_activeFiles.size()) < _options.parallel
			&& !_pendingFiles.empty()) {
			auto file = std::move(_pendingFiles.front());
			_pendingFiles.erase(begin(_pendingFiles));
			const auto index = int(_activeFiles.size());
			_activeFiles.push_back(std::move(file));
			requestParts(index);
		}
		for (auto index = 0; index != int(_activeFiles.size());) {
			const auto file = &_activeFiles[index];
			if (!file->busy() && file->offset < file->size) {
				requestParts(index);
			} else {
				++index;
			}
		}
	} while (_pumpingAgain);
	_pumping = false;
	addDebug(u"pump a/="_q + QString::number(int(_activeFiles.size()))
		+ u"/"_q + QString::number(int(_pendingFiles.size())));
	emitState();
	finishIfDone();
}

void FastControllerObject::requestParts(int index) {
	const auto file = &_activeFiles[index];
	const auto finalPath = _options.dest + u"/"_q + file->path;
	addDebug(u"rp: "_q + file->path + u" sz="_q
		+ QString::number(file->size));
	if (QFile::exists(finalPath)
		&& QFileInfo(finalPath).size() == file->size) {
		addDebug(u"rp-skip-exists"_q);
		fileFinished(index);
		return;
	}
	const auto dirPath = file->path.mid(0, file->path.lastIndexOf('/'));
	if (!QDir(_options.dest).mkpath(dirPath)) {
		addDebug(u"rp-mkpath-fail: "_q + dirPath);
		fileFailed(index, u"IO error"_q);
		return;
	}
	addDebug(u"rp-issued"_q);
	const auto tempPath = finalPath + u".dltmp"_q;
	if (QFile::exists(tempPath)) {
		file->offset = std::min<qint64>(
			file->size,
			QFileInfo(tempPath).size());
	}
	file->requestId = uint64(_mtp.request(MTPupload_GetFile(
		MTP_flags(0),
		file->location.data,
		MTP_long(file->offset),
		MTP_int(kFileChunkSize)
	)).done([=](const MTPupload_File &result) {
		if (stopped()) {
			return;
		}
		const auto f = &_activeFiles[index];
		f->requestId = 0;
		partsDone(index, result);
	}).fail([=](const MTP::Error &error) {
		if (stopped()) {
			return;
		}
		const auto f = &_activeFiles[index];
		f->requestId = 0;
		fileFailed(index, error.type());
	}).toDC(MTP::downloadDcId(
		MTP::DcId(file->location.dcId),
		0)).send());
}

void FastControllerObject::partsDone(
		int index,
		const MTPupload_File &result) {
	const auto file = &_activeFiles[index];
	result.match([&](const MTPDupload_file &data) {
		const auto bytes = data.vbytes().v;
		addDebug(u"part +"_q
			+ QString::number(bytes.size())
			+ u"/"_q + QString::number(file->size));
		const auto finalPath = _options.dest + u"/"_q + file->path;
		const auto tempPath = finalPath + u".dltmp"_q;
		auto write = QFile(tempPath);
		const auto opened = write.open(QIODevice::WriteOnly)
			&& write.seek(file->offset)
			&& write.write(bytes);
		if (opened) {
			write.flush();
			file->offset += bytes.size();
			_state.bytesLoadedTotal = std::min<qint64>(
				_state.bytesLoadedTotal + bytes.size(),
				_state.bytesTotalAll);
			write.close();
			if (file->offset >= file->size) {
				fileFinished(index);
			} else {
				requestParts(index);
			}
		} else {
			write.close();
			fileFailed(index, u"IO error"_q);
		}
	}, [&](const MTPDupload_fileCdnRedirect &data) {
		Q_UNUSED(data);
		fileFailed(index, u"CDN redirect not supported"_q);
	});
	emitState();
	pumpFiles();
}

void FastControllerObject::fileFailed(int index, const QString &error) {
	if (stopped()) {
		return;
	}
	Q_UNUSED(error);
	_activeFiles.erase(begin(_activeFiles) + index);
	++_state.skippedFiles;
	emitState();
	pumpFiles();
}

void FastControllerObject::fileFinished(int index) {
	if (stopped()) {
		return;
	}
	const auto file = &_activeFiles[index];
	const auto finalPath = _options.dest + u"/"_q + file->path;
	const auto tempPath = finalPath + u".dltmp"_q;
	if (QFile::exists(tempPath)) {
		Q_UNUSED(QFile::rename(tempPath, finalPath));
	}
	++_state.filesFinished;
	addDebug(u"done: "_q + file->path + u" off="_q
		+ QString::number(file->offset));
	_activeFiles.erase(begin(_activeFiles) + index);
}

void FastControllerObject::finishIfDone() {
	if (stopped()) {
		return;
	}
	if (!_pendingFiles.empty() || !_activeFiles.empty()) {
		return;
	}
	_finished = true;
	addDebug(u"FINISH a/="_q + QString::number(int(_activeFiles.size()))
		+ u" p/="_q + QString::number(int(_pendingFiles.size())));
	auto state = JobState();
	state.status = JobState::Status::Finished;
	state.startedAt = _startedAt;
	state.debug = _state.debug;
	state.mainFile = _options.dest;
	state.filesCount = _state.filesFinished;
	state.bytesCount = _state.bytesLoadedTotal;
	state.filesFinished = _state.filesFinished;
	state.filesTotal = _state.filesTotal;
	state.bytesLoadedTotal = _state.bytesLoadedTotal;
	state.bytesTotalAll = _state.bytesTotalAll;
	state.dialogName = _chatName;
	setState(std::move(state));
}

} // namespace DownloadApi