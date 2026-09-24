/*
This file is part of AyuGram Desktop,
the unofficial fork of Telegram Desktop with a built-in download API.

For license and copyright information please follow this link:
https://github.com/AyuGram/AyuGramDesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QString>

#include <utility>
#include <vector>

namespace DownloadApi {

enum class JsonType {
	Null,
	Bool,
	Number,
	String,
	Array,
	Object,
};

class JsonValue final {
public:
	JsonValue() = default;

	[[nodiscard]] static JsonValue Null();
	[[nodiscard]] static JsonValue Bool(bool value);
	[[nodiscard]] static JsonValue Number(double value);
	[[nodiscard]] static JsonValue Number(qint64 value);
	[[nodiscard]] static JsonValue String(const QString &value);
	[[nodiscard]] static JsonValue Array(const std::vector<JsonValue> &items);
	[[nodiscard]] static JsonValue Object(
		const std::vector<std::pair<QString, JsonValue>> &fields);

	[[nodiscard]] JsonType type() const;
	[[nodiscard]] bool isNull() const;
	[[nodiscard]] bool isBool() const;
	[[nodiscard]] bool isNumber() const;
	[[nodiscard]] bool isString() const;
	[[nodiscard]] bool isArray() const;
	[[nodiscard]] bool isObject() const;

	[[nodiscard]] bool boolValue() const;
	[[nodiscard]] double numberValue() const;
	[[nodiscard]] QString stringValue() const;
	[[nodiscard]] const std::vector<JsonValue> &items() const;
	[[nodiscard]] bool hasField(const QString &key) const;
	[[nodiscard]] JsonValue field(const QString &key) const;

	[[nodiscard]] QString dump() const;

private:
	JsonType _type = JsonType::Null;
	bool _bool = false;
	double _number = 0;
	QString _string;
	std::vector<JsonValue> _array;
	std::vector<std::pair<QString, JsonValue>> _object;

};

JsonValue ParseJson(const QByteArray &data);
JsonValue ParseJson(const QString &text);

} // namespace DownloadApi