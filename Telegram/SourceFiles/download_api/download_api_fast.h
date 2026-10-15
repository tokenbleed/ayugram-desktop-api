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
#include <QtCore/QSet>
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
		int64 nextOffset = 0;
		int64 received = 0;
		int inflight = 0;
		bool resumeChecked = false;
		bool failed = false;

		[[nodiscard]] bool busy() const {
			return inflight > 0;
		}
	};

	struct InFlight {
		int file = -1;
		mtpRequestId id = 0;
		int64 offset = 0;
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
	void requestSearchPage(int offsetId);
	void searchPageDone(const MTPmessages_Messages &result);
	void handleMessagesPage(const MTPmessages_Messages &result);
	void collectFiles(Export::Data::MessagesSlice &&slice);
	void pumpFiles();
	void requestParts(int index);
	void partsDone(int token, int64 offset, const MTPupload_File &result);
	void fileFailed(int index, const QString &error);
	void fileFinished(int index);
	void partReceived(int token, int64 offset, const QByteArray &bytes);
	void failPart(int token, const QString &error);
	void finishIfDone();

	QString mediaRelativePath() const;
	QString fileName(const Export::Data::File &file) const;
	QString uniquePath(const QString &path, int64 messageId);

	MTP::ConcurrentSender _mtp;
	QPointer<MTP::Instance> _instance;
	// Every destination this run has claimed, so a second message that
	// suggests an identical file name gets its own path instead of sharing
	// one .dltmp with the first.
	QSet<QString> _usedPaths;

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
	std::map<int, ActiveFile> _activeFiles;
	std::vector<InFlight> _inFlight;
	base::flat_set<int32> _listedIds;
	int _nextToken = 1;

	static constexpr auto kPartsPerFile = 5;
	static constexpr int kMediaSessions = 4;
	static constexpr int64 kPartSize = 1 * 1024 * 1024;

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