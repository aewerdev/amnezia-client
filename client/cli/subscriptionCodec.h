#ifndef SUBSCRIPTIONCODEC_H
#define SUBSCRIPTIONCODEC_H

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace amnezia::cli
{

    class SubscriptionCodec
    {
    public:
        enum class Encoding {
            Auto,
            Plain,
            Base64,
            Hex
        };

        struct DecodeResult
        {
            bool success = false;
            Encoding encoding = Encoding::Plain;
            QStringList entries;
            QString error;
            bool deviceBound = false;
        };

        static bool parseEncoding(const QString &value, Encoding &encoding);
        static QString encodingName(Encoding encoding);

        static DecodeResult decode(const QByteArray &source, Encoding encoding, const QString &deviceId);
        static QString pack16x(const QByteArray &payload, const QString &deviceId, QString &error);

        static QString createDeviceId(const QByteArray &machineSeed);
        static bool isValidDeviceId(const QString &deviceId);
        static QString entryKind(const QString &entry);
    };

} // namespace amnezia::cli

#endif // SUBSCRIPTIONCODEC_H
