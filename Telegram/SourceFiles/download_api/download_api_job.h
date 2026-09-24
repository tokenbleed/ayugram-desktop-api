/*
This file is part of AyuGram Desktop,
the unofficial fork of Telegram Desktop with a built-in download API.

For license and copyright information please follow this link:
https://github.com/AyuGram/AyuGramDesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"
#include "base/flat_map.h"
#include "base/flat_set.h"
#include "base/timer.h"
#include "base/weak_ptr.h"

#include <rpl/rpl.h>

#include "base/unixtime.h"

#include "scheme.h"

#include "data/data_peer_id.h"

#include "export/export_api_wrap.h"
#include "export/export_settings.h"
#include "export/data/export_data_types.h"
#include "export/output/export_output_abstract.h"
#include "export/output/export_output_result.h"
#include "export/output/export_output_stats.h"

#include <QtCore/QPointer>
#include <crl/crl_object_on_queue.h>

#include <vector>

namespace MTP {
class Instance;
} // namespace MTP

namespace DownloadApi {

class ControllerObject;

struct JobOptions {
	enum class Mode {
		Download,
		List,
	};
	Mode mode = Mode::Download;
	QString dest;
	QString format;
	QString engine;
	QString chatSelector;
	QString peer;
	uint64 peerId = 0;
	int32 topicRootId = 0;
	int32 messageId = 0;
	int chatLimit = 0;
	int parallel = 3;
	Export::MediaSettings::Types mediaTypes = Export::MediaSettings::Type::AllMask;
	int64 sizeLimit = 0;
	TimeId from = 0;
	TimeId till = 0;
};

struct ListedChat {
	PeerId peerId = 0;
	QString name;
	QString typeName;
	TimeId date = 0;
};

struct JobState {
	enum class Status {
		Pending,
		Running,
		Finished,
		Cancelled,
		Error,
	};
	Status status = Status::Pending;
	QString error;
	QString mainFile;
	int filesCount = 0;
	int64 bytesCount = 0;
	int dialogIndex = 0;
	int dialogCount = 0;
	QString dialogName;
	int itemIndex = 0;
	int itemCount = 0;
	int64 bytesLoaded = 0;
	int64 bytesTotal = 0;
	int64 bytesLoadedTotal = 0;
	int64 bytesTotalAll = 0;
	int filesFinished = 0;
	int filesTotal = 0;
	int skippedFiles = 0;
	double bytesPerSecond = 0;
	double filesPerSecond = 0;
	int32 etaSeconds = 0;
	int64 startedAt = 0;
	QString debug;
	uint64 bytesRandomId = 0;
	std::vector<ListedChat> chats;
	QStringList candidates;
	QString notice;
};

class Controller final {
public:
	explicit Controller(QPointer<MTP::Instance> mtproto);
	Controller(const Controller &other) = delete;
	Controller &operator=(const Controller &other) = delete;
	~Controller();

	void start(JobOptions options, const Export::Environment &environment);
	void skipFile(uint64 randomId);
	void cancel();

	[[nodiscard]] rpl::producer<JobState> state() const;
	[[nodiscard]] rpl::lifetime &lifetime() {
		return _lifetime;
	}

private:
	using Implementation = ControllerObject;
	crl::object_on_queue<Implementation> _wrapped;
	rpl::lifetime _lifetime;

};

class ControllerObject {
public:
	ControllerObject(
		crl::weak_on_queue<ControllerObject> weak,
		QPointer<MTP::Instance> mtproto);

	[[nodiscard]] rpl::producer<JobState> state() const;
	void start(JobOptions options, const Export::Environment &environment);
	void skipFile(uint64 randomId);
	void cancel();

private:
	using DownloadProgress = Export::ApiWrap::DownloadProgress;

	void setState(JobState &&state);
	void fail(const QString &error);
	bool ioCatchError(const Export::Output::Result &result);
	void initialize();
	void initialized(const Export::ApiWrap::StartInfo &info);
	void collectDialogs();
	void dialogsCollected(Export::Data::DialogsInfo &&info);
	void selectDialogs();
	void exportTopic(const Export::Data::DialogInfo &info);
	void exportNextDialog();
	void finishJob();
	void fillProgressState(const DownloadProgress &progress);
	void fillDialogState();

	Export::ApiWrap _api;
	Export::Output::Stats _stats;
	Export::Settings _settings;
	JobOptions _options;
	Export::Environment _environment;
	std::unique_ptr<Export::Output::AbstractWriter> _writer;
	Export::Data::DialogsInfo _dialogs;
	std::vector<int> _queue;
	int _cursor = -1;
	int _messagesWritten = 0;
	int _messagesCount = 0;
	bool _cancelled = false;

	JobState _state;
	rpl::event_stream<JobState> _stateChanges;
	rpl::lifetime _lifetime;
	base::flat_map<uint64, int64> _loadedByFile;
	base::flat_set<uint64> _finishedFiles;

};

} // namespace DownloadApi