/*
This file is part of AyuGram Desktop,
the unofficial fork of Telegram Desktop with a built-in download API.

For license and copyright information please follow this link:
https://github.com/AyuGram/AyuGramDesktop/blob/master/LEGAL
*/
#pragma once

#include "download_api/download_api_pch.h"

#include "download_api/download_api_json.h"

#include "base/basic_types.h"

#include "scheme.h"
#include "mtproto/sender.h"

#include <QtCore/QObject>

#include <map>

namespace MTP {
class Instance;
} // namespace MTP

namespace DownloadApi {

// Enumerates every forum topic of one channel through messages.getForumTopics,
// paging until the server-reported count is reached, so the caller can learn
// which topics exist (id + title) without having harvested them first.
//
// The channel is resolved through a messages.getDialogs walk, the same way
// the download jobs do it, because the download_api target deliberately does
// not link the session data layer.
class TopicsRequest final : public QObject, private MTP::Sender {
public:
	TopicsRequest(
		not_null<MTP::Instance*> instance,
		uint64 peerId,
		Fn<void(int status, JsonValue &&json)> done);

	void start();

private:
	struct Topic {
		int32 id = 0;
		QString title;
		int32 date = 0;
		int32 topMessage = 0;
		bool closed = false;
		bool hidden = false;
	};

	void requestDialogsPage(
		int offsetId,
		int offsetDate,
		MTPInputPeer offsetPeer);
	void dialogsPageDone(const MTPmessages_Dialogs &result);
	void requestTopicsPage();
	void topicsPageDone(const MTPmessages_ForumTopics &result);
	void finish();
	void fail(const QString &error);
	void answer(int status, JsonValue &&json);

	not_null<MTP::Instance *> _instance;
	uint64 _peerId = 0;
	Fn<void(int status, JsonValue &&json)> _done;
	QString _chatName;

	MTPInputPeer _peer;
	std::map<int32, Topic> _topics;
	int32 _total = 0;
	int32 _offsetDate = 0;
	int32 _offsetId = 0;
	int32 _offsetTopic = 0;
	int _pages = 0;
	int _passes = 0;
	int _passFresh = 0;
	int _dialogsPages = 0;
	bool _finished = false;

	rpl::lifetime _lifetime;

};

} // namespace DownloadApi
