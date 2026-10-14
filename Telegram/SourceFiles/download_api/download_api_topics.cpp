/*
This file is part of AyuGram Desktop,
the unofficial fork of Telegram Desktop with a built-in download API.

For license and copyright information please follow this link:
https://github.com/AyuGram/AyuGramDesktop/blob/master/LEGAL
*/
#include "download_api/download_api_topics.h"

#include "download_api/download_api_pch.h"

#include "export/data/export_data_types.h"
#include "logs.h"

#include <QtCore/QDateTime>

#include <set>

namespace DownloadApi {
namespace {

using Json = JsonValue;

constexpr auto kTopicsLimit = 100;
constexpr auto kMaxTopicsPages = 200;
constexpr auto kDialogsLimit = 100;
constexpr auto kMaxDialogsPages = 10;

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

TopicsRequest::TopicsRequest(
		not_null<MTP::Instance*> instance,
		uint64 peerId,
		Fn<void(int status, JsonValue &&json)> done)
: QObject()
, MTP::Sender(instance)
, _instance(instance)
, _peerId(peerId)
, _done(std::move(done)) {
}

void TopicsRequest::start() {
	requestDialogsPage(0, 0, MTP_inputPeerEmpty());
}

void TopicsRequest::requestDialogsPage(
		int offsetId,
		int offsetDate,
		MTPInputPeer offsetPeer) {
	request(MTPmessages_GetDialogs(
		MTP_flags(0),
		MTPint(), // folder_id
		MTP_int(offsetDate),
		MTP_int(offsetId),
		offsetPeer,
		MTP_int(kDialogsLimit),
		MTP_long(0)
	)).done([=](const MTPmessages_Dialogs &result) {
		dialogsPageDone(result);
	}).fail([=](const MTP::Error &error) {
		fail(error.type());
	}).send();
}

void TopicsRequest::dialogsPageDone(
		const MTPmessages_Dialogs &result) {
	if (_finished) {
		return;
	}
	const auto info = Export::Data::ParseDialogsInfo(result);
	for (const auto &dialog : info.chats) {
		if (MatchDialogsPeer(dialog, _peerId)) {
			_peer = dialog.input;
			_chatName = QString::fromUtf8(dialog.name);
			requestTopicsPage();
			return;
		}
	}
	if (info.chats.empty() || _dialogsPages >= kMaxDialogsPages) {
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

void TopicsRequest::requestTopicsPage() {
	request(MTPmessages_GetForumTopics(
		MTP_flags(0),
		_peer,
		MTP_string(QString()), // no search query
		MTP_int(_offsetDate),
		MTP_int(_offsetId),
		MTP_int(_offsetTopic),
		MTP_int(kTopicsLimit)
	)).done([=](const MTPmessages_ForumTopics &result) {
		topicsPageDone(result);
	}).fail([=](const MTP::Error &error) {
		fail(error.type());
	}).send();
}

void TopicsRequest::topicsPageDone(
		const MTPmessages_ForumTopics &result) {
	if (_finished) {
		return;
	}
	++_pages;
	auto fresh = 0;
	const auto &data = result.data();
	_total = data.vcount().v;
	for (const auto &topic : data.vtopics().v) {
		// A deleted topic is a tombstone (it carries only the id); it still
		// counts toward the server total, so record it as hidden rather than
		// skipping it, or the page walk could loop on the same offsets.
		topic.match([&](const MTPDforumTopic &t) {
			auto entry = Topic();
			entry.id = t.vid().v;
			entry.date = t.vdate().v;
			entry.title = QString::fromUtf8(t.vtitle().v);
			entry.topMessage = t.vtop_message().v;
			entry.closed = t.is_closed();
			entry.hidden = t.is_hidden();
			_offsetDate = entry.date;
			_offsetId = entry.topMessage;
			_offsetTopic = entry.id;
			if (_topics.emplace(entry.id, entry).second) {
				++fresh;
			}
		}, [&](const MTPDforumTopicDeleted &t) {
			auto entry = Topic();
			entry.id = t.vid().v;
			entry.hidden = true;
			_offsetTopic = entry.id;
			if (_topics.emplace(entry.id, entry).second) {
				++fresh;
			}
		});
	}
	if (_topics.empty()) {
		fail(u"No topics returned"_q);
		return;
	}
	const auto done = (fresh == 0)
		|| (int32(_topics.size()) >= _total && _total > 0)
		|| (_pages >= kMaxTopicsPages);
	if (done) {
		finish();
	} else {
		requestTopicsPage();
	}
}

void TopicsRequest::finish() {
	if (_finished) {
		return;
	}
	auto topics = std::vector<JsonValue>();
	topics.reserve(_topics.size());
	for (const auto &[id, topic] : _topics) {
		topics.push_back(Json::Object({
			{ u"id"_q, Json::Number(qint64(topic.id)) },
			{ u"title"_q, Json::String(topic.title) },
			{ u"date"_q, Json::Number(qint64(topic.date)) },
			{ u"top_message"_q, Json::Number(qint64(topic.topMessage)) },
			{ u"closed"_q, Json::Bool(topic.closed) },
			{ u"hidden"_q, Json::Bool(topic.hidden) },
		}));
	}
	answer(200, Json::Object({
		{ u"ok"_q, Json::Bool(true) },
		{ u"peer_id"_q, Json::Number(qint64(_peerId)) },
		{ u"chat_name"_q, Json::String(_chatName) },
		{ u"count"_q, Json::Number(qint64(_topics.size())) },
		{ u"server_count"_q, Json::Number(qint64(_total)) },
		{ u"topics"_q, Json::Array(std::move(topics)) },
	}));
}

void TopicsRequest::fail(const QString &error) {
	if (_finished) {
		return;
	}
	LOG(("DownloadApi Topics Error: %1.").arg(error));
	answer(500, Json::Object({
		{ u"ok"_q, Json::Bool(false) },
		{ u"error"_q, Json::String(error) },
	}));
}

void TopicsRequest::answer(int status, JsonValue &&json) {
	_finished = true;
	if (_done) {
		_done(status, std::move(json));
	}
	deleteLater();
}

} // namespace DownloadApi
