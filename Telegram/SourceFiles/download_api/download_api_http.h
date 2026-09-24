/*
This file is part of AyuGram Desktop,
the unofficial fork of Telegram Desktop with a built-in download API.

For license and copyright information please follow this link:
https://github.com/AyuGram/AyuGramDesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"

#include <QtCore/QObject>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

#include <map>

namespace DownloadApi {

struct HttpRequest {
	enum class Method {
		Get,
		Post,
		Head,
		Unsupported,
	};
	Method method = Method::Unsupported;
	QString path;
	QString token;
	QByteArray body;
};

class HttpServer final : public QObject {
public:
	HttpServer(
		not_null<QObject*> parent,
		Fn<void(quint64, HttpRequest&&)> handler,
		Fn<void(quint64, int, QByteArray)> responder);

	[[nodiscard]] bool listen(quint16 port);
	void stop();
	void respond(quint64 id, int status, QByteArray body);

private:
	class Connection final : public QObject {
	public:
		Connection(
			not_null<HttpServer*> server,
			not_null<QTcpSocket*> socket,
			quint64 id);

		[[nodiscard]] quint64 id() const;
		void respond(int status, QByteArray body);
	private:
		void readyRead();
		void disconnected();
		void parseMore();
		void abort(int status);

		not_null<HttpServer*> _server;
		not_null<QTcpSocket*> _socket;
		quint64 _id = 0;
		QByteArray _buffer;
		HttpRequest::Method _method = HttpRequest::Method::Get;
		QString _path;
		QString _token;
		int _headerSize = -1;
		int _contentLength = 0;
		bool _dispatched = false;
		bool _handled = false;

	};

	void newConnection();

	QTcpServer _tcp;
	std::map<quint64, std::unique_ptr<Connection>> _connections;
	quint64 _nextId = 0;
	Fn<void(quint64, HttpRequest&&)> _handler;
	Fn<void(quint64, int, QByteArray)> _responder;

};

} // namespace DownloadApi