#include <QCommandLineParser>
#include <QStringList>
#include <QGuiApplication>
#include <QScreen>
#include <QImage>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTimer>
#include <QUrl>

#include <cstdio>

#include "core/Log.h"
#include "core/PlaylistModel.h"
#include "core/Settings.h"
#include "core/SubtitleTrack.h"
#include "render/VideoItem.h"

int runSelfTest(const std::string& path, const std::string& multiAudioPath);

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

#ifdef _WIN32
// Logs the module and offset of a fatal crash; makes driver vs. own-code
// failures obvious from the log alone.
LONG WINAPI crashHandler(EXCEPTION_POINTERS* info) {
    const auto* rec = info->ExceptionRecord;
    HMODULE mod = nullptr;
    char path[MAX_PATH]{};
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(rec->ExceptionAddress), &mod)) {
        GetModuleFileNameA(mod, path, MAX_PATH);
    }
    LOGE("crash: code=0x%08lx addr=%p module=%s",
         static_cast<unsigned long>(rec->ExceptionCode), rec->ExceptionAddress,
         path[0] ? path : "?");
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

// Route Qt's own diagnostics through our logger so stdout stays a single,
// ordered, timestamped stream (device-loss reports land next to our frames).
void qtMessageHandler(QtMsgType type, const QMessageLogContext&, const QString& msg) {
    const char* level = "D";
    switch (type) {
    case QtDebugMsg: level = "D"; break;
    case QtInfoMsg: level = "I"; break;
    case QtWarningMsg: level = "W"; break;
    case QtCriticalMsg: level = "E"; break;
    case QtFatalMsg: level = "F"; break;
    }
    logPrint(level, "qt: %s", qPrintable(msg));
}

}  // namespace

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("play"));
    app.setOrganizationName(QStringLiteral("play"));

    // The control backgrounds/handles are fully customised in QML; the native
    // Windows style ignores those properties, so pin the Basic style.
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    qInstallMessageHandler(qtMessageHandler);
#ifdef _WIN32
    SetUnhandledExceptionFilter(&crashHandler);
#endif

    // The render path uploads NV12 planes, so any RHI backend works, but D3D11
    // is the target for the zero-copy upgrade later.
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);

    qmlRegisterType<VideoItem>("Play", 1, 0, "VideoItem");

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("play"));
    parser.addHelpOption();
    parser.addPositionalArgument(QStringLiteral("file"), QStringLiteral("Video file to play"));
    QCommandLineOption selfTestOpt(QStringLiteral("selftest"),
                                   QStringLiteral("Run the headless playback self-test on <file> "
                                                  "[second file for audio-track checks]."));
    parser.addOption(selfTestOpt);
    QCommandLineOption shotOpt(QStringLiteral("shot"),
                               QStringLiteral("Save a window screenshot to <file> and exit."),
                               QStringLiteral("file"));
    parser.addOption(shotOpt);
    QCommandLineOption shotDelayOpt(QStringLiteral("shot-delay"),
                                    QStringLiteral("Milliseconds to wait before the screenshot."),
                                    QStringLiteral("ms"), QStringLiteral("2500"));
    parser.addOption(shotDelayOpt);
    QCommandLineOption speedOpt(QStringLiteral("speed"),
                                QStringLiteral("Initial playback rate, 0.25..4 (default 1)."),
                                QStringLiteral("rate"), QStringLiteral("1"));
    parser.addOption(speedOpt);
    QCommandLineOption fullOpt(QStringLiteral("fullscreen"),
                               QStringLiteral("Start in fullscreen (F or a double click toggles)."));
    parser.addOption(fullOpt);
    QCommandLineOption scalerOpt(
        QStringLiteral("scaler"),
        QStringLiteral("Resampling filter when the window does not match the video: "
                       "bilinear (default), bicubic, lanczos3."),
        QStringLiteral("filter"));
    parser.addOption(scalerOpt);
    QCommandLineOption subInfoOpt(QStringLiteral("subinfo"),
                                  QStringLiteral("List the subtitle streams of <file> and decode "
                                                 "every text track (cue counts)."),
                                  QStringLiteral("file"));
    parser.addOption(subInfoOpt);
    QCommandLineOption hdrOpt(
        QStringLiteral("hdr"),
        QStringLiteral("HDR output: auto (default) asks for a scRGB swapchain and keeps it "
                       "when Windows HDR is on for the display; off stays SDR."),
        QStringLiteral("mode"), QStringLiteral("auto"));
    parser.addOption(hdrOpt);
    QCommandLineOption recordOpt(QStringLiteral("record"),
                                 QStringLiteral("Record the playback to <file> (MP4) as soon as "
                                                "it starts; the file is finalised on exit."),
                                 QStringLiteral("file"));
    parser.addOption(recordOpt);
    QCommandLineOption seekOpt(QStringLiteral("seek"),
                               QStringLiteral("Start playback at <seconds>."),
                               QStringLiteral("seconds"));
    parser.addOption(seekOpt);
    QCommandLineOption exportOpt(
        QStringLiteral("export"),
        QStringLiteral("Export the clip <start>:<end> (seconds) to a file next to the "
                       "source, then exit; playback runs in real time."),
        QStringLiteral("start:end"));
    parser.addOption(exportOpt);
    QCommandLineOption screenshotOpt(
        QStringLiteral("screenshot"),
        QStringLiteral("Save the current frame to <file> (PNG) through the render "
                       "chain and exit. Use --shot-delay to pick the moment."),
        QStringLiteral("file"));
    parser.addOption(screenshotOpt);
    parser.process(app);

    // Qt only creates the scRGB swapchain if the window's display has HDR
    // switched on; otherwise it logs and falls back to SDR. The renderer reads
    // the real format back, so "auto" is safe on SDR-only machines.
    const QString hdrMode = parser.value(hdrOpt).trimmed().toLower();
    const bool hdrRequested = hdrMode != QStringLiteral("off") && hdrMode != QStringLiteral("0");
    if (hdrRequested && qEnvironmentVariableIsEmpty("QSG_RHI_HDR")) {
        qputenv("QSG_RHI_HDR", "scrgb");
    }

    if (parser.isSet(subInfoOpt)) {
#ifdef _WIN32
        SetConsoleOutputCP(CP_UTF8);  // cue text is UTF-8
#endif
        const QString path = parser.value(subInfoOpt);
        const auto tracks = SubtitleTrack::embeddedTracks(path);
        std::printf("subtitle streams: %zu\n", tracks.size());
        for (const auto& t : tracks) {
            std::printf("  #%d codec=%s lang=%s title=%s\n", t.index, qPrintable(t.codec),
                        qPrintable(t.language), qPrintable(t.title));
            if (!t.text) {
                std::printf("    bitmap format, skipped\n");
                continue;
            }
            SubtitleTrack st;
            QString err;
            if (!st.loadEmbedded(path, t.index, &err)) {
                std::printf("    FAILED: %s\n", qPrintable(err));
                continue;
            }
            const QString first = st.textAt(1.2);
            std::printf("    %zu cues, at 1.2s: %s\n", st.cueCount(),
                        first.isEmpty() ? "(silent)" : qPrintable(first));
        }
        std::fflush(stdout);
        std::_Exit(0);
    }

    if (parser.isSet(selfTestOpt)) {
        const auto args = parser.positionalArguments();
        if (args.isEmpty()) {
            std::fprintf(stderr, "--selftest needs a video file\n");
            return 2;
        }
        const std::string second = args.size() > 1 ? args.at(1).toStdString() : std::string();
        return runSelfTest(args.at(0).toStdString(), second);
    }

    // Everything on the command line becomes the queue; the first one plays.
    const QStringList files = parser.positionalArguments();
    const QString file = files.value(0);

    // Outlives the engine so the context property stays valid during teardown.
    PlaylistModel playlist;
    // Volume/rate/scaler/window size survive a restart.
    Settings settings;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("playlist"), &playlist);
    engine.rootContext()->setContextProperty(QStringLiteral("settings"), &settings);
    engine.rootContext()->setContextProperty(QStringLiteral("startupFile"), file);
    engine.rootContext()->setContextProperty(QStringLiteral("startupFiles"), files);
    // Dev hook: open the queue drawer on start (screenshots, manual checks).
    engine.rootContext()->setContextProperty(
        QStringLiteral("startupPlaylistOpen"),
        qEnvironmentVariableIsSet("PLAY_OPEN_PLAYLIST"));
    // Only when given: otherwise the stored preference wins (0 = "not set").
    engine.rootContext()->setContextProperty(
        QStringLiteral("startupSpeed"),
        parser.isSet(speedOpt) ? parser.value(speedOpt).toDouble() : 0.0);
    engine.rootContext()->setContextProperty(QStringLiteral("startupFullscreen"),
                                             parser.isSet(fullOpt));
    engine.rootContext()->setContextProperty(
        QStringLiteral("startupScaler"),
        parser.isSet(scalerOpt) ? parser.value(scalerOpt) : QString());
    engine.rootContext()->setContextProperty(QStringLiteral("startupRecord"),
                                             parser.isSet(recordOpt) ? parser.value(recordOpt)
                                                                     : QString());
    engine.rootContext()->setContextProperty(QStringLiteral("startupSeek"),
                                             parser.isSet(seekOpt) ? parser.value(seekOpt).toDouble()
                                                                   : -1.0);
    engine.rootContext()->setContextProperty(
        QStringLiteral("startupExport"),
        parser.isSet(exportOpt) ? parser.value(exportOpt) : QString());
    engine.rootContext()->setContextProperty(
        QStringLiteral("startupScreenshot"),
        parser.isSet(screenshotOpt) ? parser.value(screenshotOpt) : QString());
    engine.rootContext()->setContextProperty(
        QStringLiteral("startupShotDelay"), parser.value(shotDelayOpt).toInt());
    // --export a:b -> two numbers, so the QML side does not have to parse or
    // wait for the duration (the item queues it until the file is open).
    {
        qreal a = -1.0, b = -1.0;
        if (parser.isSet(exportOpt)) {
            const QStringList parts = parser.value(exportOpt).split(QLatin1Char(':'));
            if (parts.size() == 2) {
                bool okA = false, okB = false;
                a = parts[0].toDouble(&okA);
                b = parts[1].toDouble(&okB);
                if (!okA || !okB) a = b = -1.0;
            }
        }
        engine.rootContext()->setContextProperty(QStringLiteral("startupExportA"), a);
        engine.rootContext()->setContextProperty(QStringLiteral("startupExportB"), b);
    }
    engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
    if (engine.rootObjects().isEmpty()) return 1;

    if (parser.isSet(shotOpt)) {
        const QString path = parser.value(shotOpt);
        const int delay = parser.value(shotDelayOpt).toInt();
        QTimer::singleShot(delay, qApp, [&engine, path] {
            auto* w = qobject_cast<QQuickWindow*>(engine.rootObjects().value(0));
            if (!w) { std::fprintf(stderr, "shot: no window\n"); QGuiApplication::exit(1); return; }
            // Note: QQuickWindow::grabWindow() re-renders the scene into an
            // offscreen target. The harness maximises the window well before
            // the shot so a resize is never in flight here.
            const QImage img = w->grabWindow();
            if (!img.save(path)) { std::fprintf(stderr, "shot: save failed\n"); QGuiApplication::exit(1); return; }
            std::printf("shot: %s %dx%d\n", qPrintable(path), img.width(), img.height());
            std::fflush(stdout);
            QGuiApplication::quit();
        });
    }

    return app.exec();
}
