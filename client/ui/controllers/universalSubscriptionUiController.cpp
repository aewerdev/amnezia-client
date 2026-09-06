#include "universalSubscriptionUiController.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSysInfo>
#include <QTimer>
#include <QUrl>

#include <utility>

#include "core/controllers/selfhosted/importController.h"
#include "core/repositories/secureAppSettingsRepository.h"
#include "core/utils/errorStrings.h"
#include "core/utils/subscriptionCodec.h"
#include "systemController.h"

namespace
{
    constexpr qsizetype maxSubscriptionSize = 4 * 1024 * 1024;
    constexpr int subscriptionTimeoutMs = 20000;
}

UniversalSubscriptionUiController::UniversalSubscriptionUiController(
        ImportController *importController, SecureAppSettingsRepository *appSettingsRepository, QObject *parent)
    : QObject(parent),
      m_importController(importController),
      m_networkManager(new QNetworkAccessManager(this))
{
    QByteArray seed = QSysInfo::machineUniqueId();
    if (seed.isEmpty()) {
        seed = appSettingsRepository->getInstallationUuid(true).toUtf8();
    }
    m_deviceId = amnezia::SubscriptionCodec::createDeviceId(seed);

    connect(m_importController, &ImportController::importFinished, this, [this]() {
        if (m_batchActive) {
            ++m_batchImportedCount;
        }
    });
    connect(m_importController, &ImportController::importErrorOccurred, this,
            [this](ErrorCode, bool) {
                if (m_batchActive) {
                    ++m_batchFailedCount;
                }
            });
}

bool UniversalSubscriptionUiController::busy() const
{
    return m_busy;
}

QString UniversalSubscriptionUiController::deviceId() const
{
    return m_deviceId;
}

QVariantList UniversalSubscriptionUiController::entries() const
{
    return m_entries;
}

int UniversalSubscriptionUiController::entryCount() const
{
    return m_entries.size();
}

int UniversalSubscriptionUiController::validCount() const
{
    return m_validCount;
}

int UniversalSubscriptionUiController::invalidCount() const
{
    return m_invalidCount;
}

QString UniversalSubscriptionUiController::detectedEncoding() const
{
    return m_detectedEncoding;
}

bool UniversalSubscriptionUiController::deviceBound() const
{
    return m_deviceBound;
}

bool UniversalSubscriptionUiController::hasInspection() const
{
    return m_hasInspection;
}

QString UniversalSubscriptionUiController::errorText() const
{
    return m_errorText;
}

QString UniversalSubscriptionUiController::generatedToken() const
{
    return m_generatedToken;
}

void UniversalSubscriptionUiController::inspectData(const QString &data, const QString &encodingName)
{
    cancelPendingRequest();
    setBusy(false);
    inspectPayload(data.toUtf8(), encodingName);
}

void UniversalSubscriptionUiController::inspectFile(const QString &fileName, const QString &encodingName)
{
    cancelPendingRequest();
    setBusy(false);

    QString data;
    if (!SystemController::readFile(fileName, data)) {
        setInspectionError(tr("Unable to open the subscription file."));
        return;
    }
    inspectPayload(data.toUtf8(), encodingName);
}

void UniversalSubscriptionUiController::inspectUrl(const QString &urlText, const QString &encodingName)
{
    cancelPendingRequest();
    setBusy(false);
    resetInspection();
    emit inspectionChanged();

    startDownload(urlText, PendingAction::Inspect, encodingName, {});
}

void UniversalSubscriptionUiController::startDownload(const QString &urlText, PendingAction action,
                                                       const QString &encodingName,
                                                       const QString &targetDeviceId)
{

    const QUrl url = QUrl::fromUserInput(urlText.trimmed());
    if (!url.isValid() || url.host().isEmpty()
        || (url.scheme().compare(QLatin1String("http"), Qt::CaseInsensitive) != 0
            && url.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) != 0)) {
        reportSourceError(action, tr("Enter a valid HTTP or HTTPS subscription URL."));
        return;
    }

    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setMaximumRedirectsAllowed(5);
    request.setTransferTimeout(subscriptionTimeoutMs);
    request.setRawHeader(QByteArrayLiteral("User-Agent"), QByteArrayLiteral("AmneziaVPN Universal Subscriptions"));

    m_downloadedData.clear();
    m_pendingEncodingName = encodingName;
    m_pendingTargetDeviceId = targetDeviceId;
    m_pendingAction = action;
    m_downloadTooLarge = false;
    m_reply = m_networkManager->get(request);
    setBusy(true);

    QNetworkReply *reply = m_reply;
    connect(reply, &QNetworkReply::readyRead, this, [this, reply]() {
        if (m_reply != reply || m_downloadTooLarge) {
            return;
        }
        m_downloadedData.append(reply->readAll());
        if (m_downloadedData.size() > maxSubscriptionSize) {
            m_downloadTooLarge = true;
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        if (m_reply != reply) {
            reply->deleteLater();
            return;
        }

        if (!m_downloadTooLarge) {
            m_downloadedData.append(reply->readAll());
        }
        const auto networkError = reply->error();
        const QString networkErrorText = reply->errorString();
        const PendingAction action = m_pendingAction;
        const QString encodingName = std::move(m_pendingEncodingName);
        const QString targetDeviceId = std::move(m_pendingTargetDeviceId);
        const QByteArray downloadedData = std::move(m_downloadedData);
        m_pendingAction = PendingAction::None;
        m_pendingEncodingName.clear();
        m_pendingTargetDeviceId.clear();
        m_downloadedData.clear();
        m_reply = nullptr;
        reply->deleteLater();
        setBusy(false);

        if (m_downloadTooLarge || downloadedData.size() > maxSubscriptionSize) {
            reportSourceError(action, tr("Subscription payload exceeds the 4 MiB limit."));
            return;
        }
        if (networkError != QNetworkReply::NoError) {
            reportSourceError(action, tr("Unable to download the subscription: %1").arg(networkErrorText));
            return;
        }

        if (action == PendingAction::Inspect) {
            inspectPayload(downloadedData, encodingName);
        } else if (action == PendingAction::CreateToken) {
            createToken(QString::fromUtf8(downloadedData), targetDeviceId);
        }
    });
}

void UniversalSubscriptionUiController::importInspected(bool allowPartial)
{
    if (m_batchActive) {
        return;
    }
    if (!m_hasInspection || m_validConfigs.isEmpty()) {
        emit operationError(tr("There are no valid configurations to import."));
        return;
    }
    if (m_invalidCount > 0 && !allowPartial) {
        emit operationError(tr("Fix invalid entries or enable partial import."));
        return;
    }

    m_batchImportedCount = 0;
    m_batchFailedCount = 0;
    m_nextImportIndex = 0;
    m_importQueue = m_validConfigs;
    m_batchActive = true;
    setBusy(true);
    QTimer::singleShot(0, this, &UniversalSubscriptionUiController::importNext);
}

void UniversalSubscriptionUiController::importNext()
{
    if (!m_batchActive) {
        return;
    }
    if (m_nextImportIndex >= m_importQueue.size()) {
        m_batchActive = false;
        m_importQueue.clear();
        setBusy(false);
        emit importCompleted(m_batchImportedCount, m_batchFailedCount);
        return;
    }

    m_importController->importConfig(m_importQueue.at(m_nextImportIndex++));
    QTimer::singleShot(0, this, &UniversalSubscriptionUiController::importNext);
}

void UniversalSubscriptionUiController::createToken(const QString &data, const QString &targetDeviceId)
{
    QString error;
    const QString token = amnezia::SubscriptionCodec::pack16x(data.toUtf8(), targetDeviceId, error);
    if (token.isEmpty()) {
        clearGeneratedToken();
        emit operationError(error);
        return;
    }

    if (m_generatedToken == token) {
        return;
    }
    m_generatedToken = token;
    emit generatedTokenChanged();
}

void UniversalSubscriptionUiController::createTokenFromFile(const QString &fileName, const QString &targetDeviceId)
{
    cancelPendingRequest();
    setBusy(false);
    clearGeneratedToken();

    QString data;
    if (!SystemController::readFile(fileName, data)) {
        emit operationError(tr("Unable to open the subscription file."));
        return;
    }
    createToken(data, targetDeviceId);
}

void UniversalSubscriptionUiController::createTokenFromUrl(const QString &url, const QString &targetDeviceId)
{
    cancelPendingRequest();
    setBusy(false);
    clearGeneratedToken();
    startDownload(url, PendingAction::CreateToken, {}, targetDeviceId);
}

bool UniversalSubscriptionUiController::saveGeneratedToken(const QString &fileName)
{
    if (m_generatedToken.isEmpty()) {
        emit operationError(tr("Create a 16x token before saving it."));
        return false;
    }
    if (!SystemController::saveFile(fileName, m_generatedToken.toUtf8())) {
        emit operationError(tr("Unable to save the 16x token."));
        return false;
    }
    return true;
}

void UniversalSubscriptionUiController::clearInspection()
{
    cancelPendingRequest();
    setBusy(false);
    resetInspection();
    emit inspectionChanged();
}

void UniversalSubscriptionUiController::clearGeneratedToken()
{
    if (m_generatedToken.isEmpty()) {
        return;
    }
    m_generatedToken.clear();
    emit generatedTokenChanged();
}

void UniversalSubscriptionUiController::inspectPayload(const QByteArray &payload, const QString &encodingName)
{
    resetInspection();

    amnezia::SubscriptionCodec::Encoding encoding;
    if (!amnezia::SubscriptionCodec::parseEncoding(encodingName, encoding)) {
        setInspectionError(tr("Unknown subscription encoding."));
        return;
    }

    const auto decoded = amnezia::SubscriptionCodec::decode(payload, encoding, m_deviceId);
    if (!decoded.success) {
        setInspectionError(decoded.error);
        return;
    }

    m_hasInspection = true;
    m_deviceBound = decoded.deviceBound;
    m_detectedEncoding = amnezia::SubscriptionCodec::encodingName(decoded.encoding);

    int index = 0;
    for (const QString &entry : decoded.entries) {
        const auto result = m_importController->extractConfigFromData(entry);
        const bool valid = result.errorCode == ErrorCode::NoError;

        QVariantMap row;
        row.insert(QStringLiteral("index"), ++index);
        row.insert(QStringLiteral("kind"), amnezia::SubscriptionCodec::entryKind(entry));
        row.insert(QStringLiteral("valid"), valid);
        row.insert(QStringLiteral("warning"), valid && !result.maliciousWarningText.isEmpty());
        if (valid) {
            ++m_validCount;
            m_validConfigs.append(result.config);
            row.insert(QStringLiteral("message"), result.maliciousWarningText);
        } else {
            ++m_invalidCount;
            row.insert(QStringLiteral("message"), normalizedImportError(static_cast<int>(result.errorCode)));
        }
        m_entries.append(row);
    }

    emit inspectionChanged();
}

void UniversalSubscriptionUiController::resetInspection()
{
    m_hasInspection = false;
    m_deviceBound = false;
    m_validCount = 0;
    m_invalidCount = 0;
    m_detectedEncoding.clear();
    m_errorText.clear();
    m_entries.clear();
    m_validConfigs.clear();
}

void UniversalSubscriptionUiController::cancelPendingRequest()
{
    if (m_reply) {
        disconnect(m_reply, nullptr, this, nullptr);
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
    m_downloadedData.clear();
    m_pendingEncodingName.clear();
    m_pendingTargetDeviceId.clear();
    m_pendingAction = PendingAction::None;
    m_downloadTooLarge = false;
}

void UniversalSubscriptionUiController::setBusy(bool busy)
{
    if (m_busy == busy) {
        return;
    }
    m_busy = busy;
    emit busyChanged();
}

void UniversalSubscriptionUiController::setInspectionError(const QString &message)
{
    resetInspection();
    m_errorText = message;
    emit inspectionChanged();
}

void UniversalSubscriptionUiController::reportSourceError(PendingAction action, const QString &message)
{
    if (action == PendingAction::Inspect) {
        setInspectionError(message);
    } else {
        emit operationError(message);
    }
}

QString UniversalSubscriptionUiController::normalizedImportError(int errorCodeValue) const
{
    const QString fullMessage = errorString(static_cast<ErrorCode>(errorCodeValue));
    const qsizetype separator = fullMessage.indexOf(QLatin1String(". "));
    return separator >= 0 ? fullMessage.mid(separator + 2) : fullMessage;
}
