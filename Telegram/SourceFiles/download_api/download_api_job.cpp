/*
This file is part of AyuGram Desktop,
the unofficial fork of Telegram Desktop with a built-in download API.

For license and copyright information please follow this link:
https://github.com/AyuGram/AyuGramDesktop/blob/master/LEGAL
*/
#include "download_api/download_api_pch.h"

#include "download_api/download_api_job.h"

#include "export/export_api_wrap.h"
#include "export/export_settings.h"
#include "export/data/export_data_types.h"
#include "export/output/export_output_abstract.h"
#include "export/output/export_output_result.h"
#include "export/output/export_output_stats.h"

#include "logs.h"
#include "mtproto/mtproto_response.h"
#include "mtproto/mtp_instance.h"

#include <algorithm>

namespace DownloadApi {
namespace {

bool MatchSelector(
		const Export::Data::DialogInfo::Type type,
		const QString &selector) {
	if (selector.isEmpty() || selector == u"all"_q) {
		return true;
	}
	using Type = Export::Data::DialogInfo::Type;
	if (selector == u"personal"_q) {
		return (type == Type::Personal) || (type == Type::Self);
	} else if (selector == u"bot"_q) {
		return type == Type::Bot;
	} else if (selector == u"groups"_q) {
		return (type == Type::PrivateGroup)
			|| (type == Type::PrivateSupergroup)
			|| (type == Type::PublicSupergroup);
	} else if (selector == u"channels"_q) {
		return (type == Type::PrivateChannel)
			|| (type == Type::PublicChannel);
	} else if (selector == u"supergroups"_q) {
		return (type == Type::PrivateSupergroup)
			|| (type == Type::PublicSupergroup);
	} else if (selector == u"saved"_q) {
		return type == Type::Self;
	}
	return false;
}

bool MatchBarePeerId(
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

QString SelectorTypeName(Export::Data::DialogInfo::Type type) {
	using Type = Export::Data::DialogInfo::Type;
	switch (type) {
	case Type::Self: return u"saved"_q;
	case Type::Personal: return u"personal"_q;
	case Type::Bot: return u"bot"_q;
	case Type::PrivateGroup: return u"group"_q;
	case Type::PrivateSupergroup:
	case Type::PublicSupergroup: return u"supergroup"_q;
	case Type::PrivateChannel:
	case Type::PublicChannel: return u"channel"_q;
	}
	return u"unknown"_q;
}

} // namespace


Controller::Controller(QPointer<MTP::Instance> mtproto)
: _wrapped(std::move(mtproto)) {
}

Controller::~Controller() {
}

void Controller::start(
		JobOptions options,
		const Export::Environment &environment) {
	_wrapped.with([=](Implementation &unwrapped) {
		unwrapped.start(std::move(options), environment);
	});
}

void Controller::skipFile(uint64 randomId) {
	_wrapped.with([=](Implementation &unwrapped) {
		unwrapped.skipFile(randomId);
	});
}

void Controller::cancel() {
	LOG(("DownloadApi Info: Cancel requested."));

	_wrapped.with([=](Implementation &unwrapped) {
		unwrapped.cancel();
	});
}

rpl::producer<JobState> Controller::state() const {
	return _wrapped.producer_on_main([=](const Implementation &unwrapped) {
		return unwrapped.state();
	});
}

ControllerObject::ControllerObject(
		crl::weak_on_queue<ControllerObject> weak,
		QPointer<MTP::Instance> mtproto)
: _api(mtproto, weak.runner())
, _state(JobState{}) {
	_api.errors(
	) | rpl::on_next([=](const MTP::Error &error) {
		fail(error.type());
	}, _lifetime);

	_api.ioErrors(
	) | rpl::on_next([=](const Export::Output::Result &result) {
		fail(u"IO error: "_q + result.path);
	}, _lifetime);
}

rpl::producer<JobState> ControllerObject::state() const {
	return rpl::single(
		_state
	) | rpl::then(
		_stateChanges.events()
	);
}

void ControllerObject::setState(JobState &&state) {
	_state = std::move(state);
	_stateChanges.fire_copy(_state);
}

void ControllerObject::fail(const QString &error) {
	auto state = JobState();
	state.status = JobState::Status::Error;
	state.error = error;
	setState(std::move(state));
}

bool ControllerObject::ioCatchError(const Export::Output::Result &result) {
	if (result) {
		return false;
	}
	fail(u"IO error: "_q + result.path);
	return true;
}

void ControllerObject::start(
		JobOptions options,
		const Export::Environment &environment) {
	_options = std::move(options);
	_environment = environment;
	_settings.path = _options.dest;
	_settings.forceSubPath = true;
	_settings.types = Export::Settings::Type::AnyChatsMask;
	_settings.fullChats = Export::Settings::Type::PersonalChats
		| Export::Settings::Type::BotChats;
	_settings.media.types = _options.mediaTypes;
	_settings.media.sizeLimit = _options.sizeLimit;
	_settings.singlePeerFrom = _options.from;
	_settings.singlePeerTill = _options.till;
	if (_options.format == u"json"_q) {
		_settings.format = Export::Output::Format::Json;
	} else if (_options.format == u"html_and_json"_q) {
		_settings.format = Export::Output::Format::HtmlAndJson;
	} else {
		_settings.format = Export::Output::Format::Html;
	}
	auto state = JobState();
	state.status = JobState::Status::Running;
	setState(std::move(state));
	if (_options.format != u"media"_q
		&& _options.mode == JobOptions::Mode::Download) {
		_writer = Export::Output::CreateWriter(_settings.format);
	}
	initialize();
}

void ControllerObject::skipFile(uint64 randomId) {
	if (_cancelled) {
		return;
	}
	_api.skipFile(randomId);
}

void ControllerObject::cancel() {
	if (_cancelled) {
		return;
	}
	_cancelled = true;
	_api.cancelExportFast();
	auto state = JobState();
	state.status = JobState::Status::Cancelled;
	setState(std::move(state));
}

void ControllerObject::initialize() {
	_api.startExport(_settings, &_stats, [=](Export::ApiWrap::StartInfo info) {
		initialized(info);
	});
}

void ControllerObject::initialized(const Export::ApiWrap::StartInfo &info) {
	if (_writer && ioCatchError(_writer->start(
			_settings,
			_environment,
			&_stats))) {
		return;
	}
	collectDialogs();
}

void ControllerObject::collectDialogs() {
	_api.requestDialogsList([=](int count) {
		return true;
	}, [=](Export::Data::DialogsInfo &&info) {
		dialogsCollected(std::move(info));
	});
}

void ControllerObject::dialogsCollected(Export::Data::DialogsInfo &&info) {
	_dialogs = std::move(info);
	selectDialogs();
}

void ControllerObject::selectDialogs() {
	const auto total = int(_dialogs.chats.size()) + int(_dialogs.left.size());
	const auto needle = _options.peer.trimmed();
	for (auto i = 0; i != total; ++i) {
		const auto info = _dialogs.item(i);
		if (!info) {
			continue;
		}
		const auto name = QString::fromUtf8(info->name);
		if (!needle.isEmpty()
			&& !name.contains(needle, Qt::CaseInsensitive)
			&& QString::number(qlonglong(info->peerId.value)) != needle) {
			continue;
		}
		if (!MatchSelector(info->type, _options.chatSelector)) {
			continue;
		}
		_queue.push_back(i);
	}
	if (!needle.isEmpty() && _queue.size() != 1) {
		auto state = JobState();
		state.status = JobState::Status::Error;
		state.error = (_queue.empty())
			? u"No matching chats"_q
			: u"Multiple matching chats"_q;
		for (const auto index : _queue) {
			const auto info = _dialogs.item(index);
			state.candidates.push_back(info
				? QString::fromUtf8(info->name)
				: QString());
		}
		setState(std::move(state));
		return;
	}
	if (_options.chatLimit > 0
		&& _queue.size() > _options.chatLimit) {
		_queue.resize(_options.chatLimit);
	}
	if (_options.topicRootId > 0) {
		auto topicIndex = -1;
		for (const auto index : _queue) {
			const auto info = _dialogs.item(index);
			if (!info || !MatchBarePeerId(*info, _options.peerId)) {
				continue;
			}
			if (topicIndex >= 0) {
				fail(u"Multiple chats match peer_id"_q);
				return;
			}
			topicIndex = index;
		}
		if (topicIndex < 0) {
			auto state = JobState();
			state.status = JobState::Status::Error;
			state.error = u"No chat matches peer_id in dialogs"_q;
			setState(std::move(state));
			return;
		}
		_queue = { topicIndex };
		_cursor = 0;
	}
	auto state = _state;
	state.dialogIndex = 0;
	state.dialogCount = int(_queue.size());
	state.notice = (_options.mode == JobOptions::Mode::List)
		? u"list"_q
		: QString();
	setState(std::move(state));
	if (!_queue.empty() && _writer) {
		if (ioCatchError(_writer->writeDialogsStart(_dialogs))) {
			return;
		}
	}
	if (_options.mode == JobOptions::Mode::List) {
		finishJob();
		return;
	}
	if (_options.topicRootId > 0) {
		exportTopic(*_dialogs.item(_queue[0]));
		return;
	}
	exportNextDialog();
}

void ControllerObject::exportTopic(const Export::Data::DialogInfo &info) {
	if (_cancelled) {
		return;
	}
	_messagesWritten = 0;
	_messagesCount = 0;
	auto topicInfo = Export::Data::DialogInfo();
	topicInfo.type = info.type;
	topicInfo.name = (u"topic_"_q
		+ QString::number(_options.topicRootId)).toUtf8();
	topicInfo.peerId = info.peerId;
	if (_writer && ioCatchError(_writer->writeDialogStart(topicInfo))) {
		return;
	}
	fillDialogState();
	_api.requestTopicMessages(
		info.peerId,
		info.input,
		_options.topicRootId,
		[=](int count) {
			_messagesCount = count;
			fillDialogState();
			return true;
		}, [=](DownloadProgress progress) {
			fillProgressState(progress);
			return true;
		}, [=](Export::Data::MessagesSlice &&slice) {
			if (_writer && ioCatchError(_writer->writeDialogSlice(slice))) {
				return false;
			}
			_messagesWritten += int(slice.list.size());
			fillDialogState();
			return true;
		}, [=] {
			if (_writer && ioCatchError(_writer->writeDialogEnd())) {
				return;
			}
			finishJob();
		});
}

void ControllerObject::exportNextDialog() {
	if (_cancelled) {
		return;
	}
	++_cursor;
	if (_cursor >= int(_queue.size())) {
		finishJob();
		return;
	}
	const auto info = _dialogs.item(_queue[_cursor]);
	if (!info) {
		exportNextDialog();
		return;
	}
	_api.requestMessages(*info, [=](const Export::Data::DialogInfo &info) {
		_messagesWritten = 0;
		_messagesCount = 0;
		for (const auto count : info.messagesCountPerSplit) {
			_messagesCount += count;
		}
		if (_writer && ioCatchError(_writer->writeDialogStart(info))) {
			return false;
		}
		fillDialogState();
		return true;
	}, [=](DownloadProgress progress) {
		fillProgressState(progress);
		return true;
	}, [=](Export::Data::MessagesSlice &&slice) {
		if (_writer && ioCatchError(_writer->writeDialogSlice(slice))) {
			return false;
		}
		_messagesWritten += int(slice.list.size());
		fillDialogState();
		return true;
	}, [=] {
		if (_writer && ioCatchError(_writer->writeDialogEnd())) {
			return;
		}
		exportNextDialog();
	});
}

void ControllerObject::finishJob() {
	if (_cancelled) {
		return;
	}
	if (_writer && ioCatchError(_writer->finish())) {
		return;
	}
	_api.finishExport([=] {
		auto state = JobState();
		state.status = JobState::Status::Finished;
		state.mainFile = _writer
			? _writer->mainFilePath()
			: _settings.path;
		state.filesCount = _stats.filesCount();
		state.bytesCount = _stats.bytesCount();
		state.dialogIndex = _cursor;
		state.dialogCount = int(_queue.size());
		for (const auto index : _queue) {
			const auto info = _dialogs.item(index);
			if (!info) {
				continue;
			}
			auto chat = ListedChat();
			chat.peerId = info->peerId;
			chat.name = QString::fromUtf8(info->name);
			chat.typeName = SelectorTypeName(info->type);
			chat.date = info->topMessageDate;
			state.chats.push_back(std::move(chat));
		}
		setState(std::move(state));
	});
}

void ControllerObject::fillProgressState(const DownloadProgress &progress) {
	auto state = _state;
	state.itemIndex = _messagesWritten + progress.itemIndex;
	state.itemCount = std::max(_messagesCount, state.itemIndex);
	state.bytesLoaded = progress.ready;
	state.bytesTotal = progress.total;
	state.bytesRandomId = progress.randomId;
	if (progress.randomId && progress.total > 0) {
		const auto i = _loadedByFile.find(progress.randomId);
		if (i == end(_loadedByFile)) {
			_loadedByFile.emplace(
				progress.randomId,
				progress.ready);
			state.bytesLoadedTotal += progress.ready;
			state.bytesTotalAll += progress.total;
		} else {
			const auto delta = progress.ready - i->second;
			if (delta > 0) {
				state.bytesLoadedTotal += delta;
			}
			i->second = progress.ready;
		}
		if (progress.ready >= progress.total
			&& !_finishedFiles.contains(progress.randomId)) {
			_finishedFiles.insert(progress.randomId);
			++state.filesFinished;
		}
		state.filesTotal = std::max(
			state.filesTotal,
			int(_loadedByFile.size()));
	}
	setState(std::move(state));
}

void ControllerObject::fillDialogState() {
	auto state = _state;
	state.dialogIndex = _cursor;
	state.dialogCount = int(_queue.size());
	const auto index = (_cursor >= 0 && _cursor < int(_queue.size()))
		? _queue[_cursor]
		: -1;
	const auto info = (index >= 0) ? _dialogs.item(index) : nullptr;
	state.dialogName = info
		? QString::fromUtf8(info->name)
		: QString();
	state.itemIndex = _messagesWritten;
	state.itemCount = std::max(_messagesCount, state.itemIndex);
	setState(std::move(state));
}

} // namespace DownloadApi