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

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

#include <QtCore/QDateTime>
#include <QtCore/QDir>

#include <algorithm>
#include <cmath>
#include <set>

namespace DownloadApi {
namespace {

constexpr auto kFileChunkSize = 8 * 128 * 1024;
constexpr auto kMessagesLimit = 100;
constexpr auto kDialogsLimit = 100;
constexpr auto kMaxDialogsPages = 5;

// messages.search takes min_date/max_date as exclusive bounds ("sending date
// bigger/smaller than the transferred one"), while the bulk API's
// since/until mean from <= date < till (SkipMessageByDate). Lower the min
// bound by one second so the first message of a window still matches; the
// upper bound is exclusive on both sides, so till passes through unchanged.
// Zero leaves a bound unset, which keeps the whole-channel walk unfiltered.
[[nodiscard]] MTPint SearchMinDate(TimeId from) {
	return (from > 0) ? MTP_int(from - 1) : MTP_int(0);
}

[[nodiscard]] MTPint SearchMaxDate(TimeId till) {
	return (till > 0) ? MTP_int(till) : MTP_int(0);
}

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
, _instance(mtproto)
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
	state.messages = _state.messages;
	state.texts = _state.texts;
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
	state.messages = _state.messages;
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
	// A document walk and a queried walk both belong on messages.search, which
	// pages only the matching messages. messages.getHistory would page every
	// message in the channel, which for a files channel is mostly attachments
	// and so does the same work in far more requests.
	if (_options.documentsOnly || !_options.query.isEmpty()) {
		requestChannelSearchPage(0);
	} else {
		requestHistoryPage();
	}
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
		_walkDone = true;
		pumpFiles();
	}
}

void FastControllerObject::startMessage() {
	addDebug(u"startMessage id="_q + QString::number(_options.messageId));
	if (_peer.type() == mtpc_inputPeerChannel) {
		_mtp.request(MTPchannels_GetMessages(
			MTP_inputChannel(
				_peer.c_inputPeerChannel().vchannel_id(),
				_peer.c_inputPeerChannel().vaccess_hash()),
			MTP_vector<MTPInputMessage>(
				1,
				MTP_inputMessageID(MTP_int(_options.messageId)))
		)).done([=](const MTPmessages_Messages &result) {
			if (stopped()) {
				return;
			}
			addDebug(u"channel fetch done"_q);
			handleMessagesPage(result);
			if (!stopped()) {
				finishIfDone();
			}
		}).fail([=](const MTP::Error &error) {
			fail(error.type());
		}).send();
		return;
	}
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
			if (_options.documentsOnly) {
				requestSearchPage(0);
			} else {
				requestRepliesPage(0);
			}
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
		_walkDone = true;
		pumpFiles();
	}
}

// requestSearchPage walks a topic with messages.search and the document filter.
//
// Paging every reply (messages.getReplies) costs one request per hundred
// messages in the topic, and the messages that matter are a small fraction of
// them. Measured on the pvt forum: a topic needed hundreds of pages to find a
// few files. The same topic answers in a handful of search pages, because the
// server returns only the messages that carry documents.
void FastControllerObject::requestSearchPage(int offsetId) {
	using Flag = MTPmessages_Search::Flag;
	_mtp.request(MTPmessages_Search(
		MTP_flags(Flag::f_top_msg_id),
		_peer,
		MTP_string(_options.query),
		MTP_inputPeerEmpty(),
		MTP_inputPeerEmpty(),
		MTP_vector<MTPReaction>(),
		MTP_int(_options.topicRootId),
		MTP_inputMessagesFilterDocument(),
		SearchMinDate(_options.from), // from-1: server min_date is exclusive
		SearchMaxDate(_options.till), // till: exclusive upper on both sides
		MTP_int(offsetId),
		MTP_int(0), // add_offset
		MTP_int(kMessagesLimit),
		MTP_int(0), // max_id
		MTP_int(0), // min_id
		MTP_long(0) // hash
	)).done([=](const MTPmessages_Messages &result) {
		if (stopped()) {
			return;
		}
		searchPageDone(result);
	}).fail([=](const MTP::Error &error) {
		fail(error.type());
	}).send();
}

void FastControllerObject::requestChannelSearchPage(int offsetId) {
	_mtp.request(MTPmessages_Search(
		MTP_flags(0), // no topic: the whole peer
		_peer,
		MTP_string(_options.query),
		MTP_inputPeerEmpty(),
		MTP_inputPeerEmpty(),
		MTP_vector<MTPReaction>(),
		MTP_int(0), // no top_msg_id
		_options.documentsOnly
			? MTPMessagesFilter(MTP_inputMessagesFilterDocument())
			: MTPMessagesFilter(MTP_inputMessagesFilterEmpty()),
		SearchMinDate(_options.from), // from-1: server min_date is exclusive
		SearchMaxDate(_options.till), // till: exclusive upper on both sides
		MTP_int(offsetId),
		MTP_int(0), // add_offset
		MTP_int(kMessagesLimit),
		MTP_int(0), // max_id
		MTP_int(0), // min_id
		MTP_long(0) // hash
	)).done([=](const MTPmessages_Messages &result) {
		if (stopped()) {
			return;
		}
		channelSearchPageDone(result);
	}).fail([=](const MTP::Error &error) {
		fail(error.type());
	}).send();
}

void FastControllerObject::channelSearchPageDone(
		const MTPmessages_Messages &result) {
	handleMessagesPage(result);
	if (stopped()) {
		return;
	}
	if (_nextOffsetId > 0) {
		requestChannelSearchPage(_nextOffsetId);
	} else {
		_walkDone = true;
		pumpFiles();
	}
}

void FastControllerObject::searchPageDone(
		const MTPmessages_Messages &result) {
	handleMessagesPage(result);
	if (stopped()) {
		return;
	}
	if (_nextOffsetId > 0) {
		requestSearchPage(_nextOffsetId);
	} else {
		_walkDone = true;
		pumpFiles();
	}
}

void FastControllerObject::handleMessagesPage(
		const MTPmessages_Messages &result) {
	addDebug(u"HMP type="_q + QString::number(int(result.type())));
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
	if (_options.mode == JobOptions::Mode::List) {
		// Every message's text, whether or not it carries a file. A post that
		// only pastes a Mega or Gofile link has no attachment, so a walk that
		// records files alone cannot see it, and the link harvest needs it.
		auto textsAdded = 0;
		for (const auto &m : slice.list) {
			if (_state.texts.size() >= 200000) {
				break;
			}
			if (!_textIds.insert(m.id).second) {
				continue;
			}
			auto text = QString();
			for (const auto &part : m.text) {
				// A text-url keeps its target in `additional`; `text` is the
				// visible label. The target is what the harvest wants.
				const auto &value =
					(part.type == Export::Data::TextPart::Type::TextUrl
						&& !part.additional.isEmpty())
					? part.additional
					: part.text;
				if (value.isEmpty()) {
					continue;
				}
				if (!text.isEmpty()) {
					text += QChar(' ');
				}
				text += QString::fromUtf8(value);
			}
			if (text.isEmpty()) {
				continue;
			}
			auto entry = MessageText();
			entry.id = m.id;
			entry.date = m.date;
			entry.text = text;
			_state.texts.push_back(std::move(entry));
			++textsAdded;
		}
		if (textsAdded > 0) {
			addDebug(u"texts+"_q + QString::number(textsAdded)
				+ u"="_q + QString::number(int(_state.texts.size())));
		} else if (!slice.list.empty()) {
			auto parts = 0;
			for (const auto &m : slice.list) {
				parts += int(m.text.size());
			}
			addDebug(u"notext n="_q + QString::number(int(slice.list.size()))
				+ u" parts="_q + QString::number(parts));
		}
	}
	if (_options.messageId || _options.topicRootId) {
		result.match([](const MTPDmessages_messagesNotModified &) {
		}, [&](const auto &data) {
			for (const auto &msg : data.vmessages().v) {
				msg.match([&](const MTPDmessage &d) {
					auto media = u"none"_q;
					if (const auto &m = d.vmedia()) {
						media = QString::number(int(m->type()));
					}
					addDebug(u"raw msg="_q + QString::number(d.vid().v)
						+ u" media="_q + media);
				}, [&](const MTPDmessageService &d) {
					auto action = d.vaction();
					auto title = QString();
					if (action.type() == mtpc_messageActionTopicCreate) {
						title = u" title=\""_q
							+ QString::fromUtf8(
								action.c_messageActionTopicCreate()
									.vtitle().v)
							+ u"\""_q;
					}
					addDebug(u"raw svc msg="_q + QString::number(d.vid().v)
						+ u" action="_q + QString::number(int(action.type()))
						+ title);
				}, [](const MTPDmessageEmpty &) {
					// empty
				}, [&](const auto &) {
					addDebug(u"raw other construct"_q);
				});
			}
			addDebug(u"raw vector n="_q
				+ QString::number(int(data.vmessages().v.size())));
		});
		for (const auto &m : slice.list) {
			auto preview = QString();
			for (const auto &p : m.text) {
				const auto t = (p.type == Export::Data::TextPart::Type::Url)
					? u"URL"_q
					: (p.type == Export::Data::TextPart::Type::TextUrl)
					? u"TURL"_q
					: u"TXT"_q;
				preview += u"["_q + t + u"]"_q
					+ QString::fromUtf8(p.text).left(140);
			}
			addDebug(u"msg "_q + QString::number(m.id)
				+ u": "_q + preview.left(700));
		}
	}
	if (slice.list.empty()
		|| int(slice.list.size()) < kMessagesLimit) {
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
		if (_state.messages.size() < 100000
			&& _listedIds.contains(message.id)) {
			continue;
		}
		if (_state.messages.size() < 100000
			&& (_options.mode == JobOptions::Mode::List
				|| _options.messageId || _options.topicRootId)) {
			_listedIds.insert(message.id);
			auto listed = ListedMessage();
			listed.id = message.id;
			listed.date = message.date;
			listed.name = file.suggestedPath.trimmed();
			listed.size = file.size;
			_state.messages.push_back(std::move(listed));
		}
		if (_options.mode == JobOptions::Mode::List) {
			continue;
		}
		auto active = ActiveFile();
		active.location = file.location;
		active.size = file.size;
		active.path = uniquePath(fileName(file), message.id);
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

// uniquePath keeps two messages that suggest the same file name from sharing
// one destination. A topic can hold the same "MOON HQ HOTS.txt" twice with
// different contents, and one relative path means one .dltmp: the parts of
// both files interleave, the size check fails, and both are thrown away as
// short. The message id disambiguates, which leaves every existing path (and
// therefore every finished download) exactly where it already is.
QString FastControllerObject::uniquePath(
		const QString &path,
		int64 messageId) {
	if (!_usedPaths.contains(path)) {
		_usedPaths.insert(path);
		return path;
	}
	const auto slash = path.lastIndexOf('/');
	const auto dot = path.lastIndexOf('.');
	auto stem = path;
	auto ext = QString();
	if (dot > slash) {
		stem = path.left(dot);
		ext = path.mid(dot);
	}
	auto candidate = stem + u"."_q + QString::number(messageId) + ext;
	for (auto n = 2; _usedPaths.contains(candidate); ++n) {
		candidate = stem + u"."_q + QString::number(messageId)
			+ u"-"_q + QString::number(n) + ext;
	}
	addDebug(u"NAME COLLISION "_q + path.left(50)
		+ u" -> "_q + candidate.left(60));
	_usedPaths.insert(candidate);
	return candidate;
}

QString FastControllerObject::fileName(const Export::Data::File &file) const {
	const auto suggested = file.suggestedPath.trimmed();
	const auto name = suggested.isEmpty()
		? u"file_"_q + QString::number(file.size)
		: suggested;
	return mediaRelativePath() + name;
}

namespace {

// allocatedBytes reports the bytes actually allocated for a path (st_blocks *
// 512), or -1 where the platform cannot say. A file whose bytes never arrived
// is sparse: its size is right while its allocation is not.
qint64 allocatedBytes(const QString &path) {
#ifdef Q_OS_WIN
	Q_UNUSED(path);
	return -1;
#else
	struct stat info = {};
	const auto utf8 = path.toUtf8();
	if (::stat(utf8.constData(), &info) != 0) {
		return -1;
	}
	return qint64(info.st_blocks) * 512;
#endif
}

} // namespace

void FastControllerObject::partReceived(
		int token,
		int64 offset,
		const QByteArray &bytes) {
	const auto i = _activeFiles.find(token);
	if (i == end(_activeFiles)) {
		return;
	}
	const auto file = &i->second;
	if (file->failed) {
		return;
	}
	const auto finalPath = _options.dest + u"/"_q + file->path;
	const auto tempPath = finalPath + u".dltmp"_q;
	auto write = QFile(tempPath);
	// ReadWrite, not WriteOnly: Qt's WriteOnly implies Truncate, so with
	// kPartsPerFile parts in flight every part wiped the file it was
	// writing into and the result was one chunk plus holes.
	const auto opened = write.open(QIODevice::ReadWrite)
		&& write.seek(offset)
		&& write.write(bytes);
	if (opened) {
		write.flush();
		write.close();
		file->received += bytes.size();
		_state.bytesLoadedTotal = std::min<qint64>(
			_state.bytesLoadedTotal + bytes.size(),
			_state.bytesTotalAll);
		--file->inflight;
		if (file->received >= file->size && file->inflight == 0) {
			fileFinished(token);
		}
	} else {
		write.close();
		failPart(token, u"IO error"_q);
	}
}

void FastControllerObject::failPart(int token, const QString &error) {
	const auto i = _activeFiles.find(token);
	if (i == end(_activeFiles)) {
		return;
	}
	const auto file = &i->second;
	if (!file->failed) {
		file->failed = true;
		addDebug(u"FAILPART "_q + error
			+ u" path="_q + file->path);
	}
	--file->inflight;
	if (file->inflight == 0) {
		++_state.skippedFiles;
		_activeFiles.erase(token);
	}
	emitState();
	pumpFiles();
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
			const auto token = _nextToken++;
			_activeFiles.emplace(token, std::move(file));
		}
		auto tokens = std::vector<int>();
		for (const auto &entry : _activeFiles) {
			const auto file = &entry.second;
			if (!file->failed
				&& file->inflight < kPartsPerFile
				&& file->nextOffset < file->size) {
				tokens.push_back(entry.first);
			}
		}
		for (const auto token : tokens) {
			requestParts(token);
		}
	} while (_pumpingAgain);
	_pumping = false;
	addDebug(u"pump a/="_q + QString::number(int(_activeFiles.size()))
		+ u"/"_q + QString::number(int(_pendingFiles.size())));
	emitState();
	if (_walkDone) {
		finishIfDone();
	}
}

void FastControllerObject::requestParts(int token) {
	const auto i = _activeFiles.find(token);
	if (i == end(_activeFiles)) {
		return;
	}
	const auto file = &i->second;
	if (file->failed || file->inflight >= kPartsPerFile
		|| file->nextOffset >= file->size) {
		return;
	}
	const auto finalPath = _options.dest + u"/"_q + file->path;
	if (!file->resumeChecked) {
		file->resumeChecked = true;
		const auto dirPath = file->path.mid(0, file->path.lastIndexOf('/'));
		if (!QDir(_options.dest).mkpath(dirPath)) {
			fileFailed(token, u"IO error"_q);
			return;
		}
		if (QFile::exists(finalPath)
			&& QFileInfo(finalPath).size() == file->size) {
			// Size alone is not evidence. A file whose bytes never arrived
			// is sparse: it reports the right size while almost nothing is
			// allocated, and trusting the size would silently accept it.
			const auto allocated = allocatedBytes(finalPath);
			if (allocated < 0 || allocated >= file->size) {
				file->received = file->size;
				file->nextOffset = file->size;
				fileFinished(token);
				return;
			}
			addDebug(u"REDO sparse "_q + file->path
				+ u" allocated="_q + QString::number(allocated)
				+ u" size="_q + QString::number(file->size));
		}
		// A partial with all of its bytes is finished work from an
		// interrupted run: promote it rather than downloading it again.
		const auto tempPath = finalPath + u".dltmp"_q;
		if (QFile::exists(tempPath)
			&& QFileInfo(tempPath).size() == file->size
			&& allocatedBytes(tempPath) >= file->size) {
			file->received = file->size;
			file->nextOffset = file->size;
			fileFinished(token);
			return;
		}
		// Anything else is downloaded again from the start: parts arrive in
		// parallel, so a partial's size is the highest offset written, not a
		// watermark of what is present.
	}
	const auto offset = file->nextOffset;
	file->nextOffset += kPartSize;
	++file->inflight;
	{
		// Once per file: record the dispatch + live session state, and
		// restart all MTP sessions once per process (a download-DC session
		// that died or was never established never reconnects on its own in
		// this headless build; requests sent to it queue forever silently).
		static std::set<int> restarted;
		const auto shifted = int(MTP::downloadDcId(
			MTP::DcId(file->location.dcId),
			token % kMediaSessions));
		const auto inst = _instance.data();
		addDebug(u"DISPATCH dc="_q
			+ QString::number(int(file->location.dcId))
			+ u" state="_q
			+ QString::number(inst
				? inst->dcstate(MTP::ShiftedDcId(shifted))
				: -999)
			+ u" path="_q + file->path.left(40));
		if (!restarted.count(shifted)) {
			restarted.insert(shifted);
			if (inst) {
				addDebug(u"RESTART all sessions"_q);
				inst->restart();
			}
		}
	}
	Q_UNUSED(_mtp.request(MTPupload_GetFile(
		MTP_flags(0),
		file->location.data,
		MTP_long(offset),
		MTP_int(kPartSize)
	)).done([=](const MTPupload_File &result) {
		if (stopped()) {
			return;
		}
		partsDone(token, offset, result);
	}).fail([=](const MTP::Error &error) {
		if (stopped()) {
			return;
		}
		failPart(token, error.type());
	}).toDC(MTP::downloadDcId(
		MTP::DcId(file->location.dcId),
		token % kMediaSessions)).send());
}

void FastControllerObject::partsDone(
		int token,
		int64 offset,
		const MTPupload_File &result) {
	if (_activeFiles.find(token) == end(_activeFiles)) {
		return;
	}
	result.match([&](const MTPDupload_file &data) {
		partReceived(token, offset, data.vbytes().v);
	}, [&](const MTPDupload_fileCdnRedirect &data) {
		Q_UNUSED(data);
		failPart(token, u"CDN redirect not supported"_q);
	});
	emitState();
	pumpFiles();
}

void FastControllerObject::fileFailed(int token, const QString &error) {
	Q_UNUSED(error);
	const auto i = _activeFiles.find(token);
	if (i == end(_activeFiles)) {
		return;
	}
	auto file = &i->second;
	file->failed = true;
	if (file->inflight == 0) {
		++_state.skippedFiles;
		_activeFiles.erase(token);
	}
	emitState();
	pumpFiles();
}

void FastControllerObject::fileFinished(int token) {
	const auto i = _activeFiles.find(token);
	if (i == end(_activeFiles)) {
		return;
	}
	const auto file = &i->second;
	const auto finalPath = _options.dest + u"/"_q + file->path;
	const auto tempPath = finalPath + u".dltmp"_q;
	const auto expected = file->size;
	if (QFile::exists(tempPath)) {
		auto check = QFile(tempPath);
		const auto onDisk = check.open(QIODevice::ReadOnly)
			? check.size()
			: 0;
		check.close();
		// Size is not evidence: the parts are written at their offsets, so a
		// file still missing regions already reports its full size, and a
		// retried part is counted twice in the byte accounting, which is
		// enough to finish a file early. Allocation is what says the bytes
		// are there.
		const auto allocated = allocatedBytes(tempPath);
		if (onDisk != expected || allocated < expected) {
			// The bytes on disk are the only evidence a transfer worked.
			// A short file must never be renamed to the finished name:
			// leave the .dltmp in place and say what happened.
			++_state.skippedFiles;
			addDebug(u"SHORT "_q + file->path
				+ u" on-disk="_q + QString::number(onDisk)
				+ u" allocated="_q + QString::number(allocated)
				+ u" expected="_q + QString::number(expected));
			_activeFiles.erase(token);
			return;
		}
		// QFile::rename refuses to overwrite, so a stale file at the
		// destination would keep the finished bytes stranded in .dltmp
		// while the job reported success. Replace explicitly and check.
		if (QFile::exists(finalPath) && !QFile::remove(finalPath)) {
			++_state.skippedFiles;
			addDebug(u"REMOVE failed "_q + file->path);
			_activeFiles.erase(token);
			return;
		}
		if (!QFile::rename(tempPath, finalPath)) {
			++_state.skippedFiles;
			addDebug(u"RENAME failed "_q + file->path);
			_activeFiles.erase(token);
			return;
		}
	}
	++_state.filesFinished;
	_activeFiles.erase(token);
	// A finished file frees a parallel slot; promotions (resume path) have
	// no network callback that would re-pump, so do it here or the queue
	// starves after exactly `parallel` on-disk files.
	emitState();
	pumpFiles();
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
	state.messages = _state.messages;
	state.texts = _state.texts;
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