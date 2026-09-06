#ifndef UNIVERSALSUBSCRIPTIONUICONTROLLER_H
#define UNIVERSALSUBSCRIPTIONUICONTROLLER_H

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QVariantList>
#include <QVector>

class ImportController;
class QNetworkAccessManager;
class QNetworkReply;
class SecureAppSettingsRepository;

class UniversalSubscriptionUiController : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString deviceId READ deviceId CONSTANT)
    Q_PROPERTY(QVariantList entries READ entries NOTIFY inspectionChanged)
    Q_PROPERTY(int entryCount READ entryCount NOTIFY inspectionChanged)
    Q_PROPERTY(int validCount READ validCount NOTIFY inspectionChanged)
    Q_PROPERTY(int invalidCount READ invalidCount NOTIFY inspectionChanged)
    Q_PROPERTY(QString detectedEncoding READ detectedEncoding NOTIFY inspectionChanged)
    Q_PROPERTY(bool deviceBound READ deviceBound NOTIFY inspectionChanged)
    Q_PROPERTY(bool hasInspection READ hasInspection NOTIFY inspectionChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY inspectionChanged)
    Q_PROPERTY(QString generatedToken READ generatedToken NOTIFY generatedTokenChanged)

public:
    explicit UniversalSubscriptionUiController(ImportController *importController,
                                               SecureAppSettingsRepository *appSettingsRepository,
                                               QObject *parent = nullptr);

    bool busy() const;
    QString deviceId() const;
    QVariantList entries() const;
    int entryCount() const;
    int validCount() const;
    int invalidCount() const;
    QString detectedEncoding() const;
    bool deviceBound() const;
    bool hasInspection() const;
    QString errorText() const;
    QString generatedToken() const;

public slots:
    void inspectData(const QString &data, const QString &encodingName);
    void inspectFile(const QString &fileName, const QString &encodingName);
    void inspectUrl(const QString &url, const QString &encodingName);
    void importInspected(bool allowPartial);
    void createToken(const QString &data, const QString &targetDeviceId);
    void createTokenFromFile(const QString &fileName, const QString &targetDeviceId);
    void createTokenFromUrl(const QString &url, const QString &targetDeviceId);
    bool saveGeneratedToken(const QString &fileName);
    void clearInspection();
    void clearGeneratedToken();

signals:
    void busyChanged();
    void inspectionChanged();
    void generatedTokenChanged();
    void operationError(const QString &message);
    void importCompleted(int importedCount, int failedCount);

private:
    enum class PendingAction {
        None,
        Inspect,
        CreateToken
    };

    void inspectPayload(const QByteArray &payload, const QString &encodingName);
    void importNext();
    void startDownload(const QString &urlText, PendingAction action, const QString &encodingName,
                       const QString &targetDeviceId);
    void resetInspection();
    void cancelPendingRequest();
    void setBusy(bool busy);
    void setInspectionError(const QString &message);
    void reportSourceError(PendingAction action, const QString &message);
    QString normalizedImportError(int errorCode) const;

    ImportController *m_importController;
    QNetworkAccessManager *m_networkManager;
    QPointer<QNetworkReply> m_reply;

    QByteArray m_downloadedData;
    QString m_pendingEncodingName;
    QString m_pendingTargetDeviceId;
    PendingAction m_pendingAction = PendingAction::None;
    bool m_downloadTooLarge = false;
    bool m_busy = false;
    bool m_hasInspection = false;
    bool m_deviceBound = false;
    bool m_batchActive = false;
    int m_validCount = 0;
    int m_invalidCount = 0;
    int m_batchImportedCount = 0;
    int m_batchFailedCount = 0;
    qsizetype m_nextImportIndex = 0;

    QString m_deviceId;
    QString m_detectedEncoding;
    QString m_errorText;
    QString m_generatedToken;
    QVariantList m_entries;
    QVector<QJsonObject> m_validConfigs;
    QVector<QJsonObject> m_importQueue;
};

#endif // UNIVERSALSUBSCRIPTIONUICONTROLLER_H
