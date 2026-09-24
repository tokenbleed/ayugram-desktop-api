/*
This file is part of AyuGram Desktop,
the unofficial fork of Telegram Desktop with a built-in download API.

For license and copyright information please follow this link:
https://github.com/AyuGram/AyuGramDesktop/blob/master/LEGAL
*/
#pragma once

#include "base/function.h"
#include "base/unixtime.h"

#include "download_api/download_api_http.h"
#include "download_api/download_api_job.h"
#include "download_api/download_api_json.h"

#include "main/main_domain.h"

#include <QtCore/QObject>

namespace DownloadApi {

class Manager final : public QObject {
public:
	explicit Manager(not_null<Main::Domain*> domain);
	~Manager();

	void start();
	void stop();

private:
	class Job final {
	public:
		Job(int id, JobOptions &&options);

		int id = 0;
		JobOptions options;
		TimeId createdAt = 0;
		JobState state;
		std::unique_ptr<Controller> controller;
		rpl::lifetime lifetime;

	};

	void handleRequest(quint64 id, HttpRequest &&request);
	void respondJson(quint64 id, int status, JsonValue &&json);
	void respondRaw(quint64 id, int status, QByteArray body);
	void methodNotAllowed(quint64 id);
	void notFound(quint64 id);

	void handleRoot(quint64 id);
	void handleHealth(quint64 id);
	void handleCreateJob(quint64 id, bool list, const QByteArray &body);
	void handleJobsList(quint64 id);
	void handleJobById(quint64 id, const QString &jobId);
	void handleCancel(quint64 id, const QString &jobId);
	void handleSkip(quint64 id, const QString &jobId, const QByteArray &body);

	Job *jobById(const QString &jobId) const;
	void maybeStartNext();
	void cancelAll(const QString &reason);

	[[nodiscard]] JsonValue writeJobJson(const Job &job) const;

	not_null<Main::Domain*> _domain;
	Main::Session *_session = nullptr;
	HttpServer *_server = nullptr;
	QString _token;
	std::vector<std::unique_ptr<Job>> _jobs;
	int _nextJobId = 1;
	rpl::lifetime _lifetime;

};

} // namespace DownloadApi