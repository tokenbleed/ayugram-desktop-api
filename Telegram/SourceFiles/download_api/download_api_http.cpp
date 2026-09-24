/*
This file is part of AyuGram Desktop,
the unofficial fork of Telegram Desktop with a built-in download API.

For license and copyright information please follow this link:
https://github.com/AyuGram/AyuGramDesktop/blob/master/LEGAL
*/
#include "download_api/download_api_pch.h"

namespace DownloadApi {
namespace {

constexpr auto kMaxRequestSize = 4 * 1024 * 1024;
constexpr auto kMaxHeaderSize = 64 * 1024;

HttpRequest::Method MethodFromString(const QString &method) {
	if (method == u"GET"_q) {
		return HttpRequest::Method::Get;
	} else if (method == u"POST"_q) {
		return HttpRequest::Method::Post;
	} else if (method == u"HEAD"_q) {
		return HttpRequest::Method::Head;
	}
	return HttpRequest::Method::Unsupported;
}

QString ReasonPhrase(int status) {
	if (status == 200) {
		return u"OK"_q;
	} else if (status == 400) {
		return u"Bad Request"_q;
	} else if (status == 401) {
		return u"Unauthorized"_q;
	} else if (status == 404) {
		return u"Not Found"_q;
	} else if (status == 405) {
		return u"Method Not Allowed"_q;
	} else if (status == 413) {
		return u"Payload Too Large"_q;
	} else if (status == 431) {
		return u"Request Header Fields Too Large"_q;
	} else if (status == 500) {
		return u"Internal Server Error"_q;
	}
	return u"Error"_q;
}

} // namespace

HttpServer::HttpServer(
		not_null<QObject*> parent,
		Fn<void(quint64, HttpRequest&&)> handler,
		Fn<void(quint64, int, QByteArray)> responder)
: QObject(parent)
, _handler(handler)
, _responder(responder) {
	connect(&_tcp, &QTcpServer::newConnection, this, &HttpServer::newConnection);
}

bool HttpServer::listen(quint16 port) {
	return _tcp.listen(QHostAddress::LocalHost, port);
}

void HttpServer::stop() {
	_tcp.close();
	for (const auto &[id, connection] : _connections) {
		connection->deleteLater();
	}
	_connections.clear();
}

void HttpServer::newConnection() {
	while (_tcp.hasPendingConnections()) {
		const auto socket = _tcp.nextPendingConnection();
		if (!socket) {
			return;
		}
		const auto id = ++_nextId;
		const auto connection = new Connection(this, socket, id);
		connect(
			connection,
			&QObject::destroyed,
			this,
			[=] { _connections.erase(id); });
		_connections.emplace(id, connection);
	}
}

void HttpServer::respond(quint64 id, int status, QByteArray body) {
	const auto i = _connections.find(id);
	if (i == end(_connections)) {
		return;
	}
	i->second->respond(status, body);
}

quint64 HttpServer::Connection::id() const {
	return _id;
}

HttpServer::Connection::Connection(
		not_null<HttpServer*> server,
		not_null<QTcpSocket*> socket,
		quint64 id)
: QObject(server)
, _server(server)
, _socket(socket)
, _id(id) {
	socket->setParent(this);
	connect(socket, &QTcpSocket::readyRead, this, &Connection::readyRead);
	connect(socket, &QTcpSocket::disconnected, this, &Connection::disconnected);
}

void HttpServer::Connection::readyRead() {
	if (_handled || _dispatched) {
		return;
	}
	_buffer += _socket->readAll();
	if (_buffer.size() > kMaxRequestSize) {
		abort(413);
		return;
	}
	parseMore();
}

void HttpServer::Connection::disconnected() {
	if (_finished) {
		return;
	}
	_finished = true;
	deleteLater();
}

void HttpServer::Connection::abort(int status) {
	respond(status, QByteArray());
}

void HttpServer::Connection::parseMore() {
	if (_headerSize < 0) {
		const auto position = _buffer.indexOf(QByteArray("\r\n\r\n"));
		if (position < 0) {
			if (_buffer.size() > kMaxHeaderSize) {
				abort(431);
			}
			return;
		}
		_headerSize = position + 4;
		const auto headerBlock = _buffer.mid(0, position);
		const auto lines = QString::fromUtf8(headerBlock).split('\n');
		if (lines.isEmpty()) {
			abort(400);
			return;
		}
		const auto parts = lines[0].trimmed().split(' ');
		if (parts.size() < 2) {
			abort(400);
			return;
		}
		const auto method = MethodFromString(parts[0]);
		if (method == HttpRequest::Method::Unsupported) {
			abort(405);
			return;
		}
		_method = method;
		_path = parts[1];
		_token = QString();
		_contentLength = 0;
		for (auto i = 1; i < lines.size(); ++i) {
			const auto line = lines[i];
			const auto colon = line.indexOf(':');
			if (colon <= 0) {
				continue;
			}
			const auto name = line.mid(0, colon).trimmed().toLower();
			const auto value = line.mid(colon + 1).trimmed();
			if (name == u"content-length"_q) {
				_contentLength = std::max(0, value.toInt());
			} else if (name == u"x-api-token"_q) {
				_token = value;
			} else if (name == u"authorization"_q
				&& value.toLower().startsWith(u"bearer "_q)) {
				_token = value.mid(7).trimmed();
			}
		}
		const auto query = _path.indexOf('?');
		if (query >= 0) {
			const auto queryPart = _path.mid(query + 1);
			_path = _path.mid(0, query);
			const auto args = queryPart.split('&');
			for (const auto &arg : args) {
				if (arg.startsWith(u"token="_q)) {
					_token = arg.mid(6);
				}
			}
		}
	}
	if (_buffer.size() < _headerSize + _contentLength) {
		return;
	}
	_dispatched = true;
	auto request = HttpRequest();
	request.method = _method;
	request.path = _path;
	request.token = _token;
	request.body = _buffer.mid(_headerSize, _contentLength);
	_server->_handler(_id, std::move(request));
}

void HttpServer::Connection::respond(int status, QByteArray body) {
	if (_handled) {
		return;
	}
	_handled = true;
	const auto header = QString(
		"HTTP/1.1 %1 %2\r\n"
		"Content-Type: application/json\r\n"
		"Content-Length: %3\r\n"
		"Connection: close\r\n\r\n")
		.arg(status)
		.arg(ReasonPhrase(status))
		.arg(body.size());
	_socket->write(header.toUtf8());
	_socket->write(body);
	_socket->flush();
	_socket->disconnectFromHost();
}

} // namespace DownloadApi