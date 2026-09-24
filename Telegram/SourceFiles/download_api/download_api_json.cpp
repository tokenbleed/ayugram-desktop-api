/*
This file is part of AyuGram Desktop,
the unofficial fork of Telegram Desktop with a built-in download API.

For license and copyright information please follow this link:
https://github.com/AyuGram/AyuGramDesktop/blob/master/LEGAL
*/
#include "download_api/download_api_json.h"

#include <cmath>

namespace DownloadApi {
namespace {

QChar QCharFromHex(const QString &hex) {
	bool ok = false;
	const auto value = hex.toUShort(&ok, 16);
	return ok ? QChar(value) : QChar();
}

QString EscapeJsonString(const QString &value) {
	auto result = QString();
	result.append('"');
	for (const auto character : value) {
		switch (character.unicode()) {
		case '"': result.append(u"\\\""_q); break;
		case '\\': result.append(u"\\\\"_q); break;
		case '\b': result.append(u"\\b"_q); break;
		case '\f': result.append(u"\\f"_q); break;
		case '\n': result.append(u"\\n"_q); break;
		case '\r': result.append(u"\\r"_q); break;
		case '\t': result.append(u"\\t"_q); break;
		default:
			if (character.unicode() < 0x20) {
				result.append(u"\\u"_q + QString::number(
					character.unicode(),
					16).rightJustified(4, '0'));
			} else {
				result.append(character);
			}
		}
	}
	result.append('"');
	return result;
}

class Parser final {
public:
	Parser(const QString &text) : _text(text) {
	}

	JsonValue parse() {
		skipWhitespace();
		const auto result = parseValue();
		skipWhitespace();
		return result;
	}

private:
	void skipWhitespace() {
		while (_position < _text.size()
			&& _text[_position].isSpace()) {
			++_position;
		}
	}

	JsonValue parseValue() {
		if (_position >= _text.size()) {
			return JsonValue();
		}
		const auto character = _text[_position].unicode();
		if (character == '{') {
			return parseObject();
		} else if (character == '[') {
			return parseArray();
		} else if (character == '"') {
			return JsonValue::String(parseString());
		} else if (character == 't') {
			return parseLiteral(u"true"_q, JsonValue::Bool(true));
		} else if (character == 'f') {
			return parseLiteral(u"false"_q, JsonValue::Bool(false));
		} else if (character == 'n') {
			return parseLiteral(u"null"_q, JsonValue());
		}
		return parseNumber();
	}

	JsonValue parseLiteral(const QString &literal, JsonValue value) {
		if (_text.mid(_position, literal.size()) == literal) {
			_position += literal.size();
			return value;
		}
		return JsonValue();
	}

	JsonValue parseObject() {
		++_position;
		auto fields = std::vector<std::pair<QString, JsonValue>>();
		skipWhitespace();
		if (_position < _text.size()
			&& _text[_position].unicode() == '}') {
			++_position;
			return JsonValue::Object(std::move(fields));
		}
		while (_position < _text.size()) {
			skipWhitespace();
			if (_position >= _text.size()
				|| _text[_position].unicode() != '"') {
				return JsonValue();
			}
			const auto key = parseString();
			skipWhitespace();
			if (_position >= _text.size()
				|| _text[_position].unicode() != ':') {
				return JsonValue();
			}
			++_position;
			skipWhitespace();
			fields.push_back({ std::move(key), parseValue() });
			skipWhitespace();
			if (_position >= _text.size()) {
				return JsonValue();
			}
			const auto separator = _text[_position].unicode();
			++_position;
			if (separator == '}') {
				return JsonValue::Object(std::move(fields));
			} else if (separator != ',') {
				return JsonValue();
			}
		}
		return JsonValue();
	}

	JsonValue parseArray() {
		++_position;
		auto items = std::vector<JsonValue>();
		skipWhitespace();
		if (_position < _text.size()
			&& _text[_position].unicode() == ']') {
			++_position;
			return JsonValue::Array(std::move(items));
		}
		while (_position < _text.size()) {
			skipWhitespace();
			items.push_back(parseValue());
			skipWhitespace();
			if (_position >= _text.size()) {
				return JsonValue();
			}
			const auto separator = _text[_position].unicode();
			++_position;
			if (separator == ']') {
				return JsonValue::Array(std::move(items));
			} else if (separator != ',') {
				return JsonValue();
			}
		}
		return JsonValue();
	}

	QString parseString() {
		if (_position >= _text.size()
			|| _text[_position].unicode() != '"') {
			return QString();
		}
		++_position;
		auto result = QString();
		while (_position < _text.size()) {
			const auto character = _text[_position];
			++_position;
			const auto code = character.unicode();
			if (code == '"') {
				return result;
			} else if (code == '\\') {
				if (_position >= _text.size()) {
					return result;
				}
				const auto escape = _text[_position];
				++_position;
				switch (escape.unicode()) {
				case '"': result.append('"'); break;
				case '\\': result.append('\\'); break;
				case '/': result.append('/'); break;
				case 'b': result.append('\b'); break;
				case 'f': result.append('\f'); break;
				case 'n': result.append('\n'); break;
				case 'r': result.append('\r'); break;
				case 't': result.append('\t'); break;
				case 'u':
					if (_position + 4 <= _text.size()) {
						result.append(QCharFromHex(
							_text.mid(_position, 4)));
						_position += 4;
					}
					break;
				default: result.append(escape);
				}
			} else {
				result.append(character);
			}
		}
		return result;
	}

	JsonValue parseNumber() {
		const auto start = _position;
		if (_position < _text.size()
			&& (_text[_position].unicode() == '-'
				|| _text[_position].unicode() == '+')) {
			++_position;
		}
		while (_position < _text.size()
			&& (_text[_position].isDigit()
				|| _text[_position].unicode() == '.'
				|| _text[_position].unicode() == 'e'
				|| _text[_position].unicode() == 'E'
				|| _text[_position].unicode() == '-'
				|| _text[_position].unicode() == '+')) {
			++_position;
		}
		if (_position == start) {
			return JsonValue();
		}
		const auto text = _text.mid(start, _position - start);
		bool ok = false;
		const auto integer = text.toLongLong(&ok);
		if (ok) {
			return JsonValue::Number(qint64(integer));
		}
		const auto number = text.toDouble(&ok);
		return ok ? JsonValue::Number(number) : JsonValue();
	}

	const QString _text;
	int _position = 0;

};

} // namespace

JsonValue::JsonValue Null() {
	return JsonValue();
}

JsonValue::JsonValue Bool(bool value) {
	auto result = JsonValue();
	result._type = JsonType::Bool;
	result._bool = value;
	return result;
}

JsonValue::JsonValue Number(double value) {
	auto result = JsonValue();
	result._type = JsonType::Number;
	result._number = value;
	return result;
}

JsonValue::JsonValue Number(qint64 value) {
	return Number(double(value));
}

JsonValue::JsonValue String(const QString &value) {
	auto result = JsonValue();
	result._type = JsonType::String;
	result._string = value;
	return result;
}

JsonValue::JsonValue Array(const std::vector<JsonValue> &items) {
	auto result = JsonValue();
	result._type = JsonType::Array;
	result._array = items;
	return result;
}

JsonValue::JsonValue Object(
		const std::vector<std::pair<QString, JsonValue>> &fields) {
	auto result = JsonValue();
	result._type = JsonType::Object;
	result._object = fields;
	return result;
}

JsonType JsonValue::type() const {
	return _type;
}

bool JsonValue::isNull() const {
	return _type == JsonType::Null;
}

bool JsonValue::isBool() const {
	return _type == JsonType::Bool;
}

bool JsonValue::isNumber() const {
	return _type == JsonType::Number;
}

bool JsonValue::isString() const {
	return _type == JsonType::String;
}

bool JsonValue::isArray() const {
	return _type == JsonType::Array;
}

bool JsonValue::isObject() const {
	return _type == JsonType::Object;
}

bool JsonValue::boolValue() const {
	return isBool() && _bool;
}

double JsonValue::numberValue() const {
	return isNumber() ? _number : 0;
}

QString JsonValue::stringValue() const {
	return isString() ? _string : QString();
}

const std::vector<JsonValue> &JsonValue::items() const {
	return _array;
}

bool JsonValue::hasField(const QString &key) const {
	if (!isObject()) {
		return false;
	}
	for (const auto &item : _object) {
		if (item.first == key) {
			return true;
		}
	}
	return false;
}

JsonValue JsonValue::field(const QString &key) const {
	if (!isObject()) {
		return JsonValue();
	}
	for (const auto &item : _object) {
		if (item.first == key) {
			return item.second;
		}
	}
	return JsonValue();
}

QString JsonValue::dump() const {
	switch (_type) {
	case JsonType::Null: return u"null"_q;
	case JsonType::Bool: return _bool ? u"true"_q : u"false"_q;
	case JsonType::Number:
		if (_number == std::floor(_number)
			&& std::abs(_number) < 1e15) {
			return QString::number(qint64(_number));
		}
		return QString::number(_number);
	case JsonType::String: return EscapeJsonString(_string);
	case JsonType::Array: {
		auto parts = QStringList();
		for (const auto &item : _array) {
			parts.push_back(item.dump());
		}
		auto result = QString('[');
		result += parts.join(u", "_q);
		result += ']';
		return result;
	}
	case JsonType::Object: {
		auto parts = QStringList();
		for (const auto &[name, value] : _object) {
			parts.push_back(
				EscapeJsonString(name) + u": "_q + value.dump());
		}
		auto result = QString('{');
		result += parts.join(u", "_q);
		result += '}';
		return result;
	}
	}
	return u"null"_q;
}

JsonValue ParseJson(const QByteArray &data) {
	return ParseJson(QString::fromUtf8(data));
}

JsonValue ParseJson(const QString &text) {
	return Parser(text).parse();
}

} // namespace DownloadApi