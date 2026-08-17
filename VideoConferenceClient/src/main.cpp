#include "MainWindow.h"

#include <QtCore/QCommandLineOption>
#include <QtCore/QCommandLineParser>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QSettings>
#include <QtCore/QString>
#include <QtGui/QSurfaceFormat>
#include <QtWidgets/QApplication>
#include <QtWidgets/QMessageBox>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace {

std::string ToStdString(const QString& value)
{
    const QByteArray utf8 = value.toUtf8();
    return std::string(utf8.constData(), static_cast<std::size_t>(utf8.size()));
}

bool LoadEngineConfig(const QString& file_path,
                      VCE::EngineConfig& config,
                      QString& error_message)
{
    const QFileInfo file_info(file_path);

    if (!file_info.exists() || !file_info.isFile()) {
        error_message = QObject::tr("Configuration file does not exist:\n%1")
                            .arg(QDir::toNativeSeparators(file_path));
        return false;
    }

    QSettings settings(file_path, QSettings::IniFormat);

    config.sample_rate =
        settings.value(QStringLiteral("engine/sample_rate"), 48000).toInt();

    config.channels =
        settings.value(QStringLiteral("engine/channels"), 2).toInt();

    config.video_width =
        settings.value(QStringLiteral("engine/video_width"), 640).toInt();

    config.video_height =
        settings.value(QStringLiteral("engine/video_height"), 480).toInt();

    config.video_fps =
        settings.value(QStringLiteral("engine/video_fps"), 30).toInt();

    config.service.server_address = ToStdString(
        settings.value(QStringLiteral("service/server_address"))
            .toString()
            .trimmed());

    config.service.client_ip = ToStdString(
        settings.value(QStringLiteral("service/client_ip"))
            .toString()
            .trimmed());

    const qint64 request_timeout_ms =
        settings.value(QStringLiteral("service/request_timeout_ms"), 5000)
            .toLongLong();

    config.service.request_timeout =
        std::chrono::milliseconds(request_timeout_ms);

    config.service.media_http_scheme = ToStdString(
        settings.value(QStringLiteral("service/media_http_scheme"), QStringLiteral("http"))
            .toString()
            .trimmed());

    const int media_http_port =
        settings.value(QStringLiteral("service/media_http_port"), 80).toInt();

    const int media_rtc_port =
        settings.value(QStringLiteral("service/media_rtc_port"), 8000).toInt();

    config.service.whip_path = ToStdString(
        settings.value(QStringLiteral("service/whip_path"), QStringLiteral("/rtc/v1/whip/"))
            .toString()
            .trimmed());

    config.service.whep_path = ToStdString(
        settings.value(QStringLiteral("service/whep_path"), QStringLiteral("/rtc/v1/whep/"))
            .toString()
            .trimmed());

    config.service.app_name = ToStdString(
        settings.value(QStringLiteral("service/app_name"), QStringLiteral("live"))
            .toString()
            .trimmed());

    config.service.publish_secret = ToStdString(
        settings.value(QStringLiteral("service/publish_secret"))
            .toString()
            .trimmed());

    if (settings.status() != QSettings::NoError) {
        error_message =
            QObject::tr("Failed to read configuration file:\n%1")
                .arg(QDir::toNativeSeparators(file_path));
        return false;
    }

    if (config.sample_rate <= 0 || config.channels <= 0) {
        error_message =
            QObject::tr("The audio format in the configuration file is invalid.");
        return false;
    }

    if (config.video_width <= 0 || config.video_height <= 0 ||
        (config.video_width % 2) != 0 ||
        (config.video_height % 2) != 0 ||
        config.video_fps <= 0) {
        error_message =
            QObject::tr("The video format in the configuration file is invalid.");
        return false;
    }

    if (config.service.server_address.empty()) {
        error_message =
            QObject::tr("service/server_address cannot be empty.");
        return false;
    }

    if (config.service.client_ip.empty()) {
        error_message =
            QObject::tr("service/client_ip cannot be empty.");
        return false;
    }

    if (request_timeout_ms <= 0) {
        error_message =
            QObject::tr("service/request_timeout_ms must be greater than zero.");
        return false;
    }

    if (media_http_port <= 0 || media_http_port > 65535 ||
        media_rtc_port <= 0 || media_rtc_port > 65535) {
        error_message =
            QObject::tr("The configured media server port is invalid.");
        return false;
    }

    if (config.service.media_http_scheme.empty() ||
        config.service.whip_path.empty() ||
        config.service.whep_path.empty() ||
        config.service.app_name.empty()) {
        error_message =
            QObject::tr("The media service configuration is incomplete.");
        return false;
    }

    config.service.media_http_port =
        static_cast<std::uint16_t>(media_http_port);

    config.service.media_rtc_port =
        static_cast<std::uint16_t>(media_rtc_port);

    return true;
}

QString DefaultConfigPath()
{
    /*
     * 开发阶段可执行程序通常位于项目build目录，
     * 默认读取项目config/client.ini。
     */
    return QDir::cleanPath(
        QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("../config/client.ini")));
}

void ConfigureOpenGL()
{
    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGL);
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setDepthBufferSize(0);
    format.setStencilBufferSize(0);
    format.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
    format.setSwapInterval(1);

    QSurfaceFormat::setDefaultFormat(format);
}

} // namespace

int main(int argc, char* argv[])
{
    /*
     * 必须在创建QApplication前启用上下文共享。
     * 多个VideoWidget需要访问RenderEngine创建的共享OpenGL资源。
     */
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    ConfigureOpenGL();

    QApplication application(argc, argv);

    QCoreApplication::setOrganizationName(QStringLiteral("VCE"));
    QCoreApplication::setApplicationName(QStringLiteral("VceMeetingClient"));
    QCoreApplication::setApplicationVersion(QStringLiteral("1.0.0"));
    QApplication::setApplicationDisplayName(QObject::tr("VCE Video Conference"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QObject::tr("VCE Qt video conference client"));

    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption config_option(
        {QStringLiteral("c"), QStringLiteral("config")},
        QObject::tr("Load the specified client configuration file."),
        QObject::tr("file"));

    parser.addOption(config_option);
    parser.addPositionalArgument(
        QStringLiteral("config"),
        QObject::tr("Optional client configuration file path."));

    parser.process(application);

    QString config_path = parser.value(config_option).trimmed();

    if (config_path.isEmpty()) {
        const QStringList positional_arguments = parser.positionalArguments();

        if (!positional_arguments.isEmpty()) {
            config_path = positional_arguments.first().trimmed();
        }
    }

    if (config_path.isEmpty()) {
        config_path = DefaultConfigPath();
    }

    config_path = QFileInfo(config_path).absoluteFilePath();

    VCE::EngineConfig engine_config;
    QString config_error;

    if (!LoadEngineConfig(config_path, engine_config, config_error)) {
        QMessageBox::critical(
            nullptr,
            QObject::tr("Configuration Error"),
            config_error);

        return EXIT_FAILURE;
    }

    VCE::CLIENT::MainWindow main_window(std::move(engine_config));

    if (!main_window.initialize()) {
        QMessageBox::critical(
            nullptr,
            QObject::tr("Initialization Error"),
            QObject::tr("Failed to initialize the meeting engine. "
                        "Check the service configuration and application log."));

        return EXIT_FAILURE;
    }

    main_window.show();
    return application.exec();
}