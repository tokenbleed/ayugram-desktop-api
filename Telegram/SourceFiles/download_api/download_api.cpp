/*
This file is part of AyuGram Desktop,
the unofficial fork of Telegram Desktop with a built-in download API.

For license and copyright information please follow this link:
https://github.com/AyuGram/AyuGramDesktop/blob/master/LEGAL
*/
#include "download_api/download_api_pch.h"

#include "download_api/download_api_json.h"

#include "ui/style/style_core_direction.h"

using namespace st;

#include "data/data_types.h"
#include "data/data_peer.h"

#include "export/export_settings.h"

#include "lang/lang_keys.h"
#include "logs.h"
#include "main/main_session.h"
#include "mtproto/mtproto_config.h"
#include "mtproto/mtp_instance.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QUuid>

#include <algorithm>

namespace DownloadApi {
namespace {

using Json = JsonValue;
using JsonFields = std::vector<std::pair<QString, JsonValue>>;

Export::Environment PrepareEnvironment(not_null<Main::Session*> session) {
	auto result = Export::Environment();
	result.internalLinksDomain = session->serverConfig().internalLinksDomain;
	result.aboutTelegram = tr::lng_export_about_telegram(tr::now).toUtf8();
	result.aboutContacts = tr::lng_export_about_contacts(tr::now).toUtf8();
	result.aboutFrequent = tr::lng_export_about_frequent(tr::now).toUtf8();
	result.aboutSessions = tr::lng_export_about_sessions(tr::now).toUtf8();
	result.aboutWebSessions = tr::lng_export_about_web_sessions(tr::now).toUtf8();
	result.aboutChats = tr::lng_export_about_chats(tr::now).toUtf8();
	result.aboutLeftChats = tr::lng_export_about_left_chats(tr::now).toUtf8();
	return result;
}

QString StatusName(JobState::Status status) {
	switch (status) {
	case JobState::Status::Pending: return u"pending"_q;
	case JobState::Status::Running: return u"running"_q;
	case JobState::Status::Finished: return u"finished"_q;
	case JobState::Status::Cancelled: return u"cancelled"_q;
	case JobState::Status::Error: return u"error"_q;
	}
	return u"unknown"_q;
}

QString ModeName(JobOptions::Mode mode) {
	return (mode == JobOptions::Mode::List)
		? u"list"_q
		: u"download"_q;
}

bool ParseTime(const JsonValue &value, TimeId &result) {
	if (value.isNumber()) {
		const auto seconds = qlonglong(value.numberValue());
		result = TimeId(std::max<qlonglong>(
			0,
			std::min<qlonglong>(seconds, 0x7FFFFFFF)));
		return true;
	} else if (value.isString()) {
		const auto text = value.stringValue();
		bool ok = false;
		const auto asNumber = text.toLongLong(&ok);
		if (ok) {
			result = TimeId(std::max<qlonglong>(
				0,
				std::min<qlonglong>(asNumber, 0x7FFFFFFF)));
			return true;
		}
		const auto date = QDateTime::fromString(text, Qt::ISODate);
		if (date.isValid()) {
			result = TimeId(std::max<qlonglong>(
				0,
				std::min<qlonglong>(
					date.toSecsSinceEpoch(),
					0x7FFFFFFF)));
			return true;
		}
	}
	return false;
}

bool ParseOptions(
		const JsonValue &object,
		JobOptions &options,
		QString &error) {
	const auto dest = object.field(u"dest"_q).stringValue().trimmed();
	if (dest.isEmpty()) {
		error = u"Missing 'dest'"_q;
		return false;
	}
	options.dest = dest;
	const auto format = object.field(u"format"_q).stringValue();
	if (format == u"json"_q
		|| format == u"html"_q
		|| format == u"html_and_json"_q
		|| format == u"media"_q
		|| format.isEmpty()) {
		options.format = format.isEmpty() ? u"html"_q : format;
	} else {
		error = u"Invalid 'format'"_q;
		return false;
	}
	options.engine = object.field(u"engine"_q).stringValue();
	if (options.engine != u"direct"_q
		&& options.engine != u"takeout"_q
		&& !options.engine.isEmpty()) {
		error = u"Invalid 'engine'"_q;
		return false;
	}
	if (options.engine.isEmpty()
		|| options.engine == u"direct"_q) {
		options.engine = u"direct"_q;
	}
	if (options.engine == u"direct"_q
		&& options.format != u"media"_q
		&& !options.format.isEmpty()) {
		error = u"Direct engine supports only 'format': 'media'"_q;
		return false;
	}
	const auto chats = object.field(u"chats"_q).stringValue();
	options.chatSelector = chats.isEmpty() ? u"all"_q : chats;
	options.peer = object.field(u"peer"_q).stringValue();
	if (object.hasField(u"limit"_q)) {
		options.chatLimit = std::max(
			0,
			int(object.field(u"limit"_q).numberValue()));
	}
	if (object.hasField(u"peer_id"_q)) {
		options.peerId = uint64(
			object.field(u"peer_id"_q).numberValue());
	}
	if (object.hasField(u"topic_root_id"_q)) {
		options.topicRootId = int32(
			object.field(u"topic_root_id"_q).numberValue());
	}
	if (options.topicRootId > 0 && !options.peerId) {
		error = u"'topic_root_id' requires 'peer_id'"_q;
		return false;
	}
	if (object.hasField(u"parallel"_q)) {
		options.parallel = std::clamp(
			int(object.field(u"parallel"_q).numberValue()),
			1,
			8);
	}
	if (options.engine == u"direct"_q && !options.peerId) {
		error = u"Direct engine requires 'peer_id'"_q;
		return false;
	}
	const auto media = object.field(u"media"_q);
	if (!media.isNull()) {
		if (!media.isArray()) {
			error = u"Invalid 'media'"_q;
			return false;
		}
		auto types = Export::MediaSettings::Types();
		for (const auto &entry : media.items()) {
			const auto name = entry.stringValue();
			if (name == u"photo"_q) {
				types = types | Export::MediaSettings::Type::Photo;
			} else if (name == u"video"_q) {
				types = types | Export::MediaSettings::Type::Video;
			} else if (name == u"voice"_q) {
				types = types | Export::MediaSettings::Type::VoiceMessage;
			} else if (name == u"video_message"_q) {
				types = types | Export::MediaSettings::Type::VideoMessage;
			} else if (name == u"sticker"_q) {
				types = types | Export::MediaSettings::Type::Sticker;
			} else if (name == u"gif"_q) {
				types = types | Export::MediaSettings::Type::GIF;
			} else if (name == u"file"_q) {
				types = types | Export::MediaSettings::Type::File;
			} else {
				error = u"Unknown media entry: "_q + name;
				return false;
			}
		}
		options.mediaTypes = types;
	}
	if (object.hasField(u"size_limit_mb"_q)) {
		const auto value = qlonglong(
			object.field(u"size_limit_mb"_q).numberValue());
		options.sizeLimit = (value <= 0)
			? 0
			: std::min<qlonglong>(value, 4000) * 1024 * 1024;
	}
	if (object.hasField(u"since"_q)
		&& !ParseTime(object.field(u"since"_q), options.from)) {
		error = u"Invalid 'since'"_q;
		return false;
	}
	if (object.hasField(u"until"_q)
		&& !ParseTime(object.field(u"until"_q), options.till)) {
		error = u"Invalid 'until'"_q;
		return false;
	}
	if (options.till > 0 && options.till <= options.from) {
		error = u"'since' must be before 'until'"_q;
		return false;
	}
	return true;
}

JsonValue JobOptionsJson(const JobOptions &options) {
	return Json::Object({
		{ u"mode"_q, Json::String(ModeName(options.mode)) },
		{ u"engine"_q, Json::String(options.engine) },
		{ u"dest"_q, Json::String(options.dest) },
		{ u"format"_q, Json::String(options.format) },
		{ u"chats"_q, Json::String(options.chatSelector) },
		{ u"peer"_q, Json::String(options.peer) },
		{ u"peer_id"_q, Json::Number(qint64(options.peerId)) },
		{ u"topic_root_id"_q, Json::Number(qint64(options.topicRootId)) },
		{ u"limit"_q, Json::Number(qint64(options.chatLimit)) },
		{ u"parallel"_q, Json::Number(qint64(options.parallel)) },
		{ u"size_limit"_q, Json::Number(options.sizeLimit) },
		{ u"since"_q, Json::Number(qint64(options.from)) },
		{ u"until"_q, Json::Number(qint64(options.till)) },
	});
}

} // namespace

Manager::Manager(not_null<Main::Domain*> domain)
: QObject()
, _domain(domain) {
	_domain->activeSessionValue(
	) | rpl::on_next([=](Main::Session *session) {
		_session = session;
		if (!session) {
			cancelAll(u"Session closed"_q);
		} else {
			maybeStartNext();
		}
	}, _lifetime);
}

Manager::~Manager() {
	stop();
}

void Manager::start() {
	if (_server) {
		return;
	}
	const auto args = QCoreApplication::arguments();
	auto port = 0;
	auto token = QString();
	for (auto i = 1; i < args.size(); ++i) {
		const auto arg = args[i];
		if (arg.startsWith(u"-api-port="_q)) {
			port = arg.mid(10).toInt();
		} else if (arg.startsWith(u"--api-port="_q)) {
			port = arg.mid(11).toInt();
		} else if (arg.startsWith(u"-api-token="_q)) {
			token = arg.mid(11);
		} else if (arg.startsWith(u"--api-token="_q)) {
			token = arg.mid(12);
		}
	}
	const auto environment = QProcessEnvironment::systemEnvironment();
	if (!port) {
		port = environment.value(u"AYUGRAM_API_PORT"_q).toInt();
	}
	if (token.isEmpty()) {
		token = environment.value(u"AYUGRAM_API_TOKEN"_q);
	}
	if (!port || port < 1 || port > 65535) {
		return;
	}
	_token = token.isEmpty()
		? QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-')
		: token;
	_server = new HttpServer(this, [=](quint64 id, HttpRequest &&request) {
		handleRequest(id, std::move(request));
	}, [=](quint64 id, int status, QByteArray body) {
		respondRaw(id, status, std::move(body));
	});
	if (!_server->listen(quint16(port))) {
		LOG(("DownloadApi Error: Could not listen on port %1.").arg(port));
		_server->deleteLater();
		_server = nullptr;
		return;
	}
	LOG(("DownloadApi Info: Listening on 127.0.0.1:%1 "
		"with token %2.").arg(port).arg(_token));
}

void Manager::stop() {
	if (!_server) {
		return;
	}
	_server->stop();
	_server->deleteLater();
	_server = nullptr;
}

void Manager::handleRequest(quint64 id, HttpRequest &&request) {
	if (request.token != _token) {
		respondRaw(id, 401, QByteArray("{\"error\":\"unauthorized\"}"));
		return;
	}
	if (request.method == HttpRequest::Method::Get) {
		if (request.path == u"/"_q) {
			handleRoot(id);
		} else if (request.path == u"/api/v1/health"_q) {
			handleHealth(id);
		} else if (request.path == u"/api/v1/jobs"_q) {
			handleJobsList(id);
		} else if (request.path.startsWith(u"/api/v1/jobs/"_q)) {
			const auto parts = request.path.split('/');
			if (parts.size() == 5) {
				handleJobById(id, parts[4]);
			} else {
				notFound(id);
			}
		} else {
			notFound(id);
		}
	} else if (request.method == HttpRequest::Method::Post) {
		if (request.path == u"/api/v1/download"_q) {
			handleCreateJob(id, false, request.body);
		} else if (request.path == u"/api/v1/list"_q) {
			handleCreateJob(id, true, request.body);
		} else if (request.path.startsWith(u"/api/v1/jobs/"_q)) {
			const auto parts = request.path.split('/');
			if (parts.size() == 6 && parts[5] == u"cancel"_q) {
				handleCancel(id, parts[4]);
			} else if (parts.size() == 6 && parts[5] == u"skip"_q) {
				handleSkip(id, parts[4], request.body);
			} else {
				notFound(id);
			}
		} else {
			notFound(id);
		}
	} else {
		methodNotAllowed(id);
	}
}

void Manager::handleRoot(quint64 id) {
	respondJson(id, 200, Json::Object({
		{ u"ok"_q, Json::Bool(true) },
		{ u"service"_q, Json::String(u"ayugram-download-api"_q) },
		{ u"endpoints"_q, Json::Array({
			Json::String(u"/api/v1/health"_q),
			Json::String(u"/api/v1/jobs"_q),
			Json::String(u"/api/v1/jobs/{id}"_q),
			Json::String(u"/api/v1/download"_q),
			Json::String(u"/api/v1/list"_q),
			Json::String(u"/api/v1/jobs/{id}/cancel"_q),
			Json::String(u"/api/v1/jobs/{id}/skip"_q),
		}) },
	}));
}

void Manager::handleHealth(quint64 id) {
	auto running = 0;
	for (const auto &job : _jobs) {
		if (job->state.status == JobState::Status::Running) {
			++running;
		}
	}
	respondJson(id, 200, Json::Object({
		{ u"ok"_q, Json::Bool(true) },
		{ u"logged_in"_q, Json::Bool(_session != nullptr) },
		{ u"user_id"_q, _session
			? Json::Number(qint64(_session->userId().bare))
			: Json::Number(qint64(0)) },
		{ u"active_jobs"_q, Json::Number(qint64(_jobs.size())) },
		{ u"running_jobs"_q, Json::Number(qint64(running)) },
	}));
}

void Manager::handleCreateJob(
		quint64 id,
		bool list,
		const QByteArray &body) {
	auto options = JobOptions();
	options.mode = list
		? JobOptions::Mode::List
		: JobOptions::Mode::Download;
	auto error = QString();
	const auto object = ParseJson(body);
	if (!ParseOptions(object, options, error)) {
		respondJson(id, 400, Json::Object({
			{ u"ok"_q, Json::Bool(false) },
			{ u"error"_q, Json::String(error) },
		}));
		return;
	}
	if (!_session) {
		respondJson(id, 409, Json::Object({
			{ u"ok"_q, Json::Bool(false) },
			{ u"error"_q, Json::String(u"No active session"_q) },
		}));
		return;
	}
	if (!QDir(options.dest).exists()) {
		respondJson(id, 400, Json::Object({
			{ u"ok"_q, Json::Bool(false) },
			{ u"error"_q, Json::String(u"Destination folder does not exist"_q) },
		}));
		return;
	}
	auto job = std::make_unique<Job>(_nextJobId++, std::move(options));
	const auto jobId = job->id;
	_jobs.push_back(std::move(job));
	respondJson(id, 200, Json::Object({
		{ u"ok"_q, Json::Bool(true) },
		{ u"job_id"_q, Json::Number(qint64(jobId)) },
	}));
	maybeStartNext();
}

void Manager::handleJobsList(quint64 id) {
	auto jobs = std::vector<JsonValue>();
	for (const auto &job : _jobs) {
		jobs.push_back(writeJobJson(*job));
	}
	respondJson(id, 200, Json::Object({
		{ u"ok"_q, Json::Bool(true) },
		{ u"jobs"_q, Json::Array(std::move(jobs)) },
	}));
}

void Manager::handleJobById(quint64 id, const QString &jobId) {
	const auto job = jobById(jobId);
	if (!job) {
		notFound(id);
		return;
	}
	respondJson(id, 200, std::move(writeJobJson(*job)));
}

void Manager::handleCancel(quint64 id, const QString &jobId) {
	const auto job = jobById(jobId);
	if (!job) {
		notFound(id);
		return;
	}
	if (job->controller) {
		job->controller->cancel();
	} else if (job->fastController) {
		job->fastController->cancel();
	} else if (job->state.status == JobState::Status::Pending) {
		auto state = JobState();
		state.status = JobState::Status::Cancelled;
		job->state = state;
	}
	respondJson(id, 200, Json::Object({
		{ u"ok"_q, Json::Bool(true) },
	}));
}

void Manager::handleSkip(
		quint64 id,
		const QString &jobId,
		const QByteArray &body) {
	const auto job = jobById(jobId);
	if (!job) {
		notFound(id);
		return;
	}
	const auto object = ParseJson(body);
	const auto randomIdField = object.field(u"random_id"_q);
	qulonglong randomId = 0;
	if (randomIdField.isNumber()) {
		randomId = qulonglong(randomIdField.numberValue());
	} else if (randomIdField.isString()) {
		bool ok = false;
		randomId = randomIdField.stringValue().toULongLong(&ok);
		if (!ok) {
			randomId = 0;
		}
	}
	if (!randomId) {
		respondJson(id, 400, Json::Object({
			{ u"ok"_q, Json::Bool(false) },
			{ u"error"_q, Json::String(u"Missing 'random_id'"_q) },
		}));
		return;
	}
	if (job->controller) {
		job->controller->skipFile(uint64(randomId));
	}
	respondJson(id, 200, Json::Object({
		{ u"ok"_q, Json::Bool(true) },
	}));
}

void Manager::respondJson(quint64 id, int status, JsonValue &&json) {
	respondRaw(id, status, json.dump().toUtf8());
}

void Manager::respondRaw(quint64 id, int status, QByteArray body) {
	if (_server) {
		_server->respond(id, status, body);
	}
}

void Manager::methodNotAllowed(quint64 id) {
	respondRaw(id, 405, QByteArray("{\"error\":\"method_not_allowed\"}"));
}

void Manager::notFound(quint64 id) {
	respondRaw(id, 404, QByteArray("{\"error\":\"not_found\"}"));
}

Manager::Job::Job(int id, JobOptions &&options)
: id(id)
, options(std::move(options))
, createdAt(TimeId(QDateTime::currentSecsSinceEpoch())) {
}

Manager::Job *Manager::jobById(const QString &jobId) const {
	const auto id = jobId.toInt();
	for (const auto &job : _jobs) {
		if (job->id == id) {
			return job.get();
		}
	}
	return nullptr;
}

void Manager::maybeStartNext() {
	if (!_session || !_server) {
		return;
	}
	for (const auto &job : _jobs) {
		if (job->controller || job->fastController) {
			return;
		}
	}
	for (const auto &candidate : _jobs) {
		if (candidate->state.status != JobState::Status::Pending) {
			continue;
		}
		const auto job = candidate.get();
		if (job->options.engine == u"direct"_q) {
			job->fastController = std::make_unique<FastController>(
				&_session->mtp());
			job->fastController->start(JobOptions(job->options));
			job->fastController->state(
			) | rpl::on_next([=](JobState &&state) {
				handleJobState(job, std::move(state));
			}, job->lifetime);
		} else {
			job->controller = std::make_unique<Controller>(
				&_session->mtp());
			job->controller->start(
				JobOptions(job->options),
				PrepareEnvironment(_session));
			job->controller->state(
			) | rpl::on_next([=](JobState &&state) {
				handleJobState(job, std::move(state));
			}, job->lifetime);
		}
		return;
	}
}

void Manager::handleJobState(Manager::Job *job, JobState &&state) {
	job->state = state;
	if (state.status == JobState::Status::Finished
		|| state.status == JobState::Status::Cancelled
		|| state.status == JobState::Status::Error) {
		job->controller = nullptr;
		job->fastController = nullptr;
		maybeStartNext();
	}
}

void Manager::cancelAll(const QString &reason) {
	for (const auto &job : _jobs) {
		if (job->controller) {
			job->controller->cancel();
		} else if (job->fastController) {
			job->fastController->cancel();
		} else if (job->state.status == JobState::Status::Pending) {
			auto state = JobState();
			state.status = JobState::Status::Error;
			state.error = reason;
			job->state = state;
		}
	}
}

JsonValue Manager::writeJobJson(const Job &job) const {
	auto fields = JsonFields();
	fields.push_back({ u"id"_q, Json::Number(qint64(job.id)) });
	fields.push_back({ u"status"_q, Json::String(StatusName(job.state.status)) });
	if (!job.state.debug.isEmpty()) {
		fields.push_back({ u"debug"_q, Json::String(job.state.debug) });
	}
	fields.push_back({ u"created_at"_q, Json::Number(qint64(job.createdAt)) });
	fields.push_back({ u"options"_q, JobOptionsJson(job.options) });
	fields.push_back({ u"error"_q, Json::String(job.state.error) });
	if (job.state.status == JobState::Status::Finished) {
		fields.push_back({ u"main_file"_q, Json::String(job.state.mainFile) });
		fields.push_back({ u"files"_q, Json::Number(qint64(job.state.filesCount)) });
		fields.push_back({ u"bytes"_q, Json::Number(job.state.bytesCount) });
	}
	fields.push_back({ u"progress"_q, Json::Object({
		{ u"dialog_index"_q, Json::Number(qint64(job.state.dialogIndex)) },
		{ u"dialog_count"_q, Json::Number(qint64(job.state.dialogCount)) },
		{ u"dialog_name"_q, Json::String(job.state.dialogName) },
		{ u"message_index"_q, Json::Number(qint64(job.state.itemIndex)) },
		{ u"message_count"_q, Json::Number(qint64(job.state.itemCount)) },
		{ u"bytes_loaded"_q, Json::Number(job.state.bytesLoaded) },
		{ u"bytes_total"_q, Json::Number(job.state.bytesTotal) },
		{ u"bytes_loaded_total"_q,
			Json::Number(job.state.bytesLoadedTotal) },
		{ u"bytes_total_all"_q,
			Json::Number(job.state.bytesTotalAll) },
		{ u"files_finished"_q,
			Json::Number(qint64(job.state.filesFinished)) },
		{ u"files_total"_q, Json::Number(qint64(job.state.filesTotal)) },
		{ u"files_skipped"_q,
			Json::Number(qint64(job.state.skippedFiles)) },
		{ u"bytes_per_second"_q,
			Json::Number(job.state.bytesPerSecond) },
		{ u"files_per_second"_q,
			Json::Number(job.state.filesPerSecond) },
		{ u"eta_seconds"_q,
			Json::Number(qint64(job.state.etaSeconds)) },
		{ u"bytes_random_id"_q,
			Json::String(QString::number(job.state.bytesRandomId)) },
	}) });
	if (!job.state.candidates.isEmpty()) {
		auto candidates = std::vector<JsonValue>();
		for (const auto &candidate : job.state.candidates) {
			candidates.push_back(Json::String(candidate));
		}
		fields.push_back({ u"candidates"_q, Json::Array(std::move(candidates)) });
	}
	if (!job.state.chats.empty()) {
		auto chats = std::vector<JsonValue>();
		for (const auto &chat : job.state.chats) {
			chats.push_back(Json::Object({
				{ u"id"_q, Json::Number(qint64(chat.peerId.value)) },
				{ u"name"_q, Json::String(chat.name) },
				{ u"type"_q, Json::String(chat.typeName) },
				{ u"date"_q, Json::Number(qint64(chat.date)) },
			}));
		}
		fields.push_back({ u"chats"_q, Json::Array(std::move(chats)) });
	}
	return Json::Object(std::move(fields));
}

} // namespace DownloadApi