/*
This file is part of AyuGram Desktop,
the unofficial fork of Telegram Desktop with a built-in download API.

For license and copyright information please follow this link:
https://github.com/AyuGram/AyuGramDesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"

#include <rpl/rpl.h>

#include "base/unixtime.h"

#include "scheme.h"

#include "download_api/download_api_job.h"

#include "export/export_api_wrap.h"

#include "mtproto/facade.h"
#include "mtproto/mtproto_concurrent_sender.h"
#include "mtproto/mtproto_response.h"

#include <QtCore/QPointer>
#include <QtCore/QFile>
#include <crl/crl_object_on_queue.h>

#include <map>
#include <vector>

namespace MTP {
class Instance;
} // namespace MTP

namespace DownloadApi {

class FastControllerObject {
public:
	FastControllerObject(
		crl::weak_on_queue<FastControllerObject> weak,
		QPointer<MTP::Instance> mtproto);

	[[nodiscard]] rpl::producer<JobState> state() const;
	void start(JobOptions options);
	void cancel();

private:
	class ActiveFile {
	public:
		Export::Data::FileLocation location;
		int64 size = 0;
		QString path;
		int64 offset = 0;
		mtpRequestId requestId = 0;

		[[nodiscard]] bool busy() const {
			return requestId != 0;
		}
	};

	void setState(JobState &&state);
	void emitState();
	void addDebug(const QString &line);
	void fail(const QString &error);
	bool stopped() const;

	void requestSelf();
	void requestDialogsPage(int offsetId, int offsetDate, MTPInputPeer offsetPeer);
	void dialogsPageDone(const MTPmessages_Dialogs &result);
	void startMessages();
	void requestHistoryPage();
	void historyPageDone(const MTPmessages_Messages &result);
	void startTopic();
	void startMessage();
	void requestRepliesPage(int offsetId);
	void repliesPageDone(const MTPmessages_Messages &result);
	void handleMessagesPage(const MTPmessages_Messages &result);
	void collectFiles(Export::Data::MessagesSlice &&slice);
	void pumpFiles();
	void requestParts(int index);
	void partsDone(int index, const MTPupload_File &result);
	void fileFailed(int index, const QString &error);
	void fileFinished(int index);
	void finishIfDone();

	QString mediaRelativePath() const;
	QString fileName(const Export::Data::File &file) const;

	MTP::ConcurrentSender _mtp;

	JobOptions _options;
	uint64 _selfId = 0;
	bool _selfRequestDone = false;
	Export::Data::DialogsInfo _dialogsInfo;
	MTPInputPeer _peer = MTP_inputPeerEmpty();
	PeerId _peerId = 0;
	QString _chatName;
	int _nextOffsetId = 0;
	QString _relativePath;
	int _dialogsLimit = 100;

	std::vector<ActiveFile> _pendingFiles;
	std::vector<ActiveFile> _activeFiles;

	bool _started = false;
	bool _cancelled = false;
	bool _finished = false;
	bool _walkDone = false;
	bool _pumping = false;
	bool _pumpingAgain = false;
	int _dialogsPages = 0;
	int64 _startedAt = 0;
	int64 _lastAt = 0;
	int64 _lastBytes = 0;
	int64 _lastFiles = 0;
	double _lastBps = 0;
	double _lastFilesBps = 0;

	JobState _state;
	rpl::event_stream<JobState> _stateChanges;
	rpl::lifetime _lifetime;

};

class FastController final {
public:
	explicit FastController(QPointer<MTP::Instance> mtproto);
	FastController(const FastController &other) = delete;
	FastController &operator=(const FastController &other) = delete;
	~FastController();

	void start(JobOptions options);
	void cancel();

	[[nodiscard]] rpl::producer<JobState> state() const;
	[[nodiscard]] rpl::lifetime &lifetime() {
		return _lifetime;
	}

private:
	using Implementation = FastControllerObject;
	crl::object_on_queue<Implementation> _wrapped;
	rpl::lifetime _lifetime;

};

} // namespace DownloadApi