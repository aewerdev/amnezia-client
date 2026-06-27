#include <atomic>
#include <csignal>
#include <functional>
#include <memory>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QFuture>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QMetaEnum>
#include <QProcess>
#include <QRegularExpression>
#include <QScopedPointer>
#include <QSharedPointer>
#include <QStringList>
#include <QTextStream>
#include <QTimer>
#include <QVariantMap>

#include <libssh/libssh.h>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include "core/controllers/allowedDnsController.h"
#include "core/controllers/api/newsController.h"
#include "core/controllers/api/servicesCatalogController.h"
#include "core/controllers/api/subscriptionController.h"
#include "core/controllers/appSplitTunnelingController.h"
#include "core/controllers/connectionController.h"
#include "core/controllers/ipSplitTunnelingController.h"
#include "core/controllers/selfhosted/exportController.h"
#include "core/controllers/selfhosted/importController.h"
#include "core/controllers/selfhosted/installController.h"
#include "core/controllers/selfhosted/usersController.h"
#include "core/controllers/serversController.h"
#include "core/controllers/settingsController.h"
#include "core/models/containerConfig.h"
#include "core/models/serverDescription.h"
#include "core/protocols/protocolUtils.h"
#include "core/repositories/secureAppSettingsRepository.h"
#include "core/repositories/secureServersRepository.h"
#include "core/utils/constants/apiKeys.h"
#include "core/utils/containers/containerUtils.h"
#include "core/utils/errorStrings.h"
#include "core/utils/ipcClient.h"
#include "core/utils/serverConfigUtils.h"
#include "logger.h"
#include "mozilla/localsocketcontroller.h"
#include "secureQSettings.h"
#include "version.h"
#include "vpnConnection.h"

using namespace amnezia;

namespace
{
std::atomic_bool g_stopRequested = false;

#ifdef Q_OS_WIN
BOOL WINAPI consoleHandler(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        g_stopRequested = true;
        return TRUE;
    }
    return FALSE;
}
#endif

void signalHandler(int)
{
    g_stopRequested = true;
}

QString boolText(bool value)
{
    return value ? QStringLiteral("on") : QStringLiteral("off");
}

QString jsonCompact(const QJsonObject &object)
{
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

QString jsonPretty(const QJsonObject &object)
{
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Indented));
}

QString jsonPretty(const QJsonArray &array)
{
    return QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Indented));
}

bool writeFile(const QString &path, const QByteArray &data, QString &error)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        error = file.errorString();
        return false;
    }
    if (file.write(data) != data.size()) {
        error = file.errorString();
        return false;
    }
    return true;
}

bool readFile(const QString &path, QByteArray &data, QString &error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error = file.errorString();
        return false;
    }
    data = file.readAll();
    return true;
}

QString stateName(Vpn::ConnectionState state)
{
    const QMetaEnum meta = QMetaEnum::fromType<Vpn::ConnectionState>();
    const char *key = meta.valueToKey(static_cast<int>(state));
    return key ? QString::fromLatin1(key) : QStringLiteral("Unknown");
}

QString routeModeName(RouteMode mode)
{
    switch (mode) {
    case RouteMode::VpnAllSites:
        return QStringLiteral("all");
    case RouteMode::VpnOnlyForwardSites:
        return QStringLiteral("only");
    case RouteMode::VpnAllExceptSites:
        return QStringLiteral("except");
    }
    return QStringLiteral("all");
}

QString appsRouteModeName(AppsRouteMode mode)
{
    switch (mode) {
    case AppsRouteMode::VpnAllApps:
        return QStringLiteral("all");
    case AppsRouteMode::VpnOnlyForwardApps:
        return QStringLiteral("only");
    case AppsRouteMode::VpnAllExceptApps:
        return QStringLiteral("except");
    }
    return QStringLiteral("all");
}

RouteMode parseRouteMode(const QString &value, bool &ok)
{
    const QString normalized = value.trimmed().toLower();
    ok = true;
    if (normalized == QLatin1String("all") || normalized == QLatin1String("vpn-all")) {
        return RouteMode::VpnAllSites;
    }
    if (normalized == QLatin1String("only") || normalized == QLatin1String("forward") || normalized == QLatin1String("vpn-only")) {
        return RouteMode::VpnOnlyForwardSites;
    }
    if (normalized == QLatin1String("except") || normalized == QLatin1String("exclude") || normalized == QLatin1String("vpn-except")) {
        return RouteMode::VpnAllExceptSites;
    }
    ok = false;
    return RouteMode::VpnAllSites;
}

AppsRouteMode parseAppsRouteMode(const QString &value, bool &ok)
{
    const QString normalized = value.trimmed().toLower();
    ok = true;
    if (normalized == QLatin1String("all") || normalized == QLatin1String("vpn-all")) {
        return AppsRouteMode::VpnAllApps;
    }
    if (normalized == QLatin1String("only") || normalized == QLatin1String("forward") || normalized == QLatin1String("vpn-only")) {
        return AppsRouteMode::VpnOnlyForwardApps;
    }
    if (normalized == QLatin1String("except") || normalized == QLatin1String("exclude") || normalized == QLatin1String("vpn-except")) {
        return AppsRouteMode::VpnAllExceptApps;
    }
    ok = false;
    return AppsRouteMode::VpnAllApps;
}

DockerContainer parseContainer(const QString &value, bool &ok)
{
    ok = true;
    QString normalized = value.trimmed().toLower();
    if (normalized.isEmpty()) {
        ok = false;
        return DockerContainer::None;
    }

    bool numberOk = false;
    int number = normalized.toInt(&numberOk);
    if (numberOk) {
        DockerContainer c = static_cast<DockerContainer>(number);
        if (ContainerUtils::allContainers().contains(c)) {
            return c;
        }
        ok = false;
        return DockerContainer::None;
    }

    normalized.replace(QLatin1Char('_'), QLatin1Char('-'));
    const QMap<QString, DockerContainer> aliases = {
        { QStringLiteral("awg"), DockerContainer::Awg2 },
        { QStringLiteral("amneziawg"), DockerContainer::Awg2 },
        { QStringLiteral("amnezia-wg"), DockerContainer::Awg2 },
        { QStringLiteral("amnezia-awg"), DockerContainer::Awg },
        { QStringLiteral("amnezia-awg2"), DockerContainer::Awg2 },
        { QStringLiteral("wg"), DockerContainer::WireGuard },
        { QStringLiteral("wireguard"), DockerContainer::WireGuard },
        { QStringLiteral("amnezia-wireguard"), DockerContainer::WireGuard },
        { QStringLiteral("ovpn"), DockerContainer::OpenVpn },
        { QStringLiteral("openvpn"), DockerContainer::OpenVpn },
        { QStringLiteral("amnezia-openvpn"), DockerContainer::OpenVpn },
        { QStringLiteral("xray"), DockerContainer::Xray },
        { QStringLiteral("amnezia-xray"), DockerContainer::Xray },
        { QStringLiteral("sftp"), DockerContainer::Sftp },
        { QStringLiteral("amnezia-sftp"), DockerContainer::Sftp },
        { QStringLiteral("dns"), DockerContainer::Dns },
        { QStringLiteral("amnezia-dns"), DockerContainer::Dns },
        { QStringLiteral("socks5"), DockerContainer::Socks5Proxy },
        { QStringLiteral("socks5proxy"), DockerContainer::Socks5Proxy },
        { QStringLiteral("mtproxy"), DockerContainer::MtProxy },
        { QStringLiteral("telemt"), DockerContainer::Telemt },
        { QStringLiteral("tor"), DockerContainer::TorWebSite },
        { QStringLiteral("torwebsite"), DockerContainer::TorWebSite },
    };

    if (aliases.contains(normalized)) {
        return aliases.value(normalized);
    }

    const DockerContainer c = ContainerUtils::containerFromString(normalized);
    if (c != DockerContainer::None || normalized == QLatin1String("none")) {
        return c;
    }

    ok = false;
    return DockerContainer::None;
}

QString containerLabel(DockerContainer container)
{
    const auto names = ContainerUtils::containerHumanNames();
    return names.value(container, ContainerUtils::containerToString(container));
}

TransportProto parseTransport(const QString &value)
{
    return ProtocolUtils::transportProtoFromString(value);
}

QString configTypeName(serverConfigUtils::ConfigType type)
{
    switch (type) {
    case serverConfigUtils::AmneziaFreeV2:
        return QStringLiteral("free-v2");
    case serverConfigUtils::AmneziaFreeV3:
        return QStringLiteral("free-v3");
    case serverConfigUtils::AmneziaPremiumV1:
        return QStringLiteral("premium-v1");
    case serverConfigUtils::AmneziaPremiumV2:
        return QStringLiteral("premium-v2");
    case serverConfigUtils::SelfHosted:
        return QStringLiteral("self-hosted");
    case serverConfigUtils::ExternalPremium:
        return QStringLiteral("external-premium");
    case serverConfigUtils::SelfHostedAdmin:
        return QStringLiteral("self-hosted-admin");
    case serverConfigUtils::SelfHostedUser:
        return QStringLiteral("self-hosted-user");
    case serverConfigUtils::Native:
        return QStringLiteral("native");
    case serverConfigUtils::Invalid:
    default:
        return QStringLiteral("invalid");
    }
}

QString cleanError(ErrorCode code)
{
    return errorString(code).replace(QLatin1Char('\n'), QLatin1Char(' '));
}

QString cliCommandName()
{
    return QStringLiteral("amnezia");
}

class ArgView
{
public:
    explicit ArgView(QStringList args) : m_args(std::move(args)) {}

    bool has(const QString &name) const
    {
        return m_args.contains(name);
    }

    bool hasAny(std::initializer_list<const char *> names) const
    {
        for (const char *name : names) {
            if (has(QString::fromLatin1(name))) {
                return true;
            }
        }
        return false;
    }

    QString value(const QString &name, const QString &fallback = QString()) const
    {
        const int index = m_args.indexOf(name);
        if (index >= 0 && index + 1 < m_args.size()) {
            return m_args.at(index + 1);
        }
        return fallback;
    }

    QString valueAny(std::initializer_list<const char *> names, const QString &fallback = QString()) const
    {
        for (const char *name : names) {
            const QString option = QString::fromLatin1(name);
            if (has(option)) {
                return value(option, fallback);
            }
        }
        return fallback;
    }

    QStringList positionals() const
    {
        QStringList result;
        for (int i = 0; i < m_args.size(); ++i) {
            const QString arg = m_args.at(i);
            if (arg.startsWith(QLatin1String("--"))) {
                if (i + 1 < m_args.size() && !m_args.at(i + 1).startsWith(QLatin1String("-"))) {
                    ++i;
                }
                continue;
            }
            if (arg.startsWith(QLatin1Char('-'))) {
                if (i + 1 < m_args.size() && !m_args.at(i + 1).startsWith(QLatin1Char('-'))) {
                    ++i;
                }
                continue;
            }
            result.append(arg);
        }
        return result;
    }

private:
    QStringList m_args;
};

class Style
{
public:
    explicit Style(bool enabled) : m_enabled(enabled) {}

    QString dim(const QString &text) const { return wrap(QStringLiteral("2"), text); }
    QString bold(const QString &text) const { return wrap(QStringLiteral("1"), text); }
    QString cyan(const QString &text) const { return wrap(QStringLiteral("38;2;128;200;191"), text); }
    QString blue(const QString &text) const { return wrap(QStringLiteral("38;2;149;195;217"), text); }
    QString purple(const QString &text) const { return wrap(QStringLiteral("38;2;109;95;164"), text); }
    QString gray(const QString &text) const { return wrap(QStringLiteral("38;2;163;158;157"), text); }
    QString pink(const QString &text) const { return wrap(QStringLiteral("38;2;251;233;231"), text); }
    QString cream(const QString &text) const { return wrap(QStringLiteral("38;2;250;218;166"), text); }
    QString peach(const QString &text) const { return wrap(QStringLiteral("38;2;245;181;114"), text); }
    QString orange(const QString &text) const { return wrap(QStringLiteral("38;2;238;139;43"), text); }
    QString green(const QString &text) const { return wrap(QStringLiteral("38;2;80;200;120"), text); }
    QString red(const QString &text) const { return wrap(QStringLiteral("38;2;240;90;90"), text); }

private:
    QString wrap(const QString &code, const QString &text) const
    {
        if (!m_enabled) {
            return text;
        }
        return QStringLiteral("\x1b[%1m%2\x1b[0m").arg(code, text);
    }

    bool m_enabled;
};

class Cli
{
public:
    Cli()
        : out(stdout),
          err(stderr),
          settings(ORGANIZATION_NAME, APPLICATION_NAME),
          serversRepository(&settings),
          appSettingsRepository(&settings),
          serversController(&serversRepository, &appSettingsRepository),
          settingsController(&serversRepository, &appSettingsRepository),
          importController(&serversRepository, &appSettingsRepository),
          exportController(&serversRepository, &appSettingsRepository),
          installController(&serversRepository, &appSettingsRepository),
          usersController(&serversRepository),
          allowedDnsController(&appSettingsRepository),
          ipSplitController(&appSettingsRepository),
          appSplitController(&appSettingsRepository),
          vpnConnection(&serversRepository, &appSettingsRepository),
          connectionController(&serversRepository, &appSettingsRepository, &vpnConnection),
          subscriptionController(&serversRepository, &appSettingsRepository),
          servicesCatalogController(&appSettingsRepository),
          newsController(&appSettingsRepository, &serversRepository),
          style(true)
    {
    }

    int run(QStringList args)
    {
        configureConsole();
        const bool noColorFlag = args.removeAll(QStringLiteral("--no-color")) > 0;
        style = Style(!qEnvironmentVariableIsSet("NO_COLOR") && !noColorFlag);
        jsonOutput = args.removeAll(QStringLiteral("--json")) > 0;

        if (args.isEmpty()) {
            return shell();
        }

        return dispatch(args, false);
    }

private:
    QTextStream out;
    QTextStream err;

    SecureQSettings settings;
    SecureServersRepository serversRepository;
    SecureAppSettingsRepository appSettingsRepository;
    ServersController serversController;
    SettingsController settingsController;
    ImportController importController;
    ExportController exportController;
    InstallController installController;
    UsersController usersController;
    AllowedDnsController allowedDnsController;
    IpSplitTunnelingController ipSplitController;
    AppSplitTunnelingController appSplitController;
    VpnConnection vpnConnection;
    ConnectionController connectionController;
    SubscriptionController subscriptionController;
    ServicesCatalogController servicesCatalogController;
    NewsController newsController;
    Style style;
    bool jsonOutput = false;

    int dispatch(QStringList args, bool fromShell)
    {
        if (args.isEmpty()) {
            return fromShell ? dashboard(false) : dashboard();
        }

        QString command = args.takeFirst().toLower();

        if (fromShell) {
            if (command == QLatin1String("?")) {
                command = QStringLiteral("help");
            } else if (command == QLatin1String("ls")) {
                command = QStringLiteral("servers");
                args.prepend(QStringLiteral("list"));
            } else if (command == QLatin1String("use")) {
                command = QStringLiteral("servers");
                args.prepend(QStringLiteral("default"));
            } else if (command == QLatin1String("up")) {
                command = QStringLiteral("connect");
            } else if (command == QLatin1String("down")) {
                command = QStringLiteral("disconnect");
            } else if (command == QLatin1String("show")) {
                command = QStringLiteral("status");
            } else if (command == QLatin1String("config") || command == QLatin1String("cfg")) {
                command = QStringLiteral("settings");
                if (args.isEmpty()) {
                    args.append(QStringLiteral("show"));
                }
            }
        }

        if (command == QLatin1String("-h") || command == QLatin1String("--help") || command == QLatin1String("help")) {
            return help(args, !fromShell);
        }
        if (command == QLatin1String("-v") || command == QLatin1String("--version") || command == QLatin1String("version")) {
            out << cliCommandName() << " " << APP_VERSION << Qt::endl;
            return 0;
        }
        if (command == QLatin1String("home") || command == QLatin1String("dashboard")) {
            return dashboard(!fromShell);
        }
        if (command == QLatin1String("logo")) {
            printLogo();
            return 0;
        }
        if (command == QLatin1String("status")) {
            return status(!fromShell);
        }
        if (command == QLatin1String("servers") || command == QLatin1String("server")) {
            return servers(args);
        }
        if (command == QLatin1String("import")) {
            return importConfig(args);
        }
        if (command == QLatin1String("export")) {
            return exportConfig(args);
        }
        if (command == QLatin1String("connect")) {
            return connect(args);
        }
        if (command == QLatin1String("disconnect")) {
            return disconnect();
        }
        if (command == QLatin1String("settings") || command == QLatin1String("setting")) {
            return settingsCommand(args);
        }
        if (command == QLatin1String("dns")) {
            return dns(args);
        }
        if (command == QLatin1String("split")) {
            return split(args);
        }
        if (command == QLatin1String("selfhost") || command == QLatin1String("self-host")) {
            return selfhost(args);
        }
        if (command == QLatin1String("clients") || command == QLatin1String("client")) {
            return clients(args);
        }
        if (command == QLatin1String("subscription") || command == QLatin1String("premium")) {
            return subscription(args);
        }
        if (command == QLatin1String("catalog")) {
            return catalog();
        }
        if (command == QLatin1String("news")) {
            return news();
        }
        if (command == QLatin1String("interactive") || command == QLatin1String("menu") || command == QLatin1String("shell") || command == QLatin1String("app")) {
            return shell();
        }

        return fail(QStringLiteral("Unknown command: %1").arg(command), QStringLiteral("Run `%1 help` for commands.").arg(cliCommandName()));
    }

    void configureConsole()
    {
#ifdef Q_OS_WIN
        SetConsoleOutputCP(CP_UTF8);
        HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
        if (output != INVALID_HANDLE_VALUE) {
            DWORD mode = 0;
            if (GetConsoleMode(output, &mode)) {
                SetConsoleMode(output, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
            }
        }
        SetConsoleCtrlHandler(consoleHandler, TRUE);
#else
        std::signal(SIGINT, signalHandler);
        std::signal(SIGTERM, signalHandler);
#endif
    }

    void printLogo()
    {
        out << QStringLiteral("              ") << style.blue(QStringLiteral("⢸⡄")) << Qt::endl;
        out << QStringLiteral("              ") << style.blue(QStringLiteral("⣸⣷⡀")) << Qt::endl;
        out << QStringLiteral("          ") << style.cyan(QStringLiteral("⣀⣠⣤"))
            << style.blue(QStringLiteral("⣿⣿⣿⣿⣿")) << style.pink(QStringLiteral("⣶⣤⡀")) << Qt::endl;
        out << QStringLiteral("        ") << style.cyan(QStringLiteral("⣀⣼⣿⣿"))
            << style.blue(QStringLiteral("⢿⣿⡿⣿⣿")) << style.pink(QStringLiteral("⠻⠿⣿⣿⣦⡄")) << Qt::endl;
        out << QStringLiteral("       ") << style.cyan(QStringLiteral("⣰⣿⡿⠋")) << QStringLiteral(" ")
            << style.blue(QStringLiteral("⣼⣿⠁⠘⣿⣇")) << QStringLiteral(" ") << style.cream(QStringLiteral("⠈⠙"))
            << style.peach(QStringLiteral("⣿⣿")) << style.orange(QStringLiteral("⣦⣤⡤⠖⠂")) << Qt::endl;
        out << QStringLiteral("      ") << style.cyan(QStringLiteral("⣰⣿⠏")) << QStringLiteral("  ")
            << style.blue(QStringLiteral("⢠⣿⠇")) << QStringLiteral("  ") << style.gray(QStringLiteral("⢸⣿⣦⣤⣶"))
            << style.cream(QStringLiteral("⣿⢿")) << style.peach(QStringLiteral("⣿⠏")) << Qt::endl;
        out << QStringLiteral("     ") << style.cyan(QStringLiteral("⢀⣿⡟")) << QStringLiteral("  ")
            << style.blue(QStringLiteral("⢀⣾⣿⣶")) << style.gray(QStringLiteral("⣶⠾⠿⠿⣿⡏"))
            << QStringLiteral("  ") << style.orange(QStringLiteral("⢸⣿⡿⠁")) << Qt::endl;
        out << QStringLiteral("     ") << style.purple(QStringLiteral("⢸⣿⣧⣶"))
            << style.gray(QStringLiteral("⣾⣿⣿⠟⠉")) << QStringLiteral("    ")
            << style.gray(QStringLiteral("⢹⣷")) << QStringLiteral("  ") << style.orange(QStringLiteral("⣸⣿⠃")) << Qt::endl;
        out << QStringLiteral("   ") << style.purple(QStringLiteral("⢀⣠⣾⣿⣿"))
            << style.gray(QStringLiteral("⠉⢸⣿⠃")) << QStringLiteral("      ")
            << style.peach(QStringLiteral("⠈⢿⣧")) << style.orange(QStringLiteral("⢠⣿⡿⠁")) << Qt::endl;
        out << QStringLiteral("   ") << style.purple(QStringLiteral("⠁")) << QStringLiteral("  ")
            << style.gray(QStringLiteral("⠘⢿⣷")) << style.peach(QStringLiteral("⣿⣿")) << QStringLiteral("        ")
            << style.orange(QStringLiteral("⢘⣿⣿⡟")) << Qt::endl;
        out << QStringLiteral("        ") << style.gray(QStringLiteral("⢹⡿⠿⣷⣶"))
            << style.peach(QStringLiteral("⣦⣤⣤⣤")) << style.orange(QStringLiteral("⣴⣾⡿⠿⣿⡇")) << Qt::endl;
        out << QStringLiteral("        ") << style.gray(QStringLiteral("⡼⠁")) << QStringLiteral("   ")
            << style.peach(QStringLiteral("⠉⠉")) << style.orange(QStringLiteral("⠉⠉⠉")) << QStringLiteral("   ")
            << style.orange(QStringLiteral("⠈⠷")) << Qt::endl;
        out << style.bold(QStringLiteral("   AMNEZIA VPN CLI")) << style.dim(QStringLiteral("  %1").arg(APP_VERSION)) << Qt::endl;
        out << Qt::endl;
    }

    int dashboard(bool includeLogo = true)
    {
        if (includeLogo) {
            printLogo();
        }
        const int count = serversController.getServersCount();
        const bool serviceReady = connectionController.isServiceReady();

        out << style.bold(QStringLiteral("Status")) << Qt::endl;
        out << "  Service: " << (serviceReady ? style.green(QStringLiteral("ready")) : style.red(QStringLiteral("not running"))) << Qt::endl;
        out << "  Servers: " << count << Qt::endl;
        if (count > 0) {
            const QString defaultId = serversController.getDefaultServerId();
            out << "  Default: " << displayName(defaultId) << style.dim(QStringLiteral(" (%1)").arg(defaultId)) << Qt::endl;
        }
        out << Qt::endl;
        out << style.bold(QStringLiteral("Common commands")) << Qt::endl;
        out << "  " << cliCommandName() << " servers list" << Qt::endl;
        out << "  " << cliCommandName() << " import --file config.vpn" << Qt::endl;
        out << "  " << cliCommandName() << " connect [server]" << Qt::endl;
        out << "  " << cliCommandName() << " settings show" << Qt::endl;
        out << "  " << cliCommandName() << " help" << Qt::endl;
        return 0;
    }

    int help(const QStringList &args, bool includeLogo = true)
    {
        Q_UNUSED(args);
        if (includeLogo) {
            printLogo();
        }
        out << style.bold(QStringLiteral("Usage")) << Qt::endl;
        out << "  " << cliCommandName() << " [--json] [--no-color] <command> [arguments]" << Qt::endl;
        out << "  " << cliCommandName() << "                         Open the continuous CLI app" << Qt::endl << Qt::endl;

        out << style.bold(QStringLiteral("Commands")) << Qt::endl;
        out << "  status                         Show service, default server, and WireGuard daemon status" << Qt::endl;
        out << "  servers list|default|rename|remove|containers|scan" << Qt::endl;
        out << "  import --file <path>           Import Amnezia/OpenVPN/WireGuard/AWG/Xray config data" << Qt::endl;
        out << "  export <type> <server>         Export full, connection, openvpn, wireguard, awg, or xray config" << Qt::endl;
        out << "  connect [server]               Connect in foreground until Ctrl+C" << Qt::endl;
        out << "  disconnect                     Best-effort cleanup/disconnect through local services" << Qt::endl;
        out << "  settings show|set|backup|restore|clear" << Qt::endl;
        out << "  dns allowed list|add|remove|replace" << Qt::endl;
        out << "  split sites|apps ...           Manage split tunneling lists and modes" << Qt::endl;
        out << "  selfhost ...                   SSH check, install, scan, remove, reboot, status" << Qt::endl;
        out << "  clients list|rename|revoke     Manage self-hosted client access" << Qt::endl;
        out << "  subscription ...               Gateway import/update/account/native config commands" << Qt::endl;
        out << "  catalog                        Fetch Gateway service catalog" << Qt::endl;
        out << "  news                           Fetch Gateway news for installed services" << Qt::endl;
        out << "  shell                          Open the continuous CLI app" << Qt::endl << Qt::endl;

        out << style.bold(QStringLiteral("Shell shortcuts")) << Qt::endl;
        out << "  ls                             servers list" << Qt::endl;
        out << "  use <server>                   servers default <server>" << Qt::endl;
        out << "  up [server]                    connect [server]" << Qt::endl;
        out << "  down                           disconnect" << Qt::endl;
        out << "  show                           status" << Qt::endl;
        out << "  cfg                            settings show" << Qt::endl;
        out << "  exit                           Leave the app" << Qt::endl;
        return 0;
    }

    int fail(const QString &message, const QString &hint = QString())
    {
        if (jsonOutput) {
            QJsonObject object;
            object.insert(QStringLiteral("ok"), false);
            object.insert(QStringLiteral("error"), message);
            if (!hint.isEmpty()) {
                object.insert(QStringLiteral("hint"), hint);
            }
            err << jsonCompact(object) << Qt::endl;
        } else {
            err << style.red(QStringLiteral("Error: ")) << message << Qt::endl;
            if (!hint.isEmpty()) {
                err << style.dim(hint) << Qt::endl;
            }
        }
        return 1;
    }

    int ok(const QString &message)
    {
        if (jsonOutput) {
            QJsonObject object;
            object.insert(QStringLiteral("ok"), true);
            object.insert(QStringLiteral("message"), message);
            out << jsonCompact(object) << Qt::endl;
        } else {
            out << style.green(QStringLiteral("OK: ")) << message << Qt::endl;
        }
        return 0;
    }

    QString displayName(const QString &serverId) const
    {
        const QVector<ServerDescription> descriptions = serversController.buildServerDescriptions(appSettingsRepository.useAmneziaDns());
        for (const ServerDescription &description : descriptions) {
            if (description.serverId == serverId) {
                if (!description.serverName.isEmpty()) {
                    return description.serverName;
                }
                if (!description.baseDescription.isEmpty()) {
                    return description.baseDescription;
                }
                if (!description.hostName.isEmpty()) {
                    return description.hostName;
                }
            }
        }
        return serverId;
    }

    QString resolveServer(const QString &token, bool allowDefault = true) const
    {
        if (token.isEmpty() && allowDefault) {
            return serversController.getDefaultServerId();
        }
        if (token.isEmpty()) {
            return {};
        }

        bool ok = false;
        const int index = token.toInt(&ok);
        if (ok) {
            if (index >= 0 && index < serversController.getServersCount()) {
                return serversController.getServerId(index);
            }
            if (index > 0 && index <= serversController.getServersCount()) {
                return serversController.getServerId(index - 1);
            }
        }

        if (serversController.indexOfServerId(token) >= 0) {
            return token;
        }

        const QVector<ServerDescription> descriptions = serversController.buildServerDescriptions(appSettingsRepository.useAmneziaDns());
        for (const ServerDescription &description : descriptions) {
            if (description.serverName.compare(token, Qt::CaseInsensitive) == 0 ||
                description.baseDescription.compare(token, Qt::CaseInsensitive) == 0 ||
                description.hostName.compare(token, Qt::CaseInsensitive) == 0) {
                return description.serverId;
            }
        }
        return {};
    }

    int status(bool includeLogo = true)
    {
        QJsonObject object;
        object.insert(QStringLiteral("serviceReady"), connectionController.isServiceReady());
        object.insert(QStringLiteral("serversCount"), serversController.getServersCount());
        object.insert(QStringLiteral("defaultServerId"), serversController.getDefaultServerId());
        object.insert(QStringLiteral("defaultServerName"), displayName(serversController.getDefaultServerId()));
        object.insert(QStringLiteral("amneziaDns"), appSettingsRepository.useAmneziaDns());
        object.insert(QStringLiteral("killSwitch"), appSettingsRepository.isKillSwitchEnabled());

        QJsonObject daemonStatus;
        probeWireGuardDaemon(daemonStatus);
        object.insert(QStringLiteral("wireGuardDaemon"), daemonStatus);

        if (jsonOutput) {
            out << jsonCompact(object) << Qt::endl;
            return 0;
        }

        if (includeLogo) {
            printLogo();
        }
        out << style.bold(QStringLiteral("Runtime")) << Qt::endl;
        out << "  Service: " << (object.value(QStringLiteral("serviceReady")).toBool() ? style.green(QStringLiteral("ready")) : style.red(QStringLiteral("not running"))) << Qt::endl;
        out << "  WireGuard daemon: " << (daemonStatus.value(QStringLiteral("reachable")).toBool() ? style.green(QStringLiteral("reachable")) : style.dim(QStringLiteral("unavailable"))) << Qt::endl;
        if (daemonStatus.value(QStringLiteral("connected")).toBool()) {
            out << "  WireGuard tunnel: " << style.green(QStringLiteral("connected")) << Qt::endl;
        }
        out << Qt::endl;
        out << style.bold(QStringLiteral("Configuration")) << Qt::endl;
        out << "  Servers: " << object.value(QStringLiteral("serversCount")).toInt() << Qt::endl;
        out << "  Default: " << object.value(QStringLiteral("defaultServerName")).toString() << style.dim(QStringLiteral(" (%1)").arg(object.value(QStringLiteral("defaultServerId")).toString())) << Qt::endl;
        out << "  Amnezia DNS: " << boolText(appSettingsRepository.useAmneziaDns()) << Qt::endl;
        out << "  Kill switch: " << boolText(appSettingsRepository.isKillSwitchEnabled()) << Qt::endl;
        return 0;
    }

    void probeWireGuardDaemon(QJsonObject &object)
    {
        LocalSocketController controller;
        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);

        object.insert(QStringLiteral("reachable"), false);
        object.insert(QStringLiteral("connected"), false);

        QObject::connect(&controller, &ControllerImpl::initialized, &loop,
                         [&](bool initialized, bool connected, const QDateTime &date) {
                             object.insert(QStringLiteral("reachable"), initialized);
                             object.insert(QStringLiteral("connected"), connected);
                             if (date.isValid()) {
                                 object.insert(QStringLiteral("connectedAt"), date.toString(Qt::ISODate));
                             }
                             loop.quit();
                         });
        QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);

        controller.initialize(nullptr, nullptr);
        timer.start(1500);
        loop.exec();
    }

    int servers(QStringList args)
    {
        const QString sub = args.isEmpty() ? QStringLiteral("list") : args.takeFirst().toLower();
        if (sub == QLatin1String("list") || sub == QLatin1String("ls")) {
            return serversList();
        }
        if (sub == QLatin1String("default")) {
            if (args.isEmpty()) {
                const QString id = serversController.getDefaultServerId();
                return ok(QStringLiteral("Default server: %1 (%2)").arg(displayName(id), id));
            }
            const QString id = resolveServer(args.join(QLatin1Char(' ')), false);
            if (id.isEmpty()) {
                return fail(QStringLiteral("Server not found."));
            }
            serversController.setDefaultServer(id);
            return ok(QStringLiteral("Default server set to %1").arg(displayName(id)));
        }
        if (sub == QLatin1String("rename")) {
            if (args.size() < 2) {
                return fail(QStringLiteral("Usage: servers rename <server> <new-name>"));
            }
            const QString id = resolveServer(args.takeFirst(), false);
            if (id.isEmpty()) {
                return fail(QStringLiteral("Server not found."));
            }
            const QString name = args.join(QLatin1Char(' '));
            if (!serversController.renameServer(id, name)) {
                return fail(QStringLiteral("Unable to rename server."));
            }
            return ok(QStringLiteral("Renamed server to %1").arg(name));
        }
        if (sub == QLatin1String("remove") || sub == QLatin1String("rm")) {
            if (args.isEmpty()) {
                return fail(QStringLiteral("Usage: servers remove <server>"));
            }
            const QString id = resolveServer(args.join(QLatin1Char(' ')), false);
            if (id.isEmpty()) {
                return fail(QStringLiteral("Server not found."));
            }
            const QString name = displayName(id);
            serversController.removeServer(id);
            return ok(QStringLiteral("Removed server %1").arg(name));
        }
        if (sub == QLatin1String("containers")) {
            if (args.isEmpty()) {
                return fail(QStringLiteral("Usage: servers containers <server>"));
            }
            const QString id = resolveServer(args.join(QLatin1Char(' ')), false);
            if (id.isEmpty()) {
                return fail(QStringLiteral("Server not found."));
            }
            return serverContainers(id);
        }
        if (sub == QLatin1String("scan")) {
            if (args.isEmpty()) {
                return fail(QStringLiteral("Usage: servers scan <server>"));
            }
            const QString id = resolveServer(args.join(QLatin1Char(' ')), false);
            if (id.isEmpty()) {
                return fail(QStringLiteral("Server not found."));
            }
            const ErrorCode code = installController.scanServerForInstalledContainers(id);
            if (code != ErrorCode::NoError) {
                return fail(cleanError(code));
            }
            return ok(QStringLiteral("Server containers scanned."));
        }
        return fail(QStringLiteral("Unknown servers command: %1").arg(sub));
    }

    int serversList()
    {
        const QVector<ServerDescription> descriptions = serversController.buildServerDescriptions(appSettingsRepository.useAmneziaDns());
        if (jsonOutput) {
            QJsonArray array;
            for (int i = 0; i < descriptions.size(); ++i) {
                const ServerDescription &server = descriptions.at(i);
                QJsonObject object;
                object.insert(QStringLiteral("index"), i);
                object.insert(QStringLiteral("id"), server.serverId);
                object.insert(QStringLiteral("name"), server.serverName);
                object.insert(QStringLiteral("description"), server.baseDescription);
                object.insert(QStringLiteral("host"), server.hostName);
                object.insert(QStringLiteral("kind"), configTypeName(serversRepository.serverKind(server.serverId)));
                object.insert(QStringLiteral("default"), server.serverId == serversController.getDefaultServerId());
                object.insert(QStringLiteral("defaultContainer"), ContainerUtils::containerToString(server.defaultContainer));
                object.insert(QStringLiteral("hasVpnContainers"), server.hasInstalledVpnContainers);
                array.append(object);
            }
            out << QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact)) << Qt::endl;
            return 0;
        }

        printLogo();
        out << style.bold(QStringLiteral("Servers")) << Qt::endl;
        if (descriptions.isEmpty()) {
            out << style.dim(QStringLiteral("  No servers imported yet.")) << Qt::endl;
            return 0;
        }
        out << style.dim(QStringLiteral("  #  Default  Name                         Kind                 Container       Host")) << Qt::endl;
        int i = 0;
        for (const ServerDescription &server : descriptions) {
            const QString mark = server.serverId == serversController.getDefaultServerId() ? QStringLiteral("*") : QStringLiteral(" ");
            const QString name = (server.serverName.isEmpty() ? server.baseDescription : server.serverName).leftJustified(28, QLatin1Char(' '), true);
            const QString kind = configTypeName(serversRepository.serverKind(server.serverId)).leftJustified(20, QLatin1Char(' '), true);
            const QString container = containerLabel(server.defaultContainer).leftJustified(15, QLatin1Char(' '), true);
            out << QStringLiteral("  %1  %2        %3 %4 %5 %6")
                           .arg(i, 2)
                           .arg(mark, name, kind, container, server.hostName)
                << Qt::endl;
            out << style.dim(QStringLiteral("       id: %1").arg(server.serverId)) << Qt::endl;
            ++i;
        }
        return 0;
    }

    int serverContainers(const QString &serverId)
    {
        const QMap<DockerContainer, ContainerConfig> containers = serversController.getServerContainersMap(serverId);
        const DockerContainer defaultContainer = serversController.getDefaultContainer(serverId);

        if (jsonOutput) {
            QJsonArray array;
            for (auto it = containers.constBegin(); it != containers.constEnd(); ++it) {
                QJsonObject object;
                object.insert(QStringLiteral("container"), ContainerUtils::containerToString(it.key()));
                object.insert(QStringLiteral("name"), containerLabel(it.key()));
                object.insert(QStringLiteral("default"), it.key() == defaultContainer);
                object.insert(QStringLiteral("protocol"), ProtocolUtils::protoToString(ContainerUtils::defaultProtocol(it.key())));
                array.append(object);
            }
            out << QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact)) << Qt::endl;
            return 0;
        }

        out << style.bold(QStringLiteral("Containers for %1").arg(displayName(serverId))) << Qt::endl;
        if (containers.isEmpty()) {
            out << style.dim(QStringLiteral("  No containers stored for this server.")) << Qt::endl;
            return 0;
        }
        for (auto it = containers.constBegin(); it != containers.constEnd(); ++it) {
            out << "  " << (it.key() == defaultContainer ? style.green(QStringLiteral("* ")) : QStringLiteral("  "))
                << ContainerUtils::containerToString(it.key()) << "  "
                << style.dim(containerLabel(it.key())) << Qt::endl;
        }
        return 0;
    }

    int importConfig(const QStringList &args)
    {
        const ArgView view(args);
        QString fileName = view.valueAny({ "--file", "-f" });
        QString data = view.valueAny({ "--data", "-d" });
        const QStringList positional = view.positionals();
        if (data.isEmpty() && fileName.isEmpty() && !positional.isEmpty()) {
            const QString candidate = positional.join(QLatin1Char(' '));
            if (QFileInfo::exists(candidate)) {
                fileName = candidate;
            } else {
                data = candidate;
            }
        }
        if (!fileName.isEmpty()) {
            QByteArray bytes;
            QString error;
            if (!readFile(fileName, bytes, error)) {
                return fail(QStringLiteral("Unable to read %1: %2").arg(fileName, error));
            }
            data = QString::fromUtf8(bytes);
        }
        if (data.trimmed().isEmpty()) {
            return fail(QStringLiteral("Usage: import --file <path> or import --data <config>"));
        }

        ImportController::ImportResult result = importController.extractConfigFromData(data, fileName);
        if (result.errorCode != ErrorCode::NoError) {
            return fail(cleanError(result.errorCode));
        }
        if (!result.maliciousWarningText.isEmpty() && !jsonOutput) {
            err << style.orange(QStringLiteral("Warning: ")) << result.maliciousWarningText << Qt::endl;
        }
        importController.importConfig(result.config);
        serversRepository.invalidateCache();
        return ok(QStringLiteral("Configuration imported. Servers: %1").arg(serversController.getServersCount()));
    }

    int exportConfig(const QStringList &args)
    {
        if (args.size() < 2) {
            return fail(QStringLiteral("Usage: export <full|connection|openvpn|wireguard|awg|xray> <server> [--out file] [--client name] [--container awg]"));
        }

        QStringList rest = args;
        const QString type = rest.takeFirst().toLower();
        const QString serverId = resolveServer(rest.takeFirst(), false);
        if (serverId.isEmpty()) {
            return fail(QStringLiteral("Server not found."));
        }

        const ArgView view(rest);
        const QString clientName = view.valueAny({ "--client", "-c" }, QStringLiteral("CLI client"));
        const QString outPath = view.valueAny({ "--out", "-o" });
        const QString qrOutPath = view.value(QStringLiteral("--qr-out"));

        ExportController::ExportResult result;
        if (type == QLatin1String("full")) {
            result = exportController.generateFullAccessConfig(serverId);
        } else if (type == QLatin1String("connection")) {
            const DockerContainer container = optionContainer(view, serverId);
            result = exportController.generateConnectionConfig(serverId, static_cast<int>(container), clientName);
        } else if (type == QLatin1String("openvpn") || type == QLatin1String("ovpn")) {
            result = exportController.generateOpenVpnConfig(serverId, clientName);
        } else if (type == QLatin1String("wireguard") || type == QLatin1String("wg")) {
            result = exportController.generateWireGuardConfig(serverId, clientName);
        } else if (type == QLatin1String("awg") || type == QLatin1String("amneziawg")) {
            const DockerContainer container = optionContainer(view, serverId);
            result = exportController.generateAwgConfig(serverId, static_cast<int>(container), clientName);
        } else if (type == QLatin1String("xray")) {
            result = exportController.generateXrayConfig(serverId, clientName);
        } else {
            return fail(QStringLiteral("Unknown export type: %1").arg(type));
        }

        if (result.errorCode != ErrorCode::NoError) {
            return fail(cleanError(result.errorCode));
        }

        const QString payload = result.nativeConfigString.isEmpty() ? result.config : result.nativeConfigString;
        if (payload.isEmpty()) {
            return fail(QStringLiteral("Export produced empty config."));
        }

        if (!outPath.isEmpty()) {
            QString error;
            if (!writeFile(outPath, payload.toUtf8(), error)) {
                return fail(QStringLiteral("Unable to write %1: %2").arg(outPath, error));
            }
        } else if (!jsonOutput) {
            out << payload << Qt::endl;
        }

        if (!qrOutPath.isEmpty()) {
            QString error;
            if (!writeFile(qrOutPath, result.qrCodes.join(QLatin1Char('\n')).toUtf8(), error)) {
                return fail(QStringLiteral("Unable to write QR data %1: %2").arg(qrOutPath, error));
            }
        }

        if (jsonOutput) {
            QJsonObject object;
            object.insert(QStringLiteral("ok"), true);
            object.insert(QStringLiteral("written"), !outPath.isEmpty() ? outPath : QString());
            object.insert(QStringLiteral("qrCount"), result.qrCodes.size());
            object.insert(QStringLiteral("config"), outPath.isEmpty() ? payload : QString());
            out << jsonCompact(object) << Qt::endl;
            return 0;
        }

        if (!outPath.isEmpty()) {
            out << style.green(QStringLiteral("OK: ")) << "Exported to " << outPath << Qt::endl;
        }
        if (!qrOutPath.isEmpty()) {
            out << style.green(QStringLiteral("OK: ")) << "QR chunks written to " << qrOutPath << Qt::endl;
        }
        return 0;
    }

    DockerContainer optionContainer(const ArgView &view, const QString &serverId) const
    {
        const QString value = view.valueAny({ "--container", "-p" });
        if (value.isEmpty()) {
            return serversController.getDefaultContainer(serverId);
        }
        bool ok = false;
        DockerContainer container = parseContainer(value, ok);
        return ok ? container : serversController.getDefaultContainer(serverId);
    }

    int connect(const QStringList &args)
    {
        const ArgView view(args);
        const QStringList positional = view.positionals();
        const QString requested = positional.isEmpty() ? QString() : positional.join(QLatin1Char(' '));
        const QString serverId = resolveServer(requested, true);
        if (serverId.isEmpty()) {
            return fail(QStringLiteral("No server selected. Import a config or pass a server index/id/name."));
        }

        const ErrorCode supported = connectionController.isConnectionSupported(serverId);
        if (supported != ErrorCode::NoError) {
            return fail(cleanError(supported));
        }

        printLogo();
        out << style.bold(QStringLiteral("Connecting")) << Qt::endl;
        out << "  Server: " << displayName(serverId) << style.dim(QStringLiteral(" (%1)").arg(serverId)) << Qt::endl;
        out << "  Mode: foreground; press Ctrl+C to disconnect" << Qt::endl << Qt::endl;

        QEventLoop loop;
        QTimer stopTimer;
        stopTimer.setInterval(250);
        QObject::connect(&stopTimer, &QTimer::timeout, &loop, [&]() {
            if (g_stopRequested.load()) {
                out << Qt::endl << style.orange(QStringLiteral("Disconnect requested.")) << Qt::endl;
                connectionController.closeConnection();
                QTimer::singleShot(1200, &loop, &QEventLoop::quit);
            }
        });

        QObject::connect(&vpnConnection, &VpnConnection::connectionStateChanged, &loop, [&](Vpn::ConnectionState state) {
            out << "  State: " << stateName(state) << Qt::endl;
            if (state == Vpn::ConnectionState::Error) {
                err << style.red(QStringLiteral("Connection error: ")) << cleanError(connectionController.lastConnectionError()) << Qt::endl;
                loop.quit();
            } else if (state == Vpn::ConnectionState::Disconnected && g_stopRequested.load()) {
                loop.quit();
            }
        });
        QObject::connect(&vpnConnection, &VpnConnection::vpnProtocolError, &loop, [&](ErrorCode code) {
            err << style.red(QStringLiteral("Protocol error: ")) << cleanError(code) << Qt::endl;
            loop.quit();
        });
        QObject::connect(&vpnConnection, &VpnConnection::bytesChanged, &loop, [&](quint64 rx, quint64 tx) {
            out << "\r  Traffic: down " << VpnConnection::bytesPerSecToText(rx)
                << " / up " << VpnConnection::bytesPerSecToText(tx) << "      " << Qt::flush;
        });

        stopTimer.start();
        const ErrorCode openError = connectionController.openConnection(serverId);
        if (openError != ErrorCode::NoError) {
            return fail(cleanError(openError));
        }
        loop.exec();

        if (vpnConnection.connectionState() != Vpn::ConnectionState::Disconnected) {
            connectionController.closeConnection();
            QCoreApplication::processEvents(QEventLoop::AllEvents, 1500);
        }

        return vpnConnection.connectionState() == Vpn::ConnectionState::Error ? 1 : 0;
    }

    int disconnect()
    {
        bool serviceReached = IpcClient::withInterface(
                [&](QSharedPointer<IpcInterfaceReplica> iface) {
                    iface->xrayStop().waitForFinished(2000);
                    iface->disableKillSwitch().waitForFinished(2000);
                    iface->restoreResolvers().waitForFinished(2000);
                    iface->clearSavedRoutes().waitForFinished(2000);
                    iface->flushDns().waitForFinished(2000);
                    iface->cleanUp();
                    return true;
                },
                []() { return false; });

        LocalSocketController controller;
        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        bool daemonReached = false;
        QObject::connect(&controller, &ControllerImpl::initialized, &loop,
                         [&](bool initialized, bool, const QDateTime &) {
                             daemonReached = initialized;
                             controller.deactivate();
                             QTimer::singleShot(250, &loop, &QEventLoop::quit);
                         });
        QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        controller.initialize(nullptr, nullptr);
        timer.start(1500);
        loop.exec();

        if (!serviceReached && !daemonReached) {
            return fail(QStringLiteral("No local Amnezia service or daemon responded."));
        }
        return ok(QStringLiteral("Disconnect/cleanup request sent."));
    }

    int settingsCommand(QStringList args)
    {
        const QString sub = args.isEmpty() ? QStringLiteral("show") : args.takeFirst().toLower();
        if (sub == QLatin1String("show")) {
            return settingsShow();
        }
        if (sub == QLatin1String("set")) {
            if (args.size() < 2) {
                return fail(QStringLiteral("Usage: settings set <key> <value>"));
            }
            const QString key = args.takeFirst();
            const QString value = args.join(QLatin1Char(' '));
            return settingsSet(key, value);
        }
        if (sub == QLatin1String("backup")) {
            const ArgView view(args);
            const QString outPath = view.valueAny({ "--out", "-o" });
            if (outPath.isEmpty()) {
                out << settingsController.backupAppConfig() << Qt::endl;
                return 0;
            }
            QString error;
            if (!writeFile(outPath, settingsController.backupAppConfig(), error)) {
                return fail(QStringLiteral("Unable to write backup: %1").arg(error));
            }
            return ok(QStringLiteral("Backup written to %1").arg(outPath));
        }
        if (sub == QLatin1String("restore")) {
            const ArgView view(args);
            const QString path = view.valueAny({ "--file", "-f" });
            if (path.isEmpty()) {
                return fail(QStringLiteral("Usage: settings restore --file <backup.json>"));
            }
            QByteArray data;
            QString error;
            if (!readFile(path, data, error)) {
                return fail(QStringLiteral("Unable to read backup: %1").arg(error));
            }
            const ErrorCode code = settingsController.restoreAppConfigFromData(data);
            if (code != ErrorCode::NoError) {
                return fail(cleanError(code));
            }
            return ok(QStringLiteral("Backup restored."));
        }
        if (sub == QLatin1String("clear")) {
            const ArgView view(args);
            if (!view.has(QStringLiteral("--yes"))) {
                return fail(QStringLiteral("Refusing to clear settings without --yes."));
            }
            settingsController.clearSettings();
            return ok(QStringLiteral("Settings cleared."));
        }
        return fail(QStringLiteral("Unknown settings command: %1").arg(sub));
    }

    int settingsShow()
    {
        QJsonObject object;
        object.insert(QStringLiteral("amneziaDns"), appSettingsRepository.useAmneziaDns());
        object.insert(QStringLiteral("primaryDns"), appSettingsRepository.primaryDns());
        object.insert(QStringLiteral("secondaryDns"), appSettingsRepository.secondaryDns());
        object.insert(QStringLiteral("allowedDns"), QJsonArray::fromStringList(appSettingsRepository.getAllowedDnsServers()));
        object.insert(QStringLiteral("logging"), appSettingsRepository.isSaveLogs());
        object.insert(QStringLiteral("autoConnect"), appSettingsRepository.isAutoConnect());
        object.insert(QStringLiteral("startMinimized"), appSettingsRepository.isStartMinimized());
        object.insert(QStringLiteral("screenshots"), appSettingsRepository.isScreenshotsEnabled());
        object.insert(QStringLiteral("newsNotifications"), appSettingsRepository.isNewsNotifications());
        object.insert(QStringLiteral("killSwitch"), appSettingsRepository.isKillSwitchEnabled());
        object.insert(QStringLiteral("strictKillSwitch"), appSettingsRepository.isStrictKillSwitchEnabled());
        object.insert(QStringLiteral("siteSplitEnabled"), appSettingsRepository.isSitesSplitTunnelingEnabled());
        object.insert(QStringLiteral("siteSplitMode"), routeModeName(appSettingsRepository.routeMode()));
        object.insert(QStringLiteral("appSplitEnabled"), appSettingsRepository.isAppsSplitTunnelingEnabled());
        object.insert(QStringLiteral("appSplitMode"), appsRouteModeName(appSettingsRepository.appsRouteMode()));
        object.insert(QStringLiteral("gatewayEndpoint"), appSettingsRepository.getGatewayEndpoint());
        object.insert(QStringLiteral("devGateway"), appSettingsRepository.isDevGatewayEnv());
        object.insert(QStringLiteral("language"), appSettingsRepository.getAppLanguage().name());

        if (jsonOutput) {
            out << jsonCompact(object) << Qt::endl;
            return 0;
        }
        out << style.bold(QStringLiteral("Settings")) << Qt::endl;
        for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
            out << "  " << it.key().leftJustified(22) << valueToText(it.value()) << Qt::endl;
        }
        return 0;
    }

    QString valueToText(const QJsonValue &value) const
    {
        if (value.isBool()) {
            return boolText(value.toBool());
        }
        if (value.isArray()) {
            QStringList items;
            for (const QJsonValue &item : value.toArray()) {
                items.append(item.toString());
            }
            return items.join(QStringLiteral(", "));
        }
        return value.toVariant().toString();
    }

    int settingsSet(const QString &key, const QString &value)
    {
        const QString normalized = key.trimmed().toLower();
        bool validBool = false;
        const bool boolean = parseBool(value, validBool);

        if (normalized == QLatin1String("amnezia-dns")) {
            if (!validBool) return fail(QStringLiteral("Expected boolean value."));
            settingsController.toggleAmneziaDns(boolean);
        } else if (normalized == QLatin1String("primary-dns")) {
            settingsController.setPrimaryDns(value);
        } else if (normalized == QLatin1String("secondary-dns")) {
            settingsController.setSecondaryDns(value);
        } else if (normalized == QLatin1String("logging")) {
            if (!validBool) return fail(QStringLiteral("Expected boolean value."));
            settingsController.toggleLogging(boolean);
        } else if (normalized == QLatin1String("autoconnect")) {
            if (!validBool) return fail(QStringLiteral("Expected boolean value."));
            settingsController.toggleAutoConnect(boolean);
        } else if (normalized == QLatin1String("autostart")) {
            if (!validBool) return fail(QStringLiteral("Expected boolean value."));
            settingsController.toggleAutoStart(boolean);
        } else if (normalized == QLatin1String("start-minimized")) {
            if (!validBool) return fail(QStringLiteral("Expected boolean value."));
            settingsController.toggleStartMinimized(boolean);
        } else if (normalized == QLatin1String("screenshots")) {
            if (!validBool) return fail(QStringLiteral("Expected boolean value."));
            settingsController.toggleScreenshotsEnabled(boolean);
        } else if (normalized == QLatin1String("news")) {
            if (!validBool) return fail(QStringLiteral("Expected boolean value."));
            settingsController.toggleNewsNotificationsEnabled(boolean);
        } else if (normalized == QLatin1String("killswitch") || normalized == QLatin1String("kill-switch")) {
            if (!validBool) return fail(QStringLiteral("Expected boolean value."));
            settingsController.toggleKillSwitch(boolean);
            connectionController.onKillSwitchModeChanged(boolean);
        } else if (normalized == QLatin1String("strict-killswitch") || normalized == QLatin1String("strict-kill-switch")) {
            if (!validBool) return fail(QStringLiteral("Expected boolean value."));
            settingsController.toggleStrictKillSwitch(boolean);
        } else if (normalized == QLatin1String("language")) {
            settingsController.setAppLanguage(QLocale(value));
        } else if (normalized == QLatin1String("gateway-endpoint")) {
            settingsController.setGatewayEndpoint(value);
        } else if (normalized == QLatin1String("dev-gateway")) {
            if (!validBool) return fail(QStringLiteral("Expected boolean value."));
            settingsController.toggleDevGatewayEnv(boolean);
        } else {
            return fail(QStringLiteral("Unknown setting key: %1").arg(key));
        }
        return ok(QStringLiteral("Setting updated."));
    }

    bool parseBool(const QString &value, bool &ok) const
    {
        const QString normalized = value.trimmed().toLower();
        ok = true;
        if (QStringList { QStringLiteral("1"), QStringLiteral("true"), QStringLiteral("on"), QStringLiteral("yes"), QStringLiteral("enable"), QStringLiteral("enabled") }.contains(normalized)) {
            return true;
        }
        if (QStringList { QStringLiteral("0"), QStringLiteral("false"), QStringLiteral("off"), QStringLiteral("no"), QStringLiteral("disable"), QStringLiteral("disabled") }.contains(normalized)) {
            return false;
        }
        ok = false;
        return false;
    }

    int dns(QStringList args)
    {
        if (args.isEmpty() || args.takeFirst().toLower() != QLatin1String("allowed")) {
            return fail(QStringLiteral("Usage: dns allowed list|add|remove|replace"));
        }
        const QString sub = args.isEmpty() ? QStringLiteral("list") : args.takeFirst().toLower();
        if (sub == QLatin1String("list")) {
            const QStringList dnsServers = allowedDnsController.getCurrentDnsServers();
            if (jsonOutput) {
                out << QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(dnsServers)).toJson(QJsonDocument::Compact)) << Qt::endl;
            } else {
                out << style.bold(QStringLiteral("Allowed DNS")) << Qt::endl;
                for (int i = 0; i < dnsServers.size(); ++i) {
                    out << "  " << i << "  " << dnsServers.at(i) << Qt::endl;
                }
            }
            return 0;
        }
        if (sub == QLatin1String("add")) {
            if (args.isEmpty()) return fail(QStringLiteral("Usage: dns allowed add <ip> [ip...]"));
            for (const QString &ip : args) {
                if (!allowedDnsController.addDns(ip)) {
                    return fail(QStringLiteral("Invalid DNS address: %1").arg(ip));
                }
            }
            return ok(QStringLiteral("Allowed DNS list updated."));
        }
        if (sub == QLatin1String("remove") || sub == QLatin1String("rm")) {
            if (args.isEmpty()) return fail(QStringLiteral("Usage: dns allowed remove <index>"));
            bool okIndex = false;
            const int index = args.first().toInt(&okIndex);
            if (!okIndex) return fail(QStringLiteral("Index must be a number."));
            allowedDnsController.removeDns(index);
            return ok(QStringLiteral("Allowed DNS entry removed."));
        }
        if (sub == QLatin1String("replace")) {
            allowedDnsController.addDnsList(args, true);
            return ok(QStringLiteral("Allowed DNS list replaced."));
        }
        return fail(QStringLiteral("Unknown dns command: %1").arg(sub));
    }

    int split(QStringList args)
    {
        if (args.isEmpty()) {
            return fail(QStringLiteral("Usage: split sites|apps ..."));
        }
        const QString area = args.takeFirst().toLower();
        if (area == QLatin1String("sites")) {
            return splitSites(args);
        }
        if (area == QLatin1String("apps")) {
            return splitApps(args);
        }
        return fail(QStringLiteral("Unknown split area: %1").arg(area));
    }

    int splitSites(QStringList args)
    {
        const QString sub = args.isEmpty() ? QStringLiteral("list") : args.takeFirst().toLower();
        if (sub == QLatin1String("list")) {
            const QVector<QPair<QString, QString>> sites = ipSplitController.getCurrentSites();
            if (jsonOutput) {
                QJsonArray array;
                for (const auto &site : sites) {
                    QJsonObject object;
                    object.insert(QStringLiteral("host"), site.first);
                    object.insert(QStringLiteral("ip"), site.second);
                    array.append(object);
                }
                out << QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact)) << Qt::endl;
            } else {
                out << style.bold(QStringLiteral("Site split tunneling")) << Qt::endl;
                out << "  Enabled: " << boolText(ipSplitController.isSplitTunnelingEnabled()) << Qt::endl;
                out << "  Mode: " << routeModeName(ipSplitController.getRouteMode()) << Qt::endl;
                for (const auto &site : sites) {
                    out << "  " << site.first << (site.second.isEmpty() ? QString() : style.dim(QStringLiteral(" -> %1").arg(site.second))) << Qt::endl;
                }
            }
            return 0;
        }
        if (sub == QLatin1String("add")) {
            if (args.isEmpty()) return fail(QStringLiteral("Usage: split sites add <host-or-cidr> [host-or-cidr...]"));
            for (const QString &site : args) {
                if (!ipSplitController.addSite(site)) {
                    return fail(QStringLiteral("Invalid site or subnet: %1").arg(site));
                }
            }
            return ok(QStringLiteral("Sites updated."));
        }
        if (sub == QLatin1String("remove") || sub == QLatin1String("rm")) {
            if (args.isEmpty()) return fail(QStringLiteral("Usage: split sites remove <host-or-cidr>"));
            if (!ipSplitController.removeSite(args.join(QLatin1Char(' ')))) {
                return fail(QStringLiteral("Site not found."));
            }
            return ok(QStringLiteral("Site removed."));
        }
        if (sub == QLatin1String("clear")) {
            ipSplitController.removeSites();
            return ok(QStringLiteral("Site split tunneling list cleared."));
        }
        if (sub == QLatin1String("enable") || sub == QLatin1String("disable")) {
            ipSplitController.toggleSplitTunneling(sub == QLatin1String("enable"));
            return ok(QStringLiteral("Site split tunneling %1.").arg(sub == QLatin1String("enable") ? QStringLiteral("enabled") : QStringLiteral("disabled")));
        }
        if (sub == QLatin1String("mode")) {
            if (args.isEmpty()) return fail(QStringLiteral("Usage: split sites mode all|only|except"));
            bool okMode = false;
            const RouteMode mode = parseRouteMode(args.first(), okMode);
            if (!okMode) return fail(QStringLiteral("Unknown route mode."));
            ipSplitController.setRouteMode(mode);
            return ok(QStringLiteral("Site split mode set to %1.").arg(routeModeName(mode)));
        }
        if (sub == QLatin1String("import")) {
            const ArgView view(args);
            const QString path = view.valueAny({ "--file", "-f" });
            if (path.isEmpty()) return fail(QStringLiteral("Usage: split sites import --file <sites.json> [--replace]"));
            QByteArray data;
            QString error;
            if (!readFile(path, data, error)) return fail(error);
            QString parseError;
            if (!ipSplitController.importSitesFromJson(data, view.has(QStringLiteral("--replace")), parseError)) {
                return fail(parseError);
            }
            return ok(QStringLiteral("Sites imported."));
        }
        if (sub == QLatin1String("export")) {
            const ArgView view(args);
            const QByteArray data = ipSplitController.exportSitesToJson();
            const QString path = view.valueAny({ "--out", "-o" });
            if (path.isEmpty()) {
                out << data << Qt::endl;
            } else {
                QString error;
                if (!writeFile(path, data, error)) return fail(error);
                return ok(QStringLiteral("Sites exported to %1.").arg(path));
            }
            return 0;
        }
        return fail(QStringLiteral("Unknown split sites command: %1").arg(sub));
    }

    int splitApps(QStringList args)
    {
        const QString sub = args.isEmpty() ? QStringLiteral("list") : args.takeFirst().toLower();
        if (sub == QLatin1String("list")) {
            const QVector<InstalledAppInfo> apps = appSplitController.getApps();
            if (jsonOutput) {
                QJsonArray array;
                for (const InstalledAppInfo &app : apps) {
                    QJsonObject object;
                    object.insert(QStringLiteral("name"), app.appName);
                    object.insert(QStringLiteral("package"), app.packageName);
                    object.insert(QStringLiteral("path"), app.appPath);
                    array.append(object);
                }
                out << QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact)) << Qt::endl;
            } else {
                out << style.bold(QStringLiteral("App split tunneling")) << Qt::endl;
                out << "  Enabled: " << boolText(appSplitController.isSplitTunnelingEnabled()) << Qt::endl;
                out << "  Mode: " << appsRouteModeName(appSplitController.getRouteMode()) << Qt::endl;
                for (int i = 0; i < apps.size(); ++i) {
                    const InstalledAppInfo &app = apps.at(i);
                    out << "  " << i << "  " << app.appName << "  " << style.dim(app.appPath.isEmpty() ? app.packageName : app.appPath) << Qt::endl;
                }
            }
            return 0;
        }
        if (sub == QLatin1String("add")) {
            const ArgView view(args);
            InstalledAppInfo app;
            app.appName = view.value(QStringLiteral("--name"), view.valueAny({ "--path", "--package" }));
            app.appPath = view.value(QStringLiteral("--path"));
            app.packageName = view.value(QStringLiteral("--package"));
            if (app.appPath.isEmpty() && app.packageName.isEmpty()) {
                return fail(QStringLiteral("Usage: split apps add --name <name> (--path <path> | --package <id>)"));
            }
            if (!appSplitController.addApp(app)) {
                return fail(QStringLiteral("App entry already exists or is invalid."));
            }
            return ok(QStringLiteral("App split entry added."));
        }
        if (sub == QLatin1String("remove") || sub == QLatin1String("rm")) {
            if (args.isEmpty()) return fail(QStringLiteral("Usage: split apps remove <index>"));
            bool okIndex = false;
            const int index = args.first().toInt(&okIndex);
            if (!okIndex) return fail(QStringLiteral("Index must be a number."));
            appSplitController.removeApp(index);
            return ok(QStringLiteral("App split entry removed."));
        }
        if (sub == QLatin1String("clear")) {
            appSplitController.clearAppsList();
            return ok(QStringLiteral("App split list cleared."));
        }
        if (sub == QLatin1String("enable") || sub == QLatin1String("disable")) {
            appSplitController.toggleSplitTunneling(sub == QLatin1String("enable"));
            return ok(QStringLiteral("App split tunneling %1.").arg(sub == QLatin1String("enable") ? QStringLiteral("enabled") : QStringLiteral("disabled")));
        }
        if (sub == QLatin1String("mode")) {
            if (args.isEmpty()) return fail(QStringLiteral("Usage: split apps mode all|only|except"));
            bool okMode = false;
            const AppsRouteMode mode = parseAppsRouteMode(args.first(), okMode);
            if (!okMode) return fail(QStringLiteral("Unknown app route mode."));
            appSplitController.setRouteMode(mode);
            return ok(QStringLiteral("App split mode set to %1.").arg(appsRouteModeName(mode)));
        }
        return fail(QStringLiteral("Unknown split apps command: %1").arg(sub));
    }

    int selfhost(QStringList args)
    {
        if (args.isEmpty()) {
            return fail(QStringLiteral("Usage: selfhost check|add|install-new|install|scan|remove-container|remove-all|reboot|status|secret"));
        }
        const QString sub = args.takeFirst().toLower();
        const ArgView view(args);

        if (sub == QLatin1String("check")) {
            ServerCredentials credentials;
            if (!credentialsFromArgs(view, credentials)) return fail(QStringLiteral("Pass --host, --user, and --secret."));
            QString output;
            ErrorCode code = installController.checkSshConnection(credentials, output);
            if (!output.isEmpty() && !jsonOutput) out << output << Qt::endl;
            return code == ErrorCode::NoError ? ok(QStringLiteral("SSH connection works.")) : fail(cleanError(code));
        }
        if (sub == QLatin1String("add")) {
            ServerCredentials credentials;
            if (!credentialsFromArgs(view, credentials)) return fail(QStringLiteral("Pass --host, --user, and --secret."));
            installController.addEmptyServer(credentials);
            return ok(QStringLiteral("Empty self-hosted server added."));
        }
        if (sub == QLatin1String("install-new")) {
            ServerCredentials credentials;
            if (!credentialsFromArgs(view, credentials, false)) return fail(QStringLiteral("Pass --host, --user, and --secret."));
            DockerContainer container = containerFromOption(view, DockerContainer::Awg2);
            const int port = view.value(QStringLiteral("--port"), QString::number(ProtocolUtils::getPortForInstall(ContainerUtils::defaultProtocol(container)))).toInt();
            const TransportProto transport = parseTransport(view.value(QStringLiteral("--transport"), ProtocolUtils::transportProtoToString(ProtocolUtils::defaultTransportProto(ContainerUtils::defaultProtocol(container)))));
            bool installed = false;
            const ErrorCode code = installController.installServer(credentials, container, port, transport, installed);
            if (code != ErrorCode::NoError) return fail(cleanError(code));
            return ok(QStringLiteral("Server installed. Container installed: %1").arg(boolText(installed)));
        }
        if (sub == QLatin1String("install")) {
            const QString serverId = resolveRequiredServer(view.positionals());
            if (serverId.isEmpty()) return fail(QStringLiteral("Server not found."));
            DockerContainer container = containerFromOption(view, DockerContainer::Awg2);
            const int port = view.value(QStringLiteral("--port"), QString::number(ProtocolUtils::getPortForInstall(ContainerUtils::defaultProtocol(container)))).toInt();
            const TransportProto transport = parseTransport(view.value(QStringLiteral("--transport"), ProtocolUtils::transportProtoToString(ProtocolUtils::defaultTransportProto(ContainerUtils::defaultProtocol(container)))));
            bool installed = false;
            const ErrorCode code = installController.installContainer(serverId, container, port, transport, installed);
            if (code != ErrorCode::NoError) return fail(cleanError(code));
            return ok(QStringLiteral("Container install finished. Installed: %1").arg(boolText(installed)));
        }
        if (sub == QLatin1String("scan")) {
            const QString serverId = resolveRequiredServer(view.positionals());
            if (serverId.isEmpty()) return fail(QStringLiteral("Server not found."));
            const ErrorCode code = installController.scanServerForInstalledContainers(serverId);
            return code == ErrorCode::NoError ? ok(QStringLiteral("Scan finished.")) : fail(cleanError(code));
        }
        if (sub == QLatin1String("remove-container")) {
            const QString serverId = resolveRequiredServer(view.positionals());
            if (serverId.isEmpty()) return fail(QStringLiteral("Server not found."));
            const ErrorCode code = installController.removeContainer(serverId, containerFromOption(view, serversController.getDefaultContainer(serverId)));
            return code == ErrorCode::NoError ? ok(QStringLiteral("Container removed.")) : fail(cleanError(code));
        }
        if (sub == QLatin1String("remove-all")) {
            const QString serverId = resolveRequiredServer(view.positionals());
            if (serverId.isEmpty()) return fail(QStringLiteral("Server not found."));
            const ErrorCode code = installController.removeAllContainers(serverId);
            return code == ErrorCode::NoError ? ok(QStringLiteral("All containers removed.")) : fail(cleanError(code));
        }
        if (sub == QLatin1String("reboot")) {
            const QString serverId = resolveRequiredServer(view.positionals());
            if (serverId.isEmpty()) return fail(QStringLiteral("Server not found."));
            const ErrorCode code = installController.rebootServer(serverId);
            return code == ErrorCode::NoError ? ok(QStringLiteral("Reboot command sent.")) : fail(cleanError(code));
        }
        if (sub == QLatin1String("status")) {
            const QString serverId = resolveRequiredServer(view.positionals());
            if (serverId.isEmpty()) return fail(QStringLiteral("Server not found."));
            int statusOut = 0;
            const ErrorCode code = installController.queryDockerContainerStatus(serverId, containerFromOption(view, serversController.getDefaultContainer(serverId)), statusOut);
            if (code != ErrorCode::NoError) return fail(cleanError(code));
            out << statusOut << Qt::endl;
            return 0;
        }
        if (sub == QLatin1String("secret")) {
            const QString serverId = resolveRequiredServer(view.positionals());
            if (serverId.isEmpty()) return fail(QStringLiteral("Server not found."));
            out << installController.fetchDockerContainerSecret(serverId, containerFromOption(view, serversController.getDefaultContainer(serverId))) << Qt::endl;
            return 0;
        }
        return fail(QStringLiteral("Unknown selfhost command: %1").arg(sub));
    }

    QString resolveRequiredServer(const QStringList &positionals) const
    {
        if (positionals.isEmpty()) {
            return {};
        }
        return resolveServer(positionals.first(), false);
    }

    DockerContainer containerFromOption(const ArgView &view, DockerContainer fallback) const
    {
        const QString value = view.valueAny({ "--container", "-p" });
        if (value.isEmpty()) {
            return fallback;
        }
        bool ok = false;
        const DockerContainer container = parseContainer(value, ok);
        return ok ? container : fallback;
    }

    bool credentialsFromArgs(const ArgView &view, ServerCredentials &credentials, bool allowPortAlias = true) const
    {
        credentials.hostName = view.value(QStringLiteral("--host"));
        credentials.userName = view.value(QStringLiteral("--user"));
        credentials.secretData = view.value(QStringLiteral("--secret"));
        credentials.port = view.value(QStringLiteral("--ssh-port"), allowPortAlias ? view.value(QStringLiteral("--port"), QStringLiteral("22")) : QStringLiteral("22")).toInt();
        return credentials.isValid();
    }

    int clients(QStringList args)
    {
        if (args.isEmpty()) return fail(QStringLiteral("Usage: clients list|rename|revoke <server> --container <container>"));
        const QString sub = args.takeFirst().toLower();
        const ArgView view(args);
        const QString serverId = resolveRequiredServer(view.positionals());
        if (serverId.isEmpty()) return fail(QStringLiteral("Server not found."));
        const DockerContainer container = containerFromOption(view, serversController.getDefaultContainer(serverId));

        if (sub == QLatin1String("list")) {
            QJsonArray clients;
            QEventLoop loop;
            QObject::connect(&usersController, &UsersController::clientsUpdated, &loop, [&](const QJsonArray &array) {
                clients = array;
                loop.quit();
            });
            const ErrorCode code = usersController.updateClients(serverId, container);
            if (code != ErrorCode::NoError) return fail(cleanError(code));
            QTimer::singleShot(10, &loop, &QEventLoop::quit);
            loop.exec();
            if (jsonOutput) {
                out << jsonPretty(clients) << Qt::endl;
            } else {
                out << style.bold(QStringLiteral("Clients")) << Qt::endl;
                for (int i = 0; i < clients.size(); ++i) {
                    const QJsonObject client = clients.at(i).toObject();
                    const QJsonObject userData = client.value(QStringLiteral("userData")).toObject();
                    out << "  " << i << "  " << userData.value(QStringLiteral("clientName")).toString()
                        << "  " << style.dim(client.value(QStringLiteral("clientId")).toString()) << Qt::endl;
                }
            }
            return 0;
        }
        if (sub == QLatin1String("rename")) {
            bool okIndex = false;
            const int row = view.value(QStringLiteral("--row")).toInt(&okIndex);
            const QString name = view.value(QStringLiteral("--name"));
            if (!okIndex || name.isEmpty()) return fail(QStringLiteral("Usage: clients rename <server> --container <container> --row <n> --name <name>"));
            const ErrorCode updateCode = usersController.updateClients(serverId, container);
            if (updateCode != ErrorCode::NoError) return fail(cleanError(updateCode));
            const ErrorCode code = usersController.renameClient(serverId, row, name, container);
            return code == ErrorCode::NoError ? ok(QStringLiteral("Client renamed.")) : fail(cleanError(code));
        }
        if (sub == QLatin1String("revoke")) {
            bool okIndex = false;
            const int row = view.value(QStringLiteral("--row")).toInt(&okIndex);
            if (!okIndex) return fail(QStringLiteral("Usage: clients revoke <server> --container <container> --row <n>"));
            usersController.updateClients(serverId, container);
            const ErrorCode code = usersController.revokeClient(serverId, row, container);
            return code == ErrorCode::NoError ? ok(QStringLiteral("Client revoked.")) : fail(cleanError(code));
        }
        return fail(QStringLiteral("Unknown clients command: %1").arg(sub));
    }

    int subscription(QStringList args)
    {
        if (args.isEmpty()) return fail(QStringLiteral("Usage: subscription account|update|deactivate|vpn-key|native-export|native-revoke|remove|import-trial|import-gateway"));
        const QString sub = args.takeFirst().toLower();
        const ArgView view(args);

        if (sub == QLatin1String("account")) {
            const QString serverId = resolveRequiredServer(view.positionals());
            if (serverId.isEmpty()) return fail(QStringLiteral("Server not found."));
            QJsonObject accountInfo;
            const ErrorCode code = subscriptionController.getAccountInfo(serverId, accountInfo);
            if (code != ErrorCode::NoError) return fail(cleanError(code));
            out << jsonPretty(accountInfo) << Qt::endl;
            return 0;
        }
        if (sub == QLatin1String("update")) {
            const QString serverId = resolveRequiredServer(view.positionals());
            const QString country = view.value(QStringLiteral("--country"));
            if (serverId.isEmpty() || country.isEmpty()) return fail(QStringLiteral("Usage: subscription update <server> --country <code>"));
            const ErrorCode code = subscriptionController.updateServiceFromGateway(serverId, country, false);
            return code == ErrorCode::NoError ? ok(QStringLiteral("Subscription config updated.")) : fail(cleanError(code));
        }
        if (sub == QLatin1String("deactivate")) {
            const QString serverId = resolveRequiredServer(view.positionals());
            if (serverId.isEmpty()) return fail(QStringLiteral("Server not found."));
            const ErrorCode code = subscriptionController.deactivateDevice(serverId);
            return code == ErrorCode::NoError ? ok(QStringLiteral("Device deactivated.")) : fail(cleanError(code));
        }
        if (sub == QLatin1String("vpn-key")) {
            const QString serverId = resolveRequiredServer(view.positionals());
            if (serverId.isEmpty()) return fail(QStringLiteral("Server not found."));
            QString vpnKey;
            const ErrorCode code = subscriptionController.prepareVpnKeyExport(serverId, vpnKey);
            if (code != ErrorCode::NoError) return fail(cleanError(code));
            return outputString(vpnKey, view.valueAny({ "--out", "-o" }));
        }
        if (sub == QLatin1String("native-export")) {
            const QString serverId = resolveRequiredServer(view.positionals());
            const QString country = view.value(QStringLiteral("--country"));
            if (serverId.isEmpty() || country.isEmpty()) return fail(QStringLiteral("Usage: subscription native-export <server> --country <code> [--out file]"));
            QString nativeConfig;
            const ErrorCode code = subscriptionController.exportNativeConfig(serverId, country, nativeConfig);
            if (code != ErrorCode::NoError) return fail(cleanError(code));
            return outputString(nativeConfig, view.valueAny({ "--out", "-o" }));
        }
        if (sub == QLatin1String("native-revoke")) {
            const QString serverId = resolveRequiredServer(view.positionals());
            const QString country = view.value(QStringLiteral("--country"));
            if (serverId.isEmpty() || country.isEmpty()) return fail(QStringLiteral("Usage: subscription native-revoke <server> --country <code>"));
            const ErrorCode code = subscriptionController.revokeNativeConfig(serverId, country);
            return code == ErrorCode::NoError ? ok(QStringLiteral("Native config revoked.")) : fail(cleanError(code));
        }
        if (sub == QLatin1String("remove")) {
            const QString serverId = resolveRequiredServer(view.positionals());
            if (serverId.isEmpty()) return fail(QStringLiteral("Server not found."));
            return subscriptionController.removeServer(serverId) ? ok(QStringLiteral("Subscription server removed.")) : fail(QStringLiteral("Unable to remove subscription server."));
        }
        if (sub == QLatin1String("import-trial")) {
            const QString country = view.value(QStringLiteral("--country"));
            const QString type = view.value(QStringLiteral("--service-type"), QStringLiteral("amnezia-free"));
            const QString protocol = view.value(QStringLiteral("--service-protocol"), QStringLiteral("awg"));
            const QString email = view.value(QStringLiteral("--email"));
            if (country.isEmpty() || email.isEmpty()) return fail(QStringLiteral("Usage: subscription import-trial --country <code> --email <email> [--service-type type] [--service-protocol protocol]"));
            const ErrorCode code = subscriptionController.importTrialFromGateway(country, type, protocol, email);
            return code == ErrorCode::NoError ? ok(QStringLiteral("Trial imported.")) : fail(cleanError(code));
        }
        if (sub == QLatin1String("import-gateway")) {
            const QString country = view.value(QStringLiteral("--country"));
            const QString type = view.value(QStringLiteral("--service-type"), QStringLiteral("amnezia-premium"));
            const QString protocol = view.value(QStringLiteral("--service-protocol"), QStringLiteral("awg"));
            if (country.isEmpty()) return fail(QStringLiteral("Usage: subscription import-gateway --country <code> [--service-type type] [--service-protocol protocol]"));
            SubscriptionController::CaptchaInfo captcha;
            const auto protocolData = subscriptionController.generateProtocolData(protocol);
            const ErrorCode code = subscriptionController.importServiceFromGateway(country, type, protocol, protocolData, captcha);
            if (code != ErrorCode::NoError) {
                if (captcha.isRequired && !jsonOutput) {
                    err << "Captcha id: " << captcha.captchaId << Qt::endl;
                    err << "Captcha hint: " << captcha.hint << Qt::endl;
                    err << "Captcha image base64: " << captcha.captchaImageBase64 << Qt::endl;
                }
                return fail(cleanError(code));
            }
            return ok(QStringLiteral("Gateway service imported."));
        }
        return fail(QStringLiteral("Unknown subscription command: %1").arg(sub));
    }

    int outputString(const QString &value, const QString &path)
    {
        if (path.isEmpty()) {
            out << value << Qt::endl;
            return 0;
        }
        QString error;
        if (!writeFile(path, value.toUtf8(), error)) return fail(error);
        return ok(QStringLiteral("Written to %1.").arg(path));
    }

    int catalog()
    {
        QJsonObject data;
        const ErrorCode code = servicesCatalogController.fillAvailableServices(data);
        if (code != ErrorCode::NoError) return fail(cleanError(code));
        out << jsonPretty(data) << Qt::endl;
        return 0;
    }

    int news()
    {
        const QFuture<QPair<ErrorCode, QJsonArray>> future = newsController.fetchNews();
        QEventLoop loop;
        QTimer timer;
        timer.setInterval(50);
        QObject::connect(&timer, &QTimer::timeout, &loop, [&]() {
            if (future.isFinished()) {
                loop.quit();
            }
        });
        timer.start();
        loop.exec();
        const auto result = future.result();
        if (result.first != ErrorCode::NoError) return fail(cleanError(result.first));
        out << jsonPretty(result.second) << Qt::endl;
        return 0;
    }

    void clearConsole()
    {
        out << QStringLiteral("\x1b[2J\x1b[H") << Qt::flush;
    }

    QString promptServerName() const
    {
        const QString id = serversController.getDefaultServerId();
        if (id.isEmpty()) {
            return QStringLiteral("none");
        }
        QString name = displayName(id);
        if (name.size() > 24) {
            name = name.left(21) + QStringLiteral("...");
        }
        return name;
    }

    int shell()
    {
        const bool previousJsonOutput = jsonOutput;
        jsonOutput = false;

        printLogo();
        dashboard(false);
        out << Qt::endl;
        out << style.dim(QStringLiteral("Type `help` for commands, `exit` to quit. Shortcuts: ls, use, up, down, show, cfg.")) << Qt::endl;

        QTextStream input(stdin);
        for (;;) {
            g_stopRequested = false;
            out << Qt::endl << style.bold(cliCommandName()) << style.dim(QStringLiteral(" [%1]> ").arg(promptServerName())) << Qt::flush;

            const QString line = input.readLine();
            if (line.isNull()) {
                out << Qt::endl;
                jsonOutput = previousJsonOutput;
                return 0;
            }

            const QString trimmed = line.trimmed();
            if (trimmed.isEmpty()) {
                continue;
            }
            const QString lower = trimmed.toLower();
            if (lower == QLatin1String("exit") || lower == QLatin1String("quit") || lower == QLatin1String("q")) {
                jsonOutput = previousJsonOutput;
                return 0;
            }
            if (lower == QLatin1String("clear") || lower == QLatin1String("cls")) {
                clearConsole();
                continue;
            }

            QStringList args = QProcess::splitCommand(trimmed);
            if (args.isEmpty()) {
                continue;
            }
            dispatch(args, true);
        }
    }
};
} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(cliCommandName());
    QCoreApplication::setOrganizationName(QStringLiteral(ORGANIZATION_NAME));
    QCoreApplication::setApplicationVersion(QStringLiteral(APP_VERSION));

    Logger::setConsoleOutputEnabled(false);

    ssh_init();
    QObject::connect(&app, &QCoreApplication::aboutToQuit, []() { ssh_finalize(); });

    Cli cli;
    QStringList args = QCoreApplication::arguments();
    args.removeFirst();
    const int code = cli.run(args);
    ssh_finalize();
    return code;
}
