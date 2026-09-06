#include "subscriptionCodec.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QUrl>

#include <openssl/evp.h>
#include <openssl/rand.h>

namespace amnezia
{
    namespace
    {
        constexpr qsizetype maxPayloadSize = 4 * 1024 * 1024;
        constexpr qsizetype maxEntries = 512;
        constexpr int nonceSize = 12;
        constexpr int tagSize = 16;
        constexpr char tokenVersion = 1;

        const QByteArray envelopeAad("amnezia-16x-v1");

        QByteArray compactEncoded(const QByteArray &input)
        {
            QByteArray result;
            result.reserve(input.size());
            for (const char ch : input) {
                if (!QChar::fromLatin1(ch).isSpace()) {
                    result.append(ch);
                }
            }
            return result;
        }

        bool decodeBase64(const QByteArray &input, QByteArray &output)
        {
            const QByteArray compact = compactEncoded(input);
            if (compact.isEmpty()) {
                return false;
            }

            auto decoded = QByteArray::fromBase64Encoding(
                    compact, QByteArray::Base64Encoding | QByteArray::AbortOnBase64DecodingErrors);
            if (decoded) {
                output = decoded.decoded;
                return true;
            }

            decoded = QByteArray::fromBase64Encoding(
                    compact, QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
            if (!decoded) {
                return false;
            }
            output = decoded.decoded;
            return true;
        }

        bool decodeHex(const QByteArray &input, QByteArray &output)
        {
            QByteArray compact = compactEncoded(input);
            if (compact.startsWith("0x")) {
                compact.remove(0, 2);
            }
            if (compact.isEmpty() || compact.size() % 2 != 0) {
                return false;
            }
            static const QRegularExpression hexPattern(QStringLiteral("^[0-9a-fA-F]+$"));
            if (!hexPattern.match(QString::fromLatin1(compact)).hasMatch()) {
                return false;
            }
            output = QByteArray::fromHex(compact);
            return true;
        }

        bool isUri(const QString &value)
        {
            static const QRegularExpression uriPattern(QStringLiteral("^[A-Za-z][A-Za-z0-9+.-]*://\\S+$"));
            return uriPattern.match(value.trimmed()).hasMatch();
        }

        bool isLikelyPlainFeed(const QByteArray &source)
        {
            const QString text = QString::fromUtf8(source).trimmed();
            if (text.isEmpty() || text.contains(QChar::ReplacementCharacter)) {
                return false;
            }
            if (text.startsWith(QLatin1String("16x")) || isUri(text)
                || text.contains(QRegularExpression(QStringLiteral("(?m)^[A-Za-z][A-Za-z0-9+.-]*://\\S+$")))) {
                return true;
            }
            if ((text.contains(QLatin1String("[Interface]")) && text.contains(QLatin1String("[Peer]")))
                || text.contains(QRegularExpression(QStringLiteral("(?m)^client\\s*$")))) {
                return true;
            }

            QJsonParseError error;
            const QJsonDocument document = QJsonDocument::fromJson(source, &error);
            return error.error == QJsonParseError::NoError && !document.isNull();
        }

        void appendJsonValue(const QJsonValue &value, QStringList &entries)
        {
            if (entries.size() > maxEntries) {
                return;
            }
            if (value.isString()) {
                const QString entry = value.toString().trimmed();
                if (!entry.isEmpty()) {
                    entries.append(entry);
                }
                return;
            }
            if (!value.isObject()) {
                return;
            }

            const QJsonObject object = value.toObject();
            for (const QString &key : { QStringLiteral("uri"), QStringLiteral("config"), QStringLiteral("data") }) {
                const QString entry = object.value(key).toString().trimmed();
                if (!entry.isEmpty()) {
                    entries.append(entry);
                    return;
                }
            }
            entries.append(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
        }

        QStringList splitEntries(const QByteArray &payload, QString &error)
        {
            const QString text = QString::fromUtf8(payload).trimmed();
            if (text.isEmpty()) {
                error = QStringLiteral("Subscription payload is empty.");
                return {};
            }
            if (text.contains(QChar::ReplacementCharacter) || text.contains(QLatin1Char('\0'))) {
                error = QStringLiteral("Subscription payload is not valid UTF-8 text.");
                return {};
            }

            QJsonParseError jsonError;
            const QJsonDocument document = QJsonDocument::fromJson(payload, &jsonError);
            if (jsonError.error == QJsonParseError::NoError) {
                QStringList entries;
                if (document.isArray()) {
                    for (const QJsonValue &value : document.array()) {
                        appendJsonValue(value, entries);
                    }
                    if (!entries.isEmpty()) {
                        return entries;
                    }
                } else if (document.isObject()) {
                    const QJsonObject object = document.object();
                    for (const QString &key : { QStringLiteral("items"), QStringLiteral("configs"),
                                                QStringLiteral("servers"), QStringLiteral("nodes") }) {
                        const QJsonArray array = object.value(key).toArray();
                        if (array.isEmpty()) {
                            continue;
                        }
                        for (const QJsonValue &value : array) {
                            appendJsonValue(value, entries);
                        }
                        if (!entries.isEmpty()) {
                            return entries;
                        }
                    }
                    return { text };
                }
            }

            const QStringList blocks = text.split(QRegularExpression(QStringLiteral("\\r?\\n[ \\t]*---[ \\t]*\\r?\\n")),
                                                  Qt::SkipEmptyParts);
            if (blocks.size() > 1) {
                QStringList entries;
                for (const QString &block : blocks) {
                    const QString entry = block.trimmed();
                    if (!entry.isEmpty()) {
                        entries.append(entry);
                    }
                }
                return entries;
            }

            if ((text.contains(QLatin1String("[Interface]")) && text.contains(QLatin1String("[Peer]")))
                || text.contains(QRegularExpression(QStringLiteral("(?m)^client\\s*$")))) {
                return { text };
            }

            QStringList entries;
            bool allUris = true;
            const QStringList lines = text.split(QRegularExpression(QStringLiteral("\\r?\\n")));
            for (const QString &line : lines) {
                const QString entry = line.trimmed();
                if (entry.isEmpty() || entry.startsWith(QLatin1Char('#'))) {
                    continue;
                }
                if (!isUri(entry)) {
                    allUris = false;
                    break;
                }
                entries.append(entry);
            }
            if (allUris && !entries.isEmpty()) {
                return entries;
            }
            return { text };
        }

        QByteArray normalizedDeviceId(const QString &deviceId)
        {
            return deviceId.trimmed().toLower().toLatin1();
        }

        QByteArray encryptionKey(const QString &deviceId)
        {
            QByteArray material("amnezia-16x-key-v1");
            material.append('\0');
            material.append(normalizedDeviceId(deviceId));
            return QCryptographicHash::hash(material, QCryptographicHash::Sha256);
        }

        bool encryptPayload(const QByteArray &payload, const QByteArray &key, QByteArray &nonce, QByteArray &ciphertext,
                            QByteArray &tag)
        {
            nonce.resize(nonceSize);
            if (RAND_bytes(reinterpret_cast<unsigned char *>(nonce.data()), nonce.size()) != 1) {
                return false;
            }

            EVP_CIPHER_CTX *context = EVP_CIPHER_CTX_new();
            if (!context) {
                return false;
            }

            ciphertext.resize(payload.size() + EVP_MAX_BLOCK_LENGTH);
            tag.resize(tagSize);
            int outputLength = 0;
            int totalLength = 0;
            int aadLength = 0;

            bool success = EVP_EncryptInit_ex(context, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1
                    && EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN, nonce.size(), nullptr) == 1
                    && EVP_EncryptInit_ex(context, nullptr, nullptr,
                                          reinterpret_cast<const unsigned char *>(key.constData()),
                                          reinterpret_cast<const unsigned char *>(nonce.constData()))
                            == 1
                    && EVP_EncryptUpdate(context, nullptr, &aadLength,
                                         reinterpret_cast<const unsigned char *>(envelopeAad.constData()),
                                         envelopeAad.size())
                            == 1
                    && EVP_EncryptUpdate(context, reinterpret_cast<unsigned char *>(ciphertext.data()), &outputLength,
                                         reinterpret_cast<const unsigned char *>(payload.constData()), payload.size())
                            == 1;
            if (success) {
                totalLength = outputLength;
                success = EVP_EncryptFinal_ex(context, reinterpret_cast<unsigned char *>(ciphertext.data()) + totalLength,
                                              &outputLength)
                        == 1;
                totalLength += outputLength;
            }
            if (success) {
                success = EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_GET_TAG, tag.size(), tag.data()) == 1;
            }

            EVP_CIPHER_CTX_free(context);
            if (!success) {
                ciphertext.clear();
                tag.clear();
                return false;
            }
            ciphertext.resize(totalLength);
            return true;
        }

        bool decryptPayload(const QByteArray &ciphertext, const QByteArray &key, const QByteArray &nonce,
                            const QByteArray &tag, QByteArray &payload)
        {
            EVP_CIPHER_CTX *context = EVP_CIPHER_CTX_new();
            if (!context) {
                return false;
            }

            payload.resize(ciphertext.size() + EVP_MAX_BLOCK_LENGTH);
            int outputLength = 0;
            int totalLength = 0;
            int aadLength = 0;

            bool success = EVP_DecryptInit_ex(context, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1
                    && EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN, nonce.size(), nullptr) == 1
                    && EVP_DecryptInit_ex(context, nullptr, nullptr,
                                          reinterpret_cast<const unsigned char *>(key.constData()),
                                          reinterpret_cast<const unsigned char *>(nonce.constData()))
                            == 1
                    && EVP_DecryptUpdate(context, nullptr, &aadLength,
                                         reinterpret_cast<const unsigned char *>(envelopeAad.constData()),
                                         envelopeAad.size())
                            == 1
                    && EVP_DecryptUpdate(context, reinterpret_cast<unsigned char *>(payload.data()), &outputLength,
                                         reinterpret_cast<const unsigned char *>(ciphertext.constData()),
                                         ciphertext.size())
                            == 1;
            if (success) {
                totalLength = outputLength;
                success = EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_TAG, tag.size(),
                                              const_cast<char *>(tag.constData()))
                                == 1
                        && EVP_DecryptFinal_ex(context, reinterpret_cast<unsigned char *>(payload.data()) + totalLength,
                                               &outputLength)
                                == 1;
                totalLength += outputLength;
            }

            EVP_CIPHER_CTX_free(context);
            if (!success) {
                payload.clear();
                return false;
            }
            payload.resize(totalLength);
            return true;
        }

        SubscriptionCodec::DecodeResult decodeInternal(const QByteArray &source, SubscriptionCodec::Encoding encoding,
                                                       const QString &deviceId, int depth)
        {
            SubscriptionCodec::DecodeResult result;
            if (source.isEmpty()) {
                result.error = QStringLiteral("Subscription payload is empty.");
                return result;
            }
            if (source.size() > maxPayloadSize) {
                result.error = QStringLiteral("Subscription payload exceeds the 4 MiB limit.");
                return result;
            }
            if (depth > 2) {
                result.error = QStringLiteral("Subscription encoding is nested too deeply.");
                return result;
            }

            const QByteArray trimmed = source.trimmed();
            if (trimmed.startsWith("16x")) {
                if (!SubscriptionCodec::isValidDeviceId(deviceId)) {
                    result.error = QStringLiteral("A valid local 32x device ID is required for this 16x token.");
                    return result;
                }

                QByteArray envelope;
                if (!decodeBase64(trimmed.mid(3), envelope) || envelope.size() < 1 + nonceSize + tagSize
                    || envelope.at(0) != tokenVersion) {
                    result.error = QStringLiteral("Invalid 16x subscription token.");
                    return result;
                }

                const QByteArray nonce = envelope.mid(1, nonceSize);
                const QByteArray tag = envelope.mid(1 + nonceSize, tagSize);
                const QByteArray ciphertext = envelope.mid(1 + nonceSize + tagSize);
                QByteArray payload;
                if (!decryptPayload(ciphertext, encryptionKey(deviceId), nonce, tag, payload)) {
                    result.error = QStringLiteral("The 16x token does not belong to this 32x device or is corrupted.");
                    return result;
                }

                result = decodeInternal(payload, SubscriptionCodec::Encoding::Auto, deviceId, depth + 1);
                result.deviceBound = true;
                return result;
            }

            QByteArray decoded = trimmed;
            SubscriptionCodec::Encoding detected = encoding;
            if (encoding == SubscriptionCodec::Encoding::Auto) {
                if (isLikelyPlainFeed(trimmed)) {
                    detected = SubscriptionCodec::Encoding::Plain;
                } else {
                    QByteArray candidate;
                    if (decodeHex(trimmed, candidate) && isLikelyPlainFeed(candidate)) {
                        decoded = candidate;
                        detected = SubscriptionCodec::Encoding::Hex;
                    } else if (decodeBase64(trimmed, candidate) && isLikelyPlainFeed(candidate)) {
                        decoded = candidate;
                        detected = SubscriptionCodec::Encoding::Base64;
                    } else {
                        detected = SubscriptionCodec::Encoding::Plain;
                    }
                }
            } else if (encoding == SubscriptionCodec::Encoding::Base64) {
                if (!decodeBase64(trimmed, decoded)) {
                    result.error = QStringLiteral("Subscription is not valid Base64 data.");
                    return result;
                }
            } else if (encoding == SubscriptionCodec::Encoding::Hex) {
                if (!decodeHex(trimmed, decoded)) {
                    result.error = QStringLiteral("Subscription is not valid hexadecimal data.");
                    return result;
                }
            }

            if (decoded.size() > maxPayloadSize) {
                result.error = QStringLiteral("Decoded subscription exceeds the 4 MiB limit.");
                return result;
            }

            result.entries = splitEntries(decoded, result.error);
            if (result.entries.isEmpty()) {
                return result;
            }
            if (result.entries.size() > maxEntries) {
                result.entries.clear();
                result.error = QStringLiteral("Subscription contains more than 512 entries.");
                return result;
            }
            result.success = true;
            result.encoding = detected;
            return result;
        }
    } // namespace

    bool SubscriptionCodec::parseEncoding(const QString &value, Encoding &encoding)
    {
        const QString normalized = value.trimmed().toLower();
        if (normalized.isEmpty() || normalized == QLatin1String("auto")) {
            encoding = Encoding::Auto;
            return true;
        }
        if (normalized == QLatin1String("plain") || normalized == QLatin1String("text")
            || normalized == QLatin1String("opentext")) {
            encoding = Encoding::Plain;
            return true;
        }
        if (normalized == QLatin1String("base64") || normalized == QLatin1String("b64")) {
            encoding = Encoding::Base64;
            return true;
        }
        if (normalized == QLatin1String("hex")) {
            encoding = Encoding::Hex;
            return true;
        }
        return false;
    }

    QString SubscriptionCodec::encodingName(Encoding encoding)
    {
        switch (encoding) {
        case Encoding::Auto: return QStringLiteral("auto");
        case Encoding::Plain: return QStringLiteral("plain");
        case Encoding::Base64: return QStringLiteral("base64");
        case Encoding::Hex: return QStringLiteral("hex");
        }
        return QStringLiteral("unknown");
    }

    SubscriptionCodec::DecodeResult SubscriptionCodec::decode(const QByteArray &source, Encoding encoding,
                                                              const QString &deviceId)
    {
        return decodeInternal(source, encoding, deviceId, 0);
    }

    QString SubscriptionCodec::pack16x(const QByteArray &payload, const QString &deviceId, QString &error)
    {
        error.clear();
        if (!isValidDeviceId(deviceId)) {
            error = QStringLiteral("Device ID must be 32x followed by 64 hexadecimal characters.");
            return {};
        }
        if (payload.isEmpty()) {
            error = QStringLiteral("Subscription payload is empty.");
            return {};
        }
        if (payload.size() > maxPayloadSize) {
            error = QStringLiteral("Subscription payload exceeds the 4 MiB limit.");
            return {};
        }

        QByteArray nonce;
        QByteArray ciphertext;
        QByteArray tag;
        if (!encryptPayload(payload, encryptionKey(deviceId), nonce, ciphertext, tag)) {
            error = QStringLiteral("Unable to encrypt the 16x subscription token.");
            return {};
        }

        QByteArray envelope;
        envelope.reserve(1 + nonce.size() + tag.size() + ciphertext.size());
        envelope.append(tokenVersion);
        envelope.append(nonce);
        envelope.append(tag);
        envelope.append(ciphertext);
        return QStringLiteral("16x")
                + QString::fromLatin1(envelope.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    }

    QString SubscriptionCodec::createDeviceId(const QByteArray &machineSeed)
    {
        QByteArray material("amnezia-32x-device-v1");
        material.append('\0');
        material.append(machineSeed);
        return QStringLiteral("32x")
                + QString::fromLatin1(QCryptographicHash::hash(material, QCryptographicHash::Sha256).toHex());
    }

    bool SubscriptionCodec::isValidDeviceId(const QString &deviceId)
    {
        static const QRegularExpression pattern(QStringLiteral("^32x[0-9a-fA-F]{64}$"));
        return pattern.match(deviceId.trimmed()).hasMatch();
    }

    QString SubscriptionCodec::entryKind(const QString &entry)
    {
        const QString trimmed = entry.trimmed();
        const QUrl url(trimmed);
        if (url.isValid() && !url.scheme().isEmpty() && trimmed.contains(QLatin1String("://"))) {
            const QString scheme = url.scheme().toLower();
            if (scheme == QLatin1String("ss") || scheme == QLatin1String("ssd")) {
                return QStringLiteral("shadowsocks");
            }
            return scheme;
        }
        if (trimmed.contains(QLatin1String("[Interface]")) && trimmed.contains(QLatin1String("[Peer]"))) {
            return trimmed.contains(QLatin1String("Jc ="), Qt::CaseInsensitive) ? QStringLiteral("awg")
                                                                                : QStringLiteral("wireguard");
        }
        if (trimmed.contains(QRegularExpression(QStringLiteral("(?m)^client\\s*$")))) {
            return QStringLiteral("openvpn");
        }

        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(trimmed.toUtf8(), &error);
        if (error.error == QJsonParseError::NoError && document.isObject()) {
            const QJsonObject object = document.object();
            if (object.contains(QStringLiteral("containers")) || object.contains(QStringLiteral("api_config"))) {
                return QStringLiteral("amnezia");
            }
            if (object.contains(QStringLiteral("inbounds")) && object.contains(QStringLiteral("outbounds"))) {
                return QStringLiteral("xray");
            }
            return QStringLiteral("json");
        }
        return QStringLiteral("unknown");
    }

} // namespace amnezia
